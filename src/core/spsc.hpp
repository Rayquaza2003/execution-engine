#pragma once
#include <atomic>
#include <cstddef>
#include <memory>

// Lock-free single-producer single-consumer queue: one thread pushes, one
// other thread pops, no locks and no allocation after construction.
//
// The producer owns tail_, the consumer owns head_. Each publishes its index
// with a release store; the other side reads it with an acquire load, which
// also makes the slot contents written before the store visible. Each side
// keeps a cached copy of the other's index and only re-reads the shared one
// when the cache says full (producer) or empty (consumer), so in steady state
// the two cores rarely touch each other's cache line. The indices sit on
// separate cache lines so they never false-share.
template <class T, size_t N>
class Spsc {
    static_assert(N > 0 && (N & (N - 1)) == 0, "N must be a power of two");

public:
    // Apple M1 reports 128-byte lines; x86's adjacent-line prefetcher pairs 64-byte lines too.
    static constexpr size_t kLine = 128;

    Spsc() : buf_(new T[N]) {}

    // Producer only. False if full.
    bool try_push(const T& v) {
        size_t t = tail_.load(std::memory_order_relaxed);
        if (t - head_cache_ == N) {
            head_cache_ = head_.load(std::memory_order_acquire);
            if (t - head_cache_ == N) return false;
        }
        buf_[t & (N - 1)] = v;
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    // Consumer only. False if empty.
    bool try_pop(T& v) {
        size_t h = head_.load(std::memory_order_relaxed);
        if (h == tail_cache_) {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (h == tail_cache_) return false;
        }
        v = buf_[h & (N - 1)];
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    // Exact only when neither side is running.
    size_t size_approx() const {
        return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire);
    }

private:
    alignas(kLine) std::atomic<size_t> tail_{0};  // next slot to write; producer's
    size_t head_cache_ = 0;                       // producer's last view of head_
    alignas(kLine) std::atomic<size_t> head_{0};  // next slot to read; consumer's
    size_t tail_cache_ = 0;                       // consumer's last view of tail_
    alignas(kLine) std::unique_ptr<T[]> buf_;
};
