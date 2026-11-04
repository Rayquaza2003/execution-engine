// What durability costs per journal record (64-byte payload).
// Usage: ./build/bench_journal [DIR]   (default /tmp)
//   buffered: append only, bytes stay in memory
//   flush:    write(2) after every record or batch: survives the process dying
//   sync:     F_FULLFSYNC (macOS) after every record: survives power loss
#include <chrono>
#include <cstdio>
#include <string>
#include <unistd.h>

#include "journal/journal.hpp"

static double ns_per(const std::string& path, int n, int every, bool sync) {
    unlink(path.c_str());
    Journal j(path);
    char payload[64] = {};
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) {
        j.append(1, Nanos(i), std::string_view(payload, sizeof payload));
        if (every && (i + 1) % every == 0) sync ? j.sync() : j.flush();
    }
    double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
    unlink(path.c_str());
    return ns / n;
}

int main(int argc, char** argv) {
    std::string path = std::string(argc > 1 ? argv[1] : "/tmp") + "/bench_journal.bin";
    std::printf("buffered append        %9.1f ns/record\n", ns_per(path, 2'000'000, 0, false));
    std::printf("flush every 64         %9.1f ns/record\n", ns_per(path, 2'000'000, 64, false));
    std::printf("flush every record     %9.1f ns/record\n", ns_per(path, 200'000, 1, false));
    std::printf("full sync every record %9.1f ns/record\n", ns_per(path, 500, 1, true));
    std::printf("full sync every 64     %9.1f ns/record\n", ns_per(path, 64 * 200, 64, true));
}
