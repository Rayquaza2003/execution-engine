// Replays a NASDAQ ITCH 5.0 file (uncompressed BinaryFILE) for a few symbols
// and writes one microstructure bar per symbol per second as CSV.
// Usage: ./build/itch_replay FILE OUT.csv SYMBOL [SYMBOL...]
// Sample days: https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/ (gunzip first).
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "analytics/microstructure.hpp"
#include "md/replay.hpp"

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s FILE OUT.csv SYMBOL [SYMBOL...]\n", argv[0]);
        return 2;
    }
    FILE* in = std::fopen(argv[1], "rb");
    FILE* csv = std::fopen(argv[2], "w");
    if (!in || !csv) {
        std::perror("open");
        return 1;
    }
    std::vector<std::string> want(argv + 3, argv + argc);

    constexpr Nanos kBar = 1'000'000'000;  // ponytail: fixed 1 s bars; add a flag when a study needs another
    Venue venue;
    ItchReplay replay(venue, want);
    MarketData md(1, want.size());
    Microstructure ms(md, want.size(), kBar);
    VenueOut out;
    std::vector<Bar> bars;
    size_t msgs = 0;

    std::fputs(Microstructure::csv_header(), csv);
    auto write = [&] {
        for (const Bar& b : bars) std::fputs(Microstructure::csv_row(b, replay.name(b.sym)).c_str(), csv);
        bars.clear();
    };
    auto on_msg = [&](const auto& m) {
        ++msgs;
        replay.apply(m, out);
        if (out.levels.empty() && out.prints.empty()) return;
        SymbolId s = out.levels.empty() ? out.prints[0].sym : out.levels[0].sym;
        ms.advance(s, replay.time(), bars);  // MarketData still holds the book before this message
        for (const LevelUpdate& u : out.levels) md.on_level(0, u);
        if (!out.levels.empty()) ms.on_book(s);
        for (const TradePrint& p : out.prints) ms.on_trade(p);
        out.clear();
        if (bars.size() > 4096) write();
    };

    auto t0 = std::chrono::steady_clock::now();
    std::vector<uint8_t> buf(1 << 24);
    size_t have = 0, bytes = 0;
    for (;;) {
        size_t got = std::fread(buf.data() + have, 1, buf.size() - have, in);
        have += got;
        bytes += got;
        size_t used = itch::parse(buf.data(), have, on_msg);
        std::memmove(buf.data(), buf.data() + used, have - used);
        have -= used;
        if (!got) break;
    }
    ms.finish(bars);
    write();
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    std::fprintf(stderr, "%zu bytes, %zu book messages in %.2f s (%.1f M msgs/s)\n", bytes, msgs, secs,
                 double(msgs) / secs / 1e6);
    std::fprintf(stderr, "our symbols: %zu applied, %zu dropped\n", replay.applied(), replay.dropped());
    for (const std::string& w : want) {
        auto s = replay.symbol(w);
        if (!s) {
            std::fprintf(stderr, "  %s: not in file\n", w.c_str());
            continue;
        }
        const OrderBook& b = venue.book(*s);
        std::fprintf(stderr, "  %s: %zu orders resting, bid %lld ask %lld\n", w.c_str(), b.order_count(),
                     (long long)b.best_bid().value_or(0), (long long)b.best_ask().value_or(0));
    }
    std::fclose(csv);
}
