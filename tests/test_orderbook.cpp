#undef NDEBUG  // keep asserts on in Release builds
#include <cassert>
#include <algorithm>
#include <cstdio>

#include "exchange/orderbook.hpp"

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

    b.limit(2, Side::Buy, 101, 5, t);
    assert(!b.modify(1, 101 + OrderBook::kMaxSpan, 5, t));  // outside band: order must survive
    assert(b.depth(Side::Buy, 100) == 5 && b.order_count() == 2);
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

static void test_result_status_and_reasons() {
    OrderBook b;
    std::vector<Trade> t;
    auto r = b.limit(1, Side::Sell, 100, 10, t);
    assert(r.status == OrderResult::Rested && r.filled == 0);

    r = b.limit(2, Side::Buy, 100, 4, t);
    assert(r.status == OrderResult::Filled && r.filled == 4);

    r = b.limit(3, Side::Buy, 100, 10, t);  // takes the remaining 6, rests 4
    assert(r.status == OrderResult::Rested && r.filled == 6);

    r = b.market(4, Side::Sell, 10, t);  // only 4 bid left
    assert(r.status == OrderResult::Cancelled && r.filled == 4);

    assert(b.limit(5, Side::Buy, 100, 0, t).reason == Reject::ZeroQty);
    assert(b.market(6, Side::Buy, 0, t).reason == Reject::ZeroQty);
    b.limit(7, Side::Buy, 100, 1, t);
    assert(b.limit(7, Side::Buy, 100, 1, t).reason == Reject::DuplicateId);
    assert(b.limit(8, Side::Buy, OrderBook::kMaxAbsPrice, 1, t).reason == Reject::PriceBand);
    assert(b.modify(99, 100, 1, t).reason == Reject::UnknownOrder);
    assert(b.modify(7, 100, 0, t).reason == Reject::ZeroQty);
}

static bool same(const std::vector<LevelChange>& a, const std::vector<LevelChange>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].side != b[i].side || a[i].price != b[i].price || a[i].qty != b[i].qty) return false;
    return true;
}

static void test_level_change_feed() {
    OrderBook b;
    std::vector<Trade> t;
    std::vector<LevelChange> lv;
    b.publish_levels_to(&lv);

    b.limit(1, Side::Sell, 101, 5, t);
    b.limit(2, Side::Sell, 102, 5, t);
    b.limit(3, Side::Sell, 102, 3, t);
    assert(same(lv, {{Side::Sell, 101, 5}, {Side::Sell, 102, 5}, {Side::Sell, 102, 8}}));

    lv.clear();
    b.limit(4, Side::Buy, 102, 7, t);  // sweeps 101, takes 2 at 102: one update per level touched
    assert(same(lv, {{Side::Sell, 101, 0}, {Side::Sell, 102, 6}}));

    lv.clear();
    b.limit(5, Side::Buy, 100, 9, t);
    b.cancel(2);
    b.cancel(5);
    assert(same(lv, {{Side::Buy, 100, 9}, {Side::Sell, 102, 3}, {Side::Buy, 100, 0}}));
}

static void test_top_snapshot() {
    OrderBook b;
    std::vector<Trade> t;
    assert(b.top(Side::Buy, 5).empty());
    b.limit(1, Side::Buy, 100, 1, t);
    b.limit(2, Side::Buy, 98, 2, t);
    b.limit(3, Side::Buy, 95, 3, t);
    b.limit(4, Side::Buy, 98, 4, t);
    b.limit(5, Side::Sell, 103, 7, t);
    assert(same(b.top(Side::Buy, 2), {{Side::Buy, 100, 1}, {Side::Buy, 98, 6}}));
    assert(same(b.top(Side::Buy, 10), {{Side::Buy, 100, 1}, {Side::Buy, 98, 6}, {Side::Buy, 95, 3}}));
    assert(same(b.top(Side::Sell, 10), {{Side::Sell, 103, 7}}));
}

static void test_ioc() {
    OrderBook b;
    std::vector<Trade> t;
    b.limit(1, Side::Sell, 100, 3, t);
    auto r = b.limit(2, Side::Buy, 100, 5, t, Tif::Ioc);
    assert(r.status == OrderResult::Cancelled && r.filled == 3 && t.size() == 1);
    assert(!b.best_bid());  // remainder did not rest

    r = b.limit(3, Side::Buy, 100, 5, t, Tif::Ioc);  // nothing to hit
    assert(r.status == OrderResult::Cancelled && r.filled == 0 && b.order_count() == 0);
}

static void test_fok() {
    OrderBook b;
    std::vector<Trade> t;
    b.limit(1, Side::Sell, 100, 3, t);
    b.limit(2, Side::Sell, 101, 3, t);
    b.limit(3, Side::Sell, 103, 10, t);

    auto r = b.limit(4, Side::Buy, 101, 7, t, Tif::Fok);  // only 6 at or below 101
    assert(r.status == OrderResult::Cancelled && r.filled == 0 && t.empty());
    assert(b.depth(Side::Sell, 100) == 3 && b.depth(Side::Sell, 101) == 3);

    r = b.limit(5, Side::Buy, 101, 6, t, Tif::Fok);  // exactly enough across two levels
    assert(r.status == OrderResult::Filled && r.filled == 6 && t.size() == 2);
    assert(*b.best_ask() == 103);

    r = b.limit(6, Side::Sell, 90, 1, t, Tif::Fok);  // empty bid side
    assert(r.status == OrderResult::Cancelled);
}

static void test_end_of_day_expires_day_orders_only() {
    OrderBook b;
    std::vector<Trade> t;
    std::vector<LevelChange> lv;
    b.limit(1, Side::Buy, 100, 5, t);             // Day by default
    b.limit(2, Side::Buy, 100, 5, t, Tif::Gtc);
    b.limit(3, Side::Sell, 105, 5, t, Tif::Day);
    b.limit(4, Side::Buy, 99, 5, t, Tif::Gtc);
    assert(b.modify(4, 98, 6, t));  // replace keeps Gtc
    b.publish_levels_to(&lv);

    std::vector<OrderId> expired;
    b.end_of_day(expired);
    std::sort(expired.begin(), expired.end());
    assert((expired == std::vector<OrderId>{1, 3}));
    assert(b.order_count() == 2 && b.depth(Side::Buy, 100) == 5 && b.depth(Side::Buy, 98) == 6);
    assert(!b.best_ask() && *b.best_bid() == 100);
    assert(lv.size() == 2);  // one level change per expired order
    assert(!b.cancel(1) && b.cancel(2));
}

static void test_self_trade_prevention() {
    OrderBook b;
    std::vector<Trade> t;
    b.limit(1, Side::Sell, 100, 3, t, Tif::Day, 1);
    b.limit(2, Side::Sell, 100, 5, t, Tif::Day, 7);  // behind account 1 in the queue

    auto r = b.limit(3, Side::Buy, 100, 10, t, Tif::Day, 7);
    assert(r.status == OrderResult::Cancelled && r.reason == Reject::SelfTrade && r.filled == 3);
    assert(t.size() == 1 && t[0].sell_id == 1);
    assert(b.depth(Side::Sell, 100) == 5 && !b.best_bid());  // own order untouched, nothing rests

    t.clear();
    r = b.market(4, Side::Buy, 5, t, 7);
    assert(r.reason == Reject::SelfTrade && r.filled == 0 && t.empty());

    r = b.limit(5, Side::Buy, 100, 5, t, Tif::Fok, 7);  // own order in the way: FOK can't complete
    assert(r.status == OrderResult::Cancelled && t.empty());

    r = b.limit(6, Side::Buy, 100, 5, t, Tif::Day, 9);  // other account trades normally
    assert(r.status == OrderResult::Filled && t.size() == 1 && t[0].sell_id == 2);

    b.limit(7, Side::Sell, 100, 1, t);  // account 0 never triggers STP
    assert(b.limit(8, Side::Buy, 100, 1, t).status == OrderResult::Filled);
}

static void test_leaves() {
    OrderBook b;
    std::vector<Trade> t;
    b.limit(1, Side::Sell, 100, 10, t);
    assert(b.leaves(1) == 10);
    b.limit(2, Side::Buy, 100, 4, t);
    assert(b.leaves(1) == 6 && b.leaves(2) == 0);
    b.cancel(1);
    assert(b.leaves(1) == 0 && b.leaves(42) == 0);
}

int main() {
    test_leaves();
    test_self_trade_prevention();
    test_ioc();
    test_fok();
    test_end_of_day_expires_day_orders_only();
    test_level_change_feed();
    test_top_snapshot();
    test_result_status_and_reasons();
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
