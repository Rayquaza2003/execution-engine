#pragma once
#include <cstring>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include "exchange/venue.hpp"
#include "md/itch.hpp"

// Feeds an ITCH stream into a Venue in replay mode, for the symbols asked for
// (each one gets a full order book, so not the whole market at once).
class ItchReplay {
public:
    static constexpr OrderId kIdBase = OrderId(1) << 62;  // feed ids never collide with client ranges

    // tick: ITCH price units (1/10000 $) per book tick; 100 = one cent.
    ItchReplay(Venue& v, std::vector<std::string> symbols, uint32_t tick = 100,
               size_t capacity_per_symbol = 1 << 16)
        : v_(v), want_(std::move(symbols)), tick_(tick), capacity_(capacity_per_symbol),
          sym_of_(1 << 16, kNone) {}

    // Applies every complete message in [p, p + n); returns bytes consumed.
    size_t feed(const uint8_t* p, size_t n, VenueOut& out) {
        return itch::parse(p, n, [&](const auto& m) { apply(m, out); });
    }

    // One parsed message, for callers that act between messages.
    template <class M>
    void apply(const M& m, VenueOut& out) {
        if constexpr (std::is_same_v<M, itch::StockDirectory>) {
            std::string stock(m.stock, 8);
            stock.erase(stock.find_last_not_of(' ') + 1);
            for (const std::string& w : want_)
                if (w == stock && sym_of_[m.locate] == kNone) {
                    SymbolId s = v_.add_symbol(capacity_);
                    sym_of_[m.locate] = s;
                    names_.resize(s + 1);
                    names_[s] = stock;
                }
            return;
        } else {
            SymbolId s = sym_of_[m.locate];
            if (s == kNone) return;  // not one of ours
            now_ = m.ts;
            v_.set_time(m.ts);
            // Dropped: a price that isn't a whole tick or is outside the book's
            // band (stub quotes far from the market), and later messages for
            // those orders. They never touch the top of the book.
            bool ok = true;
            if constexpr (std::is_same_v<M, itch::Add>) {
                ok = m.price % tick_ == 0 &&
                     v_.replay_add(s, kIdBase + m.ref, m.side, Price(m.price / tick_), m.shares, out);
            } else if constexpr (std::is_same_v<M, itch::Execute>) {
                std::optional<Price> px;
                if (m.type == 'C') px = Price(m.price / tick_);
                ok = v_.replay_execute(s, kIdBase + m.ref, m.shares, px, m.printable, out).qty > 0;
            } else if constexpr (std::is_same_v<M, itch::Cancel>) {
                ok = v_.replay_cancel(s, kIdBase + m.ref, m.shares, out).qty > 0;
            } else if constexpr (std::is_same_v<M, itch::Delete>) {
                ok = v_.replay_cancel(s, kIdBase + m.ref, ~Qty(0), out).qty > 0;
            } else if constexpr (std::is_same_v<M, itch::Replace>) {
                OrderBook::Reduced old = v_.replay_cancel(s, kIdBase + m.old_ref, ~Qty(0), out);
                ok = old.qty > 0 && m.price % tick_ == 0 &&  // a replace keeps the side
                     v_.replay_add(s, kIdBase + m.new_ref, old.side, Price(m.price / tick_), m.shares, out);
            }
            (ok ? applied_ : dropped_)++;
        }
    }

    // Venue symbol id for a stock, once its directory message has arrived.
    std::optional<SymbolId> symbol(const std::string& stock) const {
        for (size_t i = 0; i < names_.size(); ++i)
            if (names_[i] == stock) return SymbolId(i);
        return std::nullopt;
    }
    const std::string& name(SymbolId s) const { return names_[s]; }

    size_t applied() const { return applied_; }  // messages that changed a book
    size_t dropped() const { return dropped_; }   // for our symbols but not applied (see on())
    Nanos time() const { return now_; }

private:
    static constexpr uint32_t kNone = UINT32_MAX;

    Venue& v_;
    std::vector<std::string> want_;
    uint32_t tick_;
    size_t capacity_;
    std::vector<uint32_t> sym_of_;  // ITCH stock locate -> venue symbol
    std::vector<std::string> names_;  // by venue symbol; empty for symbols added by others
    size_t applied_ = 0;
    size_t dropped_ = 0;
    Nanos now_ = 0;
};
