#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

// Fixed-capacity history: keeps the newest N items, overwriting the oldest.
// No allocation after construction. Single-threaded (the S9 SPSC queue is a
// different structure).
template <class T, size_t N>
class Ring {
    static_assert(N > 0 && (N & (N - 1)) == 0, "N must be a power of two");

public:
    void push(const T& v) { buf_[head_++ & (N - 1)] = v; }

    size_t size() const { return head_ < N ? size_t(head_) : N; }
    uint64_t total_pushed() const { return head_; }

    // i = 0 is the oldest item still kept, size() - 1 the newest.
    const T& operator[](size_t i) const { return buf_[(head_ - size() + i) & (N - 1)]; }
    const T& newest() const { return buf_[(head_ - 1) & (N - 1)]; }

private:
    std::array<T, N> buf_{};
    uint64_t head_ = 0;
};
