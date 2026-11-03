#undef NDEBUG  // keep asserts on in Release builds
#include <cassert>
#include <cmath>
#include <cstdio>

#include "sor/sor_sim.hpp"

constexpr uint32_t D = 10'000;  // $1 in ITCH units; one cent = 100 = one book tick

// A: cheap and thin; B: deep, expensive to take, best rebate; C: pays takers, far away.
static std::vector<VenueConfig> venues() {
    return {{"A", 0.1, -0.05, 50'000, 0.1}, {"B", 0.3, -0.25, 100'000, 0.6}, {"C", -0.1, 0.15, 1'000'000, 0.3}};
}

struct Rig {
    Router router{venues()};
    FillTable table;
    Risk risk{1, RiskLimits{}};
    Oms oms{1, 1'000'000, &risk};
    SorSim sim{{"AAPL"}, router, {&table}, oms, risk, 1'000'000};
    uint64_t next = 1;
    // A fresh order ref that lands on venue v.
    uint64_t ref_on(uint32_t v) {
        while (sim.venue_of(next) != v) ++next;
        return next++;
    }
    void feed(const itch::Writer& w) {
        itch::parse(w.buf.data(), w.buf.size(), [&](const auto& m) { sim.apply(m); });
    }
};

// Every real order lands on exactly one venue: the venues' books add up to
// the single-venue replay of the same feed, level by level.
static void test_venues_split_the_market() {
    itch::Writer w;
    itch::synthetic(w, 200'000, 2, 3);
    Router router(venues());
    FillTable t;
    Risk risk(2, RiskLimits{});
    Oms oms(1, 1'000'000, &risk);
    SorSim sim({"SYM0", "SYM1"}, router, {&t, &t}, oms, risk);
    Venue one;
    ItchReplay replay(one, {"SYM0", "SYM1"});
    VenueOut out;
    itch::parse(w.buf.data(), w.buf.size(), [&](const auto& m) {
        sim.apply(m);
        replay.apply(m, out);
        out.clear();
    });
    for (SymbolId s = 0; s < 2; ++s) {
        size_t orders = 0;
        for (uint32_t v = 0; v < 3; ++v) orders += sim.venue(v).book(s).order_count();
        assert(orders == one.book(s).order_count());
        double share_a = double(sim.venue(0).book(s).order_count()) / double(orders);
        assert(share_a > 0.05 && share_a < 0.15);
        for (Side side : {Side::Buy, Side::Sell})
            for (const LevelChange& l : one.book(s).top(side, 30)) {
                Qty sum = 0;
                for (uint32_t v = 0; v < 3; ++v) sum += sim.venue(v).book(s).depth(side, l.price);
                assert(sum == l.qty);
            }
    }
}

// Same book, buy 150. Smart takes C first (it pays takers), then A; naive
// takes whichever venue it sees first at the best price (A), then comes back.
static void test_smart_and_naive_take() {
    for (SorSim::Mode mode : {SorSim::Mode::SmartTake, SorSim::Mode::NaiveTake}) {
        Rig r;
        itch::Writer w;
        w.stock_directory(1, 0, "AAPL");
        w.add(1, 10, r.ref_on(0), Side::Sell, 100, "AAPL", 10 * D);
        w.add(1, 11, r.ref_on(2), Side::Sell, 100, "AAPL", 10 * D);
        w.add(1, 12, r.ref_on(1), Side::Sell, 300, "AAPL", 10 * D + 100);
        w.add(1, 13, r.ref_on(1), Side::Buy, 300, "AAPL", 10 * D - 200);
        w.add(1, 10'000'000, r.ref_on(1), Side::Buy, 100, "AAPL", 9 * D);  // clock
        r.sim.add({0, Side::Buy, 150, 1000, mode});
        r.feed(w);
        const SorSim::Result& res = r.sim.result(0);
        assert(res.filled == 150 && res.done_at > 0 && res.arrival_mid == 999.0 && res.passive == 0);
        if (mode == SorSim::Mode::SmartTake) {
            assert(res.rounds == 1 && std::fabs(res.cost - (100 * 999.9 + 50 * 1000.1)) < 1e-6);
        } else {
            assert(res.rounds == 2 && std::fabs(res.cost - (100 * 1000.1 + 50 * 999.9)) < 1e-6);
        }
        assert(r.oms.invalid_transitions() == 0 && r.risk.position(0) == 150);
    }
}

// Naive rests where the rebate is biggest (B). A trade behind us there fills
// part passively; after rest_for the rest is cancelled and taken.
static void test_post_then_take_the_rest() {
    Rig r;
    itch::Writer w;
    w.stock_directory(1, 0, "AAPL");
    uint64_t b1 = r.ref_on(1), b2 = r.ref_on(1);
    w.add(1, 10, b1, Side::Buy, 100, "AAPL", 10 * D);
    w.add(1, 11, r.ref_on(0), Side::Sell, 500, "AAPL", 10 * D + 100);
    w.add(1, 500'000, b2, Side::Buy, 100, "AAPL", 10 * D);  // behind us on B
    w.execute(1, 600'000, b2, 60, 1);                        // we fill 60 first
    w.add(1, 10'000'000, r.ref_on(1), Side::Buy, 100, "AAPL", 9 * D);
    r.sim.add({0, Side::Buy, 200, 1000, SorSim::Mode::NaivePost});
    r.feed(w);
    const SorSim::Result& res = r.sim.result(0);
    assert(res.venue_post == 1 && res.filled == 200 && res.passive == 60 && res.rounds == 1);
    assert(std::fabs(res.cost - (60 * (1000 - 0.25) + 140 * (1001 + 0.1))) < 1e-6);
}

// All four modes on a synthetic feed: every parent ends, nothing overfills,
// fills agree with results, and a second run is identical.
static void test_modes_on_synthetic_feed() {
    itch::Writer w;
    itch::synthetic(w, 200'000, 1, 4);
    double first = 0;
    for (int pass = 0; pass < 2; ++pass) {
        Router router(venues());
        FillTable t;
        Risk risk(1, RiskLimits{});
        Oms oms(1, 1'000'000, &risk);
        SorSim sim({"SYM0"}, router, {&t}, oms, risk, 5'000'000);
        Nanos t0 = 34'200'000'000'000;
        for (int k = 0; k < 40; ++k)
            sim.add({0, k % 2 ? Side::Sell : Side::Buy, 800, t0 + Nanos(k + 1) * 4'000'000, SorSim::Mode(k % 4)});
        itch::parse(w.buf.data(), w.buf.size(), [&](const auto& m) { sim.apply(m); });
        double total = 0;
        for (size_t i = 0; i < sim.parents(); ++i) {
            const SorSim::Result& res = sim.result(i);
            Qty sum = 0;
            for (const SorSim::Fill& f : sim.fills())
                if (f.parent == i) sum += f.qty;
            assert(res.done_at && res.filled <= 800 && sum == res.filled && res.filled > 0);
            total += res.cost;
        }
        assert(oms.invalid_transitions() == 0);
        if (pass == 0) first = total;
        else assert(total == first);
    }
}

int main() {
    test_venues_split_the_market();
    test_smart_and_naive_take();
    test_post_then_take_the_rest();
    test_modes_on_synthetic_feed();
    std::puts("all tests passed");
}
