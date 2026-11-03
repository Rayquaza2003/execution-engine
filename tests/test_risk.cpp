#undef NDEBUG  // keep asserts on in Release builds
#include <cassert>
#include <cstdio>

#include "risk/risk.hpp"

constexpr Side B = Side::Buy;
constexpr Side Sl = Side::Sell;
constexpr OrdType L = OrdType::Limit;

static void test_each_limit_and_its_boundary() {
    RiskLimits lim;
    lim.max_order_qty = 100;
    lim.max_order_notional = 100 * 1000.0;
    lim.collar_bps = 500;  // 5%
    Risk r(2, lim);
    assert(r.check(0, B, L, 1000, 10) == Reject::NoReferencePrice);
    assert(r.check(2, B, L, 1000, 10) == Reject::UnknownSymbol);
    r.mark(0, 1000);

    assert(r.check(0, B, L, 1000, 100) == Reject::None);
    assert(r.check(0, B, L, 1000, 101) == Reject::MaxOrderQty);
    assert(r.check(0, B, L, 1050, 10) == Reject::None);  // exactly 5% away
    assert(r.check(0, B, L, 1051, 10) == Reject::FatFinger);
    assert(r.check(0, Sl, L, 949, 10) == Reject::FatFinger);
    assert(r.check(0, B, L, 1001, 100) == Reject::MaxNotional);
    assert(r.check(0, B, OrdType::Market, 0, 100) == Reject::None);  // valued at the mark, not px 0
    r.mark(0, 1001);
    assert(r.check(0, B, OrdType::Market, 0, 100) == Reject::MaxNotional);
}

static void test_position_counts_open_orders_worst_case() {
    RiskLimits lim;
    lim.max_position = 100;
    Risk r(1, lim);
    r.mark(0, 50);
    r.on_new(0, B, 60);
    assert(r.check(0, B, L, 50, 40) == Reject::None);
    assert(r.check(0, B, L, 50, 41) == Reject::MaxPosition);  // 60 open + 41
    assert(r.check(0, Sl, L, 50, 100) == Reject::None);       // sells don't add to long exposure

    r.on_fill(0, B, 50, 60);  // open 60 becomes position 60
    r.on_leaves(0, B, 60, 0, true);
    assert(r.position(0) == 60 && r.open_qty(0, B) == 0 && r.open_orders() == 0);
    assert(r.check(0, B, L, 50, 41) == Reject::MaxPosition);
    assert(r.check(0, Sl, L, 50, 160) == Reject::None);  // 60 long -> 100 short
    assert(r.check(0, Sl, L, 50, 161) == Reject::MaxPosition);

    r.on_new(0, B, 30);
    assert(r.check(0, B, L, 50, 10, 30) == Reject::None);  // replace 30 -> 10: 60 + 10
    assert(r.check(0, B, L, 50, 41, 30) == Reject::MaxPosition);
}

static void test_open_order_count() {
    RiskLimits lim;
    lim.max_open_orders = 2;
    Risk r(1, lim);
    r.mark(0, 10);
    r.on_new(0, B, 1);
    r.on_new(0, Sl, 1);
    assert(r.check(0, B, L, 10, 1) == Reject::MaxOpenOrders);
    assert(r.check(0, B, L, 10, 5, 1) == Reject::None);  // a replace adds no order
    r.on_leaves(0, Sl, 1, 0, true);
    assert(r.open_orders() == 1 && r.check(0, B, L, 10, 1) == Reject::None);
}

static void test_mark_to_market_pnl_and_loss_limit() {
    RiskLimits lim;
    lim.max_loss = 90;
    Risk r(2, lim);
    r.mark(0, 100);
    r.on_fill(0, B, 100, 10);  // long 10 @ 100
    assert(r.pnl() == 0);
    r.mark(0, 105);
    assert(r.pnl() == 50);
    r.on_fill(0, Sl, 106, 4);  // cash -1000 + 424, 6 left at 105: 54
    assert(r.pnl() == 54 && r.position(0) == 6);

    r.on_fill(1, Sl, 20, 5);  // first fill sets the mark
    r.mark(1, 50);            // short 5 loses 150
    assert(r.pnl() == 54 - 150);
    assert(r.check(0, Sl, L, 105, 6) == Reject::LossLimit);
    r.mark(1, 45);
    assert(r.pnl() == -71 && r.check(0, Sl, L, 105, 6) == Reject::None);
}

static void test_kill_switch_blocks_everything() {
    Risk r(1, {});
    r.mark(0, 10);
    assert(r.check(0, B, L, 10, 1) == Reject::None);
    r.kill();
    assert(r.killed() && r.check(0, B, L, 10, 1) == Reject::KillSwitch);
    assert(r.check(5, B, L, 10, 1) == Reject::KillSwitch);  // checked first
}

int main() {
    test_each_limit_and_its_boundary();
    test_position_counts_open_orders_worst_case();
    test_open_order_count();
    test_mark_to_market_pnl_and_loss_limit();
    test_kill_switch_blocks_everything();
    std::puts("all tests passed");
}
