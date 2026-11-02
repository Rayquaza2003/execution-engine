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

// Market-data view of one price level after a change; qty 0 means the level is gone.
struct LevelChange {
    Side side;
    Price price;
    Qty qty;
};

struct OrderResult {
    enum Status : uint8_t {
        Rejected,   // book untouched; see reason
        Rested,     // some or none filled, remainder rests on the book
        Filled,     // fully executed
        Cancelled,  // remainder dropped without resting (market, IOC, unfillable FOK,
                    // or self-trade prevention, which sets reason = SelfTrade)
    };
    Status status;
    Reject reason = Reject::None;
    Qty filled = 0;  // quantity executed by this call
    explicit operator bool() const { return status != Rejected; }
};

// Price-time priority: one flat array of price levels per side, FIFO within a level.
// Orders live in a pool (vector + free list) and link to each other by index,
// so steady-state limit/cancel/match allocate nothing.
class OrderBook {
public:
    // Price band: a resting side may span at most this many ticks. Real venues
    // enforce collars too; it also bounds ladder memory (16 B per tick per side).
    static constexpr Price kMaxSpan = 1 << 20;
    // Accepted price domain; keeps every price difference inside int64.
    static constexpr Price kMaxAbsPrice = Price(1) << 62;

    explicit OrderBook(size_t capacity = 1 << 16) : index_(capacity) {
        pool_.reserve(capacity);
        free_.reserve(capacity);
    }

    // Rejected (book untouched) on zero qty, a live duplicate id, or a price
    // outside the domain or the band of its own side.
    //
    // Self-trade prevention (cancel newest): if the incoming order reaches a
    // resting order of the same non-zero account, matching stops there and the
    // incoming remainder is cancelled; the resting order is left alone.
    OrderResult limit(OrderId id, Side side, Price px, Qty qty, std::vector<Trade>& out,
                      Tif tif = Tif::Day, AccountId account = 0) {
        Ladder& own = side == Side::Buy ? bids_ : asks_;
        Ladder& opp = side == Side::Buy ? asks_ : bids_;
        if (qty == 0) return {OrderResult::Rejected, Reject::ZeroQty};
        if (!price_ok(own, px)) return {OrderResult::Rejected, Reject::PriceBand};
        if (index_.find(id) != IdIndex::kMissing) return {OrderResult::Rejected, Reject::DuplicateId};
        if (tif == Tif::Fok && !can_fill(opp, side, px, qty, account)) return {OrderResult::Cancelled};
        bool stp = false;
        Qty left = match(opp, id, side, px, qty, account, stp, out);
        if (!left) return {OrderResult::Filled, Reject::None, qty};
        if (stp) return {OrderResult::Cancelled, Reject::SelfTrade, qty - left};
        if (tif == Tif::Ioc) return {OrderResult::Cancelled, Reject::None, qty - left};
        rest(own, id, side, px, left, tif, account);
        return {OrderResult::Rested, Reject::None, qty - left};
    }

    // Fills against whatever is on the book; any remainder is dropped (IOC).
    OrderResult market(OrderId id, Side side, Qty qty, std::vector<Trade>& out,
                       AccountId account = 0) {
        if (qty == 0) return {OrderResult::Rejected, Reject::ZeroQty};
        bool stp = false;
        Qty left = side == Side::Buy
                       ? match(asks_, id, side, std::numeric_limits<Price>::max(), qty, account, stp, out)
                       : match(bids_, id, side, std::numeric_limits<Price>::min(), qty, account, stp, out);
        if (!left) return {OrderResult::Filled, Reject::None, qty};
        return {OrderResult::Cancelled, stp ? Reject::SelfTrade : Reject::None, qty - left};
    }

    bool cancel(OrderId id) {
        uint32_t o = index_.take(id);
        if (o == IdIndex::kMissing) return false;
        remove(pool_[o].side == Side::Buy ? bids_ : asks_, o);
        return true;
    }

    // Cancel-replace: order loses time priority and may trade at the new price.
    // Rejected if the id is unknown, new qty is zero, or the new price is out
    // of band. Everything is checked before cancelling, so a rejected replace
    // leaves the original order resting untouched.
    OrderResult modify(OrderId id, Price px, Qty qty, std::vector<Trade>& out) {
        uint32_t o = index_.find(id);
        if (o == IdIndex::kMissing) return {OrderResult::Rejected, Reject::UnknownOrder};
        if (qty == 0) return {OrderResult::Rejected, Reject::ZeroQty};
        Side side = pool_[o].side;
        Tif tif = pool_[o].tif;
        AccountId account = pool_[o].account;
        if (!price_ok(side == Side::Buy ? bids_ : asks_, px))
            return {OrderResult::Rejected, Reject::PriceBand};
        cancel(id);
        return limit(id, side, px, qty, out, tif, account);
    }

    // Session close: removes every resting Day order (Gtc survives) and appends
    // its id to `expired`.
    void end_of_day(std::vector<OrderId>& expired) {
        for (Ladder* L : {&bids_, &asks_})
            for (size_t i = 0; i < L->levels.size() && L->live; ++i)
                for (uint32_t o = L->levels[i].head; o != kNil;) {
                    uint32_t next = pool_[o].next;
                    if (pool_[o].tif == Tif::Day) {
                        expired.push_back(pool_[o].id);
                        index_.take(pool_[o].id);
                        remove(*L, o);
                    }
                    o = next;
                }
    }

    std::optional<Price> best_bid() const { return bids_.top(); }
    std::optional<Price> best_ask() const { return asks_.top(); }

    Qty depth(Side side, Price px) const {
        const Level* q = (side == Side::Buy ? bids_ : asks_).find(px);
        return q ? q->qty : 0;
    }

    size_t order_count() const { return index_.size(); }

    // Up to n non-empty levels from the best price outward (L2 snapshot).
    std::vector<LevelChange> top(Side side, size_t n) const {
        const Ladder& L = side == Side::Buy ? bids_ : asks_;
        std::vector<LevelChange> v;
        if (!L.live) return v;
        Price step = L.is_bid ? -1 : 1, end = L.base + Price(L.levels.size());
        for (Price p = L.best; v.size() < n && p >= L.base && p < end; p += step)
            if (Qty q = L.levels[p - L.base].qty) v.push_back({side, p, q});
        return v;
    }

    // When set, every level whose quantity changes is appended once per call
    // (L2 incremental feed). Null by default, so the bench pays one branch.
    void publish_levels_to(std::vector<LevelChange>* out) { levels_out_ = out; }

private:
    static constexpr uint32_t kNil = std::numeric_limits<uint32_t>::max();

    struct Order {
        OrderId id;
        Price price;
        Qty qty;
        uint32_t prev, next;  // pool indices within the price level
        Side side;
        Tif tif;  // Day or Gtc; Ioc/Fok never rest
        AccountId account;
    };
    static_assert(sizeof(Order) == 40, "keep orders small: more per cache line");

    struct Level {
        uint32_t head = kNil, tail = kNil;
        Qty qty = 0;  // total resting quantity, kept current on every change
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

    // FOK pre-check: is there at least qty resting at prices that cross px,
    // ahead of any order that self-trade prevention would stop at?
    bool can_fill(const Ladder& opp, Side side, Price px, Qty qty, AccountId account) const {
        if (!opp.live) return false;
        Price step = opp.is_bid ? -1 : 1, end = opp.base + Price(opp.levels.size());
        Qty avail = 0;
        for (Price p = opp.best; p >= opp.base && p < end; p += step) {
            if (side == Side::Buy ? p > px : p < px) return false;
            const Level& q = opp.levels[p - opp.base];
            if (!account) {
                avail += q.qty;
            } else {
                for (uint32_t o = q.head; o != kNil && avail < qty; o = pool_[o].next) {
                    if (pool_[o].account == account) return false;
                    avail += pool_[o].qty;
                }
            }
            if (avail >= qty) return true;
        }
        return false;
    }

    // Fill against the opposite side while it crosses px. Trades print at the
    // resting order's price. Returns the unfilled quantity.
    // Sets stp when it stopped at a resting order of the same account.
    Qty match(Ladder& opp, OrderId id, Side side, Price px, Qty qty, AccountId account, bool& stp,
              std::vector<Trade>& out) {
        while (qty && opp.live && !stp) {
            Price p = opp.best;
            if (side == Side::Buy ? p > px : p < px) break;
            Level& q = opp.levels[p - opp.base];
            while (qty && q.head != kNil) {
                uint32_t h = q.head;
                Order& resting = pool_[h];
                if (account && resting.account == account) {
                    stp = true;
                    break;
                }
                Qty n = std::min(qty, resting.qty);
                out.push_back(side == Side::Buy ? Trade{id, resting.id, p, n}
                                                : Trade{resting.id, id, p, n});
                qty -= n;
                resting.qty -= n;
                q.qty -= n;
                if (resting.qty == 0) {
                    index_.take(resting.id);
                    unlink(q, h);
                    free_.push_back(h);
                }
            }
            changed(side == Side::Buy ? Side::Sell : Side::Buy, p, q.qty);
            if (q.head == kNil) opp.emptied(p);
        }
        return qty;
    }

    void rest(Ladder& own, OrderId id, Side side, Price px, Qty qty, Tif tif, AccountId account) {
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
        pool_[o] = {id, px, qty, q.tail, kNil, side, tif, account};
        if (q.tail != kNil) pool_[q.tail].next = o;
        else q.head = o;
        q.tail = o;
        q.qty += qty;
        index_.insert(id, o);
        changed(side, px, q.qty);
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
        q.qty -= pool_[o].qty;
        unlink(q, o);
        free_.push_back(o);
        changed(pool_[o].side, p, q.qty);
        if (q.head == kNil) own.emptied(p);
    }

    void changed(Side side, Price p, Qty qty) {
        if (levels_out_) levels_out_->push_back({side, p, qty});
    }

    std::vector<Order> pool_;
    std::vector<uint32_t> free_;
    Ladder bids_{true}, asks_{false};
    IdIndex index_;
    std::vector<LevelChange>* levels_out_ = nullptr;
};
