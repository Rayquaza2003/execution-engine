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

int main() {
    test_http_parsing();
    test_trading_through_the_bench();
    test_flow_and_burst();
    std::puts("all tests passed");
}
