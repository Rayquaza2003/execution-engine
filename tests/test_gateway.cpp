#undef NDEBUG  // keep asserts on in Release builds
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <thread>

#include "net/gateway.hpp"

using Fields = std::map<uint32_t, std::string>;

// A blocking test client: real socket, real Session.
struct Client {
    int fd;
    Session s;
    std::vector<Fields> got;  // application messages received, in order

    Client(uint16_t port, const char* name) : s({name, "EXCH", 30'000'000'000, true}) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(port);
        assert(connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        timeval tv{0, 20'000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        s.logon(Gateway::now());
        assert(wait([&] { return s.state() == Session::State::Active; }));
    }
    ~Client() { close(fd); }

    // Sends what the session has queued, then reads until done() or 2 s pass.
    template <class P>
    bool wait(P done) {
        for (int i = 0; i < 100; ++i) {
            std::string& out = s.output();
            while (!out.empty()) {
                ssize_t n = write(fd, out.data(), out.size());
                assert(n > 0);
                out.erase(0, size_t(n));
            }
            if (done()) return true;
            char buf[4096];
            ssize_t n = read(fd, buf, sizeof buf);
            if (n > 0)
                s.on_bytes(buf, size_t(n), Gateway::now(), [&](const fix::Message& m) {
                    Fields f;
                    f[fix::MsgType] = std::string(m.type());
                    for (uint32_t t : {11u, 14u, 17u, 31u, 32u, 37u, 39u, 41u, 58u, 150u, 151u, 434u})
                        if (auto v = m.get(t)) f[t] = std::string(*v);
                    got.push_back(f);
                });
        }
        return done();
    }
    // The next reply that matches, waiting for it if needed.
    Fields expect(const std::string& cl) {
        size_t seen = 0;
        bool ok = wait([&] {
            for (; seen < got.size(); ++seen)
                if (got[seen][fix::ClOrdID] == cl) return true;
            return false;
        });
        assert(ok);
        Fields f = got[seen];
        got.erase(got.begin() + long(seen));
        return f;
    }
    void order(const char* cl, char side, const char* qty, const char* px, const char* sym = "AAPL") {
        s.send("D", Gateway::now(), [&](fix::Writer& w) {
            w.field(fix::ClOrdID, cl).field(fix::Symbol, sym).field(fix::Side, side).field(fix::OrderQty, qty);
            w.field(fix::OrdType, '2').field(fix::Price, px);
        });
    }
    void cancel(const char* cl, const char* orig) {
        s.send("F", Gateway::now(), [&](fix::Writer& w) {
            w.field(fix::ClOrdID, cl).field(fix::OrigClOrdID, orig).field(fix::Symbol, "AAPL").field(fix::Side, '1');
        });
    }
};

static void test_orders_over_tcp() {
    Gateway gw({"AAPL", "MSFT"});
    std::atomic<bool> stop{false};
    std::thread server([&] { gw.run(stop); });
    {
        Client buyer(gw.port(), "BUYER"), seller(gw.port(), "SELLER");

        buyer.order("b1", '1', "100", "10.00");
        Fields f = buyer.expect("b1");
        assert(f[fix::MsgType] == "8" && f[fix::ExecType] == "0" && f[fix::OrdStatus] == "0" && f[fix::LeavesQty] == "100");

        seller.order("s1", '2', "60", "10.00");
        assert(seller.expect("s1")[fix::ExecType] == "0");
        f = seller.expect("s1");
        assert(f[fix::ExecType] == "F" && f[fix::OrdStatus] == "2" && f[fix::LastQty] == "60" && f[fix::LastPx] == "10.00");
        f = buyer.expect("b1");
        assert(f[fix::ExecType] == "F" && f[fix::OrdStatus] == "1" && f[fix::CumQty] == "60" && f[fix::LeavesQty] == "40");

        // Replace: FIX OrderQty is the total, so 100 with 60 done leaves 40.
        buyer.s.send("G", Gateway::now(), [](fix::Writer& w) {
            w.field(fix::ClOrdID, "b2").field(fix::OrigClOrdID, "b1").field(fix::Symbol, "AAPL").field(fix::Side, '1');
            w.field(fix::OrderQty, "100").field(fix::OrdType, '2').field(fix::Price, "10.01");
        });
        f = buyer.expect("b2");
        assert(f[fix::ExecType] == "5" && f[fix::OrigClOrdID] == "b1" && f[fix::LeavesQty] == "40" && f[fix::OrdStatus] == "1");

        buyer.cancel("b3", "b2");
        f = buyer.expect("b3");
        assert(f[fix::ExecType] == "4" && f[fix::OrdStatus] == "4" && f[fix::OrigClOrdID] == "b2" && f[fix::CumQty] == "60");

        buyer.cancel("b4", "b3");  // already cancelled: the venue refuses
        f = buyer.expect("b4");
        assert(f[fix::MsgType] == "9" && f[fix::CxlRejResponseTo] == "1" && f[fix::OrdStatus] == "4");
        assert(f[fix::Text] == "order already done");

        buyer.order("b5", '1', "10", "10.00", "TSLA");
        f = buyer.expect("b5");
        assert(f[fix::ExecType] == "8" && f[fix::Text] == "unknown symbol");
        buyer.order("b1", '1', "10", "10.00");
        assert(buyer.expect("b1")[fix::Text] == "duplicate ClOrdID");
        seller.cancel("s2", "nope");
        f = seller.expect("s2");
        assert(f[fix::MsgType] == "9" && f[fix::Text] == "unknown order");

        buyer.s.logout(Gateway::now());
        assert(buyer.wait([&] { return buyer.s.state() == Session::State::LoggedOut; }));
    }
    stop = true;
    server.join();
    assert(gw.orders() == 2 && gw.in_to_out().count() == 2);
}

int main() {
    test_orders_over_tcp();
    std::puts("all tests passed");
}
