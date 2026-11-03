#undef NDEBUG  // keep asserts on in Release builds
#include <cassert>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <thread>

#include "core/logger.hpp"

// Two threads log through their own channels; every line comes out formatted,
// and each thread's lines stay in order.
static void test_two_producers_every_line_formatted() {
    std::FILE* f = std::tmpfile();
    constexpr int kEach = 20'000;
    uint64_t dropped = 0;
    {
        Logger log(f, 2);
        auto work = [&](int who) {
            for (int i = 0; i < kEach; ++i)
                while (!log.channel(who).log("t%d i=%d px=%.2f side=%s", who, i, i * 0.5, i % 2 ? "B" : "S")) {
                }  // retry until the writer catches up: this test wants every line
        };
        std::thread a(work, 0), b(work, 1);
        a.join();
        b.join();
        log.channel(0).log("done");
        dropped = log.channel(0).dropped() + log.channel(1).dropped();
    }  // destructor drains everything
    std::rewind(f);
    char line[256];
    int next[2] = {0, 0}, done = 0;
    unsigned long long ts;
    while (std::fgets(line, sizeof line, f)) {
        int who, i;
        double px;
        char side;
        if (std::sscanf(line, "%llu t%d i=%d px=%lf side=%c", &ts, &who, &i, &px, &side) == 5) {
            assert(i == next[who]++ && px == i * 0.5 && side == (i % 2 ? 'B' : 'S'));
        } else {
            assert(std::strstr(line, " done\n"));
            ++done;
        }
    }
    assert(next[0] == kEach && next[1] == kEach && done == 1);
    std::printf("retried pushes while the writer caught up: %llu\n", (unsigned long long)dropped);
    std::fclose(f);
}

int main() {
    test_two_producers_every_line_formatted();
    std::puts("all tests passed");
}
