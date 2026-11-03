// Queue-position / fill-probability study over a NASDAQ ITCH 5.0 file: joins
// the best bid and offer every 100 ms, and every new best level as it opens,
// at each latency in the sweep, and writes
// one CSV row per virtual order (see analytics/fill_study.hpp).
// Usage: ./build/fill_study FILE OUT.csv SYMBOL [SYMBOL...]
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "analytics/fill_study.hpp"

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s FILE OUT.csv SYMBOL [SYMBOL...]\n", argv[0]);
        return 2;
    }
    FILE* in = std::fopen(argv[1], "rb");
    FILE* csv = std::fopen(argv[2], "w");
    if (!in || !csv) {
        std::perror("open");
        return 1;
    }
    std::vector<std::string> want(argv + 3, argv + argc);

    // ponytail: FillConfig defaults (100 ms decisions, 10 s horizon, 1 s markout); add flags when a study needs others
    Venue venue;
    ItchReplay replay(venue, want);
    FillStudy study(replay, venue);
    VenueOut out;
    std::vector<FillRow> rows;
    std::map<Nanos, std::pair<size_t, size_t>> by_latency;  // filled, total

    std::fputs("sym,side,latency_ns,new_level,decided_ns,price,queue_ahead,filled,time_to_fill_ns,markout_ticks\n", csv);
    auto write = [&] {
        for (const FillRow& r : rows) {
            std::fprintf(csv, "%s,%c,%llu,%d,%llu,%lld,%llu,%d,%llu,", replay.name(r.sym).c_str(),
                         r.side == Side::Buy ? 'B' : 'S', (unsigned long long)r.latency, int(r.new_level),
                         (unsigned long long)r.decided, (long long)r.px, (unsigned long long)r.queue_ahead,
                         int(r.filled), (unsigned long long)r.time_to_fill);
            if (!std::isnan(r.markout)) std::fprintf(csv, "%g", r.markout);
            std::fputc('\n', csv);
            auto& [f, n] = by_latency[r.latency];
            f += r.filled;
            ++n;
        }
        rows.clear();
    };
    auto t0 = std::chrono::steady_clock::now();
    itch::parse_file(in, [&](const auto& m) {
        study.apply(m, out, rows);
        out.clear();
        if (rows.size() > 4096) write();
    });
    write();
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    std::fprintf(stderr, "%.2f s; %zu virtual orders, %zu post-only rejects, %zu open at end of data\n", secs,
                 study.decided(), study.rejected(), study.open());
    for (auto& [lat, fn] : by_latency)
        std::fprintf(stderr, "  latency %9llu ns: %zu orders, %.1f%% filled\n", (unsigned long long)lat, fn.second,
                     100.0 * double(fn.first) / double(fn.second));
    std::fclose(csv);
}
