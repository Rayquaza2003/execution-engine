#undef NDEBUG  // keep asserts on in Release builds
#include <cassert>
#include <cstdio>
#include <random>
#include <unordered_map>

#include "core/id_index.hpp"

// Random insert/take/find against std::unordered_map; small start forces growth
// and wraparound, narrow key range forces long probe runs and backward shifts.
static void test_id_index_matches_reference() {
    IdIndex idx(1);
    std::unordered_map<OrderId, uint32_t> ref;
    std::mt19937_64 rng(7);
    for (uint32_t step = 0; step < 200'000; ++step) {
        OrderId key = rng() % 512;  // includes key 0
        int op = int(rng() % 3);
        auto it = ref.find(key);
        if (op == 0 && it == ref.end()) {
            idx.insert(key, step);
            ref[key] = step;
        } else if (op == 1) {
            uint32_t want = it == ref.end() ? IdIndex::kMissing : it->second;
            assert(idx.take(key) == want);
            if (it != ref.end()) ref.erase(it);
        } else {
            assert(idx.find(key) == (it == ref.end() ? IdIndex::kMissing : it->second));
        }
        assert(idx.size() == ref.size());
    }
    for (auto [k, v] : ref) assert(idx.find(k) == v);
}

int main() {
    test_id_index_matches_reference();
    std::puts("all tests passed");
}
