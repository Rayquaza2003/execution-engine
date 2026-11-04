// The test bench: open http://127.0.0.1:8080 to trade against the engine.
// Usage: ./build/web_ui [PORT]   (default 8080; listens on loopback only)
//
// One thread: a poll() loop over HTTP connections that also runs the
// background flow between requests. The page (web/index.html, re-read on
// every load so edits show up on refresh) polls GET /api/state and sends
// orders as form-encoded POSTs.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

#include "web/sandbox.hpp"

#ifndef WEB_ROOT
#define WEB_ROOT "web"
#endif

static volatile std::sig_atomic_t g_stop = 0;

static Nanos now_ns() {
    return Nanos(std::chrono::duration_cast<std::chrono::nanoseconds>(
                     std::chrono::system_clock::now().time_since_epoch())
                     .count());
}

static std::string handle(Sandbox& sb, const web::Request& r) {
    Nanos now = now_ns();
    if (r.method == "GET" && (r.path == "/" || r.path == "/index.html")) {
        std::ifstream in(std::string(WEB_ROOT) + "/index.html");
        if (!in) return web::response(404, "text/plain", "web/index.html not found");
        std::string page((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return web::response(200, "text/html; charset=utf-8", page);
    }
    if (r.method == "GET" && r.path == "/api/state") {
        auto q = web::form(r.query);
        std::optional<SymbolId> s = sb.symbol(q.count("sym") ? q["sym"] : "0");
        return web::response(200, "application/json", sb.state(s.value_or(0), now));
    }
    if (r.method != "POST") return web::response(404, "text/plain", "not found");
    auto f = web::form(r.body);
    std::string err;
    if (r.path == "/api/order") err = sb.order(f, now);
    else if (r.path == "/api/cancel") err = sb.cancel(f, now);
    else if (r.path == "/api/replace") err = sb.replace(f, now);
    else if (r.path == "/api/kill") err = sb.kill(now);
    else if (r.path == "/api/reset") sb.reset();
    else if (r.path == "/api/flow") sb.set_flow(std::atof(f["rate"].c_str()));
    else if (r.path == "/api/burst") err = sb.burst(std::atoll(f["n"].c_str()), now);
    else return web::response(404, "text/plain", "not found");
    return web::response(err.empty() ? 200 : 400, "application/json",
                         err.empty() ? "{\"ok\":true}" : "{\"error\":" + web::quote(err) + "}");
}

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT, [](int) { g_stop = 1; });
    uint16_t port = uint16_t(argc > 1 ? std::atoi(argv[1]) : 8080);

    RiskLimits lim;
    lim.max_order_qty = 10'000;
    lim.max_order_notional = 5'000'000.0 * 100;  // $5M, in ticks x shares
    lim.max_position = 20'000;
    lim.max_open_orders = 50;
    lim.collar_bps = 500;                          // 5% from the mid
    lim.max_loss = 50'000.0 * 100;                 // $50k
    Sandbox sb({"AAPL", "MSFT", "SPY"}, {15'000, 41'000, 50'000}, lim);

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(ls, 64) != 0) {
        std::perror("bind/listen");
        return 1;
    }
    fcntl(ls, F_SETFL, fcntl(ls, F_GETFL) | O_NONBLOCK);
    std::printf("test bench on http://127.0.0.1:%u  (Ctrl-C to stop)\n", port);
    std::fflush(stdout);

    struct Conn {
        int fd;
        std::string in;
    };
    std::vector<Conn> conns;
    std::vector<pollfd> fds;
    while (!g_stop) {
        fds.assign(1, {ls, POLLIN, 0});
        for (const Conn& c : conns) fds.push_back({c.fd, POLLIN, 0});
        poll(fds.data(), nfds_t(fds.size()), 2);
        if (fds[0].revents & POLLIN)
            for (int fd; (fd = accept(ls, nullptr, nullptr)) >= 0;) conns.push_back({fd, {}});
        for (size_t i = conns.size(); i-- > 0;) {
            if (i + 1 >= fds.size() || !(fds[i + 1].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            Conn& c = conns[i];
            char buf[16384];
            ssize_t n = read(c.fd, buf, sizeof buf);
            bool done = n <= 0;
            if (n > 0) {
                c.in.append(buf, size_t(n));
                web::Request req;
                size_t used = web::parse(c.in, req);
                if (used == std::string_view::npos) {
                    done = true;
                } else if (used) {
                    std::string out = handle(sb, req);
                    for (size_t at = 0; at < out.size();) {  // small replies: a blocking write is fine
                        ssize_t w = write(c.fd, out.data() + at, out.size() - at);
                        if (w <= 0) break;
                        at += size_t(w);
                    }
                    done = true;
                }
            }
            if (done) {
                close(c.fd);
                conns.erase(conns.begin() + long(i));
            }
        }
        sb.tick(now_ns());
    }
    for (const Conn& c : conns) close(c.fd);
    close(ls);
}
