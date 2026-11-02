#undef NDEBUG  // keep asserts on in Release builds
#include <cassert>
#include <cstdio>

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

int main() {
    test_resting_orders();
    std::puts("all tests passed");
}
