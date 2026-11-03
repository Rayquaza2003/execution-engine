#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

#include "algo/fill_table.hpp"
#include "md/market_data.hpp"
#include "oms/oms.hpp"

// Execution algorithms: work a parent order through child orders sent via the
// OMS, so pre-trade risk applies to every child.
//
// Every style is a schedule, target(t) = shares that should be done by t, plus
// one placement rule. Each step the algo rests the new part of the schedule
// passively at the near touch. Anything that should already have been done by
// the previous step and isn't (the passive child didn't fill) is crossed with
// an IOC at the far touch. After `end` everything left is crossed, for up to
// `give_up` more; whatever is still unfilled then counts as opportunity cost.
//
//   TWAP      target grows linearly over [start, end]
//   VWAP      follows an expected cumulative volume curve
//   POV       a fixed share of the market volume seen since start
//   Iceberg   everything at once, but only `display` shares shown at a time,
//             replenished as each child fills; crosses only after `end`
//   IS        Almgren-Chriss: remaining = X sinh(k(T-t)) / sinh(kT), so it
//             front-loads more as urgency kT grows (kT -> 0 is TWAP)
//   Adaptive  the TWAP schedule, but each step scores the book and crosses the
//             whole remaining slice when the score is positive (see score())
//
// ponytail: algos act on timer steps and their own fills, not on every market
// data tick; add an on_md hook if a style needs to react within a step.

enum class Style : uint8_t { Twap, Vwap, Pov, Iceberg, Is, Adaptive };

inline const char* style_name(Style s) {
    constexpr const char* names[] = {"TWAP", "VWAP", "POV", "Iceberg", "IS", "Adaptive"};
    return names[int(s)];
}

struct ParentOrder {
    SymbolId sym;
    Side side;
    Qty qty;
    Nanos start, end;
};

struct AlgoParams {
    Style style = Style::Twap;
    Nanos step = 1'000'000'000;
    Nanos give_up = 30'000'000'000;  // after end, keep crossing this long, then stop
    std::vector<double> curve;       // VWAP: expected cumulative volume fraction at the end of
                                     // each of curve.size() equal parts of [start, end]
    double pov_rate = 0.1;           // POV: share of market volume
    Qty display = 100;               // Iceberg: shown size
    double urgency = 2;              // IS: kT

    // Adaptive. Weights are hand-set, not fitted.
    const FillTable* fills = nullptr;  // S6 fill probabilities for this symbol
    Nanos latency = 0;                 // ours, to read the table
    double w_behind = 1;     // per slice behind schedule
    double w_imbalance = 1;  // book imbalance leaning against us (-1..1)
    double w_fill = 2;       // per unit of (0.5 - P(passive fill within a step))
    double w_spread = 0.5;   // per tick of spread beyond one (crossing costs more)
    double w_vol = 0.5;      // per tick of recent mid movement per step
};

// What the algo sees: the venue's top of book and market volume (not ours) since start.
struct AlgoView {
    Nanos now;
    Quote q;
    Qty volume;
};

class Algo {
public:
    // Children get ClOrdIds base + 1, base + 2, ...
    Algo(ClOrdId base, ParentOrder p, AlgoParams a) : base_(base), p_(p), a_(std::move(a)) {}

    const ParentOrder& parent() const { return p_; }
    const AlgoParams& params() const { return a_; }
    bool owns(ClOrdId cl) const { return cl > base_ && cl <= base_ + sent_; }
    bool done() const { return done_; }
    Qty filled() const { return filled_; }

    // Shares that should be done by `now`, given market volume since start.
    Qty target(Nanos now, Qty volume) const {
        if (now <= p_.start) return 0;
        if (now >= p_.end) return p_.qty;
        double x = double(now - p_.start) / double(p_.end - p_.start), f = x;
        switch (a_.style) {
            case Style::Twap:
            case Style::Adaptive: break;
            case Style::Iceberg: f = 1; break;
            case Style::Pov: f = a_.pov_rate * double(volume) / double(p_.qty); break;
            case Style::Is:
                if (a_.urgency > 0) f = 1 - std::sinh(a_.urgency * (1 - x)) / std::sinh(a_.urgency);
                break;
            case Style::Vwap: {
                size_t n = a_.curve.size();
                if (!n) break;
                double at = x * double(n);
                size_t i = std::min(size_t(at), n - 1);
                double lo = i ? a_.curve[i - 1] : 0;
                f = lo + (a_.curve[i] - lo) * (at - double(i));
                break;
            }
        }
        return std::min(p_.qty, Qty(std::max(0.0, f) * double(p_.qty)));
    }

    // Every step: drop passive children the market moved away from, and the
    // ones that fell behind schedule (they get crossed once the cancel lands),
    // then place what the schedule asks for.
    void on_timer(const AlgoView& v, Oms& oms, std::vector<VenueRequest>& out) {
        if (done_) return;
        tally(oms);
        bool late = v.now >= p_.end;
        if (late && v.now >= p_.end + a_.give_up) return finish(oms, out);
        due_ = late ? p_.qty : want_;
        want_ = late ? p_.qty : target(v.now, v.volume);
        Qty behind = sat(due_, filled_ + crossing_);  // resting children didn't deliver it
        bool schedule = a_.style != Style::Iceberg && a_.style != Style::Adaptive;
        cross_all_ = late || (a_.style == Style::Adaptive && score(v, behind) > 0);
        bool drop_all = cross_all_ || (schedule && behind > 0);
        Price touch = near(v.q);
        for (ClOrdId cl : live_) {
            const OmsOrder& o = *oms.find(cl);
            if (o.tif != Tif::Ioc && o.state != OrdState::CancelPending && (drop_all || o.px != touch))
                oms.cancel(cl, out);
        }
        place(v, oms, out);
        last_mid_ = mid(v.q);
    }

    // One of our children changed (fill, cancel, IOC done): place what that freed up.
    void on_report(const AlgoView& v, Oms& oms, std::vector<VenueRequest>& out) {
        if (done_) return;
        tally(oms);
        place(v, oms, out);
    }

private:
    static Qty sat(Qty a, Qty b) { return a > b ? a - b : 0; }
    static double mid(const Quote& q) { return q.two_sided() ? double(q.bid + q.ask) / 2 : NAN; }
    Price near(const Quote& q) const { return p_.side == Side::Buy ? q.bid : q.ask; }
    Price far(const Quote& q) const { return p_.side == Side::Buy ? q.ask : q.bid; }

    // filled_ and the open quantity of live children; drops finished children.
    void tally(const Oms& oms) {
        Qty live_filled = 0;
        crossing_ = resting_ = cancelling_ = 0;
        size_t keep = 0;
        for (ClOrdId cl : live_) {
            const OmsOrder& o = *oms.find(cl);
            if (is_terminal(o.state)) {
                filled_done_ += o.cum;
                continue;
            }
            live_filled += o.cum;
            (o.tif == Tif::Ioc ? crossing_ : resting_) += o.leaves;
            cancelling_ += o.state == OrdState::CancelPending;
            live_[keep++] = cl;
        }
        live_.resize(keep);
        filled_ = filled_done_ + live_filled;
    }

    // Crosses what is due (or everything, in cross-all mode) and rests the rest.
    // Counting every open child in `open` means nothing is ever sent twice.
    // While a cancel is in flight it waits: the freed shares must be split
    // between crossing and resting once all of them are back.
    void place(const AlgoView& v, Oms& oms, std::vector<VenueRequest>& out) {
        if (filled_ >= p_.qty) return finish(oms, out);
        if (cancelling_) return;
        Qty open = filled_ + crossing_ + resting_;
        Qty need = sat(want_, open);  // the schedule moves on timer steps only
        Qty lag = 0;
        if (cross_all_) lag = need;
        else if (a_.style != Style::Iceberg && a_.style != Style::Adaptive) lag = std::min(need, sat(due_, open));
        Qty passive = need - lag;
        if (a_.style == Style::Iceberg) passive = resting_ ? 0 : std::min(passive, a_.display);
        if (lag && (p_.side == Side::Buy ? v.q.ask_sz : v.q.bid_sz))
            send(oms, out, Tif::Ioc, far(v.q), lag);
        if (passive && (p_.side == Side::Buy ? v.q.bid_sz : v.q.ask_sz))
            send(oms, out, Tif::Day, near(v.q), passive);
    }

    void send(Oms& oms, std::vector<VenueRequest>& out, Tif tif, Price px, Qty qty) {
        ClOrdId cl = base_ + ++sent_;
        oms.submit({cl, p_.sym, p_.side, OrdType::Limit, tif, px, qty}, out);
        live_.push_back(cl);
    }

    void finish(Oms& oms, std::vector<VenueRequest>& out) {
        for (ClOrdId cl : live_) oms.cancel(cl, out);
        done_ = true;
    }

    // Adaptive: positive = cross the whole slice now, else wait in the queue.
    double score(const AlgoView& v, Qty behind) const {
        if (!v.q.two_sided()) return -1;
        double slice = double(p_.qty) * double(a_.step) / double(p_.end - p_.start);
        double sg = p_.side == Side::Buy ? 1 : -1;
        // Bids heavier than asks tends to precede an up move: bad to wait when buying.
        double imb = sg * (double(v.q.bid_sz) - double(v.q.ask_sz)) / double(v.q.bid_sz + v.q.ask_sz);
        Qty queue = p_.side == Side::Buy ? v.q.bid_sz : v.q.ask_sz;
        double p = a_.fills ? a_.fills->p_fill(queue, a_.latency, a_.step) : 0.5;
        double moved = std::isnan(last_mid_) ? 0 : std::fabs(mid(v.q) - last_mid_);
        return a_.w_behind * double(behind) / slice + a_.w_imbalance * imb + a_.w_fill * (0.5 - p) -
               a_.w_spread * double(v.q.ask - v.q.bid - 1) + a_.w_vol * moved - 1;
    }

    ClOrdId base_;
    ParentOrder p_;
    AlgoParams a_;
    uint64_t sent_ = 0;
    std::vector<ClOrdId> live_;  // children not yet terminal
    Qty filled_ = 0, filled_done_ = 0, crossing_ = 0, resting_ = 0;
    size_t cancelling_ = 0;  // live children with a cancel in flight
    Qty want_ = 0;  // schedule target at the last step
    Qty due_ = 0;   // target at the step before: should be done by now
    bool cross_all_ = false;  // until the next step: cross everything needed
    double last_mid_ = NAN;
    bool done_ = false;
};
