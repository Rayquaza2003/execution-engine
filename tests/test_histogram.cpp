#undef NDEBUG  // keep asserts on in Release builds
#include <cassert>
#include <cstdio>
#include <random>
#include <vector>

#include "core/histogram.hpp"

static void test_buckets_cover_every_value_once() {
    for (uint64_t v : {0ull, 1ull, 15ull, 16ull, 17ull, 31ull, 32ull, 1000ull, 123456789ull, ~0ull}) {
        size_t i = Histogram::index(v);
        assert(Histogram::top(i) >= v);
        assert(i == 0 || Histogram::top(i - 1) < v);  // the bucket below ends before v
    }
    assert(Histogram::index(~0ull) == (64 - Histogram::kSubBits + 1) * Histogram::kSub - 1);
}

// Percentiles from the histogram vs exact ones from the sorted samples: within 1/16.
static void test_percentiles_within_resolution() {
    std::mt19937_64 rng(7);
    std::lognormal_distribution<double> lat(5.0, 1.2);  // a latency-like long tail around 150
    Histogram h, a, b;
    std::vector<uint64_t> xs;
    for (int i = 0; i < 200'000; ++i) {
        uint64_t x = uint64_t(lat(rng));
        xs.push_back(x);
        h.record(x);
        (i % 2 ? a : b).record(x);
    }
    std::sort(xs.begin(), xs.end());
    for (double p : {0.5, 0.9, 0.99, 0.999, 1.0}) {
        uint64_t exact = xs[size_t(p * double(xs.size()) + 0.999999) - 1];
        uint64_t got = h.percentile(p);
        assert(got >= exact && double(got - exact) <= double(exact) / 16 + 1);
    }
    a.merge(b);
    assert(a.count() == h.count() && a.max() == h.max() && a.min() == h.min() && a.percentile(0.99) == h.percentile(0.99));
    assert(Histogram().percentile(0.5) == 0);
}

int main() {
    test_buckets_cover_every_value_once();
    test_percentiles_within_resolution();
    std::puts("all tests passed");
}
