#undef NDEBUG  // keep asserts on in Release builds
#include <cassert>
#include <cmath>
#include <cstdio>

#include "analytics/fill_study.hpp"

// $1 in ITCH units (1/10000 $); one cent = 100, one book tick.
constexpr uint32_t D = 10'000;

struct Run {
    Venue venue;
    ItchReplay replay{venue, {"AAPL"}};
    FillStudy study;
    VenueOut out;
    std::vector<FillRow> rows;
    explicit Run(FillConfig cfg) : study(replay, venue, std::move(cfg)) {}
    void feed(const itch::Writer& w) {
        itch::parse(w.buf.data(), w.buf.size(), [&](const auto& m) { study.apply(m, out, rows); });
    }
    const FillRow& row(Side side, Nanos latency) const {
        for (const FillRow& r : rows)
            if (r.side == side && r.latency == latency) return r;
        assert(false);
        return rows[0];
    }
};

FillConfig config(std::vector<Nanos> latencies) {
    FillConfig c;
    c.latencies = std::move(latencies);
    c.start = 1000;
    c.every = 1'000'000;  // one decision in these tests
    c.horizon = 1000;
    c.markout = 100;
    c.join_new_levels = false;
    return c;
}

// Queue ahead is exact: cancels and executions of orders ahead drain it, an
// order added during our latency lands ahead of the slower order only, and
// we fill on the first execution of an order behind us.
static void test_queue_drains_then_fills() {
    itch::Writer w;
    w.stock_directory(1, 0, "AAPL");
    w.add(1, 10, 1, Side::Buy, 100, "AAPL", 10 * D);
    w.add(1, 20, 2, Side::Buy, 200, "AAPL", 10 * D);
    w.add(1, 30, 3, Side::Sell, 100, "AAPL", 10 * D + 100);
    // decision at 1000: join the bid at 1000 and the ask at 1001; latencies 0 and 50
    w.add(1, 1010, 4, Side::Buy, 100, "AAPL", 10 * D);  // behind the fast order, ahead of the slow one
    w.cancel(1, 1020, 1, 50);
    w.execute(1, 1100, 1, 50, 1);
    w.execute(1, 1110, 2, 200, 2);  // fast order now at the front
    w.execute(1, 1120, 4, 30, 3);   // ref 4 is behind the fast order: it fills
    w.add(1, 3000, 5, Side::Buy, 100, "AAPL", 9 * D);  // moves the clock past every horizon

    Run r(config({0, 50}));
    r.feed(w);
    assert(r.rows.size() == 4 && r.study.decided() == 4 && r.study.open() == 0 && r.study.rejected() == 0);
    const FillRow& fast = r.row(Side::Buy, 0);
    assert(fast.px == 1000 && fast.decided == 1000 && fast.queue_ahead == 300);
    assert(fast.filled && fast.time_to_fill == 120 && fast.markout == 0.5);  // mid 1000.5 at 1220
    const FillRow& slow = r.row(Side::Buy, 50);
    assert(!slow.filled && slow.time_to_fill == 0 && std::isnan(slow.markout));
    assert(slow.queue_ahead == 350);  // 300 - 50 cancelled + 100 added during its latency
    for (Nanos lat : {0, 50}) {
        const FillRow& sell = r.row(Side::Sell, lat);
        assert(sell.px == 1001 && sell.queue_ahead == 100 && !sell.filled);
    }
}

// Trading through our price fills us, and so does an opposite-side order
// added at our price once the real orders ahead are gone.
static void test_trade_through_and_crossing_add() {
    itch::Writer w;
    w.stock_directory(1, 0, "AAPL");
    w.add(1, 10, 1, Side::Buy, 100, "AAPL", 10 * D);
    w.add(1, 20, 2, Side::Sell, 100, "AAPL", 10 * D + 200);
    w.del(1, 1010, 1);                                       // we are alone at 10.00
    w.add(1, 1020, 3, Side::Buy, 100, "AAPL", 10 * D - 100);
    w.execute(1, 1030, 3, 100, 1);                           // sold at 9.99: through our 10.00 bid
    w.del(1, 1040, 2);                                       // we are alone at 10.02
    w.add(1, 1050, 4, Side::Buy, 100, "AAPL", 10 * D + 200);  // would have hit our offer
    w.add(1, 3000, 5, Side::Buy, 100, "AAPL", 9 * D);
    Run r(config({0}));
    r.feed(w);
    const FillRow& buy = r.row(Side::Buy, 0);
    const FillRow& sell = r.row(Side::Sell, 0);
    assert(buy.filled && buy.time_to_fill == 30 && buy.queue_ahead == 100);
    assert(sell.filled && sell.time_to_fill == 50);
    assert(std::isnan(buy.markout) && std::isnan(sell.markout));  // one-sided book at both markouts
}

// Post-only: if the other side has reached our price by arrival, we don't rest.
static void test_post_only_reject() {
    itch::Writer w;
    w.stock_directory(1, 0, "AAPL");
    w.add(1, 10, 1, Side::Buy, 100, "AAPL", 10 * D);
    w.add(1, 20, 2, Side::Sell, 100, "AAPL", 10 * D + 100);
    w.del(1, 1010, 1);
    w.add(1, 1020, 3, Side::Sell, 100, "AAPL", 10 * D);  // offer now at our 10.00 bid
    w.add(1, 3000, 4, Side::Buy, 100, "AAPL", 9 * D);
    Run r(config({100}));
    r.feed(w);
    assert(r.study.decided() == 2 && r.study.rejected() == 1 && r.rows.size() == 1);
    assert(r.rows[0].side == Side::Sell && r.rows[0].queue_ahead == 100);
}

// An order that opens a new best level triggers a join: the fast order is
// second in the fresh queue, the slow one also queues behind a later add.
static void test_join_new_level() {
    itch::Writer w;
    w.stock_directory(1, 0, "AAPL");
    w.add(1, 10, 1, Side::Buy, 100, "AAPL", 10 * D);
    w.add(1, 20, 2, Side::Sell, 100, "AAPL", 10 * D + 200);
    w.add(1, 1010, 3, Side::Buy, 100, "AAPL", 10 * D + 100);  // new best bid 10.01
    w.add(1, 1020, 4, Side::Buy, 300, "AAPL", 10 * D + 100);  // joins it
    w.add(1, 1030, 5, Side::Buy, 100, "AAPL", 10 * D);        // not a new level
    w.add(1, 3000, 6, Side::Buy, 100, "AAPL", 9 * D);         // a new level, but below the best
    FillConfig c = config({0, 50});
    c.join_new_levels = true;
    Run r(std::move(c));
    r.feed(w);
    size_t joins = 0;
    for (const FillRow& x : r.rows) {
        if (!x.new_level) continue;
        ++joins;
        assert(x.side == Side::Buy && x.px == 1001 && x.decided == 1010);
        assert(x.queue_ahead == (x.latency == 0 ? 100 : 400));
    }
    assert(joins == 2 && r.rows.size() == 6);  // plus the 4 timer orders at 1000
}

// A whole synthetic day: every decision ends as a row, a reject or still open,
// and rows obey the horizon.
static void test_synthetic_accounting() {
    itch::Writer w;
    itch::synthetic(w, 300'000, 2, 5);
    Venue venue;
    ItchReplay replay(venue, {"SYM0", "SYM1"});
    FillConfig cfg;
    cfg.every = 1'000'000;  // the feed covers about 0.3 s
    cfg.horizon = 50'000'000;
    cfg.markout = 10'000'000;
    FillStudy study(replay, venue, cfg);
    VenueOut out;
    std::vector<FillRow> rows;
    itch::parse(w.buf.data(), w.buf.size(), [&](const auto& m) {
        study.apply(m, out, rows);
        out.clear();
    });
    assert(study.decided() == rows.size() + study.rejected() + study.open());
    size_t filled = 0;
    for (const FillRow& r : rows) {
        assert(r.time_to_fill <= cfg.horizon && r.filled == !std::isnan(r.markout));
        filled += r.filled;
    }
    // Synthetic executions pick random resting orders, not the front of the
    // queue, so nearly every virtual order fills; real data is the study.
    assert(rows.size() > 1000 && filled > 0);
    std::printf("synthetic: %zu rows, %zu filled, %zu rejected\n", rows.size(), filled, study.rejected());
}

int main() {
    test_queue_drains_then_fills();
    test_trade_through_and_crossing_add();
    test_post_only_reject();
    test_join_new_level();
    test_synthetic_accounting();
    std::puts("all tests passed");
}
