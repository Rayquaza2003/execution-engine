#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "core/id_index.hpp"
#include "core/types.hpp"

struct Trade {
    OrderId buy_id;
    OrderId sell_id;
    Price price;
    Qty qty;
};

// Price-time priority: one flat array of price levels per side, FIFO within a level.
// Orders live in a pool (vector + free list) and link to each other by index,
// so steady-state limit/cancel/match allocate nothing.
class OrderBook {
public:
    // Price band: a resting side may span at most this many ticks. Real venues
    // enforce collars too; it also bounds ladder memory (8 B per tick per side).
    static constexpr Price kMaxSpan = 1 << 20;
    // Accepted price domain; keeps every price difference inside int64.
    static constexpr Price kMaxAbsPrice = Price(1) << 62;

    explicit OrderBook(size_t capacity = 1 << 16) : index_(capacity) {
        pool_.reserve(capacity);
        free_.reserve(capacity);
    }

    // Returns false (rejected, book untouched) on zero qty, a live duplicate id,
    // or a price outside the domain or the band of its own side.
    bool limit(OrderId id, Side side, Price px, Qty qty, std::vector<Trade>& out) {
        Ladder& own = side == Side::Buy ? bids_ : asks_;
        if (qty == 0 || !price_ok(own, px) || index_.find(id) != IdIndex::kMissing) return false;
        qty = match(side == Side::Buy ? asks_ : bids_, id, side, px, qty, out);
        if (qty) rest(own, id, side, px, qty);
        return true;
    }

    // Fills against whatever is on the book; any remainder is dropped (IOC).
    void market(OrderId id, Side side, Qty qty, std::vector<Trade>& out) {
        if (side == Side::Buy) match(asks_, id, side, std::numeric_limits<Price>::max(), qty, out);
        else match(bids_, id, side, std::numeric_limits<Price>::min(), qty, out);
    }

    bool cancel(OrderId id) {
        uint32_t o = index_.take(id);
        if (o == IdIndex::kMissing) return false;
        remove(pool_[o].side == Side::Buy ? bids_ : asks_, o);
        return true;
    }

    // Cancel-replace: order loses time priority and may trade at the new price.
    // Returns false if the id is unknown, new qty is zero, or the new price is
    // out of band. Everything is checked before cancelling, so a rejected
    // replace leaves the original order resting untouched.
    bool modify(OrderId id, Price px, Qty qty, std::vector<Trade>& out) {
        uint32_t o = index_.find(id);
        if (o == IdIndex::kMissing || qty == 0) return false;
        Side side = pool_[o].side;
        if (!price_ok(side == Side::Buy ? bids_ : asks_, px)) return false;
        cancel(id);
        return limit(id, side, px, qty, out);
    }

    std::optional<Price> best_bid() const { return bids_.top(); }
    std::optional<Price> best_ask() const { return asks_.top(); }

    Qty depth(Side side, Price px) const {
        const Level* q = (side == Side::Buy ? bids_ : asks_).find(px);
        Qty total = 0;
        if (q)
            for (uint32_t o = q->head; o != kNil; o = pool_[o].next) total += pool_[o].qty;
        return total;
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

    // levels[i] holds price base + i. Grows (and recenters) on demand, never shrinks.
    // ponytail: a side that drifts kMaxSpan ticks while never emptying starts
    // rejecting; slide the window (copy live levels) if a symbol ever does that.
    struct Ladder {
        bool is_bid;
        Price base = 0;
        std::vector<Level> levels;
        Price best = 0;   // valid only when live > 0
        size_t live = 0;  // non-empty levels

        explicit Ladder(bool bid) : is_bid(bid) {}

        std::optional<Price> top() const {
            return live ? std::optional<Price>(best) : std::nullopt;
        }

        const Level* find(Price p) const {
            if (p < base || p - base >= Price(levels.size())) return nullptr;
            return &levels[p - base];
        }

        bool can_hold(Price p) const {
            if (!live) return true;
            Price lo = std::min(p, best), hi = std::max(p, best);
            // The far edge of the ladder is unknown, so bound by the spread of
            // the full array; cheap and conservative.
            lo = std::min(lo, base);
            hi = std::max(hi, base + Price(levels.size()) - 1);
            return hi - lo < kMaxSpan;
        }

        Level& at(Price p) {
            if (!live) {  // all empty: recenter for free
                if (levels.empty()) levels.resize(4096);
                if (!find(p)) base = p - Price(levels.size()) / 2;
            } else if (!find(p)) {
                Price lo = std::min(p, base), hi = std::max(p + 1, base + Price(levels.size()));
                Price size = std::min<Price>(std::max<Price>(2 * Price(levels.size()), 2 * (hi - lo)), kMaxSpan);
                Price new_base = lo - (size - (hi - lo)) / 2;
                std::vector<Level> grown(size);
                std::copy(levels.begin(), levels.end(), grown.begin() + (base - new_base));
                levels.swap(grown);
                base = new_base;
            }
            return levels[p - base];
        }

        void filled(Price p) {  // level p went empty -> non-empty
            if (!live++ || (is_bid ? p > best : p < best)) best = p;
        }

        void emptied(Price p) {  // level p went non-empty -> empty
            if (--live && p == best) {
                // ponytail: linear scan to next level, O(gap); fine for dense books.
                Price step = is_bid ? -1 : 1;
                do best += step;
                while (levels[best - base].head == kNil);
            }
        }
    };

    static bool price_ok(const Ladder& own, Price px) {
        return px > -kMaxAbsPrice && px < kMaxAbsPrice && own.can_hold(px);
    }

    // Fill against the opposite side while it crosses px. Trades print at the
    // resting order's price. Returns the unfilled quantity.
    Qty match(Ladder& opp, OrderId id, Side side, Price px, Qty qty, std::vector<Trade>& out) {
        while (qty && opp.live) {
            Price p = opp.best;
            if (side == Side::Buy ? p > px : p < px) break;
            Level& q = opp.levels[p - opp.base];
            while (qty && q.head != kNil) {
                uint32_t h = q.head;
                Order& resting = pool_[h];
                Qty n = std::min(qty, resting.qty);
                out.push_back(side == Side::Buy ? Trade{id, resting.id, p, n}
                                                : Trade{resting.id, id, p, n});
                qty -= n;
                resting.qty -= n;
                if (resting.qty == 0) {
                    index_.take(resting.id);
                    unlink(q, h);
                    free_.push_back(h);
                }
            }
            if (q.head == kNil) opp.emptied(p);
        }
        return qty;
    }

    void rest(Ladder& own, OrderId id, Side side, Price px, Qty qty) {
        uint32_t o;
        if (free_.empty()) {
            o = uint32_t(pool_.size());
            pool_.push_back({});  // ponytail: vector growth copies the pool; size capacity up front
        } else {
            o = free_.back();
            free_.pop_back();
        }
        Level& q = own.at(px);
        if (q.head == kNil) own.filled(px);
        pool_[o] = {id, px, qty, q.tail, kNil, side};
        if (q.tail != kNil) pool_[q.tail].next = o;
        else q.head = o;
        q.tail = o;
        index_.insert(id, o);
    }

    void unlink(Level& q, uint32_t o) {
        Order& x = pool_[o];
        if (x.prev != kNil) pool_[x.prev].next = x.next;
        else q.head = x.next;
        if (x.next != kNil) pool_[x.next].prev = x.prev;
        else q.tail = x.prev;
    }

    void remove(Ladder& own, uint32_t o) {
        Price p = pool_[o].price;
        Level& q = own.levels[p - own.base];
        unlink(q, o);
        free_.push_back(o);
        if (q.head == kNil) own.emptied(p);
    }

    std::vector<Order> pool_;
    std::vector<uint32_t> free_;
    Ladder bids_{true}, asks_{false};
    IdIndex index_;
};
