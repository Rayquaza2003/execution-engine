// Market data throughput on a synthetic ITCH feed, one stage added at a time.
// Usage: ./build/bench_md [num_messages]   (default 5M, 8 symbols)
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "analytics/microstructure.hpp"
#include "md/replay.hpp"

using Clock = std::chrono::steady_clock;

int main(int argc, char** argv) {
    size_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 5'000'000;
    constexpr size_t kSyms = 8;
    itch::Writer w;
    itch::synthetic(w, n, kSyms, 42);
    std::vector<std::string> names;
    for (size_t s = 0; s < kSyms; ++s) names.push_back("SYM" + std::to_string(s));

    // Returns messages per second for one pass of the given stage.
    auto pass = [&](int stage) {
        Venue venue;
        ItchReplay replay(venue, names);
        MarketData md(1, kSyms);
        Microstructure ms(md, kSyms, 1'000'000'000);
        VenueOut out;
        std::vector<Bar> bars;
        size_t msgs = 0, sink = 0;
        auto t0 = Clock::now();
        itch::parse(w.buf.data(), w.buf.size(), [&](const auto& m) {
            ++msgs;
            if (stage == 0) {
                sink += m.ts;
                return;
            }
            replay.apply(m, out);
            if (stage == 2 && !(out.levels.empty() && out.prints.empty())) {
                SymbolId s = out.levels.empty() ? out.prints[0].sym : out.levels[0].sym;
                ms.advance(s, replay.time(), bars);
                for (const LevelUpdate& u : out.levels) md.on_level(0, u);
                if (!out.levels.empty()) ms.on_book(s);
                for (const TradePrint& p : out.prints) ms.on_trade(p);
            }
            out.clear();
            bars.clear();
        });
        double secs = std::chrono::duration<double>(Clock::now() - t0).count();
        if (sink == 1) std::puts("");  // keep the parse-only pass from being optimized away
        return double(msgs) / secs;
    };

    const char* names_of[] = {"parse only", "+ venue book replay", "+ L2 market data + bars"};
    std::printf("%zu messages, %zu symbols, median of 5 passes\n", n, kSyms);
    for (int stage = 0; stage < 3; ++stage) {
        std::vector<double> r;
        for (int i = 0; i < 5; ++i) r.push_back(pass(stage));
        std::sort(r.begin(), r.end());
        std::printf("  %-26s %7.1f M msgs/s  (%.0f ns/msg)\n", names_of[stage], r[2] / 1e6, 1e9 / r[2]);
    }
}
