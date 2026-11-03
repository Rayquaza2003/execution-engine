// FIX load generator: one session against fix_gateway, measuring wire to wire
// latency: from write() of a request to read() of the first reply to it.
// Usage: ./build/fix_client PORT SYMBOL COUNT [WINDOW]
//   WINDOW = requests in flight at once (default 1: pure round trips)
// Mix: 70% new limit orders (100 shares, random side, within 5 cents of
// $100.00, so many cross and fill), 30% cancels of a random live order.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/histogram.hpp"
#include "fix/session.hpp"
#include "net/gateway.hpp"

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s PORT SYMBOL COUNT [WINDOW]\n", argv[0]);
        return 2;
    }
    uint16_t port = uint16_t(std::atoi(argv[1]));
    std::string sym = argv[2];
    size_t count = std::strtoull(argv[3], nullptr, 10), window = argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 1;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        std::perror("connect");
        return 1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    Session s({"LOADGEN", "EXCH", 30'000'000'000, true});
    std::vector<uint64_t> sent_at(count + 1, 0);  // by ClOrdID; 0 once answered
    std::unordered_set<uint64_t> live;             // resting orders we may cancel
    std::vector<uint64_t> live_list;
    Histogram wire;
    std::mt19937_64 rng(7);
    size_t sent = 0, answered = 0, fills = 0, rejects = 0;
    char buf[1 << 16];

    auto write_all = [&] {
        std::string& out = s.output();
        while (!out.empty()) {
            ssize_t n = write(fd, out.data(), out.size());
            if (n <= 0) return false;
            out.erase(0, size_t(n));
        }
        return true;
    };
    auto read_some = [&](int timeout_ms) {
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, timeout_ms) <= 0) return true;
        ssize_t n = read(fd, buf, sizeof buf);
        if (n <= 0) return false;
        uint64_t t = Gateway::now();
        s.on_bytes(buf, size_t(n), t, [&](const fix::Message& m) {
            uint64_t cl = uint64_t(fix::to_int(m.get(fix::ClOrdID).value_or("0")).value_or(0));
            if (cl && cl <= count && sent_at[cl]) {
                wire.record(t - sent_at[cl]);
                sent_at[cl] = 0;
                ++answered;
            }
            std::string_view st = m.get(fix::OrdStatus).value_or("");
            fills += m.get(fix::ExecType) == "F";
            rejects += m.type() == "9" || m.get(fix::ExecType) == "8";
            uint64_t order = m.get(fix::OrigClOrdID) ? uint64_t(fix::to_int(*m.get(fix::OrigClOrdID)).value_or(0)) : cl;
            if (st == "0" || st == "1") live.insert(order);
            else live.erase(order);
        });
        return true;
    };

    s.logon(Gateway::now());
    write_all();
    while (s.state() != Session::State::Active)
        if (!read_some(1000)) return 1;

    uint64_t t0 = Gateway::now();
    while (answered < count) {
        while (sent < count && sent - answered < window) {
            uint64_t cl = ++sent;
            bool cancel = !live.empty() && rng() % 10 < 3;
            if (cancel) {
                if (live_list.size() != live.size()) live_list.assign(live.begin(), live.end());
                uint64_t target = live_list[rng() % live_list.size()];
                live.erase(target);
                live_list.clear();
                s.send("F", Gateway::now(), [&](fix::Writer& w) {
                    w.field(fix::ClOrdID, cl).field(fix::OrigClOrdID, target).field(fix::Symbol, sym).field(fix::Side, '1');
                });
            } else {
                Price px = 10'000 + Price(rng() % 11) - 5;
                s.send("D", Gateway::now(), [&](fix::Writer& w) {
                    w.field(fix::ClOrdID, cl).field(fix::Symbol, sym).field(fix::Side, rng() % 2 ? '1' : '2');
                    w.field(fix::OrderQty, 100).field(fix::OrdType, '2').price(fix::Price, px);
                });
            }
            if (!write_all()) return 1;
            sent_at[cl] = Gateway::now();
        }
        if (!read_some(1000)) return 1;
    }
    double secs = double(Gateway::now() - t0) / 1e9;
    s.logout(Gateway::now());
    write_all();

    std::printf("%zu requests, window %zu: %.3f s, %.0f requests/s, %zu fills, %zu rejects\n", count, window, secs,
                double(count) / secs, fills, rejects);
    wire.print(stdout, "wire to wire");
    close(fd);
}
