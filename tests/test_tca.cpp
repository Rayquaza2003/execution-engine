#undef NDEBUG  // keep asserts on in Release builds
#include <cassert>
#include <cmath>
#include <cstdio>

#include "analytics/tca.hpp"

static bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

// Buy 100: 60 crossed at 101 and 20 passive at 100, both while the mid was
// 100.5; 20 never filled while the price ran to 102.
static void test_buy_decomposition() {
    TcaInput in{Side::Buy, 100, 100.0, 102.0, 101.5, 100.8, {{101, 60, 100.5}, {100, 20, 100.5}}};
    Tca t = tca(in);
    assert(t.filled == 80 && near(t.avg_px, 100.75));
    assert(near(t.spread, 0.25) && near(t.timing, 0.5) && near(t.execution, 0.75));
    assert(near(t.opportunity, 0.4) && near(t.shortfall, 1.0));
    assert(near(t.vs_vwap, -0.05) && near(t.reversion, 0.5));
}

// The mirror-image sell costs the same.
static void test_sell_is_symmetric() {
    TcaInput in{Side::Sell, 100, 100.0, 98.0, 98.5, 99.2, {{99, 60, 99.5}, {100, 20, 99.5}}};
    Tca t = tca(in);
    assert(near(t.spread, 0.25) && near(t.timing, 0.5) && near(t.opportunity, 0.4));
    assert(near(t.shortfall, 1.0) && near(t.vs_vwap, -0.05) && near(t.reversion, 0.5));
}

static void test_nothing_filled() {
    Tca t = tca({Side::Buy, 100, 100.0, 101.0, 101.0, NAN, {}});
    assert(t.filled == 0 && std::isnan(t.avg_px) && std::isnan(t.spread) && near(t.shortfall, 1.0));
}

int main() {
    test_buy_decomposition();
    test_sell_is_symmetric();
    test_nothing_filled();
    std::puts("all tests passed");
}
