// FIX 4.4 order entry gateway in front of the matching engine.
// Usage: ./build/fix_gateway PORT SYMBOL [SYMBOL...]   (Ctrl-C to stop)
// On exit it prints its own latency: order received -> first report written.
#include <csignal>
#include <cstdio>
#include <cstdlib>

#include "net/gateway.hpp"

static std::atomic<bool> g_stop{false};

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s PORT SYMBOL [SYMBOL...]\n", argv[0]);
        return 2;
    }
    std::signal(SIGINT, [](int) { g_stop = true; });
    std::signal(SIGTERM, [](int) { g_stop = true; });
    Gateway gw(std::vector<std::string>(argv + 2, argv + argc), uint16_t(std::atoi(argv[1])));
    std::fprintf(stderr, "listening on 127.0.0.1:%u\n", gw.port());
    gw.run(g_stop);
    std::printf("%llu orders\n", (unsigned long long)gw.orders());
    gw.in_to_out().print(stdout, "gateway in->out");
}
