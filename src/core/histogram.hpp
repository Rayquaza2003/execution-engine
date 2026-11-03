#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>

// Log-linear latency histogram, HdrHistogram style: each power of two is cut
// into 16 equal buckets, so any recorded value is known to within 1/16
// (6.25%) of itself, from 1 ns to the full uint64 range, in a fixed 8 KB.
// record() is a few instructions and never allocates.
class Histogram {
public:
    static constexpr int kSubBits = 4;
    static constexpr uint64_t kSub = 1 << kSubBits;

    void record(uint64_t v) {
        ++counts_[index(v)];
        ++n_;
        max_ = std::max(max_, v);
        min_ = std::min(min_, v);
    }

    void merge(const Histogram& o) {
        for (size_t i = 0; i < counts_.size(); ++i) counts_[i] += o.counts_[i];
        n_ += o.n_;
        max_ = std::max(max_, o.max_);
        min_ = std::min(min_, o.min_);
    }

    uint64_t count() const { return n_; }
    uint64_t max() const { return n_ ? max_ : 0; }
    uint64_t min() const { return n_ ? min_ : 0; }

    // Smallest value v such that at least p (0..1) of the samples are <= v,
    // reported as the top of v's bucket (never above the true max).
    uint64_t percentile(double p) const {
        if (!n_) return 0;
        uint64_t rank = std::max<uint64_t>(1, uint64_t(p * double(n_) + 0.999999));
        uint64_t seen = 0;
        for (size_t i = 0; i < counts_.size(); ++i)
            if ((seen += counts_[i]) >= rank) return std::min(top(i), max_);
        return max_;
    }

    // One line: count and p50 / p90 / p99 / p99.9 / max.
    void print(std::FILE* f, const char* name, const char* unit = "ns") const {
        std::fprintf(f, "%-22s n=%-9llu p50 %6llu  p90 %6llu  p99 %6llu  p99.9 %7llu  max %8llu %s\n", name,
                     (unsigned long long)n_, (unsigned long long)percentile(0.5),
                     (unsigned long long)percentile(0.9), (unsigned long long)percentile(0.99),
                     (unsigned long long)percentile(0.999), (unsigned long long)max(), unit);
    }

    // Bucket math, public for the test.
    static size_t index(uint64_t v) {
        if (v < kSub) return size_t(v);
        int e = std::bit_width(v) - 1;  // v in [2^e, 2^(e+1)), e >= kSubBits
        return size_t(e - kSubBits + 1) * kSub + size_t((v >> (e - kSubBits)) & (kSub - 1));
    }
    static uint64_t top(size_t i) {  // largest value in bucket i
        if (i < kSub) return i;
        int e = int(i / kSub) + kSubBits - 1;
        uint64_t width = uint64_t(1) << (e - kSubBits);
        return (uint64_t(1) << e) + (i % kSub) * width + (width - 1);
    }

private:
    std::array<uint64_t, (64 - kSubBits + 1) * kSub> counts_{};
    uint64_t n_ = 0, max_ = 0, min_ = UINT64_MAX;
};
