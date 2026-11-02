#undef NDEBUG  // keep asserts on in Release builds
#include <cassert>
#include <cstdio>
#include <random>
#include <unordered_map>

#include "orderbook.hpp"

static void test_resting_orders() {
    OrderBook b;
    std::vector<Trade> t;
    assert(!b.best_bid() && !b.best_ask());

    b.limit(1, Side::Buy, 100, 10, t);
    b.limit(2, Side::Buy, 101, 5, t);
    b.limit(3, Side::Sell, 105, 7, t);
    b.limit(4, Side::Sell, 103, 3, t);
    b.limit(5, Side::Buy, 101, 4, t);

    assert(t.empty());
    assert(*b.best_bid() == 101);
    assert(*b.best_ask() == 103);
    assert(b.depth(Side::Buy, 101) == 9);
    assert(b.depth(Side::Sell, 105) == 7);
    assert(b.depth(Side::Sell, 104) == 0);
    assert(b.order_count() == 5);
}

static void test_crossing_limit_partial_fill() {
    OrderBook b;
    std::vector<Trade> t;
    b.limit(1, Side::Sell, 100, 10, t);
    b.limit(2, Side::Buy, 100, 4, t);

    assert(t.size() == 1);
    assert(t[0].buy_id == 2 && t[0].sell_id == 1 && t[0].price == 100 && t[0].qty == 4);
    assert(b.depth(Side::Sell, 100) == 6);
    assert(!b.best_bid());  // aggressor fully filled, nothing rests
}

static void test_time_priority_within_level() {
    OrderBook b;
    std::vector<Trade> t;
    b.limit(1, Side::Buy, 100, 3, t);
    b.limit(2, Side::Buy, 100, 3, t);
    b.limit(3, Side::Sell, 100, 4, t);

    assert(t.size() == 2);
    assert(t[0].buy_id == 1 && t[0].qty == 3);
    assert(t[1].buy_id == 2 && t[1].qty == 1);
    assert(b.depth(Side::Buy, 100) == 2);
}

static void test_sweep_levels_then_rest() {
    OrderBook b;
    std::vector<Trade> t;
    b.limit(1, Side::Sell, 101, 2, t);
    b.limit(2, Side::Sell, 102, 2, t);
    b.limit(3, Side::Sell, 104, 2, t);
    b.limit(4, Side::Buy, 103, 10, t);

    // Fills at resting prices, best first, stops at the limit price.
    assert(t.size() == 2);
    assert(t[0].price == 101 && t[1].price == 102);
    assert(*b.best_bid() == 103 && b.depth(Side::Buy, 103) == 6);
    assert(*b.best_ask() == 104);
    assert(b.order_count() == 2);
}

static void test_sell_aggressor_hits_highest_bid() {
    OrderBook b;
    std::vector<Trade> t;
    b.limit(1, Side::Buy, 99, 5, t);
    b.limit(2, Side::Buy, 100, 5, t);
    b.limit(3, Side::Sell, 98, 7, t);

    assert(t.size() == 2);
    assert(t[0].buy_id == 2 && t[0].price == 100 && t[0].qty == 5);
    assert(t[1].buy_id == 1 && t[1].price == 99 && t[1].qty == 2);
    assert(*b.best_bid() == 99 && b.depth(Side::Buy, 99) == 3);
    assert(!b.best_ask());
}

static void test_market_order_sweeps_and_drops_remainder() {
    OrderBook b;
    std::vector<Trade> t;
    b.limit(1, Side::Sell, 100, 2, t);
    b.limit(2, Side::Sell, 500, 3, t);
    b.market(3, Side::Buy, 10, t);

    assert(t.size() == 2);
    assert(t[0].price == 100 && t[1].price == 500);
    assert(!b.best_ask() && !b.best_bid());  // remainder 5 does not rest
    assert(b.order_count() == 0);

    t.clear();
    b.market(4, Side::Sell, 5, t);  // empty book: no-op
    assert(t.empty());
}

static void test_cancel() {
    OrderBook b;
    std::vector<Trade> t;
    b.limit(1, Side::Buy, 100, 5, t);
    b.limit(2, Side::Buy, 100, 5, t);
    b.limit(3, Side::Sell, 105, 5, t);

    assert(b.cancel(1));
    assert(!b.cancel(1));    // already gone
    assert(!b.cancel(999));  // never existed
    assert(b.depth(Side::Buy, 100) == 5);

    assert(b.cancel(3));
    assert(!b.best_ask());  // empty level removed

    b.limit(4, Side::Sell, 100, 5, t);  // cancelled order must not trade
    assert(t.size() == 1 && t[0].buy_id == 2);
}

static void test_cancel_after_full_fill_fails() {
    OrderBook b;
    std::vector<Trade> t;
    b.limit(1, Side::Sell, 100, 5, t);
    b.market(2, Side::Buy, 5, t);
    assert(!b.cancel(1));
}

static void test_rejects() {
    OrderBook b;
    std::vector<Trade> t;
    assert(!b.limit(1, Side::Buy, 100, 0, t));  // zero qty
    assert(b.order_count() == 0);

    assert(b.limit(1, Side::Buy, 100, 5, t));
    assert(!b.limit(1, Side::Sell, 90, 5, t));  // duplicate live id, must not trade
    assert(t.empty());
    assert(b.depth(Side::Buy, 100) == 5);

    assert(b.cancel(1));
    assert(b.limit(1, Side::Buy, 100, 5, t));  // id reusable once gone
}

static void test_modify_loses_priority() {
    OrderBook b;
    std::vector<Trade> t;
    b.limit(1, Side::Buy, 100, 5, t);
    b.limit(2, Side::Buy, 100, 5, t);
    assert(b.modify(1, 100, 5, t));  // same price, back of queue
    b.limit(3, Side::Sell, 100, 5, t);
    assert(t.size() == 1 && t[0].buy_id == 2);
}

static void test_modify_can_cross() {
    OrderBook b;
    std::vector<Trade> t;
    b.limit(1, Side::Buy, 99, 5, t);
    b.limit(2, Side::Sell, 101, 3, t);
    assert(b.modify(1, 101, 5, t));
    assert(t.size() == 1 && t[0].buy_id == 1 && t[0].price == 101 && t[0].qty == 3);
    assert(*b.best_bid() == 101 && b.depth(Side::Buy, 101) == 2);
    assert(b.depth(Side::Buy, 99) == 0);
}

static void test_modify_rejects() {
    OrderBook b;
    std::vector<Trade> t;
    assert(!b.modify(42, 100, 5, t));  // unknown id
    b.limit(1, Side::Buy, 100, 5, t);
    assert(!b.modify(1, 100, 0, t));   // zero qty rejected, order kept
    assert(b.depth(Side::Buy, 100) == 5);
}

static void test_ladder_growth_and_gaps() {
    OrderBook b;
    std::vector<Trade> t;
    b.limit(1, Side::Sell, 100, 1, t);
    b.limit(2, Side::Sell, 200'000, 1, t);  // forces growth well past initial size
    b.limit(3, Side::Sell, -50'000, 1, t);  // negative prices allowed; grows downward
    assert(*b.best_ask() == -50'000);

    assert(b.cancel(3));
    assert(*b.best_ask() == 100);  // scans across the gap
    b.market(4, Side::Buy, 2, t);
    assert(t.size() == 2 && t[0].price == 100 && t[1].price == 200'000);
    assert(!b.best_ask());

    b.limit(5, Side::Sell, 9'000'000, 1, t);  // empty side recenters anywhere
    assert(*b.best_ask() == 9'000'000);
}

static void test_price_band_rejects() {
    OrderBook b;
    std::vector<Trade> t;
    b.limit(1, Side::Buy, 1000, 5, t);
    assert(!b.limit(2, Side::Buy, 1000 + OrderBook::kMaxSpan, 5, t));  // too far from own side
    assert(!b.limit(3, Side::Sell, OrderBook::kMaxAbsPrice, 5, t));      // outside price domain
    assert(t.empty() && b.order_count() == 1);
    assert(b.limit(4, Side::Sell, 1000 + OrderBook::kMaxSpan, 5, t));    // other side is empty: ok
}

// Random insert/take/find against std::unordered_map; small start forces growth
// and wraparound, narrow key range forces long probe runs and backward shifts.
static void test_id_index_matches_reference() {
    IdIndex idx(1);
    std::unordered_map<OrderId, uint32_t> ref;
    std::mt19937_64 rng(7);
    for (uint32_t step = 0; step < 200'000; ++step) {
        OrderId key = rng() % 512;  // includes key 0
        int op = int(rng() % 3);
        auto it = ref.find(key);
        if (op == 0 && it == ref.end()) {
            idx.insert(key, step);
            ref[key] = step;
        } else if (op == 1) {
            uint32_t want = it == ref.end() ? IdIndex::kMissing : it->second;
            assert(idx.take(key) == want);
            if (it != ref.end()) ref.erase(it);
        } else {
            assert(idx.find(key) == (it == ref.end() ? IdIndex::kMissing : it->second));
        }
        assert(idx.size() == ref.size());
    }
    for (auto [k, v] : ref) assert(idx.find(k) == v);
}

int main() {
    test_id_index_matches_reference();
    test_ladder_growth_and_gaps();
    test_price_band_rejects();
    test_modify_loses_priority();
    test_modify_can_cross();
    test_modify_rejects();
    test_rejects();
    test_cancel();
    test_cancel_after_full_fill_fails();
    test_market_order_sweeps_and_drops_remainder();
    test_resting_orders();
    test_crossing_limit_partial_fill();
    test_time_priority_within_level();
    test_sweep_levels_then_rest();
    test_sell_aggressor_hits_highest_bid();
    std::puts("all tests passed");
}
