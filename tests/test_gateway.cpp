#undef NDEBUG  // keep asserts on in Release builds
#include <cassert>
#include <csignal>
#include <cstdio>
#include <map>
#include <string>
#include <thread>
#include <tuple>

#include "net/gateway.hpp"
#include "oms/oms.hpp"

using Fields = std::map<uint32_t, std::string>;

// Every test client logs on as NAME with password "pw-NAME".
static GatewayConfig config(std::string journal = "", size_t max_held = 10'000) {
    GatewayConfig c;
    for (const char* who : {"BUYER", "SELLER", "MALLORY"}) c.users[who] = std::string("pw-") + who;
    c.journal = std::move(journal);
    c.max_held = max_held;
    return c;
}

// A blocking test client: real socket, real Session.
struct Client {
    int fd = -1;
    bool closed = false;  // the gateway hung up
    Session s;
    std::vector<Fields> got;  // application messages received, in order

    Client(uint16_t port, const char* name, std::string password = "")
        : s({name, "EXCH", 30'000'000'000, true, password.empty() ? "pw-" + std::string(name) : password}) {
        connect_to(port, false);
    }
    ~Client() { drop(); }
    void drop() {  // the connection dies; the session stays
        if (fd >= 0) close(fd);
        fd = -1;
    }

    // (Re)connect and log on; the session keeps its sequence numbers unless reset.
    void connect_to(uint16_t port, bool reset) {
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
        s.logon(Gateway::now(), reset);
        assert(try_logon(reset));
    }
    // Logon already sent by connect: true once active, false if the gateway hangs up.
    bool try_logon(bool) {
        return wait([&] { return s.state() == Session::State::Active || closed; }) && s.state() == Session::State::Active;
    }
    void reconnect(uint16_t port, bool reset) {
        drop();
        s.reconnect();
        connect_to(port, reset);
    }
    // OrderMassStatusRequest: every order's status, until LastRptRequested=Y.
    std::vector<Fields> mass_status(const char* id) {
        s.send("AF", Gateway::now(), [&](fix::Writer& w) {
            w.field(fix::MassStatusReqID, id).field(fix::MassStatusReqType, 7);
        });
        assert(wait([&] {
            for (const Fields& f : got)
                if (f.count(fix::LastRptRequested)) return true;
            return false;
        }));
        std::vector<Fields> v;
        for (const Fields& f : got)
            if (f.count(fix::MassStatusReqID) && f.at(fix::MassStatusReqID) == id) v.push_back(f);
        got.clear();
        return v;
    }

    // Sends what the session has queued, then reads until done() or 2 s pass.
    template <class P>
    bool wait(P done, double seconds = 2) {
        Nanos deadline = Gateway::now() + Nanos(seconds * 1e9);
        while (Gateway::now() < deadline) {
            std::string& out = s.output();
            while (!out.empty()) {
                ssize_t n = write(fd, out.data(), out.size());
                if (n <= 0) return false;  // the gateway closed the connection
                out.erase(0, size_t(n));
            }
            if (done()) return true;
            char buf[1 << 16];
            ssize_t n = read(fd, buf, sizeof buf);
            if (n == 0) closed = true;
            if (n == 0) return done();
            if (n > 0)
                s.on_bytes(buf, size_t(n), Gateway::now(), [&](const fix::Message& m) {
                    Fields f;
                    f[fix::MsgType] = std::string(m.type());
                    for (uint32_t t : {6u, 11u, 14u, 17u, 31u, 32u, 37u, 39u, 41u, 58u, 150u, 151u, 434u, 584u, 911u, 912u})
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
    Gateway gw({"AAPL", "MSFT"}, config());
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

static std::string temp_journal() {
    char p[] = "/tmp/gateway_journal_XXXXXX";
    close(mkstemp(p));
    unlink(p);
    return p;
}

// The gateway dies with orders resting. A new one comes back from the
// journal: same books, same matching-engine output checksum, same orders and
// ClOrdIDs, same FIX sequence numbers, and a torn record at the end of the
// journal is ignored. The buyer reconnects without a reset and carries on.
static void test_crash_and_recover_from_journal() {
    std::string path = temp_journal();
    uint64_t sum, digest, last_exec;
    std::unique_ptr<Client> buyer;
    {
        Gateway gw({"AAPL"}, config(path));
        std::atomic<bool> stop{false};
        std::thread server([&] { gw.run(stop); });
        buyer = std::make_unique<Client>(gw.port(), "BUYER");
        Client seller(gw.port(), "SELLER");
        buyer->order("b1", '1', "100", "10.00");
        buyer->expect("b1");
        seller.order("s1", '2', "60", "10.00");
        seller.expect("s1");
        seller.expect("s1");
        buyer->expect("b1");
        buyer->order("b2", '1', "50", "9.99");
        last_exec = std::stoull(buyer->expect("b2")[fix::ExecID]);
        stop = true;
        server.join();
        sum = gw.checksum();
        digest = gw.book_digest();
    }  // gone: sockets closed, nothing logged out
    FILE* f = std::fopen(path.c_str(), "ab");
    std::fwrite("\x40\x00\x00\x00\x02", 1, 5, f);  // a record cut short mid-write
    std::fclose(f);

    Gateway gw({"AAPL"}, config(path));
    assert(gw.checksum() == sum && gw.book_digest() == digest && gw.orders() == 3);
    std::atomic<bool> stop{false};
    std::thread server([&] { gw.run(stop); });
    buyer->reconnect(gw.port(), false);  // sequence numbers carry on: no "too low", no reset
    assert(buyer->s.gaps() == 0);

    buyer->cancel("b3", "b1");  // the order and its ClOrdID survived
    Fields c = buyer->expect("b3");
    assert(c[fix::ExecType] == "4" && c[fix::CumQty] == "60" && std::stoull(c[fix::ExecID]) > last_exec);
    buyer->order("b2", '1', "10", "9.99");
    assert(buyer->expect("b2")[fix::Text] == "duplicate ClOrdID");

    std::vector<Fields> st = buyer->mass_status("m1");
    assert(st.size() == 2 && st[0][fix::ClOrdID] == "b3" && st[0][fix::OrdStatus] == "4" && st[0][fix::CumQty] == "60");
    assert(st[1][fix::ClOrdID] == "b2" && st[1][fix::OrdStatus] == "0" && st[1][fix::LeavesQty] == "50");
    assert(st[1][fix::TotNumReports] == "2" && st[1][fix::LastRptRequested] == "Y" && !st[0].count(fix::LastRptRequested));
    buyer.reset();
    stop = true;
    server.join();
    unlink(path.c_str());
}

// The seller's OMS loses its connection with two orders resting; both trade
// while it is away. It comes back with a sequence reset, so those execution
// reports are gone for good, and rebuilds the truth from a mass status request.
static void test_oms_reconciles_after_reset() {
    Gateway gw({"AAPL"}, config());
    std::atomic<bool> stop{false};
    std::thread server([&] { gw.run(stop); });
    Risk risk(1, RiskLimits{});
    risk.mark(0, 1000);
    Oms oms(1, 1000, &risk);
    Client seller(gw.port(), "SELLER"), buyer(gw.port(), "BUYER");
    std::vector<VenueRequest> reqs;
    for (ClOrdId cl : {1, 2}) {
        Price px = cl == 1 ? 1000 : 1001;
        oms.submit({cl, 0, Side::Sell, OrdType::Limit, Tif::Day, px, 100}, reqs);
        seller.s.send("D", Gateway::now(), [&](fix::Writer& w) {
            w.field(fix::ClOrdID, cl).field(fix::Symbol, "AAPL").field(fix::Side, '2').field(fix::OrderQty, 100);
            w.field(fix::OrdType, '2').price(fix::Price, px);
        });
        assert(seller.expect(std::to_string(cl))[fix::ExecType] == "0");
        oms.on_report({oms.find(cl)->id, 0, ExecType::New, Reject::None, 0, 0, 100});
    }
    seller.drop();  // connection lost
    buyer.order("x", '1', "150", "10.01");  // takes 100 at 10.00 and 50 at 10.01
    buyer.expect("x");
    seller.reconnect(gw.port(), true);
    assert(seller.s.next_in() == 2);  // the fills sent while away are not coming

    std::vector<Oms::VenueStatus> venue;
    for (Fields& f : seller.mass_status("r1")) {
        char st = f[fix::OrdStatus][0];
        OrdState s = st == '1' ? OrdState::PartiallyFilled : st == '2' ? OrdState::Filled : st == '4' ? OrdState::Cancelled
                     : st == '8' ? OrdState::Rejected : OrdState::Acknowledged;
        venue.push_back({std::stoull(f[fix::ClOrdID]), s, std::stoull(f[fix::CumQty]),
                         double(*fix::to_price(f[fix::AvgPx])), std::stoull(f[fix::LeavesQty])});
    }
    assert(oms.reconcile(venue) == 2);
    assert(oms.find(1)->state == OrdState::Filled && oms.find(1)->cum == 100 && oms.find(1)->avg_px() == 1000);
    assert(oms.find(2)->state == OrdState::PartiallyFilled && oms.find(2)->cum == 50 && oms.find(2)->leaves == 50);
    assert(risk.position(0) == -150 && risk.open_qty(0, Side::Sell) == 50);
    stop = true;
    server.join();
}

// A logon attempt on a fresh socket; true if the gateway lets it in.
static bool logs_on(uint16_t port, const char* comp, const char* password) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    assert(connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
    timeval tv{1, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    Session s({comp, "EXCH", 30'000'000'000, true, password});
    s.logon(Gateway::now());
    assert(write(fd, s.output().data(), s.output().size()) > 0);
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof buf);
    if (n > 0) s.on_bytes(buf, size_t(n), Gateway::now(), [](const fix::Message&) {});
    close(fd);
    return s.state() == Session::State::Active;
}

// No credentials, no session: a wrong or missing password, an unknown CompID,
// or another client's valid password are all refused before any session
// attaches. That also stops a takeover: a disconnected client's session (and
// its resting orders) can only be resumed by that client.
static void test_logon_needs_credentials_and_no_takeover() {
    Gateway gw({"AAPL"}, config());
    std::atomic<bool> stop{false};
    std::thread server([&] { gw.run(stop); });
    {
        Client buyer(gw.port(), "BUYER");
        buyer.order("b1", '1', "100", "10.00");
        assert(buyer.expect("b1")[fix::ExecType] == "0");
        assert(!logs_on(gw.port(), "BUYER", "pw-BUYER"));  // right password, but BUYER is already connected
        buyer.drop();

        assert(!logs_on(gw.port(), "BUYER", "wrong"));
        assert(!logs_on(gw.port(), "BUYER", ""));            // no Username/Password at all
        assert(!logs_on(gw.port(), "BUYER", "pw-MALLORY"));  // someone else's valid password
        assert(!logs_on(gw.port(), "NOBODY", "pw-NOBODY"));  // not a user

        buyer.reconnect(gw.port(), false);  // the real BUYER resumes: order still there
        buyer.cancel("b2", "b1");
        assert(buyer.expect("b2")[fix::ExecType] == "4");
    }
    stop = true;
    server.join();
    assert(gw.auth_failures() == 4);
}

// Hard per-order limits at the door: quantity in (0, 10M], limit price in
// (0, $1M], on new orders and on replaces (where -1 used to become 2^64 - 1).
static void test_order_limits() {
    Gateway gw({"AAPL"}, config());
    std::atomic<bool> stop{false};
    std::thread server([&] { gw.run(stop); });
    {
        Client buyer(gw.port(), "BUYER");
        int n = 0;
        auto rejected = [&](const char* qty, const char* px, const char* why) {
            std::string cl = "x" + std::to_string(++n);
            buyer.order(cl.c_str(), '1', qty, px);
            Fields f = buyer.expect(cl);
            return f[fix::ExecType] == "8" && f[fix::Text] == why;
        };
        assert(rejected("-1", "10.00", "bad OrderQty") && rejected("0", "10.00", "bad OrderQty"));
        assert(rejected("10000001", "10.00", "bad OrderQty") && rejected("18446744073709551615", "10.00", "bad OrderQty"));
        assert(rejected("100", "-5.00", "bad Price") && rejected("100", "0", "bad Price"));
        assert(rejected("100", "1000000.01", "bad Price") && rejected("100", "99999999999999999.00", "bad Price"));

        buyer.order("b1", '1', "100", "10.00");
        assert(buyer.expect("b1")[fix::ExecType] == "0");
        for (auto [cl, qty, px] : {std::tuple{"r1", "-1", "10.00"}, {"r2", "100", "-5.00"}, {"r3", "0", "10.00"}}) {
            buyer.s.send("G", Gateway::now(), [&](fix::Writer& w) {
                w.field(fix::ClOrdID, cl).field(fix::OrigClOrdID, "b1").field(fix::Symbol, "AAPL").field(fix::Side, '1');
                w.field(fix::OrderQty, qty).field(fix::OrdType, '2').field(fix::Price, px);
            });
            Fields f = buyer.expect(cl);
            assert(f[fix::MsgType] == "9" && f[fix::Text] == "bad Price or OrderQty");
        }
        std::vector<Fields> st = buyer.mass_status("m");
        assert(st.size() == 1 && st[0][fix::ClOrdID] == "b1" && st[0][fix::LeavesQty] == "100");
    }
    stop = true;
    server.join();
}

// A client that floods messages beyond a sequence gap is logged out and cut
// off once it passes the hold limit, instead of growing memory without end;
// everyone else carries on.
static void test_flood_beyond_gap_is_cut_off() {
    Gateway gw({"AAPL"}, config("", 100));
    std::atomic<bool> stop{false};
    std::thread server([&] { gw.run(stop); });
    {
        Client mallory(gw.port(), "MALLORY");
        mallory.order("lost", '1', "1", "1.00");
        mallory.s.output().clear();  // never sent: everything after it is beyond a gap
        for (int i = 0; i < 101; ++i) mallory.order(("f" + std::to_string(i)).c_str(), '1', "1", "1.00");
        mallory.wait([&] { return mallory.closed; });
        assert(mallory.closed);
        Client seller(gw.port(), "SELLER");
        seller.order("s1", '2', "10", "10.00");
        assert(seller.expect("s1")[fix::ExecType] == "0");
    }
    stop = true;
    server.join();
    assert(gw.orders() == 1);
}

// The deadlock: a gap fill releasing more orders than both 64k queues hold
// used to leave the poll thread waiting for room toward the matcher while the
// matcher waited for room back. The poll thread now empties the way back
// while it waits.
static void test_big_gap_fill_does_not_deadlock() {
    constexpr int kHeld = 140'000;  // > 65,536 + 65,536
    Gateway gw({"AAPL"}, config("", 200'000));
    std::atomic<bool> stop{false};
    std::thread server([&] { gw.run(stop); });
    {
        Client buyer(gw.port(), "BUYER");
        buyer.order("gap", '1', "1", "1.00");
        buyer.s.output().clear();  // lost: the next 140k are held until it is resent
        for (int i = 0; i < kHeld; ++i) buyer.order(("h" + std::to_string(i)).c_str(), '1', "1", "1.00");
        buyer.order("last", '1', "1", "1.00");
        Nanos t0 = Gateway::now();
        bool ok = buyer.wait([&] {
            for (const Fields& f : buyer.got)
                if (f.at(fix::ClOrdID) == "last") return true;
            buyer.got.clear();
            return false;
        }, 120);
        assert(ok);
        std::printf("released %d held orders in %.2f s\n", kHeld + 1, double(Gateway::now() - t0) / 1e9);
    }
    stop = true;
    server.join();
    assert(gw.orders() == size_t(kHeld) + 2);
}

int main() {
    std::signal(SIGPIPE, SIG_IGN);  // a peer hanging up is an error to handle, not a reason to die
    test_logon_needs_credentials_and_no_takeover();
    test_order_limits();
    test_flood_beyond_gap_is_cut_off();
    test_big_gap_fill_does_not_deadlock();
    test_crash_and_recover_from_journal();
    test_oms_reconciles_after_reset();
    test_orders_over_tcp();
    std::puts("all tests passed");
}
