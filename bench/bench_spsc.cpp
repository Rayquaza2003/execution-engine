// Lock-free SPSC queue vs a mutex-protected deque, between two threads.
// Usage: ./build/bench_spsc [messages]   (default 20M)
//
//   throughput: one thread pushes as fast as it can, the other pops
//   round trip: thread A pushes a timestamp, B echoes it back on a second
//               queue, A records the round trip; one message in flight
//
// macOS can't pin threads to cores, so placement (and P vs E cores) is up to
// the scheduler; medians over 5 runs.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/histogram.hpp"
#include "core/spsc.hpp"

using Clock = std::chrono::steady_clock;
static uint64_t now_ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

// Same interface and capacity, one lock around a deque.
template <class T, size_t N>
class Locked {
public:
    bool try_push(const T& v) {
        std::lock_guard<std::mutex> g(m_);
        if (q_.size() == N) return false;
        q_.push_back(v);
        return true;
    }
    bool try_pop(T& v) {
        std::lock_guard<std::mutex> g(m_);
        if (q_.empty()) return false;
        v = q_.front();
        q_.pop_front();
        return true;
    }

private:
    std::mutex m_;
    std::deque<T> q_;
};

constexpr size_t kCap = 1 << 12;

template <class Q>
double throughput(uint64_t n) {
    auto q = std::make_unique<Q>();
    uint64_t t0 = now_ns();
    std::thread producer([&] {
        for (uint64_t i = 0; i < n; ++i)
            while (!q->try_push(i)) {
            }
    });
    uint64_t v = 0, sum = 0;
    for (uint64_t got = 0; got < n;)
        if (q->try_pop(v)) sum += v, ++got;
    producer.join();
    if (sum != n * (n - 1) / 2) std::abort();
    return double(n) / (double(now_ns() - t0) / 1e9);
}

template <class Q>
Histogram round_trip(uint64_t n) {
    auto there = std::make_unique<Q>(), back = std::make_unique<Q>();
    std::thread echo([&] {
        uint64_t v;
        for (uint64_t i = 0; i < n; ++i) {
            while (!there->try_pop(v)) {
            }
            while (!back->try_push(v)) {
            }
        }
    });
    Histogram h;
    uint64_t v;
    for (uint64_t i = 0; i < n; ++i) {
        uint64_t t = now_ns();
        while (!there->try_push(t)) {
        }
        while (!back->try_pop(v)) {
        }
        h.record(now_ns() - v);
    }
    echo.join();
    return h;
}

template <class Q>
void run(const char* name, uint64_t n) {
    std::vector<double> mps;
    for (int r = 0; r < 5; ++r) mps.push_back(throughput<Q>(n) / 1e6);
    std::sort(mps.begin(), mps.end());
    std::printf("%-10s throughput: median %7.1f M msgs/s  (min %.1f, max %.1f)\n", name, mps[2], mps[0], mps[4]);
    std::vector<Histogram> rt;
    for (int r = 0; r < 5; ++r) rt.push_back(round_trip<Q>(n / 100));
    std::sort(rt.begin(), rt.end(), [](const Histogram& a, const Histogram& b) {
        return a.percentile(0.5) < b.percentile(0.5);
    });
    rt[2].print(stdout, (std::string(name) + " round trip").c_str());
}

int main(int argc, char** argv) {
    uint64_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 20'000'000;
    run<Spsc<uint64_t, kCap>>("spsc", n);
    run<Locked<uint64_t, kCap>>("mutex", n);
}
