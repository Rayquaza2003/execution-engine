#undef NDEBUG  // keep asserts on in Release builds
#include <cassert>
#include <cstdio>
#include <string>

#include "web/sandbox.hpp"

static bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

static void test_http_parsing() {
    web::Request r;
    std::string get = "GET /api/state?sym=1 HTTP/1.1\r\nHost: x\r\n\r\n";
    assert(web::parse(get, r) == get.size() && r.method == "GET" && r.path == "/api/state" && r.query == "sym=1");
    std::string post = "POST /api/order HTTP/1.1\r\nContent-Length: 13\r\n\r\nside=buy&px=1";
    assert(web::parse(post.substr(0, post.size() - 3), r) == 0);  // body not complete yet
    assert(web::parse(post, r) == post.size() && r.body == "side=buy&px=1");
    assert(web::parse("nonsense\r\n\r\n", r) == std::string_view::npos);
    auto f = web::form("a=1&b=x%20y+z&c=");
    assert(f["a"] == "1" && f["b"] == "x y z" && f["c"].empty());
    assert(web::quote("a\"b\\c\n") == "\"a\\\"b\\\\c\\u000a\"");
}

static Sandbox sandbox() {
    RiskLimits lim;
    lim.max_order_qty = 1000;
    lim.max_position = 5000;
    lim.collar_bps = 500;
    Sandbox sb({"AAPL"}, {15'000}, lim);
    sb.set_flow(0);
    return sb;
}

// Orders go through the OMS and risk; the snapshot shows them with their
// place in the queue; cancel, modify, the other trader and the kill switch
// all reach the engine.
static void test_trading_through_the_bench() {
    Sandbox sb = sandbox();
    Nanos t = 1'000'000'000;
    assert(sb.order({{"sym", "AAPL"}, {"side", "buy"}, {"type", "limit"}, {"px", "149.00"}, {"qty", "100"}}, t).empty());
    std::string s = sb.state(0, t);
    assert(has(s, "\"cl\":1,") && has(s, "\"state\":\"working\"") && has(s, "\"ahead\":"));

    assert(has(sb.order({{"sym", "AAPL"}, {"side", "buy"}, {"px", "149.00"}, {"qty", "5000"}}, t), "order too large"));
    assert(has(sb.order({{"sym", "AAPL"}, {"side", "buy"}, {"px", "100.00"}, {"qty", "10"}}, t), "too far from the market"));
    assert(!sb.order({{"sym", "NOPE"}, {"side", "buy"}, {"px", "1"}, {"qty", "1"}}, t).empty());
    assert(!sb.order({{"sym", "AAPL"}, {"side", "buy"}, {"px", "-1"}, {"qty", "1"}}, t).empty());

    assert(sb.replace({{"cl", "1"}, {"px", "148.50"}, {"qty", "200"}}, t).empty());
    assert(has(sb.state(0, t), "\"px\":148.50,\"qty\":200"));

    // I offer above everything; the other trader lifts me.
    assert(sb.order({{"sym", "AAPL"}, {"side", "sell"}, {"px", "151.00"}, {"qty", "100"}}, t).empty());
    assert(sb.order({{"sym", "AAPL"}, {"side", "buy"}, {"type", "limit"}, {"px", "151.00"}, {"qty", "500000"}, {"who", "other"}}, t).empty());
    s = sb.state(0, t);
    assert(has(s, "\"cl\":4,") && has(s, "\"state\":\"filled\"") && has(s, "\"position\":-100"));

    assert(sb.cancel({{"cl", "1"}}, t).empty() && !sb.cancel({{"cl", "1"}}, t).empty());
    assert(sb.kill(t).empty() && has(sb.state(0, t), "\"killed\":true"));
    assert(has(sb.order({{"sym", "AAPL"}, {"side", "buy"}, {"px", "149.00"}, {"qty", "1"}}, t), "kill switch"));
    assert(sb.resume().empty() && has(sb.state(0, t), "\"killed\":false"));
    assert(sb.order({{"sym", "AAPL"}, {"side", "buy"}, {"px", "149.00"}, {"qty", "1"}}, t).empty());
    assert(has(sb.state(0, t), "\"cl\":1,") && has(sb.state(0, t), "\"state\":\"cancelled\""));  // stays cancelled
    sb.reset();
    assert(has(sb.state(0, t), "\"killed\":false") && has(sb.state(0, t), "\"orders\":[]"));
}

// Flow keeps the book two-sided and its own order list bounded, and a burst reports its rate.
static void test_flow_and_burst() {
    Sandbox sb = sandbox();
    assert(sb.burst(50'000, 1).empty());
    std::string s = sb.state(0, 2);
    assert(has(s, "\"burst\":\"50000 orders in") && !has(s, "\"bid\":null") && !has(s, "\"ask\":null"));
    sb.set_flow(1000);
    sb.tick(1'000'000'000);
    sb.tick(2'000'000'000);  // one second at 1000/s
    assert(has(sb.state(0, 3), "\"flow\":1000.00"));
}

// Replay: the books come from an ITCH feed played at a chosen speed. Our
// order queues behind the real one at the bid, the real one trades first,
// and we fill when an execution reaches an order queued behind us.
static void test_replay_mode() {
    constexpr uint32_t D = 10'000;  // $1 in ITCH units
    constexpr Nanos kSec = 1'000'000'000;
    itch::Writer w;
    w.stock_directory(1, 0, "AAPL");
    w.add(1, 1 * kSec, 1, Side::Buy, 100, "AAPL", 150 * D);    // before the start: fast-forwarded
    w.add(1, 1 * kSec, 2, Side::Sell, 100, "AAPL", 150 * D + 100);
    w.add(1, 15 * kSec, 3, Side::Buy, 300, "AAPL", 150 * D);   // behind us (we join at 12 s)
    w.execute(1, 20 * kSec, 1, 100, 1);                         // the order ahead of us trades
    w.execute(1, 25 * kSec, 3, 50, 2);                          // one behind us trades: we fill first
    ReplayFeed feed{w.buf.data(), w.buf.size(), {"AAPL"}, 10 * kSec};
    RiskLimits lim;
    lim.collar_bps = 500;
    Sandbox sb({"AAPL"}, {15'000}, lim, feed);
    assert(sb.replay_available() && sb.set_mode(true).empty());
    std::string s = sb.state(0, 0);
    assert(has(s, "\"mode\":\"replay\"") && has(s, "\"bid\":150.00") && has(s, "\"clock\":10000"));
    assert(!sb.set_flow(100).empty() && !sb.burst(10, 0).empty());  // no simulated flow in replay

    sb.set_speed(1);
    assert(sb.play(true).empty());
    Nanos wall = 1'000 * kSec;
    sb.tick(wall);
    sb.tick(wall += 2 * kSec);  // replay clock 12 s
    assert(sb.order({{"sym", "AAPL"}, {"side", "buy"}, {"px", "150.00"}, {"qty", "40"}}, wall).empty());
    s = sb.state(0, wall);
    assert(has(s, "\"clock\":12000") && has(s, "\"ahead\":100"));
    sb.tick(wall += 10 * kSec);  // to 22 s: ref 1 traded, still working
    assert(has(sb.state(0, wall), "\"state\":\"working\""));
    sb.tick(wall += 5 * kSec);   // to 27 s: ref 3's execution reaches us
    s = sb.state(0, wall);
    assert(has(s, "\"state\":\"filled\"") && has(s, "\"position\":40"));
    sb.tick(wall += 100 * kSec);
    s = sb.state(0, wall);
    assert(has(s, "\"end\":true") && has(s, "\"playing\":false") && !sb.play(true).empty());
    sb.reset();  // back to the start time, everything fresh
    assert(has(sb.state(0, wall), "\"clock\":10000") && has(sb.state(0, wall), "\"orders\":[]"));
    assert(sb.set_mode(false).empty() && has(sb.state(0, wall), "\"mode\":\"sim\""));
}

int main() {
    test_replay_mode();
    test_http_parsing();
    test_trading_through_the_bench();
    test_flow_and_burst();
    std::puts("all tests passed");
}
