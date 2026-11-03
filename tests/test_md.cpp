#undef NDEBUG  // keep asserts on in Release builds
#include <cassert>
#include <cstdio>
#include <string>

#include "md/market_data.hpp"
#include "md/replay.hpp"

static LevelUpdate lv(SymbolId s, Side side, Price px, Qty qty) { return {s, {side, px, qty}}; }

static void test_nbbo_across_venues() {
    MarketData md(2, 1);
    md.on_level(0, lv(0, Side::Buy, 100, 5));
    md.on_level(0, lv(0, Side::Buy, 99, 50));
    md.on_level(0, lv(0, Side::Sell, 103, 5));
    md.on_level(1, lv(0, Side::Buy, 101, 3));
    md.on_level(1, lv(0, Side::Sell, 103, 4));
    Quote n = md.nbbo(0);
    assert(n.bid == 101 && n.bid_sz == 3 && n.ask == 103 && n.ask_sz == 9);
    assert(md.bbo(0, 0).bid == 100 && md.depth(0, Side::Buy, 2) == 5 + 50 + 3);

    md.on_level(1, lv(0, Side::Buy, 101, 0));  // level gone
    n = md.nbbo(0);
    assert(n.bid == 100 && n.bid_sz == 5 && md.book(1, 0).bids.empty());
}

// The L2 book rebuilt from level updates must equal the venue's own book:
// top of book after every message, and every level at the end.
static void test_l2_from_feed_matches_venue_book() {
    itch::Writer w;
    itch::synthetic(w, 100'000, 2, 5);
    Venue v;
    ItchReplay r(v, {"SYM0", "SYM1"});
    MarketData md(1, 2);
    VenueOut out;
    size_t at = 0;
    while (at < w.buf.size()) {
        size_t len = 2 + (size_t(w.buf[at]) << 8 | w.buf[at + 1]);
        r.feed(w.buf.data() + at, len, out);  // one message at a time
        at += len;
        for (const LevelUpdate& u : out.levels) md.on_level(0, u);
        out.clear();
        for (SymbolId s = 0; s < v.symbols(); ++s) {
            Quote q = md.bbo(0, s);
            const OrderBook& b = v.book(s);
            assert(q.bid_sz ? b.best_bid() == q.bid && b.depth(Side::Buy, q.bid) == q.bid_sz : !b.best_bid());
            assert(q.ask_sz ? b.best_ask() == q.ask && b.depth(Side::Sell, q.ask) == q.ask_sz : !b.best_ask());
        }
    }
    for (SymbolId s = 0; s < 2; ++s) {
        const MarketData::L2& l2 = md.book(0, s);
        auto bids = v.book(s).top(Side::Buy, 1000), asks = v.book(s).top(Side::Sell, 1000);
        assert(bids.size() == l2.bids.size() && asks.size() == l2.asks.size());
        size_t i = 0;
        for (auto [px, q] : l2.bids) assert(bids[i].price == px && bids[i++].qty == q);
        i = 0;
        for (auto [px, q] : l2.asks) assert(asks[i].price == px && asks[i++].qty == q);
    }
}

int main() {
    test_nbbo_across_venues();
    test_l2_from_feed_matches_venue_book();
    std::puts("all tests passed");
}
