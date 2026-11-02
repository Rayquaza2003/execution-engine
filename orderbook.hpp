#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

using OrderId = uint64_t;
using Price = int64_t;  // integer ticks, never floating point
using Qty = uint64_t;

enum class Side : uint8_t { Buy, Sell };

struct Trade {
    OrderId buy_id;
    OrderId sell_id;
    Price price;
    Qty qty;
};

// Price-time priority: levels sorted by price, FIFO within a level.
// Orders live in a pool (vector + free list) and link to each other by index,
// so resting an order allocates nothing once the pool has warmed up.
// ponytail: std::map levels still allocate per new price; flat ladder next.
class OrderBook {
public:
    explicit OrderBook(size_t capacity = 1 << 16) { pool_.reserve(capacity); }

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
        uint32_t o = it->second;
        if (pool_[o].side == Side::Buy) remove(bids_, o);
        else remove(asks_, o);
        index_.erase(it);
        return true;
    }

    // Cancel-replace: order loses time priority and may trade at the new price.
    // Returns false if the id is unknown or new qty is zero (book untouched).
    bool modify(OrderId id, Price px, Qty qty, std::vector<Trade>& out) {
        auto it = index_.find(id);
        if (it == index_.end() || qty == 0) return false;
        Side side = pool_[it->second].side;
        cancel(id);
        return limit(id, side, px, qty, out);
    }

    std::optional<Price> best_bid() const { return best(bids_); }
    std::optional<Price> best_ask() const { return best(asks_); }

    Qty depth(Side side, Price px) const {
        return side == Side::Buy ? level_qty(bids_, px) : level_qty(asks_, px);
    }

    size_t order_count() const { return index_.size(); }

private:
    static constexpr uint32_t kNil = std::numeric_limits<uint32_t>::max();

    struct Order {
        OrderId id;
        Price price;
        Qty qty;
        uint32_t prev, next;  // pool indices within the price level
        Side side;
    };

    struct Level {
        uint32_t head = kNil, tail = kNil;
    };

    // Fill against the opposite side while it crosses px. Trades print at the
    // resting order's price. Returns the unfilled quantity.
    template <class Levels>
    Qty match(Levels& levels, OrderId id, Side side, Price px, Qty qty, std::vector<Trade>& out) {
        while (qty && !levels.empty()) {
            auto lvl = levels.begin();
            if (side == Side::Buy ? lvl->first > px : lvl->first < px) break;
            Level& q = lvl->second;
            while (qty && q.head != kNil) {
                uint32_t h = q.head;
                Order& resting = pool_[h];
                Qty n = std::min(qty, resting.qty);
                out.push_back(side == Side::Buy ? Trade{id, resting.id, resting.price, n}
                                                : Trade{resting.id, id, resting.price, n});
                qty -= n;
                resting.qty -= n;
                if (resting.qty == 0) {
                    index_.erase(resting.id);
                    unlink(q, h);
                    free_.push_back(h);
                }
            }
            if (q.head == kNil) levels.erase(lvl);
        }
        return qty;
    }

    template <class Levels>
    void rest(Levels& levels, OrderId id, Side side, Price px, Qty qty) {
        uint32_t o;
        if (free_.empty()) {
            o = uint32_t(pool_.size());
            pool_.push_back({});  // ponytail: vector growth copies the pool; size capacity up front
        } else {
            o = free_.back();
            free_.pop_back();
        }
        Level& q = levels[px];
        pool_[o] = {id, px, qty, q.tail, kNil, side};
        if (q.tail != kNil) pool_[q.tail].next = o;
        else q.head = o;
        q.tail = o;
        index_[id] = o;
    }

    void unlink(Level& q, uint32_t o) {
        Order& x = pool_[o];
        if (x.prev != kNil) pool_[x.prev].next = x.next;
        else q.head = x.next;
        if (x.next != kNil) pool_[x.next].prev = x.prev;
        else q.tail = x.prev;
    }

    template <class Levels>
    void remove(Levels& levels, uint32_t o) {
        auto lvl = levels.find(pool_[o].price);
        unlink(lvl->second, o);
        free_.push_back(o);
        if (lvl->second.head == kNil) levels.erase(lvl);
    }

    template <class Levels>
    static std::optional<Price> best(const Levels& levels) {
        if (levels.empty()) return std::nullopt;
        return levels.begin()->first;
    }

    template <class Levels>
    Qty level_qty(const Levels& levels, Price px) const {
        auto lvl = levels.find(px);
        if (lvl == levels.end()) return 0;
        Qty total = 0;
        for (uint32_t o = lvl->second.head; o != kNil; o = pool_[o].next) total += pool_[o].qty;
        return total;
    }

    std::vector<Order> pool_;
    std::vector<uint32_t> free_;
    std::map<Price, Level, std::greater<Price>> bids_;  // best (highest) first
    std::map<Price, Level> asks_;                        // best (lowest) first
    std::unordered_map<OrderId, uint32_t> index_;
};
