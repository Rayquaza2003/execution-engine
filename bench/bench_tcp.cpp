// The floor under the FIX gateway: a bare TCP echo over loopback with the same
// socket settings (TCP_NODELAY, busy poll()), a 150-byte message, one in flight.
// Usage: ./build/bench_tcp [round_trips]   (default 100k)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <thread>

#include "core/histogram.hpp"
#include "net/gateway.hpp"

int main(int argc, char** argv) {
    int n = argc > 1 ? std::atoi(argv[1]) : 100'000;
    int l = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof a;
    if (bind(l, reinterpret_cast<sockaddr*>(&a), sizeof a) || listen(l, 1) ||
        getsockname(l, reinterpret_cast<sockaddr*>(&a), &len)) {
        std::perror("listen");
        return 1;
    }
    int one = 1;
    std::thread echo([&] {
        int c = accept(l, nullptr, nullptr);
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        char b[256];
        for (;;) {
            pollfd p{c, POLLIN, 0};
            while (poll(&p, 1, 0) == 0) {
            }
            ssize_t got = read(c, b, sizeof b);
            if (got <= 0 || write(c, b, size_t(got)) != got) break;
        }
        close(c);
    });
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a)) return 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    char msg[150] = {}, b[256];
    Histogram h;
    for (int i = 0; i < n; ++i) {
        uint64_t t = Gateway::now();
        if (write(fd, msg, sizeof msg) != sizeof msg) return 1;
        for (size_t got = 0; got < sizeof msg;) {
            pollfd p{fd, POLLIN, 0};
            poll(&p, 1, 1000);
            ssize_t r = read(fd, b, sizeof b);
            if (r <= 0) return 1;
            got += size_t(r);
        }
        h.record(Gateway::now() - t);
    }
    close(fd);
    echo.join();
    close(l);
    h.print(stdout, "tcp echo round trip");
}
