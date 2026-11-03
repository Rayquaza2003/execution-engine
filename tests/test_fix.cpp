#undef NDEBUG  // keep asserts on in Release builds
#include <cassert>
#include <cstdio>
#include <string>

#include "fix/fix.hpp"

using namespace std::string_literals;

// A heartbeat laid out by hand (| is SOH). BodyLength 45 and CheckSum 056 were
// computed outside this codebase, so encoder and decoder can't share a mistake.
static const std::string kHeartbeat =
    "8=FIX.4.4\x01" "9=45\x01" "35=0\x01" "49=A\x01" "56=B\x01" "34=1\x01" "52=20261104-00:00:00.000\x01" "10=056\x01"s;

static void test_writer_matches_hand_built_message() {
    fix::Writer w;
    w.begin("0").field(fix::SenderCompID, "A").field(fix::TargetCompID, "B").field(fix::MsgSeqNum, 1);
    w.field(fix::SendingTime, "20261104-00:00:00.000");
    std::string out;
    w.finish(out);
    assert(out == kHeartbeat);
}

static void test_frame_and_parse() {
    std::string two = kHeartbeat + kHeartbeat;
    fix::Frame f = fix::frame(two);
    assert(f.status == fix::Frame::Ok && f.len == kHeartbeat.size());
    fix::Message m;
    assert(m.parse(std::string_view(two).substr(0, f.len)));
    assert(m.type() == "0" && m.get(fix::SenderCompID) == "A" && m.get_int(fix::MsgSeqNum) == 1);
    assert(!m.get(fix::Text) && !m.get_int(fix::SenderCompID));

    for (size_t cut = 0; cut < kHeartbeat.size(); ++cut)  // every partial prefix asks for more
        assert(fix::frame(std::string_view(kHeartbeat).substr(0, cut)).status == fix::Frame::More);
}

static void test_garbled_input_is_skipped() {
    std::string bad_sum = kHeartbeat;
    bad_sum[bad_sum.size() - 2] = '7';  // 10=057
    fix::Frame f = fix::frame(bad_sum + kHeartbeat);
    assert(f.status == fix::Frame::Garbled && f.len == kHeartbeat.size());  // skip exactly that message

    std::string junk = "xx" + kHeartbeat;
    f = fix::frame(junk);
    assert(f.status == fix::Frame::Garbled && f.len == 2);  // resync at the next BeginString

    std::string bad_len = kHeartbeat;
    bad_len.replace(bad_len.find("9=45"), 4, "9=44");
    assert(fix::frame(bad_len).status == fix::Frame::Garbled);
}

static void test_prices_and_order_fields() {
    assert(fix::to_price("150.25") == 15025 && fix::to_price("150") == 15000 && fix::to_price("0.5") == 50);
    assert(fix::to_price("-1.05") == -105);
    assert(!fix::to_price("1.005") && !fix::to_price("abc") && !fix::to_price("") && !fix::to_price("1.x"));

    fix::Writer w;
    w.begin("D").field(fix::ClOrdID, "abc").field(fix::Side, '1').field(fix::OrderQty, uint64_t(300));
    w.price(fix::Price, 15025).price(fix::LastPx, -105).price(fix::AvgPx, 7);
    std::string out;
    w.finish(out);
    fix::Message m;
    assert(fix::frame(out).status == fix::Frame::Ok && m.parse(out));
    assert(m.type() == "D" && m.get(fix::ClOrdID) == "abc" && m.get(fix::Side) == "1");
    assert(m.get_int(fix::OrderQty) == 300 && m.get(fix::Price) == "150.25" && m.get(fix::LastPx) == "-1.05");
    assert(m.get(fix::AvgPx) == "0.07");
}

int main() {
    test_writer_matches_hand_built_message();
    test_frame_and_parse();
    test_garbled_input_is_skipped();
    test_prices_and_order_fields();
    std::puts("all tests passed");
}
