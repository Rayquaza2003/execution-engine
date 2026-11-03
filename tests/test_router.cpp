#undef NDEBUG  // keep asserts on in Release builds
#include <cassert>
#include <cstdio>
#include <sstream>

#include "sor/router.hpp"

// A: cheap and thin; B: deep and expensive; C: pays takers but is far away.
static std::vector<VenueConfig> venues() {
    return {{"A", 0.1, -0.05, 50'000, 0.1}, {"B", 0.3, -0.25, 100'000, 0.6}, {"C", -0.1, 0.15, 1'000'000, 0.3}};
}

static VenueBook asks(std::vector<std::pair<Price, Qty>> lv) {
    VenueBook b;
    for (auto [p, q] : lv) b.far.push_back({Side::Sell, p, q});
    return b;
}

static void test_take_water_fills_by_all_in_cost() {
    Router r(venues());
    r.w_latency = 0;
    // Per-share cost at 100: A 100.1, B 100.3, C 99.9. At 101: A 101.1, C 100.9.
    std::vector<VenueBook> books{asks({{100, 100}, {101, 500}}), asks({{100, 300}}), asks({{100, 50}, {101, 50}})};
    std::vector<Slice> s = r.take(Side::Buy, 450, books, 0);
    // Order taken: C@100 50, A@100 100, B@100 300 (100.3 < C@101 100.9).
    assert(s.size() == 3);
    assert(s[0].venue == 2 && s[0].limit == 100 && s[0].qty == 50);
    assert(s[1].venue == 0 && s[1].limit == 100 && s[1].qty == 100);
    assert(s[2].venue == 1 && s[2].limit == 100 && s[2].qty == 300);
    s = r.take(Side::Buy, 470, books, 0);  // 20 more: C@101 (100.9) before A@101 (101.1)
    assert(s.size() == 3 && s[0].venue == 2 && s[0].limit == 101 && s[0].qty == 70);

    // Latency: with enough volatility, far-away C loses its fee advantage.
    r.w_latency = 1;
    s = r.take(Side::Buy, 50, books, 0.001);  // C: +1 tick at 1 ms, A: +0.22
    assert(s.size() == 1 && s[0].venue == 0);

    // A venue whose IOCs keep missing gets pushed down the list.
    Router miss(venues());
    miss.w_latency = 0;
    for (int i = 0; i < 20; ++i) miss.learn(2, 100, 10);
    s = miss.take(Side::Buy, 50, books, 0);
    assert(miss.ratio(2) < 0.3 && s[0].venue == 0);
}

static void test_naive_goes_to_best_price_only() {
    std::vector<VenueBook> books{asks({{101, 100}}), asks({{100, 30}, {101, 900}}), asks({})};
    std::vector<Slice> s = Router::take_naive(Side::Buy, 500, books);
    assert(s.size() == 1 && s[0].venue == 1 && s[0].limit == 100 && s[0].qty == 30);
    assert(Router::take_naive(Side::Buy, 5, {asks({})}).empty());
}

static void test_post_weighs_rebate_against_fill_chance() {
    std::istringstream csv(
        "sym,queue_lo,queue_hi,latency_ns,horizon_ns,n,p_fill\n"
        "X,0,1000,0,1000000000,1,0.8\n"
        "X,1000,,0,1000000000,1,0.2\n");
    FillTable t;
    t.load(csv, "X");
    Router r(venues());
    VenueBook a, b, c;
    a.near = {Side::Buy, 100, 50};    // 50 / 0.1 = 500 market-equivalent: p 0.8
    b.near = {Side::Buy, 100, 1200};  // 1200 / 0.6 = 2000: p 0.2
    c.near = {Side::Buy, 100, 600};   // 2000: p 0.2
    // half spread 0.5, cheapest take -0.1: A 0.8*0.55 - 0.2*0.4 = 0.36; B 0.2*0.75 - 0.8*0.4 = -0.17
    assert(r.post(100, {a, b, c}, t, 1'000'000'000, 0.5) == 0);
    b.near.qty = 60;  // B's queue is now short too: its bigger rebate wins
    assert(r.post(100, {a, b, c}, t, 1'000'000'000, 0.5) == 1);
    assert(r.post_naive() == 1);
}

int main() {
    test_take_water_fills_by_all_in_cost();
    test_naive_goes_to_best_price_only();
    test_post_weighs_rebate_against_fill_chance();
    std::puts("all tests passed");
}
