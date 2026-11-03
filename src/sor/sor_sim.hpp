#pragma once
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "core/event_loop.hpp"
#include "md/market_data.hpp"
#include "md/replay.hpp"
#include "oms/oms.hpp"
#include "sor/router.hpp"

// Several venues built from one ITCH feed. Every real order is sent to one
// venue, picked by a hash of its order reference in proportion to the venues'
// flow shares; its executions, cancels and replaces follow it there. So each
// venue holds a random slice of the real market, and together they hold all
// of it.
//
// Parent orders work through one OMS. Each child goes to one venue and pays
// that venue's latency both ways and its fees.
//   Take: route the whole order as IOCs, then route what is left again when
//         every child is back, up to max_rounds.
//   Post: rest the whole order at the best price on our side on one venue for
//         rest_for, then cancel and take what is left (with the smart router,
//         so only the choice of venue differs between the two post modes).
//
// ponytail: the router reads every venue's book with no market data delay;
// only orders pay latency. Delay the books per venue if quote fading matters.
class SorSim {
public:
    enum class Mode : uint8_t { SmartTake, NaiveTake, SmartPost, NaivePost };

    struct Parent {
        SymbolId sym;
        Side side;
        Qty qty;
        Nanos start;
        Mode mode;
    };
    struct Fill {
        uint32_t parent, venue;
        Nanos time;
        Price px;
        Qty qty;
        bool passive;
        double fee;  // ticks per share; negative = rebate
    };
    struct Result {
        double arrival_mid = NAN;
        Nanos done_at = 0;
        Qty filled = 0;
        double cost = 0;  // sum of qty * (price + fee), ticks
        uint32_t rounds = 0;
        Qty passive = 0;
        uint32_t venue_post = 0;  // post modes: where it rested
    };

    SorSim(std::vector<std::string> symbols, Router& router, std::vector<const FillTable*> tables, Oms& oms,
           Risk& risk, Nanos rest_for = 2'000'000'000, uint32_t max_rounds = 20)
        : router_(router), tables_(std::move(tables)), oms_(oms), risk_(risk), rest_for_(rest_for),
          max_rounds_(max_rounds) {
        double cum = 0;
        for (const VenueConfig& c : router.venues()) {
            venues_.push_back(std::make_unique<Venue>());
            replays_.push_back(std::make_unique<ItchReplay>(*venues_.back(), symbols));
            cum_.push_back(cum += c.flow);
        }
    }

    // Which venue a feed order lives on.
    uint32_t venue_of(uint64_t ref) const {
        double u = double((ref * 0x9E3779B97F4A7C15ull) >> 11) / double(1ull << 53);  // uniform in [0, 1)
        uint32_t v = 0;
        while (v + 1 < cum_.size() && u * cum_.back() >= cum_[v]) ++v;
        return v;
    }

    void add(const Parent& p) {
        parents_.push_back({p, {}, 0, false});
        loop_.schedule_at(std::max(p.start, loop_.now()), {Ev::Start, uint32_t(parents_.size() - 1)});
    }

    template <class M>
    void apply(const M& m) {
        if constexpr (std::is_same_v<M, itch::StockDirectory>) {
            for (auto& r : replays_) r->apply(m, out_);
            out_.clear();
        } else {
            std::optional<SymbolId> s = replays_[0]->symbol_at(m.locate);
            if (!s) return;
            loop_.run_until(m.ts, [&](const Ev& e) { fire(e); });
            for (uint32_t v = 0; v < venues_.size(); ++v) {
                if constexpr (std::is_same_v<M, itch::Add>)
                    if (venue_of(m.ref) != v) continue;
                replays_[v]->apply(m, out_);
                route(v);
            }
            sample(*s);
        }
    }

    const Venue& venue(uint32_t v) const { return *venues_[v]; }
    const std::vector<Fill>& fills() const { return fills_; }
    const Result& result(size_t i) const { return parents_[i].r; }
    const Parent& parent(size_t i) const { return parents_[i].p; }
    size_t parents() const { return parents_.size(); }

private:
    struct Ev {
        enum Kind : uint8_t { Start, RestOver, ToVenue, ToClient } kind;
        uint32_t idx;  // parent, or venue for ToVenue / ToClient
        VenueRequest rq{};
        ExecReport rep{};
    };
    struct P {
        Parent p;
        Result r;
        uint32_t live;  // children not yet terminal
        bool done;
    };
    struct Child {
        uint32_t parent, venue;
        bool ioc;
        Qty sent;
    };
    struct Vol {  // per symbol: mid sampled every 100 ms, EWMA of squared moves
        Nanos at = 0;
        double mid = NAN, var = 0;
    };
    static constexpr Nanos kSample = 100'000'000;

    Quote nbbo(SymbolId s) const {
        Quote q;
        for (const auto& v : venues_) {
            const OrderBook& b = v->book(s);
            if (auto bid = b.best_bid(); bid && (!q.bid_sz || *bid > q.bid)) q.bid = *bid, q.bid_sz = 1;
            if (auto ask = b.best_ask(); ask && (!q.ask_sz || *ask < q.ask)) q.ask = *ask, q.ask_sz = 1;
        }
        return q;  // sizes are only flags here
    }

    void sample(SymbolId s) {
        if (s >= vol_.size()) vol_.resize(s + 1);
        Vol& v = vol_[s];
        Nanos now = loop_.now();
        if (now < v.at + kSample) return;
        Quote q = nbbo(s);
        if (!q.two_sided()) return;
        double mid = double(q.bid + q.ask) / 2;
        if (!std::isnan(v.mid)) v.var = 0.95 * v.var + 0.05 * (mid - v.mid) * (mid - v.mid);
        v.mid = mid;
        v.at = now;
    }
    double sigma(SymbolId s) const {  // ticks per sqrt(ns)
        return s < vol_.size() ? std::sqrt(vol_[s].var / double(kSample)) : 0;
    }

    std::vector<VenueBook> books(SymbolId s, Side side) const {
        std::vector<VenueBook> out(venues_.size());
        Side opp = side == Side::Buy ? Side::Sell : Side::Buy;
        for (uint32_t v = 0; v < venues_.size(); ++v) {
            const OrderBook& b = venues_[v]->book(s);
            out[v].far = b.top(opp, 10);
            std::vector<LevelChange> near = b.top(side, 1);
            if (!near.empty()) out[v].near = near[0];
        }
        return out;
    }

    void fire(const Ev& e) {
        switch (e.kind) {
            case Ev::Start: return start(e.idx);
            case Ev::RestOver: {
                P& p = parents_[e.idx];
                if (p.done) return;
                for (size_t slot = first_child_[e.idx]; slot < children_.size(); ++slot)
                    if (children_[slot].parent == e.idx) send_cancel(slot);
                return;
            }
            case Ev::ToVenue:
                venues_[e.idx]->set_time(loop_.now());
                venues_[e.idx]->on_request(e.rq, out_);
                return route(e.idx);
            case Ev::ToClient: return report(e.rep);
        }
    }

    void start(uint32_t i) {
        P& p = parents_[i];
        Quote q = nbbo(p.p.sym);
        if (q.two_sided()) {
            p.r.arrival_mid = double(q.bid + q.ask) / 2;
            risk_.mark(p.p.sym, (q.bid + q.ask) / 2);
        }
        first_child_.resize(i + 1, children_.size());
        first_child_[i] = children_.size();
        bool post = p.p.mode == Mode::SmartPost || p.p.mode == Mode::NaivePost;
        Price px = p.p.side == Side::Buy ? q.bid : q.ask;
        if (!post || !(p.p.side == Side::Buy ? q.bid_sz : q.ask_sz) || !q.two_sided()) return take(i);
        uint32_t v = p.p.mode == Mode::NaivePost
                         ? router_.post_naive()
                         : router_.post(px, books(p.p.sym, p.p.side), *tables_[p.p.sym], rest_for_,
                                        double(q.ask - q.bid) / 2);
        p.r.venue_post = v;
        submit(i, v, Tif::Day, px, p.p.qty);
        loop_.schedule_in(rest_for_, {Ev::RestOver, i});
    }

    void take(uint32_t i) {
        P& p = parents_[i];
        Qty left = p.p.qty - p.r.filled;
        if (!left || p.r.rounds >= max_rounds_) return finish(i);
        ++p.r.rounds;
        std::vector<VenueBook> b = books(p.p.sym, p.p.side);
        std::vector<Slice> slices = p.p.mode == Mode::NaiveTake ? Router::take_naive(p.p.side, left, b)
                                                                 : router_.take(p.p.side, left, b, sigma(p.p.sym));
        if (slices.empty()) return finish(i);
        Quote q = nbbo(p.p.sym);
        if (q.two_sided()) risk_.mark(p.p.sym, (q.bid + q.ask) / 2);
        for (const Slice& s : slices) submit(i, s.venue, Tif::Ioc, s.limit, s.qty);
        if (!p.live) finish(i);  // every slice refused by risk
    }

    void submit(uint32_t i, uint32_t v, Tif tif, Price px, Qty qty) {
        const P& p = parents_[i];
        size_t slot = oms_.order_count();
        children_.push_back({i, v, tif == Tif::Ioc, qty});
        oms_.submit({ClOrdId(slot + 1), p.p.sym, p.p.side, OrdType::Limit, tif, px, qty}, reqs_);
        if (!is_terminal(oms_.find(ClOrdId(slot + 1))->state)) ++parents_[i].live;
        flush(v);
    }

    void send_cancel(size_t slot) {
        if (oms_.cancel(ClOrdId(slot + 1), reqs_)) flush(children_[slot].venue);
    }

    void flush(uint32_t v) {
        for (const VenueRequest& rq : reqs_) {
            Ev e{Ev::ToVenue, v};
            e.rq = rq;
            loop_.schedule_in(router_.venues()[v].latency, e);
        }
        reqs_.clear();
    }

    // Venue output: record our fills, send our reports back after the venue's latency.
    void route(uint32_t v) {
        for (const ExecReport& r : out_.reports) {
            const OmsOrder* o = oms_.by_id(r.id);
            if (!o) continue;  // a feed order
            const Child& c = children_[o->cl - 1];
            if (r.type == ExecType::Trade) {
                const VenueConfig& cfg = router_.venues()[v];
                double fee = c.ioc ? cfg.take_fee : cfg.make_fee;
                fills_.push_back({c.parent, v, loop_.now(), r.last_px, r.last_qty, !c.ioc, fee});
                Result& res = parents_[c.parent].r;
                res.filled += r.last_qty;
                res.cost += double(r.last_qty) * (double(r.last_px) + fee);
                if (!c.ioc) res.passive += r.last_qty;
            }
            Ev e{Ev::ToClient, v};
            e.rep = r;
            loop_.schedule_in(router_.venues()[v].latency, e);
        }
        out_.clear();
    }

    void report(const ExecReport& r) {
        oms_.set_time(loop_.now());
        bool was_open = !is_terminal(oms_.by_id(r.id)->state);
        oms_.on_report(r);
        const OmsOrder& o = *oms_.by_id(r.id);
        const Child& c = children_[o.cl - 1];
        if (!was_open || !is_terminal(o.state)) return;
        if (c.ioc) router_.learn(c.venue, c.sent, o.cum);
        P& p = parents_[c.parent];
        if (--p.live || p.done) return;
        take(c.parent);  // everything is back: route what is left (also ends a post)
    }

    void finish(uint32_t i) {
        parents_[i].done = true;
        parents_[i].r.done_at = loop_.now();
    }

    Router& router_;
    std::vector<const FillTable*> tables_;
    Oms& oms_;
    Risk& risk_;
    Nanos rest_for_;
    uint32_t max_rounds_;
    std::vector<std::unique_ptr<Venue>> venues_;
    std::vector<std::unique_ptr<ItchReplay>> replays_;
    std::vector<double> cum_;  // cumulative flow shares
    EventLoop<Ev> loop_;
    std::vector<P> parents_;
    std::vector<size_t> first_child_;  // by parent: its first OMS slot
    std::vector<Child> children_;      // by OMS slot (ClOrdId - 1)
    std::vector<Fill> fills_;
    std::vector<Vol> vol_;
    VenueOut out_;
    std::vector<VenueRequest> reqs_;
};
