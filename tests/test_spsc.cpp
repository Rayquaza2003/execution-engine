#undef NDEBUG  // keep asserts on in Release builds
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <thread>

#include "core/spsc.hpp"

static void test_fifo_full_and_empty() {
    Spsc<int, 4> q;
    int v = 0;
    assert(!q.try_pop(v));
    for (int i = 1; i <= 4; ++i) assert(q.try_push(i));
    assert(!q.try_push(5) && q.size_approx() == 4);
    assert(q.try_pop(v) && v == 1);
    assert(q.try_push(5));  // wraps around
    for (int want = 2; want <= 5; ++want) assert(q.try_pop(v) && v == want);
    assert(!q.try_pop(v) && q.size_approx() == 0);
}

// Two threads, a small ring so it is constantly full and empty: every value
// must arrive exactly once, in order, with its payload intact.
static void test_two_threads_keep_order() {
    struct Msg {
        uint64_t seq, check;
    };
    constexpr uint64_t kCount = 2'000'000;
    Spsc<Msg, 64> q;
    std::thread producer([&] {
        for (uint64_t i = 0; i < kCount; ++i)
            while (!q.try_push({i, i * 0x9E3779B97F4A7C15ull})) {
            }
    });
    uint64_t next = 0;
    Msg m;
    while (next < kCount)
        if (q.try_pop(m)) {
            assert(m.seq == next && m.check == next * 0x9E3779B97F4A7C15ull);
            ++next;
        }
    producer.join();
    assert(!q.try_pop(m));
}

int main() {
    test_fifo_full_and_empty();
    test_two_threads_keep_order();
    std::puts("all tests passed");
}
