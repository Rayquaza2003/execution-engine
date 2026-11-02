#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

using OrderId = uint64_t;
using Price = int64_t;  // integer ticks, never floating point
using Qty = uint64_t;

enum class Side : uint8_t { Buy, Sell };

struct Order {
    OrderId id;
    Side side;
    Price price;
    Qty qty;
};

struct Trade {
    OrderId buy_id;
    OrderId sell_id;
    Price price;
    Qty qty;
};

// Price-time priority: levels sorted by price, FIFO within a level.
// ponytail: std::map + std::list, node allocations on every insert; flat price ladder + pool in Phase 3.
class OrderBook {
public:
    // Returns false (rejected, book untouched) on zero qty or a live duplicate id.
    bool limit(OrderId id, Side side, Price px, Qty qty, std::vector<Trade>& out) {
        if (qty == 0 || index_.contains(id)) return false;
        if (side == Side::Buy) {
            qty = match(asks_, id, side, px, qty, out);
            if (qty) rest(bids_, id, side, px, qty);
        } else {
            qty = match(bids_, id, side, px, qty, out);
            if (qty) rest(asks_, id, side, px, qty);
        }
        return true;
    }

    // Fills against whatever is on the book; any remainder is dropped (IOC).
    void market(OrderId id, Side side, Qty qty, std::vector<Trade>& out) {
        if (side == Side::Buy) match(asks_, id, side, std::numeric_limits<Price>::max(), qty, out);
        else match(bids_, id, side, std::numeric_limits<Price>::min(), qty, out);
    }

    bool cancel(OrderId id) {
        auto it = index_.find(id);
        if (it == index_.end()) return false;
        Level::iterator o = it->second;
        if (o->side == Side::Buy) unlink(bids_, o);
        else unlink(asks_, o);
        index_.erase(it);
        return true;
    }

    std::optional<Price> best_bid() const { return best(bids_); }
    std::optional<Price> best_ask() const { return best(asks_); }

    Qty depth(Side side, Price px) const {
        return side == Side::Buy ? level_qty(bids_, px) : level_qty(asks_, px);
    }

    size_t order_count() const { return index_.size(); }

private:
    using Level = std::list<Order>;

    // Fill against the opposite side while it crosses px. Trades print at the
    // resting order's price. Returns the unfilled quantity.
    template <class Levels>
    Qty match(Levels& levels, OrderId id, Side side, Price px, Qty qty, std::vector<Trade>& out) {
        while (qty && !levels.empty()) {
            auto lvl = levels.begin();
            if (side == Side::Buy ? lvl->first > px : lvl->first < px) break;
            Level& q = lvl->second;
            while (qty && !q.empty()) {
                Order& resting = q.front();
                Qty n = std::min(qty, resting.qty);
                out.push_back(side == Side::Buy ? Trade{id, resting.id, resting.price, n}
                                                : Trade{resting.id, id, resting.price, n});
                qty -= n;
                resting.qty -= n;
                if (resting.qty == 0) {
                    index_.erase(resting.id);
                    q.pop_front();
                }
            }
            if (q.empty()) levels.erase(lvl);
        }
        return qty;
    }

    template <class Levels>
    void rest(Levels& levels, OrderId id, Side side, Price px, Qty qty) {
        Level& q = levels[px];
        q.push_back({id, side, px, qty});
        index_[id] = std::prev(q.end());
    }

    template <class Levels>
    static void unlink(Levels& levels, Level::iterator o) {
        auto lvl = levels.find(o->price);
        lvl->second.erase(o);
        if (lvl->second.empty()) levels.erase(lvl);
    }

    template <class Levels>
    static std::optional<Price> best(const Levels& levels) {
        if (levels.empty()) return std::nullopt;
        return levels.begin()->first;
    }

    template <class Levels>
    static Qty level_qty(const Levels& levels, Price px) {
        auto lvl = levels.find(px);
        if (lvl == levels.end()) return 0;
        Qty total = 0;
        for (const Order& o : lvl->second) total += o.qty;
        return total;
    }

    std::map<Price, Level, std::greater<Price>> bids_;  // best (highest) first
    std::map<Price, Level> asks_;                        // best (lowest) first
    std::unordered_map<OrderId, Level::iterator> index_;
};
