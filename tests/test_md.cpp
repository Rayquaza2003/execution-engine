#undef NDEBUG  // keep asserts on in Release builds
#include <cassert>
#include <cstdio>
#include <string>

#include "analytics/microstructure.hpp"
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

static void test_ofi_by_hand() {
    Quote a{100, 101, 10, 10};
    Quote b{100, 101, 15, 10};  // 5 more bid at the same price
    Quote c{100, 102, 15, 7};   // ask level 101 cleared, ask steps up: its 10 counts as buying pressure
    Quote d{99, 102, 20, 7};    // bid level 100 cleared: its 15 counts as removed
    assert(ofi_step(a, b) == 5);
    assert(ofi_step(b, c) == 10);
    assert(ofi_step(c, d) == -15);
    assert(ofi_step(a, a) == 0);
}

static void test_bars_close_on_interval_with_flow() {
    MarketData md(1, 1);
    Microstructure ms(md, 1, 1000);
    std::vector<Bar> bars;
    auto book = [&](Nanos t, Side side, Price px, Qty q) {
        ms.advance(0, t, bars);
        md.on_level(0, lv(0, side, px, q));
        ms.on_book(0);
    };
    book(10, Side::Buy, 100, 10);
    book(20, Side::Sell, 101, 10);
    book(500, Side::Buy, 100, 15);  // OFI +5
    ms.on_trade({0, 101, 4, Side::Buy});
    ms.on_trade({0, 100, 6, Side::Sell});
    book(1500, Side::Sell, 101, 30);  // closes bar [0, 1000) from the book before this change
    assert(bars.size() == 1);
    const Bar& b = bars[0];
    assert(b.t == 1000 && b.q.bid == 100 && b.q.bid_sz == 15 && b.q.ask_sz == 10);
    assert(b.ofi == 5 && b.quotes == 3 && b.trades == 2 && b.buy_vol == 4 && b.sell_vol == 6);
    assert(b.depth_bid == 15 && b.depth_ask == 10);
    assert(Microstructure::csv_row(b, "X") ==
           "1000,X,100,101,15,10,1,100.5,0.2000,100.6000,15,10,5,3,2,4,6,100.4000\n");

    book(5000, Side::Buy, 100, 0);  // empty bars between are skipped
    ms.finish(bars);
    assert(bars.size() == 3 && bars[1].t == 2000 && bars[1].ofi == -20 && bars[2].t == 6000);
    assert(Microstructure::csv_row(bars[2], "X") == "6000,X,0,101,0,30,,,,,0,30,0,1,0,0,0,\n");
}

int main() {
    test_nbbo_across_venues();
    test_l2_from_feed_matches_venue_book();
    test_ofi_by_hand();
    test_bars_close_on_interval_with_flow();
    std::puts("all tests passed");
}
