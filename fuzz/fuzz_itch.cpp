// Fuzz target: ITCH parsing and replay into the venue's order books.
// Invariants: the parser never consumes more than it was given, and however
// hostile the feed, a book never ends up crossed.
#include <cstdint>
#include <cstdlib>

#include "md/replay.hpp"

static void check(bool ok) {
    if (!ok) std::abort();
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t n) {
    itch::Writer w;
    w.stock_directory(1, 0, "SYM0");  // so messages for locate 1 reach a book
    w.buf.insert(w.buf.end(), data, data + n);
    Venue venue;
    ItchReplay replay(venue, {"SYM0", "SYM1"}, 100, 1 << 10);
    VenueOut out;
    size_t used = itch::parse(w.buf.data(), w.buf.size(), [&](const auto& m) {
        replay.apply(m, out);
        out.clear();
    });
    check(used <= w.buf.size());
    for (SymbolId s = 0; s < venue.symbols(); ++s) {
        const OrderBook& b = venue.book(s);
        if (b.best_bid() && b.best_ask()) check(*b.best_bid() < *b.best_ask());
    }
    return 0;
}
