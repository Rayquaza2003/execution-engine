#undef NDEBUG  // keep asserts on in Release builds
#include <cassert>
#include <cstdio>
#include <cstring>
#include <variant>
#include <vector>

#include "md/itch.hpp"

using Msg = std::variant<itch::StockDirectory, itch::Add, itch::Execute, itch::Cancel, itch::Delete,
                         itch::Replace>;

static std::vector<Msg> parse_all(const std::vector<uint8_t>& b, size_t* used = nullptr) {
    std::vector<Msg> v;
    size_t n = itch::parse(b.data(), b.size(), [&](const auto& m) { v.push_back(m); });
    if (used) *used = n;
    return v;
}

// Bytes laid out by hand from the ITCH 5.0 spec, not by our Writer, so a
// field-order mistake can't cancel out between encoder and decoder.
static void test_add_from_spec_bytes() {
    std::vector<uint8_t> b = {
        0x00, 0x24,                                      // length 36
        'A',                                             // type
        0x00, 0x0D,                                      // stock locate 13
        0x00, 0x00,                                      // tracking number
        0x00, 0x00, 0x00, 0x0B, 0x8C, 0x61,              // timestamp 756833 ns
        0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03,  // order ref 66051
        'S',                                             // side
        0x00, 0x00, 0x01, 0x2C,                          // shares 300
        'A', 'A', 'P', 'L', ' ', ' ', ' ', ' ',          // stock
        0x00, 0x16, 0xE3, 0x60,                          // price 1500000 = $150.0000
    };
    auto v = parse_all(b);
    assert(v.size() == 1);
    const auto& a = std::get<itch::Add>(v[0]);
    assert(a.type == 'A' && a.locate == 13 && a.ts == 756833 && a.ref == 66051);
    assert(a.side == Side::Sell && a.shares == 300 && a.price == 1500000);
}

static void test_round_trip_every_type() {
    itch::Writer w;
    w.stock_directory(7, 1, "MSFT");
    w.add(7, 2, 10, Side::Buy, 100, "MSFT", 1005000);
    w.execute(7, 3, 10, 40, 555);
    w.execute_at(7, 4, 10, 10, 556, false, 1004900);
    w.cancel(7, 5, 10, 20);
    w.replace(7, 6, 10, 11, 70, 1005100);
    w.del(7, 0xFFFFFFFFFFFF, 11);  // largest 48-bit timestamp
    auto v = parse_all(w.buf);
    assert(v.size() == 7);
    assert(std::memcmp(std::get<itch::StockDirectory>(v[0]).stock, "MSFT    ", 8) == 0);
    const auto& e = std::get<itch::Execute>(v[2]);
    assert(e.type == 'E' && e.ref == 10 && e.shares == 40 && e.printable && e.price == 0);
    const auto& c = std::get<itch::Execute>(v[3]);
    assert(c.type == 'C' && c.shares == 10 && !c.printable && c.price == 1004900);
    const auto& x = std::get<itch::Cancel>(v[4]);
    assert(x.ref == 10 && x.shares == 20 && x.ts == 5);
    const auto& u = std::get<itch::Replace>(v[5]);
    assert(u.old_ref == 10 && u.new_ref == 11 && u.shares == 70 && u.price == 1005100);
    const auto& d = std::get<itch::Delete>(v[6]);
    assert(d.ref == 11 && d.ts == 0xFFFFFFFFFFFF && d.locate == 7);
}

static void test_skips_unknown_and_stops_at_partial_message() {
    itch::Writer w;
    w.add(1, 1, 1, Side::Buy, 1, "A", 100);
    w.buf.insert(w.buf.end(), {0x00, 0x0C, 'S', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 'O'});  // system event
    w.del(1, 2, 1);
    size_t whole = w.buf.size();
    w.cancel(1, 3, 1, 1);
    std::vector<uint8_t> cut(w.buf.begin(), w.buf.end() - 5);  // last message incomplete

    size_t used = 0;
    auto v = parse_all(cut, &used);
    assert(v.size() == 2 && used == whole);
    std::vector<uint8_t> rest(w.buf.begin() + long(used), w.buf.end());  // next chunk carries it
    assert(parse_all(rest).size() == 1);
}

int main() {
    test_add_from_spec_bytes();
    test_round_trip_every_type();
    test_skips_unknown_and_stops_at_partial_message();
    std::puts("all tests passed");
}
