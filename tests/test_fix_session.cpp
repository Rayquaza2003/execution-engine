#undef NDEBUG  // keep asserts on in Release builds
#include <cassert>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "fix/session.hpp"

constexpr Nanos kSec = 1'000'000'000;
constexpr Nanos T0 = 1'793'750'400 * kSec;  // 2026-11-04 00:00:00 UTC

struct Pair {
    Session client{{"CLIENT", "EXCH", 30 * kSec, true}};
    Session server{{"EXCH", "CLIENT", 30 * kSec, false}};
    std::vector<std::string> at_server, at_client;  // ClOrdIDs of application messages received
    Nanos now = T0;

    // Moves bytes both ways until quiet. drop(i, message) can lose or corrupt
    // the i-th message (counting from 0) going client -> server.
    std::function<bool(size_t, std::string&)> drop = [](size_t, std::string&) { return false; };
    size_t sent = 0;

    void pump() {
        for (int round = 0; round < 10; ++round) {
            std::string c2s = take(client), s2c = take(server);
            if (c2s.empty() && s2c.empty()) return;
            std::string kept;
            for (size_t at = 0; at < c2s.size();) {
                size_t len = fix::frame(std::string_view(c2s).substr(at)).len;
                std::string m = c2s.substr(at, len);
                if (!drop(sent++, m)) kept += m;
                at += len;
            }
            server.on_bytes(kept.data(), kept.size(), now, [&](const fix::Message& m) {
                at_server.push_back(std::string(*m.get(fix::ClOrdID)));
            });
            client.on_bytes(s2c.data(), s2c.size(), now, [&](const fix::Message& m) {
                at_client.push_back(std::string(*m.get(fix::ClOrdID)));
            });
        }
        assert(false && "sessions never went quiet");
    }
    static std::string take(Session& s) {
        std::string out;
        out.swap(s.output());
        return out;
    }
    void order(Session& s, const char* id) {
        s.send("D", now, [&](fix::Writer& w) { w.field(fix::ClOrdID, id); });
    }
    void up() {
        client.logon(now);
        pump();
        assert(client.state() == Session::State::Active && server.state() == Session::State::Active);
    }
};

static void test_logon_and_messages_both_ways() {
    Pair p;
    p.up();
    p.order(p.client, "a");
    p.order(p.server, "x");
    p.pump();
    assert(p.at_server == std::vector<std::string>{"a"} && p.at_client == std::vector<std::string>{"x"});
    assert(p.client.next_out() == 3 && p.server.next_in() == 3 && p.server.gaps() == 0);
}

// A lost application message: the next one is held, exactly the missing one
// is resent (PossDupFlag=Y), and the application still sees a, b, c in order.
static void test_gap_is_resent_in_order() {
    Pair p;
    p.up();
    p.drop = [](size_t i, std::string&) { return i == 2; };  // logon=0, a=1, b=2
    for (const char* id : {"a", "b", "c"}) p.order(p.client, id);
    p.pump();
    assert((p.at_server == std::vector<std::string>{"a", "b", "c"}));
    assert(p.server.gaps() == 1 && p.client.resent() == 1 && p.server.next_in() == 5);
}

// A corrupted message is dropped as garbled, then recovered the same way.
static void test_garbled_message_recovered() {
    Pair p;
    p.up();
    p.drop = [](size_t i, std::string& m) {
        if (i == 1) m[m.size() - 3] = m[m.size() - 3] == '0' ? '1' : '0';  // break the checksum
        return false;
    };
    p.order(p.client, "a");
    p.order(p.client, "b");
    p.pump();
    assert((p.at_server == std::vector<std::string>{"a", "b"}) && p.server.garbled() == 1);
}

// A lost heartbeat comes back as a SequenceReset-GapFill, not a resend.
static void test_lost_admin_message_becomes_gap_fill() {
    Pair p;
    p.up();
    p.drop = [](size_t i, std::string&) { return i == 1; };
    p.now += 31 * kSec;
    p.client.on_timer(p.now);  // heartbeat, lost
    p.order(p.client, "a");
    p.pump();
    assert(p.at_server == std::vector<std::string>{"a"} && p.client.resent() == 0 && p.server.gaps() == 1);
}

// Silence: heartbeat after HeartBtInt, TestRequest a little later, dead after another interval.
static void test_heartbeat_test_request_and_dead_peer() {
    Pair p;
    p.up();
    p.now += 30 * kSec;
    assert(p.client.on_timer(p.now));
    assert(p.client.output().find("35=0\x01") != std::string::npos);  // heartbeat
    p.client.output().clear();
    p.now += 7 * kSec;  // server silent for 37 s > 30 * 1.2
    assert(p.client.on_timer(p.now) && p.client.output().find("35=1\x01") != std::string::npos);
    p.pump();  // the server answers the TestRequest
    p.now += 29 * kSec;
    assert(p.client.on_timer(p.now));
    p.now += 40 * kSec;  // now nobody answers
    p.client.output().clear();
    assert(p.client.on_timer(p.now));  // TestRequest goes out
    p.now += 31 * kSec;
    assert(!p.client.on_timer(p.now));  // still nothing: dead
}

// A message below the expected number without PossDupFlag is fatal.
static void test_seq_too_low_logs_out() {
    Pair p;
    p.up();
    Session stale({"CLIENT", "EXCH", 30 * kSec, true});  // restarted at 1, as if it lost its state
    stale.send("D", p.now, [](fix::Writer& w) { w.field(fix::ClOrdID, "z"); });
    std::string bytes = stale.output();
    p.server.on_bytes(bytes.data(), bytes.size(), p.now, [](const fix::Message&) { assert(false); });
    const std::string& out = p.server.output();
    assert(out.find("35=5\x01") != std::string::npos && out.find("58=MsgSeqNum too low") != std::string::npos);
}

static void test_acceptor_learns_peer_from_logon() {
    Session client({"ANYONE", "EXCH", 30 * kSec, true});
    Session server({"EXCH", "", 30 * kSec, false});
    client.logon(T0);
    server.on_bytes(client.output().data(), client.output().size(), T0, [](const fix::Message&) {});
    assert(server.state() == Session::State::Active && server.peer() == "ANYONE");
    client.on_bytes(server.output().data(), server.output().size(), T0, [](const fix::Message&) {});
    assert(client.state() == Session::State::Active);
}

static void test_logout_handshake_and_utc() {
    Pair p;
    p.up();
    p.client.logout(p.now);
    p.pump();
    assert(p.client.state() == Session::State::LoggedOut && p.server.state() == Session::State::LoggedOut);
    assert(Session::utc(T0 + 1'234'000'000) == "20261104-00:00:01.234");
}

int main() {
    test_logon_and_messages_both_ways();
    test_gap_is_resent_in_order();
    test_garbled_message_recovered();
    test_lost_admin_message_becomes_gap_fill();
    test_heartbeat_test_request_and_dead_peer();
    test_seq_too_low_logs_out();
    test_logout_handshake_and_utc();
    test_acceptor_learns_peer_from_logon();
    std::puts("all tests passed");
}
