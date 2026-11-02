#pragma once
#include <bit>
#include <cstdint>
#include <limits>
#include <vector>

#include "core/types.hpp"

// OrderId -> uint32 handle (e.g. pool index). Open addressing, linear probing, load factor <= 1/2,
// backward-shift deletion (no tombstones), so insert/erase never allocate
// unless the table has to double.
class IdIndex {
public:
    static constexpr uint32_t kMissing = std::numeric_limits<uint32_t>::max();

    explicit IdIndex(size_t expected) {
        size_t cap = 16;
        while (cap < 2 * expected) cap *= 2;
        slots_.assign(cap, {0, kMissing});
    }

    size_t size() const { return size_; }

    uint32_t find(OrderId key) const {
        for (size_t i = home(key);; i = (i + 1) & mask()) {
            if (slots_[i].val == kMissing) return kMissing;
            if (slots_[i].key == key) return slots_[i].val;
        }
    }

    // Key must be absent.
    void insert(OrderId key, uint32_t val) {
        if (2 * (size_ + 1) > slots_.size()) grow();
        size_t i = home(key);
        while (slots_[i].val != kMissing) i = (i + 1) & mask();
        slots_[i] = {key, val};
        ++size_;
    }

    // Removes key and returns its value, or kMissing if absent.
    uint32_t take(OrderId key) {
        size_t i = home(key);
        while (slots_[i].key != key || slots_[i].val == kMissing) {
            if (slots_[i].val == kMissing) return kMissing;
            i = (i + 1) & mask();
        }
        uint32_t val = slots_[i].val;
        // Pull later entries of the probe run back so lookups never hit a hole.
        for (size_t j = (i + 1) & mask(); slots_[j].val != kMissing; j = (j + 1) & mask()) {
            size_t h = home(slots_[j].key);
            if (((j - h) & mask()) >= ((j - i) & mask())) {
                slots_[i] = slots_[j];
                i = j;
            }
        }
        slots_[i].val = kMissing;
        --size_;
        return val;
    }

private:
    struct Slot {
        OrderId key;
        uint32_t val;  // kMissing marks an empty slot
    };

    size_t mask() const { return slots_.size() - 1; }

    // Fibonacci hashing: spreads sequential and strided ids across the table.
    size_t home(OrderId key) const {
        return size_t((key * 0x9E3779B97F4A7C15ull) >> (64 - std::countr_zero(slots_.size())));
    }

    void grow() {  // ponytail: doubling rehash is a latency spike; size `expected` up front
        std::vector<Slot> old;
        old.swap(slots_);
        slots_.assign(old.size() * 2, {0, kMissing});
        size_ = 0;
        for (const Slot& s : old)
            if (s.val != kMissing) insert(s.key, s.val);
    }

    std::vector<Slot> slots_;
    size_t size_ = 0;
};
