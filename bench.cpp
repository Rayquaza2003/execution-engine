// Replays a deterministic synthetic order flow through the book.
// Usage: ./bench [num_ops]   (default 10M)
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>

#include "orderbook.hpp"

using Clock = std::chrono::steady_clock;

enum class OpType : uint8_t { Limit, Market, Cancel, Modify };

struct Op {
    OpType type;
    Side side;
    OrderId id;
    Price px;
    Qty qty;
};

constexpr size_t kWarmup = 10'000;  // resting orders placed before timing starts

// Mix after warmup: 45% limit, 45% cancel, 5% modify, 5% market, so book size
// stays roughly flat. Prices sit 0-40 ticks behind a slowly walking mid; ~7%
// of limits cross. The generator ignores fills, so some cancels and modifies
// target already-filled orders and get rejected, as they do in real markets.
static std::vector<Op> generate(size_t n, uint64_t seed) {
    std::mt19937_64 rng(seed);
    auto uni = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };

    struct Live { OrderId id; Side side; };
    std::vector<Live> live;
    std::vector<Op> ops;
    ops.reserve(n);
    OrderId next_id = 1;
    Price mid = 100'000;

    auto passive_px = [&](Side s) {
        int off = uni(-2, 40);
        return s == Side::Buy ? mid - off : mid + off;
    };

    for (size_t i = 0; i < n; ++i) {
        if (uni(0, 9) == 0) mid += uni(-1, 1);
        Side side = uni(0, 1) ? Side::Buy : Side::Sell;
        Qty qty = uni(1, 100);
        int r = uni(0, 99);

        if (i < kWarmup || live.empty() || r < 45) {
            ops.push_back({OpType::Limit, side, next_id, passive_px(side), qty});
            live.push_back({next_id++, side});
        } else if (r < 90) {
            size_t k = uni(0, int(live.size()) - 1);
            ops.push_back({OpType::Cancel, live[k].side, live[k].id, 0, 0});
            live[k] = live.back();
            live.pop_back();
        } else if (r < 95) {
            const Live& l = live[uni(0, int(live.size()) - 1)];
            ops.push_back({OpType::Modify, l.side, l.id, passive_px(l.side), qty});
        } else {
            ops.push_back({OpType::Market, side, next_id++, 0, qty});
        }
    }
    return ops;
}

static void print_percentiles(const char* name, std::vector<uint32_t>& ns) {
    if (ns.empty()) return;
    std::sort(ns.begin(), ns.end());
    auto pct = [&](double p) { return ns[size_t(p * (ns.size() - 1))]; };
    std::printf("%-8s %10zu %8u %8u %8u %8u %8u %10u\n", name, ns.size(), pct(0.5), pct(0.9),
                pct(0.99), pct(0.999), pct(0.9999), ns.back());
}

static inline void apply(OrderBook& b, const Op& op, std::vector<Trade>& t) {
    switch (op.type) {
        case OpType::Limit: b.limit(op.id, op.side, op.px, op.qty, t); break;
        case OpType::Market: b.market(op.id, op.side, op.qty, t); break;
        case OpType::Cancel: b.cancel(op.id); break;
        case OpType::Modify: b.modify(op.id, op.px, op.qty, t); break;
    }
}

int main(int argc, char** argv) {
    size_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 10'000'000;
    if (n <= kWarmup) {
        std::fprintf(stderr, "num_ops must exceed warmup (%zu)\n", kWarmup);
        return 1;
    }
    std::vector<Op> ops = generate(n, 42);
    std::vector<Trade> trades;
    trades.reserve(1024);

    OrderBook book;
    for (size_t i = 0; i < kWarmup; ++i) apply(book, ops[i], trades);
    trades.clear();

    size_t total_trades = 0;
    auto t0 = Clock::now();
    for (size_t i = kWarmup; i < n; ++i) {
        apply(book, ops[i], trades);
        total_trades += trades.size();
        trades.clear();
    }
    double secs = std::chrono::duration<double>(Clock::now() - t0).count();

    size_t timed = n - kWarmup;
    std::printf("ops %zu  trades %zu  resting %zu\n", timed, total_trades, book.order_count());
    std::printf("throughput %.2f M ops/s  (%.1f ns/op avg)\n\n", timed / secs / 1e6, secs * 1e9 / timed);

    // Latency pass: same ops, fresh book, each op bracketed by clock reads.
    // Kept separate so clock overhead does not pollute the throughput number.
    std::vector<uint32_t> lat[4];
    size_t counts[4] = {};
    for (size_t i = kWarmup; i < n; ++i) ++counts[size_t(ops[i].type)];
    for (int k = 0; k < 4; ++k) lat[k].reserve(counts[k]);

    OrderBook book2;
    for (size_t i = 0; i < kWarmup; ++i) apply(book2, ops[i], trades);
    trades.clear();
    for (size_t i = kWarmup; i < n; ++i) {
        auto a = Clock::now();
        apply(book2, ops[i], trades);
        auto b = Clock::now();
        lat[size_t(ops[i].type)].push_back(
            uint32_t(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count()));
        trades.clear();
    }

    std::printf("latency ns %10s %8s %8s %8s %8s %8s %10s\n", "count", "p50", "p90", "p99", "p99.9",
                "p99.99", "max");
    const char* names[4] = {"limit", "market", "cancel", "modify"};
    std::vector<uint32_t> all;
    all.reserve(timed);
    for (int k = 0; k < 4; ++k) all.insert(all.end(), lat[k].begin(), lat[k].end());
    for (int k = 0; k < 4; ++k) print_percentiles(names[k], lat[k]);
    print_percentiles("all", all);
}
