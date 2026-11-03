// Pre-trade risk check latency on its own.
// Usage: ./build/bench_risk [num_checks]   (default 10M)
//
// One check is a few ns, far below the 41.67 ns macOS clock tick, so the
// bench times whole passes and divides. Inputs are pregenerated so the timed
// loop is the check and nothing else.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "risk/risk.hpp"

using Clock = std::chrono::steady_clock;

struct In {
    SymbolId sym;
    Side side;
    OrdType type;
    Price px;
    Qty qty;
};

int main(int argc, char** argv) {
    size_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 10'000'000;
    constexpr size_t kSyms = 64;
    constexpr size_t kInputs = 1 << 16;  // reused round robin; fits in L2

    RiskLimits lim;
    lim.max_order_qty = 900;
    lim.max_order_notional = 900 * 100'000.0;
    lim.max_position = 5'000;
    lim.max_open_orders = 10'000;
    lim.collar_bps = 50;
    lim.max_loss = 1e12;
    Risk risk(kSyms, lim);

    std::mt19937_64 rng(42);
    auto uni = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    for (SymbolId s = 0; s < kSyms; ++s) {
        risk.mark(s, 100'000);
        risk.on_new(s, Side::Buy, Qty(uni(0, 3000)));  // some exposure already open
        risk.on_fill(s, Side::Sell, 100'000, Qty(uni(0, 2000)));
    }
    std::vector<In> in(kInputs);
    for (In& x : in)
        x = {SymbolId(uni(0, kSyms - 1)), uni(0, 1) ? Side::Buy : Side::Sell,
             uni(0, 19) ? OrdType::Limit : OrdType::Market, 100'000 + uni(-600, 600), Qty(uni(1, 1000))};

    std::vector<double> ns_per;
    size_t rejects = 0;
    for (int run = 0; run < 7; ++run) {
        rejects = 0;
        auto t0 = Clock::now();
        for (size_t i = 0; i < n; ++i) {
            const In& x = in[i & (kInputs - 1)];
            rejects += risk.check(x.sym, x.side, x.type, x.px, x.qty) != Reject::None;
        }
        double secs = std::chrono::duration<double>(Clock::now() - t0).count();
        ns_per.push_back(secs * 1e9 / double(n));
    }
    std::sort(ns_per.begin(), ns_per.end());
    std::printf("checks: %zu x 7 runs, %.1f%% rejected\n", n, 100.0 * double(rejects) / double(n));
    std::printf("ns/check: median %.2f  min %.2f  max %.2f\n", ns_per[3], ns_per[0], ns_per[6]);
}
