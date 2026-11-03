#pragma once
#include <functional>
#include <map>
#include <tuple>
#include <vector>

#include "exchange/venue.hpp"

using VenueId = uint32_t;

struct Quote {
    Price bid = 0, ask = 0;
    Qty bid_sz = 0, ask_sz = 0;  // 0 = that side is empty
    bool two_sided() const { return bid_sz && ask_sz; }
};

// Client-side market data. Rebuilds one L2 book per (venue, symbol) from the
// venues' incremental level updates in the order they arrive, which is after
// feed latency, so it can lag the venue's own book. Consolidates the best bid
// and offer across venues (NBBO).
//
// ponytail: std::map allocates per new price level; a flat ladder like
// OrderBook's if MD throughput ever matters more than simplicity.
class MarketData {
public:
    struct L2 {
        std::map<Price, Qty, std::greater<Price>> bids;  // best first
        std::map<Price, Qty> asks;                       // best first
    };

    MarketData(size_t venues, size_t symbols) : symbols_(symbols), books_(venues * symbols) {}

    void on_level(VenueId v, const LevelUpdate& u) {
        L2& b = books_[v * symbols_ + u.sym];
        if (u.level.side == Side::Buy) set(b.bids, u.level);
        else set(b.asks, u.level);
    }

    const L2& book(VenueId v, SymbolId s) const { return books_[v * symbols_ + s]; }

    Quote bbo(VenueId v, SymbolId s) const {
        const L2& b = book(v, s);
        Quote q;
        if (!b.bids.empty()) std::tie(q.bid, q.bid_sz) = *b.bids.begin();
        if (!b.asks.empty()) std::tie(q.ask, q.ask_sz) = *b.asks.begin();
        return q;
    }

    // Best bid and best ask across venues; sizes add up over venues at that price.
    Quote nbbo(SymbolId s) const {
        Quote n;
        for (VenueId v = 0; v < venues(); ++v) {
            Quote q = bbo(v, s);
            merge(q.bid, q.bid_sz, n.bid, n.bid_sz, q.bid > n.bid);
            merge(q.ask, q.ask_sz, n.ask, n.ask_sz, q.ask < n.ask);
        }
        return n;
    }

    // Quantity in the best `levels` price levels on one side, summed over venues.
    Qty depth(SymbolId s, Side side, size_t levels) const {
        Qty total = 0;
        for (VenueId v = 0; v < venues(); ++v) {
            const L2& b = book(v, s);
            if (side == Side::Buy) total += sum(b.bids, levels);
            else total += sum(b.asks, levels);
        }
        return total;
    }

    size_t venues() const { return symbols_ ? books_.size() / symbols_ : 0; }

private:
    template <class Map>
    static void set(Map& m, const LevelChange& c) {
        if (c.qty) m[c.price] = c.qty;
        else m.erase(c.price);
    }

    template <class Map>
    static Qty sum(const Map& m, size_t levels) {
        Qty total = 0;
        for (auto it = m.begin(); it != m.end() && levels--; ++it) total += it->second;
        return total;
    }

    static void merge(Price px, Qty sz, Price& best, Qty& best_sz, bool better) {
        if (!sz) return;
        if (!best_sz || better) {
            best = px;
            best_sz = sz;
        } else if (px == best) {
            best_sz += sz;
        }
    }

    size_t symbols_;
    std::vector<L2> books_;  // venue-major
};
