#undef NDEBUG  // keep asserts on in Release builds
#include <cassert>
#include <cstdio>
#include <map>
#include <tuple>
#include <unordered_map>

#include "md/replay.hpp"

constexpr OrderId K = ItchReplay::kIdBase;

static void test_each_message_type_moves_the_book() {
    itch::Writer w;
    w.stock_directory(5, 0, "AAPL");
    w.stock_directory(6, 0, "MSFT");  // not asked for
    w.add(5, 10, 1, Side::Buy, 100, "AAPL", 1'500'000);  // $150.00 = 15000 cents
    w.add(5, 11, 2, Side::Buy, 200, "AAPL", 1'500'000);
    w.add(5, 12, 3, Side::Sell, 300, "AAPL", 1'500'500);
    w.add(6, 13, 4, Side::Sell, 300, "MSFT", 1'000'000);
    w.execute(5, 20, 1, 40, 1);                          // prints at the order's price
    w.execute_at(5, 21, 3, 100, 2, false, 1'500'400);    // non-printable: book only
    w.execute_at(5, 22, 3, 50, 3, true, 1'500'400);      // prints at $150.04
    w.cancel(5, 23, 2, 50);
    w.replace(5, 24, 3, 7, 100, 1'501'000);              // keeps the sell side, new ref
    w.add(5, 25, 8, Side::Buy, 100, "AAPL", 1'500'050);  // sub-penny: dropped
    w.del(5, 26, 8);                                     // ...and so is its delete
    w.del(5, 27, 1);

    Venue v;
    ItchReplay r(v, {"AAPL"});
    VenueOut out;
    assert(r.feed(w.buf.data(), w.buf.size(), out) == w.buf.size());
    assert(v.symbols() == 1 && r.symbol("AAPL") == 0u && !r.symbol("MSFT") && r.name(0) == "AAPL");
    assert(r.symbol_at(5) == 0u && !r.symbol_at(6));
    const OrderBook& b = v.book(0);
    assert(b.best_bid() == 15000 && b.depth(Side::Buy, 15000) == 150 && b.leaves(K + 2) == 150);
    assert(b.best_ask() == 15010 && b.depth(Side::Sell, 15010) == 100 && b.leaves(K + 3) == 0);
    assert(b.order_count() == 2 && r.applied() == 9 && r.dropped() == 2 && r.time() == 27);

    assert(out.prints.size() == 2 && out.reports.empty());  // feed orders have no owner
    assert(out.prints[0].price == 15000 && out.prints[0].qty == 40 && out.prints[0].aggressor == Side::Sell);
    assert(out.prints[1].price == 15004 && out.prints[1].qty == 50 && out.prints[1].aggressor == Side::Buy);
    assert(!out.levels.empty() && out.levels.back().level.qty == 150);  // delete of ref 1 leaves 150
}

// Replaying a synthetic day must leave exactly the book an independent model
// builds from the same messages (ref -> side, price, shares; summed per level).
static void test_synthetic_day_matches_independent_model() {
    itch::Writer w;
    itch::synthetic(w, 200'000, 3, 11);
    struct O { uint16_t loc; Side side; uint32_t px, shares; };
    std::unordered_map<uint64_t, O> live;
    itch::parse(w.buf.data(), w.buf.size(), [&](const auto& m) {
        using M = std::decay_t<decltype(m)>;
        if constexpr (std::is_same_v<M, itch::Add>) live[m.ref] = {m.locate, m.side, m.price, m.shares};
        else if constexpr (std::is_same_v<M, itch::Execute> || std::is_same_v<M, itch::Cancel>) {
            if (!(live[m.ref].shares -= m.shares)) live.erase(m.ref);
        } else if constexpr (std::is_same_v<M, itch::Delete>) live.erase(m.ref);
        else if constexpr (std::is_same_v<M, itch::Replace>) {
            O o = live[m.old_ref];
            live.erase(m.old_ref);
            live[m.new_ref] = {o.loc, o.side, m.price, m.shares};
        }
    });
    std::map<std::tuple<uint16_t, Side, Price>, Qty> want;
    for (auto& [ref, o] : live) want[{o.loc, o.side, Price(o.px / 100)}] += o.shares;

    Venue v;
    ItchReplay r(v, {"SYM0", "SYM1", "SYM2"});
    VenueOut out;
    // Feed in uneven chunks so messages straddle chunk boundaries.
    size_t at = 0, used_total = 0;
    std::vector<uint8_t> carry;
    for (size_t chunk = 997; at < w.buf.size(); at += chunk) {
        size_t end = std::min(at + chunk, w.buf.size());
        carry.insert(carry.end(), w.buf.begin() + long(at), w.buf.begin() + long(end));
        size_t used = r.feed(carry.data(), carry.size(), out);
        used_total += used;
        carry.erase(carry.begin(), carry.begin() + long(used));
        out.clear();
    }
    assert(carry.empty() && used_total == w.buf.size() && r.dropped() == 0);

    std::map<std::tuple<uint16_t, Side, Price>, Qty> got;
    size_t orders = 0;
    for (uint16_t loc = 1; loc <= 3; ++loc) {
        SymbolId s = *r.symbol("SYM" + std::to_string(loc - 1));
        orders += v.book(s).order_count();
        for (Side side : {Side::Buy, Side::Sell})
            for (const LevelChange& l : v.book(s).top(side, 1000)) got[{loc, side, l.price}] = l.qty;
    }
    assert(got == want && orders == live.size());
}

int main() {
    test_each_message_type_moves_the_book();
    test_synthetic_day_matches_independent_model();
    std::puts("all tests passed");
}
