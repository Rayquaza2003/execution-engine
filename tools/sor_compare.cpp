// Smart vs naive order routing over three venues carved out of one NASDAQ
// ITCH 5.0 file (see sor/sor_sim.hpp). Writes one row per parent order.
// Usage: ./build/sor_compare FILE FILL_PROB.csv OUT.csv SYMBOL [SYMBOL...]
//
// Parents: for each symbol, one every 5 s from 09:30:30 to 09:37:00,
// alternating buy and sell, sized at 1% of the symbol's volume in the minute
// before. Each of the four modes (smart/naive x take/post) runs the same
// parents in its own replay.
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "sor/sor_sim.hpp"

constexpr Nanos kSec = 1'000'000'000;
constexpr Nanos kFirst = 34'230 * kSec;  // 09:30:30
constexpr Nanos kStride = 5 * kSec;
constexpr int kStarts = 79;               // last at 09:37:00
constexpr double kSize = 0.01;            // of the previous minute's volume
constexpr Nanos kRest = 2 * kSec;         // post modes rest this long

// Fees in ticks (cents) per share; negative = paid to us.
const std::vector<VenueConfig> kVenues = {
    {"A", 0.10, -0.05, 50'000, 0.1},     // cheap, thin, close
    {"B", 0.30, -0.25, 100'000, 0.6},    // deep, expensive to take, big rebate
    {"C", -0.10, 0.15, 1'000'000, 0.3},  // inverted: pays takers; 1 ms away
};

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: %s FILE FILL_PROB.csv OUT.csv SYMBOL [SYMBOL...]\n", argv[0]);
        return 2;
    }
    int fd = open(argv[1], O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0) {
        std::perror(argv[1]);
        return 1;
    }
    size_t bytes = size_t(st.st_size);
    auto* data = static_cast<const uint8_t*>(mmap(nullptr, bytes, PROT_READ, MAP_PRIVATE, fd, 0));
    FILE* csv = std::fopen(argv[3], "w");
    if (data == MAP_FAILED || !csv) {
        std::perror("open");
        return 1;
    }
    std::vector<std::string> want(argv + 4, argv + argc);
    std::vector<FillTable> tables(want.size());
    std::vector<const FillTable*> table_ptrs;
    for (size_t i = 0; i < want.size(); ++i) {
        std::ifstream in(argv[2]);
        if (!tables[i].load(in, want[i])) std::fprintf(stderr, "no fill probabilities for %s\n", want[i].c_str());
        table_ptrs.push_back(&tables[i]);
    }

    // Pass 1: printed volume per symbol per second, for sizing.
    std::vector<std::map<Nanos, Qty>> per_sec(want.size());
    {
        Venue venue;
        ItchReplay replay(venue, want);
        VenueOut out;
        itch::parse(data, bytes, [&](const auto& m) {
            replay.apply(m, out);
            for (const TradePrint& p : out.prints) per_sec[p.sym][replay.time() / kSec] += p.qty;
            out.clear();
        });
    }
    std::vector<SorSim::Parent> plans;
    for (SymbolId s = 0; s < want.size(); ++s)
        for (int k = 0; k < kStarts; ++k) {
            Nanos start = kFirst + Nanos(k) * kStride;
            Qty vol = 0;
            for (auto it = per_sec[s].lower_bound(start / kSec - 60); it != per_sec[s].end() && it->first < start / kSec;
                 ++it)
                vol += it->second;
            Qty qty = std::max<Qty>(100, Qty(kSize * double(vol) / 100) * 100);
            plans.push_back({s, k % 2 ? Side::Sell : Side::Buy, qty, start, SorSim::Mode::SmartTake});
        }

    std::fputs("mode,sym,side,start_ns,qty,filled,rounds,passive_share,venue_post,time_ms,arrival_mid,"
               "cost_bps,fee_bps,share_A,share_B,share_C\n",
               csv);
    const char* names[] = {"SmartTake", "NaiveTake", "SmartPost", "NaivePost"};
    auto t0 = std::chrono::steady_clock::now();
    for (int mode = 0; mode < 4; ++mode) {
        Router router(kVenues);
        Risk risk(want.size(), RiskLimits{});
        Oms oms(1, 1'000'000, &risk);
        SorSim sim(want, router, table_ptrs, oms, risk, kRest);
        for (SorSim::Parent p : plans) {
            p.mode = SorSim::Mode(mode);
            sim.add(p);
        }
        itch::parse(data, bytes, [&](const auto& m) { sim.apply(m); });

        double sum_cost = 0, sum_fill = 0;
        size_t n = 0;
        for (size_t i = 0; i < sim.parents(); ++i) {
            const SorSim::Parent& p = sim.parent(i);
            const SorSim::Result& r = sim.result(i);
            double by_venue[3] = {0, 0, 0};
            for (const SorSim::Fill& f : sim.fills())
                if (f.parent == i) by_venue[f.venue] += double(f.qty);
            double filled = double(r.filled), sg = p.side == Side::Buy ? 1 : -1;
            double fee = filled ? r.fees / filled / r.arrival_mid * 1e4 : NAN;
            double cost = filled ? sg * (r.notional / filled - r.arrival_mid) / r.arrival_mid * 1e4 + fee : NAN;
            std::fprintf(csv, "%s,%s,%c,%llu,%llu,%llu,%u,%.4f,%s,%.3f,%.2f,%.4f,%.4f,%.4f,%.4f,%.4f\n", names[mode],
                         want[p.sym].c_str(), p.side == Side::Buy ? 'B' : 'S', (unsigned long long)p.start,
                         (unsigned long long)p.qty, (unsigned long long)r.filled, r.rounds,
                         filled ? double(r.passive) / filled : 0.0,
                         mode >= 2 ? kVenues[r.venue_post].name : "", double(r.done_at - p.start) / 1e6,
                         r.arrival_mid, cost, fee, filled ? by_venue[0] / filled : 0.0,
                         filled ? by_venue[1] / filled : 0.0, filled ? by_venue[2] / filled : 0.0);
            if (filled) sum_cost += cost, ++n;
            sum_fill += filled / double(p.qty);
        }
        std::fprintf(stderr, "%-9s mean all-in cost %+6.2f bps, fill rate %.1f%%, ratios A %.2f B %.2f C %.2f\n",
                     names[mode], sum_cost / double(n), 100 * sum_fill / double(sim.parents()), router.ratio(0),
                     router.ratio(1), router.ratio(2));
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr, "%zu parents x 4 modes in %.2f s\n", plans.size(), secs);
    std::fclose(csv);
}
