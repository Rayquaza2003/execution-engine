#undef NDEBUG  // keep asserts on in Release builds
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <random>
#include <vector>

#include "exchange/orderbook.hpp"

// Differential test: OrderBook against a reference book written the obvious
// way (one vector of orders, a linear scan for everything) straight from the
// rules: price-time priority, trades at the resting price, IOC / FOK / Day /
// GTC, self-trade prevention (cancel newest), modify loses priority, reduce
// keeps it, end of day expires Day orders. Random streams; after every call
// the results, the trades and the book must agree exactly.
class RefBook {
public:
    struct O {
        OrderId id;
        Side side;
        Price px;
        Qty qty;
        Tif tif;
        AccountId account;
        uint64_t seq;
    };

    OrderResult limit(OrderId id, Side side, Price px, Qty qty, std::vector<Trade>& out, Tif tif, AccountId account) {
        if (qty == 0) return {OrderResult::Rejected, Reject::ZeroQty};
        if (find(id)) return {OrderResult::Rejected, Reject::DuplicateId};
        if (tif == Tif::Fok && !can_fill(side, px, qty, account)) return {OrderResult::Cancelled};
        bool stp = false;
        Qty left = match(id, side, &px, qty, account, stp, out);
        if (!left) return {OrderResult::Filled, Reject::None, qty};
        if (stp) return {OrderResult::Cancelled, Reject::SelfTrade, qty - left};
        if (tif == Tif::Ioc) return {OrderResult::Cancelled, Reject::None, qty - left};
        orders_.push_back({id, side, px, left, tif, account, seq_++});
        return {OrderResult::Rested, Reject::None, qty - left};
    }

    OrderResult market(OrderId id, Side side, Qty qty, std::vector<Trade>& out, AccountId account) {
        if (qty == 0) return {OrderResult::Rejected, Reject::ZeroQty};
        bool stp = false;
        Qty left = match(id, side, nullptr, qty, account, stp, out);
        if (!left) return {OrderResult::Filled, Reject::None, qty};
        return {OrderResult::Cancelled, stp ? Reject::SelfTrade : Reject::None, qty - left};
    }

    bool cancel(OrderId id) {
        auto it = std::find_if(orders_.begin(), orders_.end(), [&](const O& o) { return o.id == id; });
        if (it == orders_.end()) return false;
        orders_.erase(it);
        return true;
    }

    OrderResult modify(OrderId id, Price px, Qty qty, std::vector<Trade>& out) {
        const O* o = find(id);
        if (!o) return {OrderResult::Rejected, Reject::UnknownOrder};
        if (qty == 0) return {OrderResult::Rejected, Reject::ZeroQty};
        O was = *o;
        cancel(id);
        return limit(id, was.side, px, qty, out, was.tif, was.account);
    }

    OrderBook::Reduced reduce(OrderId id, Qty qty) {
        auto it = std::find_if(orders_.begin(), orders_.end(), [&](const O& o) { return o.id == id; });
        if (it == orders_.end()) return {Side::Buy, 0, 0};
        OrderBook::Reduced r{it->side, it->px, std::min(qty, it->qty)};
        if ((it->qty -= r.qty) == 0) orders_.erase(it);
        return r;
    }

    // Day orders expire; reported by price, low to high, bids then asks, oldest first.
    void end_of_day(std::vector<OrderId>& expired) {
        for (Side side : {Side::Buy, Side::Sell}) {
            std::vector<O> day;
            for (const O& o : orders_)
                if (o.side == side && o.tif == Tif::Day) day.push_back(o);
            std::sort(day.begin(), day.end(), [](const O& a, const O& b) { return a.px != b.px ? a.px < b.px : a.seq < b.seq; });
            for (const O& o : day) {
                expired.push_back(o.id);
                cancel(o.id);
            }
        }
    }

    std::optional<Price> best(Side side) const {
        std::optional<Price> b;
        for (const O& o : orders_)
            if (o.side == side && (!b || (side == Side::Buy ? o.px > *b : o.px < *b))) b = o.px;
        return b;
    }
    Qty depth(Side side, Price px) const {
        Qty q = 0;
        for (const O& o : orders_)
            if (o.side == side && o.px == px) q += o.qty;
        return q;
    }
    Qty leaves(OrderId id) const {
        const O* o = find(id);
        return o ? o->qty : 0;
    }
    size_t size() const { return orders_.size(); }

private:
    const O* find(OrderId id) const {
        for (const O& o : orders_)
            if (o.id == id) return &o;
        return nullptr;
    }

    // Resting orders on the other side that px reaches, best first, then oldest.
    std::vector<size_t> queue(Side side, const Price* px) const {
        std::vector<size_t> v;
        for (size_t i = 0; i < orders_.size(); ++i) {
            const O& o = orders_[i];
            if (o.side == side) continue;
            if (px && (side == Side::Buy ? o.px > *px : o.px < *px)) continue;
            v.push_back(i);
        }
        std::sort(v.begin(), v.end(), [&](size_t a, size_t b) {
            const O &x = orders_[a], &y = orders_[b];
            if (x.px != y.px) return side == Side::Buy ? x.px < y.px : x.px > y.px;
            return x.seq < y.seq;
        });
        return v;
    }

    bool can_fill(Side side, Price px, Qty qty, AccountId account) const {
        Qty avail = 0;
        for (size_t i : queue(side, &px)) {
            if (account && orders_[i].account == account) return false;
            if ((avail += orders_[i].qty) >= qty) return true;
        }
        return false;
    }

    Qty match(OrderId id, Side side, const Price* px, Qty qty, AccountId account, bool& stp, std::vector<Trade>& out) {
        std::vector<OrderId> done;
        for (size_t i : queue(side, px)) {
            if (!qty) break;
            O& r = orders_[i];
            if (account && r.account == account) {
                stp = true;
                break;
            }
            Qty n = std::min(qty, r.qty);
            out.push_back(side == Side::Buy ? Trade{id, r.id, r.px, n} : Trade{r.id, id, r.px, n});
            qty -= n;
            if ((r.qty -= n) == 0) done.push_back(r.id);
        }
        for (OrderId d : done) cancel(d);
        return qty;
    }

    std::vector<O> orders_;
    uint64_t seq_ = 0;
};

static bool same(const OrderResult& a, const OrderResult& b) {
    return a.status == b.status && a.reason == b.reason && a.filled == b.filled;
}
static bool same(const std::vector<Trade>& a, const std::vector<Trade>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].buy_id != b[i].buy_id || a[i].sell_id != b[i].sell_id || a[i].price != b[i].price || a[i].qty != b[i].qty)
            return false;
    return true;
}

static void run(uint64_t seed, int ops) {
    std::mt19937_64 rng(seed);
    auto uni = [&](int lo, int hi) { return int(std::uniform_int_distribution<int>(lo, hi)(rng)); };
    OrderBook book(1 << 10);
    RefBook ref;
    std::vector<Trade> tb, tr;
    OrderId next = 1;
    size_t trades = 0;
    for (int i = 0; i < ops; ++i) {
        tb.clear();
        tr.clear();
        int k = uni(0, 99);
        Side side = uni(0, 1) ? Side::Buy : Side::Sell;
        Price px = 100 + uni(-6, 6);
        Qty qty = Qty(uni(0, 12));  // 0 now and then: must be rejected the same way
        AccountId acct = AccountId(uni(0, 3));
        OrderId some = OrderId(uni(1, int(next)));  // live, done or never used
        if (k < 45) {
            Tif tif = Tif(uni(0, 3));
            OrderId id = uni(0, 30) ? next++ : some;  // sometimes a duplicate
            assert(same(book.limit(id, side, px, qty, tb, tif, acct), ref.limit(id, side, px, qty, tr, tif, acct)));
        } else if (k < 52) {
            OrderId id = next++;
            assert(same(book.market(id, side, qty, tb, acct), ref.market(id, side, qty, tr, acct)));
        } else if (k < 75) {
            assert(book.cancel(some) == ref.cancel(some));
        } else if (k < 87) {
            assert(same(book.modify(some, px, qty, tb), ref.modify(some, px, qty, tr)));
        } else if (k < 99) {
            OrderBook::Reduced a = book.reduce(some, qty), b = ref.reduce(some, qty);
            assert(a.qty == b.qty && (!a.qty || (a.side == b.side && a.price == b.price)));
        } else {
            std::vector<OrderId> ea, eb;
            book.end_of_day(ea);
            ref.end_of_day(eb);
            assert(ea == eb);
        }
        assert(same(tb, tr));
        trades += tb.size();
        assert(book.order_count() == ref.size());
        assert(book.best_bid() == ref.best(Side::Buy) && book.best_ask() == ref.best(Side::Sell));
        if (i % 64 == 0) {
            for (Price p = 92; p <= 108; ++p)
                assert(book.depth(Side::Buy, p) == ref.depth(Side::Buy, p) && book.depth(Side::Sell, p) == ref.depth(Side::Sell, p));
            for (OrderId id = next > 200 ? next - 200 : 1; id < next; ++id) assert(book.leaves(id) == ref.leaves(id));
        }
    }
    assert(trades > size_t(ops) / 10);  // the streams really do trade
}

int main() {
    for (uint64_t seed = 1; seed <= 20; ++seed) run(seed, 20'000);
    std::puts("all tests passed");
}
