#pragma once
#include <vector>

#include "core/types.hpp"
#include "exchange/orderbook.hpp"

// Exchange simulator: one OrderBook per symbol behind a message interface.
// Requests come in, and everything the outside world learns comes back as
// messages in VenueOut: execution reports to the order's owner, plus public
// market data (level updates and trade prints). The caller (the event loop)
// decides when those messages arrive, which is how latency is modeled.

using SymbolId = uint32_t;

enum class OrdType : uint8_t { Limit, Market };

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

enum class ExecType : uint8_t {
    New,              // accepted
    Trade,            // fill (partial or full); see last_px/last_qty/leaves
    Cancelled,        // removed by request, IOC/FOK remainder, market remainder, or STP
    Replaced,         // replace accepted; leaves is the new quantity
    Rejected,         // new order refused
    Expired,          // Day order at end of day
    CancelRejected,   // cancel refused (unknown or already gone)
    ReplaceRejected,  // replace refused; original order unchanged
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

class Venue {
public:
    Venue() = default;
    Venue(const Venue&) = delete;  // books publish into changes_ by address
    Venue& operator=(const Venue&) = delete;

    SymbolId add_symbol(size_t capacity = 1 << 16) {
        books_.emplace_back(capacity);
        for (OrderBook& b : books_) b.publish_levels_to(&changes_);  // vector may have moved
        return SymbolId(books_.size() - 1);
    }

    const OrderBook& book(SymbolId sym) const { return books_[sym]; }
    size_t symbols() const { return books_.size(); }

    void on_new(const NewOrder& o, VenueOut& out) {
        if (o.sym >= books_.size()) return report(out, o.id, o.sym, ExecType::Rejected, Reject::UnknownSymbol);
        OrderBook& b = books_[o.sym];
        OrderResult r = o.type == OrdType::Market
                            ? b.market(o.id, o.side, o.qty, trades_, o.account)
                            : b.limit(o.id, o.side, o.px, o.qty, trades_, o.tif, o.account);
        if (!r) return report(out, o.id, o.sym, ExecType::Rejected, r.reason);
        report(out, o.id, o.sym, ExecType::New, Reject::None, 0, 0, o.qty);
        fills(o.sym, o.id, o.qty, out);
        if (r.status == OrderResult::Cancelled) report(out, o.id, o.sym, ExecType::Cancelled, r.reason);
        flush_levels(o.sym, out);
    }

    void on_cancel(const CancelRequest& c, VenueOut& out) {
        if (c.sym >= books_.size())
            return report(out, c.id, c.sym, ExecType::CancelRejected, Reject::UnknownSymbol);
        if (!books_[c.sym].cancel(c.id))
            return report(out, c.id, c.sym, ExecType::CancelRejected, Reject::UnknownOrder);
        report(out, c.id, c.sym, ExecType::Cancelled, Reject::None);
        flush_levels(c.sym, out);
    }

    // Cancel-replace: the order keeps its id, loses time priority, and may trade.
    void on_replace(const ReplaceRequest& rq, VenueOut& out) {
        if (rq.sym >= books_.size())
            return report(out, rq.id, rq.sym, ExecType::ReplaceRejected, Reject::UnknownSymbol);
        OrderResult r = books_[rq.sym].modify(rq.id, rq.px, rq.qty, trades_);
        if (!r) return report(out, rq.id, rq.sym, ExecType::ReplaceRejected, r.reason);
        report(out, rq.id, rq.sym, ExecType::Replaced, Reject::None, 0, 0, rq.qty);
        fills(rq.sym, rq.id, rq.qty, out);
        if (r.status == OrderResult::Cancelled) report(out, rq.id, rq.sym, ExecType::Cancelled, r.reason);
        flush_levels(rq.sym, out);
    }

    // Session close: every resting Day order expires.
    void end_of_day(VenueOut& out) {
        for (SymbolId s = 0; s < books_.size(); ++s) {
            expired_.clear();
            books_[s].end_of_day(expired_);
            for (OrderId id : expired_) report(out, id, s, ExecType::Expired, Reject::None);
            flush_levels(s, out);
        }
    }

private:
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
        }
        trades_.clear();
    }

    void flush_levels(SymbolId sym, VenueOut& out) {
        for (const LevelChange& c : changes_) out.levels.push_back({sym, c});
        changes_.clear();
    }

    std::vector<OrderBook> books_;
    std::vector<Trade> trades_;          // scratch, reused across calls
    std::vector<LevelChange> changes_;   // scratch, every book publishes here
    std::vector<OrderId> expired_;       // scratch
};
