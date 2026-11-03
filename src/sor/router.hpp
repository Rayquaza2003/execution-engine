#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

#include "algo/fill_table.hpp"
#include "exchange/orderbook.hpp"

// Smart order routing across venues that differ in fees, latency and liquidity.
//
// Taking: every displayed share on every venue gets an all-in cost, and the
// order fills from the cheapest shares up (water-filling), so it may split
// across venues and walk deeper on a cheap venue before touching an expensive
// one. Cost of one share at price p on venue v, in ticks, for a buy:
//
//   p + take_fee(v) + w_latency * sigma * sqrt(latency(v)) + (1/ratio(v) - 1) * retry
//
// sigma * sqrt(latency) is how far the price typically moves before the order
// lands; ratio(v) is the share of what we send there that actually fills
// (learned from our own IOCs), and a miss costs a retry.
//
// Resting: post where the expected value of one share is highest:
//
//   p_fill * (half_spread - make_fee) - (1 - p_fill) * (half_spread + cheapest take_fee)
//
// p_fill comes from the S6 table, reading the venue's queue as the queue that
// would face the same flow in the whole market (queue / flow share).
//
// The naive router sends everything to whichever venue shows the best price,
// and rests wherever the maker rebate is largest.

struct VenueConfig {
    const char* name;
    double take_fee;  // ticks per share; negative = the venue pays takers
    double make_fee;  // ticks per share; negative = rebate to makers
    Nanos latency;    // one way
    double flow;      // share of the market's order flow it gets
};

// What the router sees of one venue: the side we would trade against, best
// price first, and the best level on our own side.
struct VenueBook {
    std::vector<LevelChange> far;
    LevelChange near{Side::Buy, 0, 0};  // qty 0 = that side is empty
};

struct Slice {
    uint32_t venue;
    Price limit;  // IOC limit: the worst price taken there
    Qty qty;
};

class Router {
public:
    double w_latency = 1;
    double retry = 1;  // ticks lost when an IOC comes back unfilled

    explicit Router(std::vector<VenueConfig> venues) : v_(std::move(venues)), ratio_(v_.size(), 1.0) {}

    const std::vector<VenueConfig>& venues() const { return v_; }
    double ratio(uint32_t v) const { return ratio_[v]; }

    // sigma: typical mid move in ticks over 1 ns, scaled by sqrt(ns).
    std::vector<Slice> take(Side side, Qty qty, const std::vector<VenueBook>& books, double sigma) const {
        struct Cand {
            double cost;
            uint32_t v;
            Price px;
            Qty qty;
        };
        std::vector<Cand> c;
        double sg = side == Side::Buy ? 1 : -1;
        for (uint32_t v = 0; v < books.size(); ++v) {
            double extra = v_[v].take_fee + w_latency * sigma * std::sqrt(double(v_[v].latency)) +
                           (1 / std::max(ratio_[v], 0.05) - 1) * retry;
            for (const LevelChange& l : books[v].far) c.push_back({sg * double(l.price) + extra, v, l.price, l.qty});
        }
        std::stable_sort(c.begin(), c.end(), [](const Cand& a, const Cand& b) { return a.cost < b.cost; });
        std::vector<Slice> out;
        for (const Cand& x : c) {
            if (!qty) break;
            Qty n = std::min(qty, x.qty);
            qty -= n;
            auto it = std::find_if(out.begin(), out.end(), [&](const Slice& s) { return s.venue == x.v; });
            if (it == out.end()) out.push_back({x.v, x.px, n});
            else it->limit = x.px, it->qty += n;  // levels arrive best first per venue
        }
        return out;
    }

    // Everything to the venue showing the best price, up to its size there.
    static std::vector<Slice> take_naive(Side side, Qty qty, const std::vector<VenueBook>& books) {
        int best = -1;
        for (uint32_t v = 0; v < books.size(); ++v) {
            if (books[v].far.empty()) continue;
            Price p = books[v].far[0].price;
            if (best < 0 || (side == Side::Buy ? p < books[best].far[0].price : p > books[best].far[0].price))
                best = int(v);
        }
        if (best < 0) return {};
        const LevelChange& top = books[best].far[0];
        return {{uint32_t(best), top.price, std::min(qty, top.qty)}};
    }

    // Venue to rest at price px (the best price on our side across venues).
    uint32_t post(Price px, const std::vector<VenueBook>& books, const FillTable& table, Nanos horizon,
                  double half_spread) const {
        double cheapest_take = v_[0].take_fee;
        for (const VenueConfig& c : v_) cheapest_take = std::min(cheapest_take, c.take_fee);
        uint32_t best = 0;
        double best_ev = -INFINITY;
        for (uint32_t v = 0; v < books.size(); ++v) {
            const LevelChange& n = books[v].near;
            Qty queue = n.qty && n.price == px ? n.qty : 0;  // joining a new level: nobody ahead
            double p = table.p_fill(Qty(double(queue) / v_[v].flow), v_[v].latency, horizon);
            double ev = p * (half_spread - v_[v].make_fee) - (1 - p) * (half_spread + cheapest_take);
            if (ev > best_ev) best = v, best_ev = ev;
        }
        return best;
    }

    // Largest maker rebate wins (ties: first venue).
    uint32_t post_naive() const {
        uint32_t best = 0;
        for (uint32_t v = 1; v < v_.size(); ++v)
            if (v_[v].make_fee < v_[best].make_fee) best = v;
        return best;
    }

    // An IOC to venue v came back: `filled` of `sent`.
    void learn(uint32_t v, Qty sent, Qty filled) {
        if (sent) ratio_[v] = 0.9 * ratio_[v] + 0.1 * double(filled) / double(sent);
    }

private:
    std::vector<VenueConfig> v_;
    std::vector<double> ratio_;  // EWMA of filled / sent per venue, for IOCs
};
