#undef NDEBUG  // keep asserts on in Release builds
#include <cassert>
#include <cmath>
#include <cstdio>

#include "algo/exec_sim.hpp"

constexpr uint32_t D = 10'000;  // $1 in ITCH units; one cent = 100 = one book tick

static Algo make(Style style, Qty qty, Nanos start, Nanos end, Nanos step = 1000) {
    AlgoParams a;
    a.style = style;
    a.step = step;
    return Algo(ExecSim::base(0), {0, Side::Buy, qty, start, end}, a);
}

static void test_schedules() {
    Algo twap = make(Style::Twap, 1000, 0, 100);
    assert(twap.target(0, 0) == 0 && twap.target(25, 0) == 250 && twap.target(100, 0) == 1000);
    assert(twap.target(500, 0) == 1000);

    AlgoParams a;
    a.style = Style::Is;
    a.urgency = 3;
    Algo is(0, {0, Side::Buy, 1000, 0, 100}, a);
    assert(is.target(25, 0) > 250 && is.target(50, 0) > 500 && is.target(100, 0) == 1000);  // front-loaded
    a.urgency = 1e-9;
    Algo is_flat(0, {0, Side::Buy, 1000, 0, 100}, a);
    assert(std::llabs(int64_t(is_flat.target(25, 0)) - 250) <= 1);  // urgency -> 0 is TWAP

    a.style = Style::Vwap;
    a.curve = {0.5, 0.75, 0.875, 1.0};  // heavy at the start
    Algo vwap(0, {0, Side::Buy, 1000, 0, 100}, a);
    assert(vwap.target(25, 0) == 500 && vwap.target(50, 0) == 750 && vwap.target(12, 0) == 240);

    a.style = Style::Pov;
    a.pov_rate = 0.1;
    Algo pov(0, {0, Side::Buy, 1000, 0, 100}, a);
    assert(pov.target(10, 3000) == 300 && pov.target(10, 50'000) == 1000);

    a.style = Style::Iceberg;
    Algo ice(0, {0, Side::Buy, 1000, 0, 100}, a);
    assert(ice.target(1, 0) == 1000);
}

struct Rig {
    Venue venue;
    ItchReplay replay{venue, {"AAPL"}};
    Risk risk{1, RiskLimits{}};
    Oms oms{1, 1'000'000, &risk};
    ExecSim sim;
    explicit Rig(Nanos latency) : sim(replay, venue, oms, risk, latency, 500) {}
    void feed(const itch::Writer& w) {
        itch::parse(w.buf.data(), w.buf.size(), [&](const auto& m) { sim.apply(m); });
    }
};

// TWAP buying 200 over two steps: the first slice rests at the bid and gets
// 50 when the flow behind it trades; the rest is crossed at the ask at the end.
static void test_twap_rests_then_crosses() {
    itch::Writer w;
    w.stock_directory(1, 0, "AAPL");
    w.add(1, 10, 1, Side::Buy, 1000, "AAPL", 10 * D);
    w.add(1, 20, 2, Side::Sell, 1000, "AAPL", 10 * D + 100);
    w.add(1, 1000, 3, Side::Buy, 100, "AAPL", 9 * D);       // clock ticks
    w.add(1, 2050, 4, Side::Buy, 100, "AAPL", 10 * D);      // behind our slice
    w.execute(1, 2100, 1, 1000, 1);                          // ahead of us: not ours
    w.execute(1, 2200, 4, 50, 2);                            // behind us: we get 50 first
    w.add(1, 5000, 5, Side::Buy, 100, "AAPL", 9 * D);

    Rig r(10);
    Algo a = make(Style::Twap, 200, 1000, 3000);
    r.sim.add(a);
    r.feed(w);
    const auto& f = r.sim.fills();
    assert(a.done() && a.filled() == 200 && f.size() == 3);
    assert(f[0].time == 2200 && f[0].px == 1000 && f[0].qty == 50 && f[0].mid == 1000.5);
    // the new 100 crosses at once; the 50 left resting crosses when its cancel lands
    assert(f[1].px == 1001 && f[1].qty == 100 && f[2].px == 1001 && f[2].qty == 50 && f[2].time > f[1].time);
    assert(r.risk.position(0) == 200 && r.oms.invalid_transitions() == 0);

    Tca t = tca(r.sim.tca_input(0));
    assert(t.filled == 200 && t.spread == (50 * -0.5 + 150 * 0.5) / 200.0);
    assert(r.sim.run(0).arrival_mid == 1000.5 && !std::isnan(r.sim.run(0).post_mid));
}

// Every style finishes a buy on a synthetic day, never overfills, keeps the
// OMS, risk and fills in agreement, and the run repeats exactly.
static void test_every_style_on_synthetic_feed() {
    itch::Writer w;
    itch::synthetic(w, 300'000, 1, 9);
    Nanos t0 = 34'200'000'000'000 + 10'000'000;
    for (Style style : {Style::Twap, Style::Vwap, Style::Pov, Style::Iceberg, Style::Is, Style::Adaptive}) {
        std::vector<ExecSim::Fill> first;
        for (int pass = 0; pass < 2; ++pass) {
            Venue venue;
            ItchReplay replay(venue, {"SYM0"});
            Risk risk(1, RiskLimits{});
            Oms oms(1, 1'000'000, &risk);
            ExecSim sim(replay, venue, oms, risk, 50'000, 5'000'000);
            AlgoParams a;
            a.style = style;
            a.step = 10'000'000;
            a.give_up = 50'000'000;
            a.curve = {0.4, 0.7, 0.9, 1.0};
            a.display = 300;
            Algo algo(ExecSim::base(0), {0, Side::Buy, 5000, t0, t0 + 150'000'000}, a);
            sim.add(algo);
            itch::parse(w.buf.data(), w.buf.size(), [&](const auto& m) { sim.apply(m); });

            Qty sum = 0;
            for (const ExecSim::Fill& f : sim.fills()) sum += f.qty;
            assert(algo.done() && algo.filled() == 5000 && sum == 5000);
            assert(risk.position(0) == 5000 && oms.invalid_transitions() == 0);
            Tca t = tca(sim.tca_input(0));
            assert(t.filled == 5000 && std::isfinite(t.shortfall) && std::isfinite(t.vs_vwap));
            if (pass == 0) {
                first = sim.fills();
                std::printf("%-8s %3zu fills  shortfall %+.3f ticks  spread %+.3f\n", style_name(style),
                            first.size(), t.shortfall, t.spread);
            } else {
                assert(sim.fills().size() == first.size());
                for (size_t i = 0; i < first.size(); ++i)
                    assert(sim.fills()[i].time == first[i].time && sim.fills()[i].px == first[i].px &&
                           sim.fills()[i].qty == first[i].qty);
            }
        }
    }
}

int main() {
    test_schedules();
    test_twap_rests_then_crosses();
    test_every_style_on_synthetic_feed();
    std::puts("all tests passed");
}
