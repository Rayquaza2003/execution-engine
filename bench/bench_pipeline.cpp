// The engine as a threaded pipeline, one component per thread, lock-free
// SPSC queues between them, nothing else shared:
//
//   feed --A--> venue --B--> market data --C--> strategy (OMS + risk)
//                 ^                                   |
//                 +------------------D----------------+   requests
//                 +------------------E--------------->    execution reports
//
// feed:     replays ITCH messages for our symbols (preloaded, so parsing isn't timed)
// venue:    ItchReplay + Venue: real orders and ours in the same books
// md:       client L2 books from level updates; a quote when a best price moves
// strategy: keeps one buy at the bid and one sell at the ask per symbol,
//           replacing them as the touch moves, through the OMS and risk;
//           every order is logged through the async logger
//
// Each hop's queueing delay (enqueue to dequeue) goes into a histogram, and so
// does tick-to-order: feed message enqueued to the order it caused enqueued.
// Runs: the same work on one thread (baseline), the pipeline flat out
// (throughput), and the pipeline paced at 250k msgs/s (latency under load).
// Idle threads busy-spin with a CPU hint. Yielding to the OS when idle was
// tried and made tails ~50x worse on macOS: a yielding thread gets parked.
//
// Usage: [PIN_CPUS=a,b,c,d] ./build/bench_pipeline [ITCH_FILE SYMBOL...]   (default: synthetic feed)
// PIN_CPUS pins feed, venue, md, strategy to those CPUs (Linux). macOS can't
// pin threads; the clock ticks every 41.67 ns on Apple silicon.
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "core/affinity.hpp"
#include "core/histogram.hpp"
#include "core/logger.hpp"
#include "core/spsc.hpp"
#include "md/market_data.hpp"
#include "md/replay.hpp"
#include "oms/oms.hpp"

static uint64_t now() { return Logger::now_ns(); }


struct End {};
using Item = std::variant<End, itch::StockDirectory, itch::Add, itch::Execute, itch::Cancel, itch::Delete, itch::Replace>;
struct FeedMsg {
    Item m;
    uint64_t t0;  // origin: when the feed thread enqueued it
};
struct MdMsg {
    LevelUpdate u;
    uint64_t t0, enq;
    bool end;
};
struct QuoteMsg {
    SymbolId sym;
    Quote q;
    uint64_t t0, enq;
    bool end;
};
struct ReqMsg {
    VenueRequest rq;
    uint64_t t0, enq;
};
struct RepMsg {
    ExecReport rep;
    uint64_t enq;
};

template <class Q, class T>
static void push(Q& q, const T& v, uint64_t& stalls) {
    if (q.try_push(v)) return;
    ++stalls;  // full: the consumer is behind; spin
    while (!q.try_push(v)) cpu_relax();
}

// The strategy: one buy at the bid and one sell at the ask per symbol,
// re-priced through the OMS (and its risk check) whenever the touch moves.
struct Chaser {
    Risk risk;
    Oms oms;
    std::vector<std::array<ClOrdId, 2>> live;
    ClOrdId next = 1;
    uint64_t rejects = 0;

    explicit Chaser(size_t nsym) : risk(nsym, RiskLimits{}), oms(1, 1'000'000, &risk, 1 << 20), live(nsym, {0, 0}) {}

    void on_quote(SymbolId sym, const Quote& q, std::vector<VenueRequest>& reqs) {
        if (!q.two_sided()) return;
        risk.mark(sym, (q.bid + q.ask) / 2);
        for (int s = 0; s < 2; ++s) {
            Side side = s ? Side::Sell : Side::Buy;
            Price px = s ? q.ask : q.bid;
            ClOrdId& cl = live[sym][s];
            const OmsOrder* o = cl ? oms.find(cl) : nullptr;
            if (!o || is_terminal(o->state)) {
                cl = next++;
                rejects += oms.submit({cl, sym, side, OrdType::Limit, Tif::Day, px, 100}, reqs).order.state ==
                           OrdState::Rejected;
            } else if ((o->state == OrdState::Acknowledged || o->state == OrdState::PartiallyFilled) && o->px != px) {
                oms.replace(cl, px, 100, reqs);
            }
        }
    }
};

struct Pipeline {
    Spsc<FeedMsg, 1 << 16> a;
    Spsc<MdMsg, 1 << 16> b;
    Spsc<QuoteMsg, 1 << 14> c;
    Spsc<ReqMsg, 1 << 14> d;
    Spsc<RepMsg, 1 << 14> e;
    std::atomic<bool> strategy_done{false};
    Histogram hop_a, hop_b, hop_c, hop_d, hop_e, tick_to_order;
    uint64_t stall_a = 0, stall_b = 0, stall_c = 0, stall_d = 0, stall_e = 0;
    uint64_t quotes = 0, orders = 0, fills = 0, rejects = 0, invalid = 0;
    uint64_t t_start = 0, t_end = 0;
};

static void run(const std::vector<FeedMsg>& feed, const std::vector<std::string>& want, double rate,
                std::FILE* log_out) {
    auto p = std::make_unique<Pipeline>();
    Logger logger(log_out, 1);
    size_t nsym = want.size();

    std::thread venue_t([&] {
        place_thread(1);
        Venue venue;
        ItchReplay replay(venue, want);
        VenueOut out;
        bool ended = false;
        // After the end marker, md and strategy are winding down: stop feeding them.
        auto emit = [&](uint64_t t0) {
            uint64_t t = now();
            if (!ended)
                for (const LevelUpdate& u : out.levels) push(p->b, MdMsg{u, t0, t, false}, p->stall_b);
            for (const ExecReport& r : out.reports) {
                if (r.id >= ItchReplay::kIdBase) continue;  // a feed order: nobody to tell
                RepMsg m{r, t};
                if (p->e.try_push(m)) continue;
                ++p->stall_e;
                while (!p->e.try_push(m) && !p->strategy_done.load(std::memory_order_acquire)) cpu_relax();
            }
            out.clear();
        };
        for (;;) {
            FeedMsg f;
            bool got = !ended && p->a.try_pop(f);
            if (got) {
                p->hop_a.record(now() - f.t0);
                if (std::holds_alternative<End>(f.m)) {
                    ended = true;
                    push(p->b, MdMsg{{}, 0, now(), true}, p->stall_b);
                } else {
                    std::visit([&](const auto& m) {
                        if constexpr (!std::is_same_v<std::decay_t<decltype(m)>, End>) replay.apply(m, out);
                    }, f.m);
                    emit(f.t0);
                }
            }
            ReqMsg r;
            while (p->d.try_pop(r)) {
                got = true;
                p->hop_d.record(now() - r.enq);
                venue.on_request(r.rq, out);
                emit(r.t0);
            }
            if (ended && p->strategy_done.load(std::memory_order_acquire) && !p->d.size_approx()) break;
            if (!got) cpu_relax();
        }
        p->t_end = now();
    });

    std::thread md_t([&] {
        place_thread(2);
        MarketData md(1, nsym);
        MdMsg m;
        for (;;) {
            if (!p->b.try_pop(m)) {
                cpu_relax();
                continue;
            }
            uint64_t t = now();
            p->hop_b.record(t - m.enq);
            if (m.end) {
                push(p->c, QuoteMsg{0, {}, 0, now(), true}, p->stall_c);
                return;
            }
            Quote before = md.bbo(0, m.u.sym);
            md.on_level(0, m.u);
            Quote after = md.bbo(0, m.u.sym);
            if (after.bid != before.bid || after.ask != before.ask)
                push(p->c, QuoteMsg{m.u.sym, after, m.t0, now(), false}, p->stall_c);
        }
    });

    std::thread strategy_t([&] {
        place_thread(3);
        Chaser chaser(nsym);
        Logger::Channel& log = logger.channel(0);
        std::vector<VenueRequest> reqs;
        for (;;) {
            RepMsg r;
            while (p->e.try_pop(r)) {
                p->hop_e.record(now() - r.enq);
                chaser.oms.on_report(r.rep);
                p->fills += r.rep.type == ExecType::Trade;
            }
            QuoteMsg q;
            if (!p->c.try_pop(q)) {
                cpu_relax();
                continue;
            }
            p->hop_c.record(now() - q.enq);
            if (q.end) break;
            ++p->quotes;
            chaser.on_quote(q.sym, q.q, reqs);
            for (const VenueRequest& rq : reqs) {
                uint64_t t = now();
                push(p->d, ReqMsg{rq, q.t0, t}, p->stall_d);
                p->tick_to_order.record(t - q.t0);
                ++p->orders;
                log.log("order sym=%u t0=%llu", q.sym, (unsigned long long)q.t0);
            }
            reqs.clear();
        }
        p->rejects = chaser.rejects;
        p->invalid = chaser.oms.invalid_transitions();
        p->strategy_done.store(true, std::memory_order_release);
    });

    // Feed: this thread.
    place_thread(0);
    p->t_start = now();
    for (size_t i = 0; i < feed.size(); ++i) {
        if (rate > 0)
            while (now() < p->t_start + uint64_t(double(i) * 1e9 / rate)) {
            }
        FeedMsg f = feed[i];
        f.t0 = now();
        push(p->a, f, p->stall_a);
    }
    push(p->a, FeedMsg{End{}, now()}, p->stall_a);
    venue_t.join();
    md_t.join();
    strategy_t.join();

    double secs = double(p->t_end - p->t_start) / 1e9;
    if (rate > 0) std::printf("\n== paced at %.2f M msgs/s ==\n", rate / 1e6);
    else std::printf("\n== flat out ==\n");
    std::printf("%zu feed messages in %.3f s: %.2f M msgs/s through the venue\n", feed.size(), secs,
                double(feed.size()) / secs / 1e6);
    std::printf("%llu quotes, %llu orders (%llu risk rejects), %llu fills, %llu invalid transitions\n",
                (unsigned long long)p->quotes, (unsigned long long)p->orders, (unsigned long long)p->rejects,
                (unsigned long long)p->fills, (unsigned long long)p->invalid);
    std::printf("queue full (producer had to wait): A %llu  B %llu  C %llu  D %llu  E %llu\n",
                (unsigned long long)p->stall_a, (unsigned long long)p->stall_b, (unsigned long long)p->stall_c,
                (unsigned long long)p->stall_d, (unsigned long long)p->stall_e);
    p->hop_a.print(stdout, "A feed->venue");
    p->hop_b.print(stdout, "B venue->md");
    p->hop_c.print(stdout, "C md->strategy");
    p->hop_d.print(stdout, "D strategy->venue");
    p->hop_e.print(stdout, "E venue->strategy");
    p->tick_to_order.print(stdout, "tick-to-order");
    std::printf("logger: %llu lines written, %llu dropped\n", (unsigned long long)logger.written(),
                (unsigned long long)logger.channel(0).dropped());
}

// The same work on one thread, no queues: the baseline the pipeline is paying to beat.
static void run_inline(const std::vector<FeedMsg>& feed, const std::vector<std::string>& want) {
    Venue venue;
    ItchReplay replay(venue, want);
    MarketData md(1, want.size());
    Chaser chaser(want.size());
    VenueOut out;
    std::vector<VenueRequest> reqs;
    std::vector<ExecReport> reports;
    std::vector<LevelUpdate> levels;
    uint64_t orders = 0, fills = 0;
    Histogram tick_to_order;
    // Venue output: level updates to md (quotes to the strategy), reports back to the OMS.
    auto drain = [&](uint64_t t0) {
        while (!out.levels.empty() || !out.reports.empty()) {
            levels.clear();
            levels.swap(out.levels);  // buffers trade places: no allocation in steady state
            reports.assign(out.reports.begin(), out.reports.end());
            out.clear();
            for (const ExecReport& r : reports)
                if (r.id < ItchReplay::kIdBase) chaser.oms.on_report(r), fills += r.type == ExecType::Trade;
            for (const LevelUpdate& u : levels) {
                Quote before = md.bbo(0, u.sym);
                md.on_level(0, u);
                Quote after = md.bbo(0, u.sym);
                if (after.bid == before.bid && after.ask == before.ask) continue;
                chaser.on_quote(u.sym, after, reqs);
                for (const VenueRequest& rq : reqs) {
                    tick_to_order.record(now() - t0);
                    ++orders;
                    venue.on_request(rq, out);  // its output joins the loop
                }
                reqs.clear();
            }
        }
    };
    uint64_t t_start = now();
    for (const FeedMsg& f : feed) {
        uint64_t t0 = now();
        std::visit([&](const auto& m) {
            if constexpr (!std::is_same_v<std::decay_t<decltype(m)>, End>) replay.apply(m, out);
        }, f.m);
        drain(t0);
    }
    double secs = double(now() - t_start) / 1e9;
    std::printf("\n== one thread, no queues ==\n%zu feed messages in %.3f s: %.2f M msgs/s\n", feed.size(), secs,
                double(feed.size()) / secs / 1e6);
    std::printf("%llu orders, %llu fills, %zu invalid transitions\n", (unsigned long long)orders,
                (unsigned long long)fills, chaser.oms.invalid_transitions());
    tick_to_order.print(stdout, "tick-to-order");
}

// Hot-path cost of one log() call vs formatting with fprintf on the spot.
static void logger_cost(std::FILE* sink) {
    constexpr int kBatch = 8192, kBatches = 50;  // a batch fits the ring: no drops
    uint64_t async_ns = 0, sync_ns = 0;
    {
        Logger logger(sink, 1);
        for (int b = 0; b < kBatches; ++b) {
            uint64_t t = now();
            for (int i = 0; i < kBatch; ++i) logger.channel(0).log("order sym=%u px=%lld qty=%llu", 3u, 15000ll, 100ull);
            async_ns += now() - t;
            while (logger.written() < uint64_t(b + 1) * kBatch) std::this_thread::yield();
        }
    }
    for (int b = 0; b < kBatches; ++b) {
        uint64_t t = now();
        for (int i = 0; i < kBatch; ++i) std::fprintf(sink, "%llu order sym=%u px=%lld qty=%llu\n", 0ull, 3u, 15000ll, 100ull);
        sync_ns += now() - t;
    }
    double n = double(kBatch) * kBatches;
    std::printf("log() on the hot path: %.1f ns/call async, %.1f ns/call with fprintf\n", double(async_ns) / n,
                double(sync_ns) / n);
}

int main(int argc, char** argv) {
    std::vector<std::string> want;
    std::vector<FeedMsg> feed;
    itch::Writer w;
    const uint8_t* data;
    size_t bytes;
    if (argc > 2) {
        int fd = open(argv[1], O_RDONLY);
        struct stat st;
        if (fd < 0 || fstat(fd, &st) != 0) {
            std::perror(argv[1]);
            return 1;
        }
        bytes = size_t(st.st_size);
        data = static_cast<const uint8_t*>(mmap(nullptr, bytes, PROT_READ, MAP_PRIVATE, fd, 0));
        if (data == MAP_FAILED) return 1;
        want.assign(argv + 2, argv + argc);
    } else {
        for (int s = 0; s < 8; ++s) want.push_back("SYM" + std::to_string(s));
        itch::synthetic(w, 2'000'000, 8, 42);
        data = w.buf.data();
        bytes = w.buf.size();
    }
    // Keep only our symbols' messages, so the timed part is the pipeline.
    std::vector<bool> ours(1 << 16);
    itch::parse(data, bytes, [&](const auto& m) {
        using M = std::decay_t<decltype(m)>;
        if constexpr (std::is_same_v<M, itch::StockDirectory>) {
            std::string s(m.stock, 8);
            s.erase(s.find_last_not_of(' ') + 1);
            for (const std::string& x : want)
                if (x == s) ours[m.locate] = true, feed.push_back({m, 0});
        } else if (ours[m.locate]) {
            feed.push_back({m, 0});
        }
    });
    std::printf("%zu messages for %zu symbols\n", feed.size(), want.size());

    std::FILE* sink = std::fopen("/dev/null", "w");
    logger_cost(sink);
    run_inline(feed, want);
    run(feed, want, 0, sink);
    run(feed, want, 250e3, sink);
    std::fclose(sink);
}
