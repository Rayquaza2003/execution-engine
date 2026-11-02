#undef NDEBUG  // keep asserts on in Release builds
#include <cassert>
#include <cstdio>
#include <vector>

#include "core/event_loop.hpp"

struct Ev {
    int id;
};

static void test_fires_in_time_order() {
    EventLoop<Ev> loop;
    loop.schedule_at(300, {3});
    loop.schedule_at(100, {1});
    loop.schedule_at(200, {2});
    std::vector<int> seen;
    std::vector<Nanos> at;
    assert(loop.run([&](Ev& e) {
        seen.push_back(e.id);
        at.push_back(loop.now());
    }) == 3);
    assert((seen == std::vector<int>{1, 2, 3}));
    assert((at == std::vector<Nanos>{100, 200, 300}));
    assert(loop.now() == 300 && loop.empty());
}

static void test_equal_times_keep_fifo_order() {
    EventLoop<Ev> loop;
    for (int i = 0; i < 100; ++i) loop.schedule_at(50, {i});
    std::vector<int> seen;
    loop.run([&](Ev& e) { seen.push_back(e.id); });
    for (int i = 0; i < 100; ++i) assert(seen[i] == i);
}

// A handler that schedules follow-ups models request -> latency -> response.
static void test_handler_schedules_follow_ups() {
    EventLoop<Ev> loop;
    loop.schedule_at(10, {0});
    std::vector<std::pair<int, Nanos>> seen;
    loop.run([&](Ev& e) {
        seen.push_back({e.id, loop.now()});
        if (e.id < 3) loop.schedule_in(5, {e.id + 1});
        if (e.id == 0) loop.schedule_in(0, {100});  // same-time event fires before later ones
    });
    assert((seen == std::vector<std::pair<int, Nanos>>{{0, 10}, {100, 10}, {1, 15}, {2, 20}, {3, 25}}));
}

static void test_run_until_stops_and_advances_clock() {
    EventLoop<Ev> loop;
    loop.schedule_at(100, {1});
    loop.schedule_at(200, {2});
    loop.schedule_at(201, {3});
    int fired = 0;
    assert(loop.run_until(200, [&](Ev&) { ++fired; }) == 2);  // boundary is inclusive
    assert(fired == 2 && loop.pending() == 1 && loop.now() == 200);

    assert(loop.run_until(150, [&](Ev&) { ++fired; }) == 0);  // never moves backwards
    assert(loop.now() == 200);

    assert(loop.run_until(1000, [&](Ev&) { ++fired; }) == 1);
    assert(loop.now() == 1000 && loop.empty());
}

// Same schedule twice -> identical trace: the property replay relies on.
static void test_deterministic() {
    auto trace = [] {
        EventLoop<Ev> loop;
        uint64_t x = 12345;
        for (int i = 0; i < 1000; ++i) {
            x = x * 6364136223846793005ull + 1442695040888963407ull;
            loop.schedule_at(x % 64, {i});
        }
        std::vector<int> seen;
        loop.run([&](Ev& e) { seen.push_back(e.id); });
        return seen;
    };
    assert(trace() == trace());
}

int main() {
    test_fires_in_time_order();
    test_equal_times_keep_fifo_order();
    test_handler_schedules_follow_ups();
    test_run_until_stops_and_advances_clock();
    test_deterministic();
    std::puts("all tests passed");
}
