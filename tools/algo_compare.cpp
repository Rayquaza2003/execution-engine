// Runs every execution algo on the same parent orders over a NASDAQ ITCH 5.0
// file and writes one TCA row per (algo, parent order) as CSV.
// Usage: ./build/algo_compare [--from HH:MM:SS] FILE FILL_PROB.csv OUT.csv SYMBOL [SYMBOL...]
//   --from: first parent start (e.g. after the window the fill table was fitted on)
//
// Parents: for each symbol, a 1-minute order starting every 10 s from 09:30:30,
// alternating buy and sell. Each is sized at 5% of the symbol's volume in the
// minute before it, and VWAP's volume curve comes from that minute too, so
// nothing looks ahead. Orders that overlap in time would trade against each
// other's liquidity, so each replay of the file runs only one start in six
// (no overlaps); every algo sees exactly the same parents.
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "algo/exec_sim.hpp"

constexpr Nanos kSec = 1'000'000'000;
constexpr Nanos kOpen = 34'230 * kSec;  // 09:30:30, after the opening cross
constexpr Nanos kLast = 34'560 * kSec;  // 09:36:00: the last parent ends 09:37:00, inside the data
constexpr Nanos kWindow = 60 * kSec;
constexpr Nanos kStride = 10 * kSec;
constexpr int kGroups = kWindow / kStride;  // replays per algo, so no two parents overlap
constexpr size_t kBins = 12;            // VWAP curve resolution
constexpr Nanos kLatency = 100'000;     // one way, 100 us
constexpr double kSize = 0.05;          // of the previous window's volume


// "HH:MM:SS" -> ns since midnight (ITCH time).
static Nanos clock_time(const char* s) {
    int h = 0, m = 0, sec = 0;
    std::sscanf(s, "%d:%d:%d", &h, &m, &sec);
    return Nanos((h * 3600 + m * 60 + sec)) * 1'000'000'000;
}

int main(int argc, char** argv) {
    Nanos kFirst = kOpen;  // --from: test window only, after the fill table's training window
    if (argc > 2 && !std::strcmp(argv[1], "--from")) {
        kFirst = std::max(kOpen, clock_time(argv[2]));
        argv += 2;
        argc -= 2;
    }
    const int kStarts = int((kLast - kFirst) / kStride) + 1;
    if (argc < 5) {
        std::fprintf(stderr, "usage: %s [--from HH:MM:SS] FILE FILL_PROB.csv OUT.csv SYMBOL [SYMBOL...]\n", argv[0]);
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
    for (size_t i = 0; i < want.size(); ++i) {
        std::ifstream in(argv[2]);
        if (!tables[i].load(in, want[i])) std::fprintf(stderr, "no fill probabilities for %s\n", want[i].c_str());
    }

    // Pass 1: printed volume per symbol per second, for sizing and VWAP curves.
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
    auto volume = [&](SymbolId s, Nanos from, Nanos to) {
        Qty v = 0;
        for (auto it = per_sec[s].lower_bound(from / kSec); it != per_sec[s].end() && it->first < to / kSec; ++it)
            v += it->second;
        return v;
    };

    struct Plan {
        ParentOrder p;
        std::vector<double> curve;
    };
    std::vector<Plan> plans;
    for (SymbolId s = 0; s < want.size(); ++s)
        for (int k = 0; k < kStarts; ++k) {
            Nanos start = kFirst + Nanos(k) * kStride, prev = start - kWindow;
            Qty qty = std::max<Qty>(100, Qty(kSize * double(volume(s, prev, start)) / 100) * 100);
            std::vector<double> curve(kBins);
            Qty total = 0;
            for (size_t b = 0; b < kBins; ++b) {
                total += volume(s, prev + b * kWindow / kBins, prev + (b + 1) * kWindow / kBins);
                curve[b] = double(total);
            }
            for (double& c : curve) c = total ? c / double(total) : 0;
            if (!total) curve.clear();  // no history: VWAP falls back to TWAP
            plans.push_back({{s, k % 2 ? Side::Sell : Side::Buy, qty, start, start + kWindow}, curve});
        }

    std::fputs("algo,sym,side,start_ns,qty,filled,children,passive_share,duration_s,arrival_mid,"
               "shortfall_bps,execution_bps,spread_bps,timing_bps,opportunity_bps,vs_vwap_bps,reversion_bps\n",
               csv);
    // One replay of the whole file for one algo and one group of non-overlapping parents.
    double sum_is = 0, sum_fill = 0;
    size_t dropped = 0;
    auto replay_group = [&](Style style, int g) {
        Venue venue;
        ItchReplay replay(venue, want);
        Risk risk(want.size(), RiskLimits{});
        Oms oms(1, 1'000'000, &risk);
        ExecSim sim(replay, venue, oms, risk, kLatency);
        std::vector<std::unique_ptr<Algo>> algos;
        for (const Plan& plan : plans) {
            if ((plan.p.start - kFirst) / kStride % kGroups != Nanos(g)) continue;
            AlgoParams a;
            a.style = style;
            a.curve = plan.curve;
            a.display = std::max<Qty>(100, plan.p.qty / 2000 * 100);  // ~5% shown
            a.fills = &tables[plan.p.sym];
            a.latency = kLatency;
            algos.push_back(std::make_unique<Algo>(ExecSim::base(algos.size()), plan.p, a));
            sim.add(*algos.back());
        }
        itch::parse(data, bytes, [&](const auto& m) { sim.apply(m); });
        dropped += replay.dropped();

        for (size_t i = 0; i < algos.size(); ++i) {
            const ParentOrder& p = algos[i]->parent();
            const ExecSim::Run& r = sim.run(i);
            Tca t = tca(sim.tca_input(i));
            Qty passive = 0;
            for (const ExecSim::Fill& f : sim.fills())
                if (f.algo == i && f.passive) passive += f.qty;
            double bps = 1e4 / r.arrival_mid;
            std::fprintf(csv, "%s,%s,%c,%llu,%llu,%llu,%zu,%.4f,%.3f,%.2f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
                         style_name(style), want[p.sym].c_str(), p.side == Side::Buy ? 'B' : 'S',
                         (unsigned long long)p.start, (unsigned long long)p.qty, (unsigned long long)t.filled,
                         algos[i]->children(), t.filled ? double(passive) / double(t.filled) : 0.0,
                         double(r.finished - p.start) / 1e9, r.arrival_mid, t.shortfall * bps, t.execution * bps,
                         t.spread * bps, t.timing * bps, t.opportunity * bps, t.vs_vwap * bps, t.reversion * bps);
            sum_is += t.shortfall * bps;
            sum_fill += double(t.filled) / double(p.qty);
        }
    };

    auto t0 = std::chrono::steady_clock::now();
    for (Style style : {Style::Twap, Style::Vwap, Style::Pov, Style::Iceberg, Style::Is, Style::Adaptive}) {
        sum_is = sum_fill = 0;
        dropped = 0;
        for (int g = 0; g < kGroups; ++g) replay_group(style, g);
        std::fprintf(stderr, "%-8s mean shortfall %+7.2f bps, fill rate %.1f%%, %zu dropped feed messages per replay\n",
                     style_name(style), sum_is / double(plans.size()), 100 * sum_fill / double(plans.size()),
                     dropped / kGroups);
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr, "%zu parents x 6 algos, %d replays, in %.2f s\n", plans.size(), 6 * kGroups, secs);
    std::fclose(csv);
}
