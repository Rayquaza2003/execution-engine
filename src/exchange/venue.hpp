#pragma once
#include <cstdlib>
#include <functional>
#include <map>
#include <unordered_map>
#include <variant>
#include <vector>

#include "core/ring.hpp"
#include "core/types.hpp"
#include "exchange/orderbook.hpp"

// Exchange simulator: one OrderBook per symbol behind a message interface.
// Requests come in, and everything the outside world learns comes back as
// messages in VenueOut: execution reports to the order's owner, plus public
// market data (level updates and trade prints). The caller (the event loop)
// decides when those messages arrive, which is how latency is modeled.

using SymbolId = uint32_t;

// StopLimit waits off-book until the last trade price reaches stop_px (buy:
// last >= stop_px, sell: last <= stop_px), then enters as a limit order at px.
enum class OrdType : uint8_t { Limit, Market, StopLimit };

// Order ids must be unique per venue; each client draws from its own range.
struct NewOrder {
    OrderId id;
    AccountId account;
    SymbolId sym;
    Side side;
    OrdType type;
    Tif tif;
    Price px;  // ignored for Market
    Qty qty;
    Price stop_px = 0;  // StopLimit only
};

struct CancelRequest {
    OrderId id;
    SymbolId sym;
};

struct ReplaceRequest {
    OrderId id;
    SymbolId sym;
    Price px;
    Qty qty;
};

// Anything a client can send to the venue, as one value for queues and the event loop.
using VenueRequest = std::variant<NewOrder, CancelRequest, ReplaceRequest>;

enum class ExecType : uint8_t {
    New,              // accepted
    Trade,            // fill (partial or full); see last_px/last_qty/leaves
    Cancelled,        // removed by request, IOC/FOK remainder, market remainder, or STP
    Replaced,         // replace accepted; leaves is the new quantity
    Rejected,         // new order refused
    Expired,          // Day order at end of day
    CancelRejected,   // cancel refused (unknown or already gone)
    ReplaceRejected,  // replace refused; original order unchanged
    Triggered,        // stop-limit fired and is now entering the book
};

struct ExecReport {
    OrderId id;
    SymbolId sym;
    ExecType type;
    Reject reason;
    Price last_px;
    Qty last_qty;
    Qty leaves;
};

struct LevelUpdate {
    SymbolId sym;
    LevelChange level;
};

struct TradePrint {
    SymbolId sym;
    Price price;
    Qty qty;
    Side aggressor;
};

struct VenueOut {
    std::vector<ExecReport> reports;
    std::vector<LevelUpdate> levels;
    std::vector<TradePrint> prints;
    void clear() {
        reports.clear();
        levels.clear();
        prints.clear();
    }
};

// Circuit breaker: if a trade prints more than band_bps away from the
// reference price, the symbol halts for `halt` ns. The reference is the first
// trade after start or a halt, and rolls to the latest trade every `window` ns.
// band_bps = 0 turns it off.
struct BreakerConfig {
    uint32_t band_bps = 0;
    Nanos window = 0;
    Nanos halt = 0;
};

class Venue {
public:
    static constexpr size_t kTradeHistory = 1024;
    using TradeHistory = Ring<TradePrint, kTradeHistory>;

    explicit Venue(BreakerConfig breaker = {}) : breaker_(breaker) {}
    Venue(const Venue&) = delete;  // books publish into changes_ by address
    Venue& operator=(const Venue&) = delete;

    SymbolId add_symbol(size_t capacity = 1 << 16) {
        books_.emplace_back(capacity);
        syms_.emplace_back();
        for (OrderBook& b : books_) b.publish_levels_to(&changes_);  // vector may have moved
        return SymbolId(books_.size() - 1);
    }

    // Simulated time; the event loop sets it before delivering each request.
    void set_time(Nanos now) { now_ = now; }
    bool halted(SymbolId sym) const { return now_ < syms_[sym].halted_until; }

    const OrderBook& book(SymbolId sym) const { return books_[sym]; }
    const TradeHistory& trades(SymbolId sym) const { return syms_[sym].recent; }
    size_t symbols() const { return books_.size(); }

    void on_request(const VenueRequest& rq, VenueOut& out) {
        if (auto* n = std::get_if<NewOrder>(&rq)) on_new(*n, out);
        else if (auto* c = std::get_if<CancelRequest>(&rq)) on_cancel(*c, out);
        else on_replace(std::get<ReplaceRequest>(rq), out);
    }

    void on_new(const NewOrder& o, VenueOut& out) {
        if (o.sym >= books_.size()) return report(out, o.id, o.sym, ExecType::Rejected, Reject::UnknownSymbol);
        if (halted(o.sym)) return report(out, o.id, o.sym, ExecType::Rejected, Reject::Halted);
        Sym& st = syms_[o.sym];
        if (st.stop_ids.count(o.id) || (o.type == OrdType::StopLimit && books_[o.sym].leaves(o.id)))
            return report(out, o.id, o.sym, ExecType::Rejected, Reject::DuplicateId);
        if (o.type == OrdType::StopLimit) {
            if (o.qty == 0) return report(out, o.id, o.sym, ExecType::Rejected, Reject::ZeroQty);
            st.stop_ids[o.id] = {o.side, o.stop_px};
            if (o.side == Side::Buy) st.buy_stops.emplace(o.stop_px, o);
            else st.sell_stops.emplace(o.stop_px, o);
            report(out, o.id, o.sym, ExecType::New, Reject::None, 0, 0, o.qty);
        } else {
            execute(o, true, out);
        }
        run_stops(o.sym, out);
    }

    void on_cancel(const CancelRequest& c, VenueOut& out) {
        if (c.sym >= books_.size())
            return report(out, c.id, c.sym, ExecType::CancelRejected, Reject::UnknownSymbol);
        if (take_stop(c.sym, c.id)) return report(out, c.id, c.sym, ExecType::Cancelled, Reject::None);
        if (!books_[c.sym].cancel(c.id))
            return report(out, c.id, c.sym, ExecType::CancelRejected, Reject::UnknownOrder);
        report(out, c.id, c.sym, ExecType::Cancelled, Reject::None);
        flush_levels(c.sym, out);
    }

    // Cancel-replace: the order keeps its id, loses time priority, and may trade.
    void on_replace(const ReplaceRequest& rq, VenueOut& out) {
        if (rq.sym >= books_.size())
            return report(out, rq.id, rq.sym, ExecType::ReplaceRejected, Reject::UnknownSymbol);
        if (halted(rq.sym)) return report(out, rq.id, rq.sym, ExecType::ReplaceRejected, Reject::Halted);
        if (NewOrder* stop = find_stop(rq.sym, rq.id)) {  // new limit price/qty, same trigger
            if (rq.qty == 0) return report(out, rq.id, rq.sym, ExecType::ReplaceRejected, Reject::ZeroQty);
            stop->px = rq.px;
            stop->qty = rq.qty;
            return report(out, rq.id, rq.sym, ExecType::Replaced, Reject::None, 0, 0, rq.qty);
        }
        OrderResult r = books_[rq.sym].modify(rq.id, rq.px, rq.qty, trades_);
        if (!r) return report(out, rq.id, rq.sym, ExecType::ReplaceRejected, r.reason);
        report(out, rq.id, rq.sym, ExecType::Replaced, Reject::None, 0, 0, rq.qty);
        fills(rq.sym, rq.id, rq.qty, out);
        if (r.status == OrderResult::Cancelled) report(out, rq.id, rq.sym, ExecType::Cancelled, r.reason);
        flush_levels(rq.sym, out);
        check_breaker(rq.sym);
        run_stops(rq.sym, out);
    }

    // Session close: every resting Day order and untriggered Day stop expires.
    void end_of_day(VenueOut& out) {
        for (SymbolId s = 0; s < books_.size(); ++s) {
            expired_.clear();
            books_[s].end_of_day(expired_);
            size_t first_stop = expired_.size();
            // Walk the ordered stop books (not the hash index) so report order is deterministic.
            for (auto& [px, o] : syms_[s].buy_stops)
                if (o.tif == Tif::Day) expired_.push_back(o.id);
            for (auto& [px, o] : syms_[s].sell_stops)
                if (o.tif == Tif::Day) expired_.push_back(o.id);
            for (size_t i = first_stop; i < expired_.size(); ++i) take_stop(s, expired_[i]);
            for (OrderId id : expired_) report(out, id, s, ExecType::Expired, Reject::None);
            flush_levels(s, out);
        }
    }

private:
    // Per-symbol state the book doesn't own: last trade and the stop book.
    struct Sym {
        bool has_last = false;
        Price last = 0;
        std::multimap<Price, NewOrder> buy_stops;                        // lowest trigger first
        std::multimap<Price, NewOrder, std::greater<Price>> sell_stops;  // highest trigger first
        std::unordered_map<OrderId, std::pair<Side, Price>> stop_ids;     // id -> where it waits
        bool has_ref = false;  // circuit breaker reference
        Price ref = 0;
        Nanos ref_time = 0;
        Nanos halted_until = 0;
        TradeHistory recent;   // last kTradeHistory prints
    };

    // ponytail: checks after the whole sweep, so one aggressive order can print
    // beyond the band before the halt; reject the crossing part (LULD-style) if needed.
    void check_breaker(SymbolId s) {
        Sym& st = syms_[s];
        if (!breaker_.band_bps || !st.has_last) return;
        if (!st.has_ref) {
            st.has_ref = true;
            st.ref = st.last;
            st.ref_time = now_;
            return;
        }
        double move = double(std::llabs(st.last - st.ref)) * 10000.0;
        if (move > double(std::llabs(st.ref)) * breaker_.band_bps) {
            st.halted_until = now_ + breaker_.halt;
            st.has_ref = false;  // first trade after the halt sets a new reference
        } else if (now_ - st.ref_time >= breaker_.window) {
            st.ref = st.last;
            st.ref_time = now_;
        }
    }

    void execute(const NewOrder& o, bool ack, VenueOut& out) {
        OrderBook& b = books_[o.sym];
        OrderResult r = o.type == OrdType::Market
                            ? b.market(o.id, o.side, o.qty, trades_, o.account)
                            : b.limit(o.id, o.side, o.px, o.qty, trades_, o.tif, o.account);
        if (!r) return report(out, o.id, o.sym, ExecType::Rejected, r.reason);
        if (ack) report(out, o.id, o.sym, ExecType::New, Reject::None, 0, 0, o.qty);
        fills(o.sym, o.id, o.qty, out);
        if (r.status == OrderResult::Cancelled) report(out, o.id, o.sym, ExecType::Cancelled, r.reason);
        flush_levels(o.sym, out);
        check_breaker(o.sym);
    }

    // Fire every stop the last trade price has reached, in trigger-price then
    // arrival order. A triggered stop can trade and move the price into more
    // stops, so keep going until nothing else fires.
    void run_stops(SymbolId s, VenueOut& out) {
        Sym& st = syms_[s];
        while (st.has_last && !halted(s)) {
            NewOrder o;
            if (!st.buy_stops.empty() && st.buy_stops.begin()->first <= st.last) {
                o = st.buy_stops.begin()->second;
            } else if (!st.sell_stops.empty() && st.sell_stops.begin()->first >= st.last) {
                o = st.sell_stops.begin()->second;
            } else {
                break;
            }
            take_stop(s, o.id);
            report(out, o.id, s, ExecType::Triggered, Reject::None, 0, 0, o.qty);
            o.type = OrdType::Limit;
            execute(o, false, out);
        }
    }

    template <class Map>
    static typename Map::iterator locate(Map& m, Price stop_px, OrderId id) {
        auto it = m.lower_bound(stop_px);  // first stop at this trigger price
        while (it->second.id != id) ++it;  // stop_ids guarantees it is in this run
        return it;
    }

    NewOrder* find_stop(SymbolId s, OrderId id) {
        Sym& st = syms_[s];
        auto w = st.stop_ids.find(id);
        if (w == st.stop_ids.end()) return nullptr;
        auto [side, px] = w->second;
        return side == Side::Buy ? &locate(st.buy_stops, px, id)->second
                                 : &locate(st.sell_stops, px, id)->second;
    }

    bool take_stop(SymbolId s, OrderId id) {
        Sym& st = syms_[s];
        auto w = st.stop_ids.find(id);
        if (w == st.stop_ids.end()) return false;
        auto [side, px] = w->second;
        if (side == Side::Buy) st.buy_stops.erase(locate(st.buy_stops, px, id));
        else st.sell_stops.erase(locate(st.sell_stops, px, id));
        st.stop_ids.erase(w);
        return true;
    }

    static void report(VenueOut& out, OrderId id, SymbolId sym, ExecType type, Reject reason,
                       Price px = 0, Qty qty = 0, Qty leaves = 0) {
        out.reports.push_back({id, sym, type, reason, px, qty, leaves});
    }

    // One fill report to each side of every trade, plus a public print.
    void fills(SymbolId sym, OrderId aggressor, Qty qty, VenueOut& out) {
        const OrderBook& b = books_[sym];
        Qty left = qty;
        for (const Trade& t : trades_) {
            bool buy = t.buy_id == aggressor;
            OrderId resting = buy ? t.sell_id : t.buy_id;
            left -= t.qty;
            report(out, aggressor, sym, ExecType::Trade, Reject::None, t.price, t.qty, left);
            report(out, resting, sym, ExecType::Trade, Reject::None, t.price, t.qty, b.leaves(resting));
            out.prints.push_back({sym, t.price, t.qty, buy ? Side::Buy : Side::Sell});
            syms_[sym].recent.push(out.prints.back());
            syms_[sym].has_last = true;
            syms_[sym].last = t.price;
        }
        trades_.clear();
    }

    void flush_levels(SymbolId sym, VenueOut& out) {
        for (const LevelChange& c : changes_) out.levels.push_back({sym, c});
        changes_.clear();
    }

    BreakerConfig breaker_;
    Nanos now_ = 0;
    std::vector<OrderBook> books_;
    std::vector<Sym> syms_;
    std::vector<Trade> trades_;          // scratch, reused across calls
    std::vector<LevelChange> changes_;   // scratch, every book publishes here
    std::vector<OrderId> expired_;       // scratch
};
