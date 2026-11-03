#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <type_traits>
#include <vector>

#include "core/event_loop.hpp"
#include "md/replay.hpp"

// Queue-position / fill-probability study on top of an ITCH replay.
//
// Every `every` ns from `start`, for each symbol and side, we decide to join
// the best price. For each latency in the sweep, a virtual order reaches the
// book `latency` later and goes to the back of the queue at that price. The
// virtual orders never touch the book, so the replay stays exact; instead
// each one remembers the ids of the real orders ahead of it:
//   - an order ahead is cancelled, deleted, replaced or executed: queue_ahead drops
//   - an order at our price that arrived after us executes, or an order at a
//     worse price executes (the market traded through us): we are filled
//   - an opposite-side order is added at or through our price: it would have
//     traded with us, so we are filled
// Post-only: if the other side has moved onto our price by the time we
// arrive, the order is not placed (counted in rejected()).
//
// ponytail: virtual orders are size-negligible: the first trade that would
// reach us fills all of it, and we never move the market. Add a size and
// partial fills if the study ever sizes orders.
// ponytail: hidden-order executions (ITCH 'P') aren't parsed, so fills are
// slightly undercounted; parse 'P' if hidden liquidity matters.

struct FillConfig {
    std::vector<Nanos> latencies{0, 10'000, 100'000, 1'000'000, 10'000'000, 100'000'000};
    Nanos start = 34'200'000'000'000;  // 09:30, first decision
    Nanos every = 100'000'000;         // decision interval
    Nanos horizon = 10'000'000'000;    // an unfilled order is cancelled after this
    Nanos markout = 1'000'000'000;     // mid measured this long after the fill
};

struct FillRow {
    SymbolId sym;
    Side side;
    Nanos latency;
    Nanos decided;      // feed time of the decision
    Price px;           // book ticks
    Qty queue_ahead;    // shares ahead of us on arrival
    bool filled;
    Nanos time_to_fill;  // from arrival; 0 if unfilled
    double markout;      // ticks in our favour: mid - px for a buy, px - mid for a sell;
                         // NaN if unfilled or the book was one-sided
};

class FillStudy {
public:
    FillStudy(ItchReplay& replay, const Venue& venue, FillConfig cfg = {})
        : replay_(replay), venue_(venue), cfg_(std::move(cfg)) {}

    // Use in place of replay.apply(m, out). Finished orders are appended to done.
    template <class M>
    void apply(const M& m, VenueOut& out, std::vector<FillRow>& done) {
        std::optional<SymbolId> s;
        if constexpr (!std::is_same_v<M, itch::StockDirectory>) {
            s = replay_.symbol_at(m.locate);
            if (s) {
                start(*s, m.ts);
                loop_.run_until(m.ts, [&](const Ev& e) { fire(e, done); });
                before(*s, m);
            }
        }
        replay_.apply(m, out);
        if (!s) return;
        if constexpr (std::is_same_v<M, itch::Add>) added(*s, ItchReplay::kIdBase + m.ref);
        if constexpr (std::is_same_v<M, itch::Replace>) added(*s, ItchReplay::kIdBase + m.new_ref);
    }

    size_t decided() const { return decided_; }    // virtual orders created
    size_t rejected() const { return rejected_; }  // post-only rejects on arrival
    size_t open() const { return open_; }          // not finished yet (censored at end of data)

private:
    struct Ev {
        enum Kind : uint8_t { Decide, Arrive, Expire, Markout } kind;
        uint32_t idx;  // symbol for Decide, else order
        uint32_t gen;  // order slots are reused; stale events are ignored
    };

    struct Virtual {
        uint32_t gen = 0;
        enum State : uint8_t { Free, Pending, Live, Filled } state = Free;
        FillRow row;
        Nanos arrived;
        Qty left;                    // shares still ahead of us
        std::vector<OrderId> ahead;  // sorted ids of the real orders ahead on arrival
    };

    using Index = std::map<Price, std::vector<uint32_t>>;  // live orders by price

    void start(SymbolId s, Nanos now) {
        if (s >= live_.size()) {
            live_.resize(s + 1);
            started_.resize(s + 1);
        }
        if (started_[s]) return;
        started_[s] = true;
        loop_.schedule_at(std::max({cfg_.start, now, loop_.now()}), {Ev::Decide, s, 0});
    }

    void fire(const Ev& e, std::vector<FillRow>& done) {
        if (e.kind == Ev::Decide) return decide(e.idx);
        Virtual& v = orders_[e.idx];
        if (v.gen != e.gen) return;
        const OrderBook& b = venue_.book(v.row.sym);
        if (e.kind == Ev::Arrive) {
            std::optional<Price> opp = v.row.side == Side::Buy ? b.best_ask() : b.best_bid();
            if (opp && (v.row.side == Side::Buy ? *opp <= v.row.px : *opp >= v.row.px)) {
                ++rejected_;
                return release(e.idx);
            }
            v.state = Virtual::Live;
            v.arrived = loop_.now();
            v.left = 0;
            b.queue(v.row.side, v.row.px, [&](OrderId id, Qty q) {
                v.ahead.push_back(id);
                v.left += q;
            });
            v.row.queue_ahead = v.left;
            std::sort(v.ahead.begin(), v.ahead.end());
            index(v.row.sym, v.row.side)[v.row.px].push_back(e.idx);
            loop_.schedule_in(cfg_.horizon, {Ev::Expire, e.idx, v.gen});
        } else if (e.kind == Ev::Expire && v.state == Virtual::Live) {
            unindex(e.idx);
            finish(e.idx, done);
        } else if (e.kind == Ev::Markout) {
            std::optional<Price> bid = b.best_bid(), ask = b.best_ask();
            if (bid && ask) {
                double mid = double(*bid + *ask) / 2;
                v.row.markout = v.row.side == Side::Buy ? mid - double(v.row.px) : double(v.row.px) - mid;
            }
            finish(e.idx, done);
        }
    }

    void decide(SymbolId s) {
        const OrderBook& b = venue_.book(s);
        for (Side side : {Side::Buy, Side::Sell}) {
            std::optional<Price> best = side == Side::Buy ? b.best_bid() : b.best_ask();
            if (!best) continue;
            for (Nanos lat : cfg_.latencies) {
                uint32_t i = alloc();
                Virtual& v = orders_[i];
                v.state = Virtual::Pending;
                v.row = {s, side, lat, loop_.now(), *best, 0, false, 0, std::nan("")};
                loop_.schedule_in(lat, {Ev::Arrive, i, v.gen});
                ++decided_;
            }
        }
        loop_.schedule_in(cfg_.every, {Ev::Decide, s, 0});
    }

    // A message is about to take qty off a resting order.
    template <class M>
    void before(SymbolId s, const M& m) {
        OrderId id;
        Qty qty = ~Qty(0);
        if constexpr (std::is_same_v<M, itch::Replace>) id = ItchReplay::kIdBase + m.old_ref;
        else if constexpr (std::is_same_v<M, itch::Add>) return;
        else id = ItchReplay::kIdBase + m.ref;
        if constexpr (std::is_same_v<M, itch::Execute> || std::is_same_v<M, itch::Cancel>) qty = m.shares;
        OrderBook::Reduced r = venue_.book(s).peek(id);
        if (!r.qty) return;  // not on the book (dropped by the replay)
        qty = std::min(qty, r.qty);
        bool exec = std::is_same_v<M, itch::Execute>;
        Index& ix = index(s, r.side);
        if (auto it = ix.find(r.price); it != ix.end()) {
            std::vector<uint32_t> hit;
            for (uint32_t i : it->second) {
                Virtual& v = orders_[i];
                if (std::binary_search(v.ahead.begin(), v.ahead.end(), id))
                    v.left -= std::min(qty, v.left);
                else if (exec)
                    hit.push_back(i);  // a later arrival at our price traded first: we were ahead of it
            }
            for (uint32_t i : hit) fill(i);
        }
        if (exec) through(s, r.side, r.price, false);
    }

    // A real order just rested; if it reaches our price on the other side, it
    // would have traded with us.
    void added(SymbolId s, OrderId id) {
        OrderBook::Reduced r = venue_.book(s).peek(id);
        if (r.qty) through(s, r.side == Side::Buy ? Side::Sell : Side::Buy, r.price, true);
    }

    // Fills every live order on `side` priced better than px (or equal, if inclusive).
    void through(SymbolId s, Side side, Price px, bool inclusive) {
        Index& ix = index(s, side);
        auto first = side == Side::Buy ? (inclusive ? ix.lower_bound(px) : ix.upper_bound(px)) : ix.begin();
        auto last = side == Side::Buy ? ix.end() : (inclusive ? ix.upper_bound(px) : ix.lower_bound(px));
        std::vector<uint32_t> hit;
        for (auto it = first; it != last; ++it) hit.insert(hit.end(), it->second.begin(), it->second.end());
        for (uint32_t i : hit) fill(i);
    }

    void fill(uint32_t i) {
        Virtual& v = orders_[i];
        unindex(i);
        v.state = Virtual::Filled;
        v.row.filled = true;
        v.row.time_to_fill = loop_.now() - v.arrived;
        loop_.schedule_in(cfg_.markout, {Ev::Markout, i, v.gen});
    }

    Index& index(SymbolId s, Side side) { return live_[s][side == Side::Buy ? 0 : 1]; }

    void unindex(uint32_t i) {
        const FillRow& r = orders_[i].row;
        Index& ix = index(r.sym, r.side);
        auto it = ix.find(r.px);
        std::vector<uint32_t>& at = it->second;
        at.erase(std::find(at.begin(), at.end(), i));
        if (at.empty()) ix.erase(it);
    }

    void finish(uint32_t i, std::vector<FillRow>& done) {
        done.push_back(orders_[i].row);
        release(i);
    }

    uint32_t alloc() {
        ++open_;
        if (free_.empty()) {
            orders_.emplace_back();
            return uint32_t(orders_.size() - 1);
        }
        uint32_t i = free_.back();
        free_.pop_back();
        return i;
    }

    void release(uint32_t i) {
        --open_;
        Virtual& v = orders_[i];
        ++v.gen;
        v.state = Virtual::Free;
        v.ahead.clear();
        free_.push_back(i);
    }

    ItchReplay& replay_;
    const Venue& venue_;
    FillConfig cfg_;
    EventLoop<Ev> loop_;
    std::vector<std::array<Index, 2>> live_;  // by symbol, then side
    std::vector<bool> started_;               // decisions scheduled
    std::vector<Virtual> orders_;
    std::vector<uint32_t> free_;
    size_t decided_ = 0, rejected_ = 0, open_ = 0;
};
