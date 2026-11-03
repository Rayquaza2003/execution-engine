#undef NDEBUG  // keep asserts on in Release builds
#include <cassert>
#include <cstdio>
#include <sstream>

#include "algo/fill_table.hpp"

static void test_lookup_picks_bucket_and_nearest_grid_point() {
    std::istringstream csv(
        "sym,queue_lo,queue_hi,latency_ns,horizon_ns,n,p_fill\n"
        "AAPL,0,100,0,1000000000,10,0.6\n"
        "AAPL,100,500,0,1000000000,10,0.4\n"
        "AAPL,500,,0,1000000000,10,0.2\n"
        "AAPL,0,100,1000000,1000000000,10,0.55\n"
        "AAPL,100,500,1000000,1000000000,10,0.35\n"
        "AAPL,500,,1000000,1000000000,10,0.15\n"
        "AAPL,0,100,0,10000000000,10,0.9\n"
        "MSFT,0,,0,1000000000,10,0.01\n");
    FillTable t;
    assert(t.load(csv, "AAPL"));
    assert(t.p_fill(0, 0, 1'000'000'000) == 0.6);
    assert(t.p_fill(99, 0, 1'000'000'000) == 0.6);
    assert(t.p_fill(100, 0, 1'000'000'000) == 0.4);
    assert(t.p_fill(5000, 0, 1'000'000'000) == 0.2);
    assert(t.p_fill(300, 800'000, 1'000'000'000) == 0.35);  // 0.8 ms -> the 1 ms row
    assert(t.p_fill(50, 0, 7'000'000'000) == 0.9);          // 7 s -> the 10 s row
    std::istringstream again(csv.str());
    FillTable none;
    assert(!none.load(again, "TSLA") && none.p_fill(0, 0, 0) == 0);
}

int main() {
    test_lookup_picks_bucket_and_nearest_grid_point();
    std::puts("all tests passed");
}
