#undef NDEBUG  // keep asserts on in Release builds
#include <cassert>
#include <cstdio>

#include "core/ring.hpp"

static void test_fills_then_overwrites_oldest() {
    Ring<int, 4> r;
    assert(r.size() == 0);
    for (int i = 1; i <= 3; ++i) r.push(i);
    assert(r.size() == 3 && r[0] == 1 && r[2] == 3 && r.newest() == 3);

    for (int i = 4; i <= 10; ++i) r.push(i);
    assert(r.size() == 4 && r.total_pushed() == 10);
    assert(r[0] == 7 && r[1] == 8 && r[2] == 9 && r[3] == 10 && r.newest() == 10);
}

int main() {
    test_fills_then_overwrites_oldest();
    std::puts("all tests passed");
}
