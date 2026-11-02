#pragma once
#include <cstdint>
#include <functional>
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
    bool limit(OrderId id, Side side, Price px, Qty qty, std::vector<Trade>& out) {
        (void)out;
        if (side == Side::Buy) rest(bids_, id, side, px, qty);
        else rest(asks_, id, side, px, qty);
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

    template <class Levels>
    void rest(Levels& levels, OrderId id, Side side, Price px, Qty qty) {
        Level& q = levels[px];
        q.push_back({id, side, px, qty});
        index_[id] = std::prev(q.end());
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
