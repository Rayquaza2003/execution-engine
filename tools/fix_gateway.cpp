// FIX 4.4 order entry gateway in front of the matching engine.
// Usage: ./build/fix_gateway --users FILE [--journal PATH [--fsync]] PORT SYMBOL [SYMBOL...]   (Ctrl-C to stop)
//   --users:   who may log on, one "COMPID PASSWORD" per line (kept out of argv on purpose)
//   --journal: write-ahead journal; if PATH already has records, recover from them first
//   --fsync:   force the journal to disk before every reply (else: to the OS)
// On exit it prints its own latency: order received -> first report written.
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include "net/gateway.hpp"

static std::atomic<bool> g_stop{false};

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);  // a peer hanging up is an error to handle, not a reason to die
    GatewayConfig cfg;
    int a = 1;
    for (; a < argc && argv[a][0] == '-'; ++a) {
        if (!std::strcmp(argv[a], "--journal") && a + 1 < argc) cfg.journal = argv[++a];
        else if (!std::strcmp(argv[a], "--fsync")) cfg.fsync = true;
        else if (!std::strcmp(argv[a], "--users") && a + 1 < argc) {
            std::ifstream in(argv[++a]);
            std::string comp, pw;
            while (in >> comp >> pw) cfg.users[comp] = pw;
        }
    }
    if (argc - a < 2 || cfg.users.empty()) {
        std::fprintf(stderr, "usage: %s --users FILE [--journal PATH [--fsync]] PORT SYMBOL [SYMBOL...]\n", argv[0]);
        return 2;
    }
    cfg.port = uint16_t(std::atoi(argv[a]));
    std::signal(SIGINT, [](int) { g_stop = true; });
    std::signal(SIGTERM, [](int) { g_stop = true; });
    auto t0 = std::chrono::steady_clock::now();
    std::string journal = cfg.journal;
    Gateway gw(std::vector<std::string>(argv + a + 1, argv + argc), std::move(cfg));
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (gw.orders())
        std::fprintf(stderr, "recovered %llu orders from %s in %.1f ms (book %016llx, reports %016llx)\n",
                     (unsigned long long)gw.orders(), journal.c_str(), ms, (unsigned long long)gw.book_digest(),
                     (unsigned long long)gw.checksum());
    std::fprintf(stderr, "listening on 127.0.0.1:%u\n", gw.port());
    gw.run(g_stop);
    std::printf("%llu orders (book %016llx, reports %016llx)\n", (unsigned long long)gw.orders(),
                (unsigned long long)gw.book_digest(), (unsigned long long)gw.checksum());
    gw.in_to_out().print(stdout, "gateway in->out");
    if (gw.auth_failures()) std::printf("%llu logons refused\n", (unsigned long long)gw.auth_failures());
}
