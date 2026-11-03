#pragma once
#include <cmath>
#include <vector>

#include "algo/algos.hpp"
#include "analytics/tca.hpp"
#include "core/event_loop.hpp"
#include "md/replay.hpp"

// Runs execution algos against an ITCH replay. Our child orders go through the
// OMS (and its risk check), reach the venue `latency` later, rest in the same
// books as the feed's orders, and their execution reports come back `latency`
// after that. Passive children fill when the real flow reaches them in the
// queue (Venue::replay_execute). Everything runs on the feed's clock, so a
// run is deterministic.
//
// ponytail: algos read the venue's book directly, with no market data delay;
// route level updates through MarketData with a delay if that ever matters.
class ExecSim {
public:
    struct Fill {
        uint32_t algo;
        Nanos time;  // at the venue
        Price px;
        Qty qty;
        double mid;    // just before the fill
        bool passive;  // a resting child, not an IOC
    };

    // What TCA needs besides the fills, captured on the feed clock.
    struct Run {
        bool begun = false;
        Nanos finished = 0;
        double arrival_mid = NAN, end_mid = NAN, post_mid = NAN;
        Qty vol0 = 0, vol1 = 0;                 // all prints on the symbol (ours too)
        double notional0 = 0, notional1 = 0;
        Qty market0 = 0;                        // prints that weren't ours, at start
    };

    ExecSim(ItchReplay& replay, Venue& venue, Oms& oms, Risk& risk, Nanos latency, Nanos post = 10'000'000'000)
        : replay_(replay), venue_(venue), oms_(oms), risk_(risk), latency_(latency), post_(post) {}

    // Algo i must have been built with ClOrdId base (i + 1) << 32 (see base()).
    // It starts at its parent's start time.
    void add(Algo& a) {
        algos_.push_back(&a);
        runs_.emplace_back();
        loop_.schedule_at(std::max(a.parent().start, loop_.now()), {Ev::Timer, uint32_t(algos_.size() - 1)});
    }
    static ClOrdId base(size_t i) { return ClOrdId(i + 1) << 32; }

    // Use in place of replay.apply(m, out).
    template <class M>
    void apply(const M& m) {
        if constexpr (std::is_same_v<M, itch::StockDirectory>) {
            replay_.apply(m, out_);
        } else {
            std::optional<SymbolId> s = replay_.symbol_at(m.locate);
            if (!s) return;
            grow(*s);
            loop_.run_until(m.ts, [&](const Ev& e) { fire(e); });
            double before = mid(*s);
            replay_.apply(m, out_);
            route(before);
        }
    }

    const std::vector<Fill>& fills() const { return fills_; }
    const Run& run(size_t i) const { return runs_[i]; }

    TcaInput tca_input(size_t i) const {
        const Run& r = runs_[i];
        const ParentOrder& p = algos_[i]->parent();
        Qty vol = r.vol1 - r.vol0;
        TcaInput in{p.side, p.qty, r.arrival_mid, r.end_mid, r.post_mid,
                    vol ? (r.notional1 - r.notional0) / double(vol) : NAN, {}};
        for (const Fill& f : fills_)
            if (f.algo == i) in.fills.push_back({f.px, f.qty, f.mid});
        return in;
    }

private:
    struct Ev {
        enum Kind : uint8_t { Timer, Post, ToVenue, ToClient } kind;
        uint32_t algo = 0;
        VenueRequest rq{};
        ExecReport rep{};
    };

    void grow(SymbolId s) {
        if (s >= vol_.size()) {
            vol_.resize(s + 1);
            ours_.resize(s + 1);
            notional_.resize(s + 1);
        }
    }

    double mid(SymbolId s) const {
        const OrderBook& b = venue_.book(s);
        std::optional<Price> bid = b.best_bid(), ask = b.best_ask();
        return bid && ask ? double(*bid + *ask) / 2 : NAN;
    }

    AlgoView view(uint32_t i) {
        SymbolId s = algos_[i]->parent().sym;
        grow(s);
        const OrderBook& b = venue_.book(s);
        Quote q;
        if (std::optional<Price> bid = b.best_bid()) q.bid = *bid, q.bid_sz = b.depth(Side::Buy, *bid);
        if (std::optional<Price> ask = b.best_ask()) q.ask = *ask, q.ask_sz = b.depth(Side::Sell, *ask);
        if (q.two_sided()) risk_.mark(s, (q.bid + q.ask) / 2);  // risk needs a reference price
        return {loop_.now(), q, vol_[s] - ours_[s] - runs_[i].market0};
    }

    void fire(const Ev& e) {
        Algo& a = *algos_[e.algo];
        Run& r = runs_[e.algo];
        SymbolId s = a.parent().sym;
        switch (e.kind) {
            case Ev::Timer: {
                if (a.done()) return;
                if (!r.begun) {
                    grow(s);
                    r.begun = true;
                    r.arrival_mid = mid(s);
                    r.vol0 = vol_[s];
                    r.notional0 = notional_[s];
                    r.market0 = vol_[s] - ours_[s];
                }
                a.on_timer(view(e.algo), oms_, reqs_);
                send(e.algo);
                if (!a.done()) loop_.schedule_in(a.params().step, {Ev::Timer, e.algo});
                return;
            }
            case Ev::Post: r.post_mid = mid(s); return;
            case Ev::ToVenue: {
                venue_.set_time(loop_.now());
                double before = mid(std::visit([](const auto& x) { return x.sym; }, e.rq));
                venue_.on_request(e.rq, out_);
                route(before);
                return;
            }
            case Ev::ToClient:
                oms_.set_time(loop_.now());
                oms_.on_report(e.rep);
                if (a.done()) return;
                a.on_report(view(e.algo), oms_, reqs_);
                send(e.algo);
                return;
        }
    }

    // Requests the algo just made leave for the venue.
    void send(uint32_t i) {
        for (const VenueRequest& rq : reqs_) loop_.schedule_in(latency_, {Ev::ToVenue, i, rq});
        reqs_.clear();
        Run& r = runs_[i];
        if (algos_[i]->done() && !r.finished) {
            SymbolId s = algos_[i]->parent().sym;
            r.finished = loop_.now();
            r.end_mid = mid(s);
            r.vol1 = vol_[s];
            r.notional1 = notional_[s];
            loop_.schedule_in(post_, {Ev::Post, i});
        }
    }

    // Venue output: count prints, record our fills, send our reports back.
    void route(double mid_before) {
        for (const TradePrint& p : out_.prints) {
            grow(p.sym);
            vol_[p.sym] += p.qty;
            notional_[p.sym] += double(p.price) * double(p.qty);
        }
        for (const ExecReport& r : out_.reports) {
            const OmsOrder* o = oms_.by_id(r.id);
            if (!o) continue;  // a feed order's report
            uint32_t i = uint32_t((o->cl >> 32) - 1);
            if (r.type == ExecType::Trade) {
                ours_[r.sym] += r.last_qty;
                fills_.push_back({i, loop_.now(), r.last_px, r.last_qty, mid_before, o->tif != Tif::Ioc});
            }
            Ev e{Ev::ToClient, i};
            e.rep = r;
            loop_.schedule_in(latency_, e);
        }
        out_.clear();
    }

    ItchReplay& replay_;
    Venue& venue_;
    Oms& oms_;
    Risk& risk_;
    Nanos latency_, post_;
    EventLoop<Ev> loop_;
    std::vector<Algo*> algos_;
    std::vector<Run> runs_;
    std::vector<Fill> fills_;
    std::vector<Qty> vol_, ours_;  // per symbol: all printed volume, our part of it
    std::vector<double> notional_;
    VenueOut out_;
    std::vector<VenueRequest> reqs_;
};
