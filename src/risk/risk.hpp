#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <vector>

#include "core/types.hpp"
#include "exchange/venue.hpp"

// Money is ticks x shares (Price * Qty), held as double like the OMS notional.
struct RiskLimits {
    Qty max_order_qty = std::numeric_limits<Qty>::max();
    double max_order_notional = INFINITY;
    Qty max_position = std::numeric_limits<Qty>::max();  // per symbol, either direction
    size_t max_open_orders = std::numeric_limits<size_t>::max();
    uint32_t collar_bps = 0;     // fat finger: limit price within this of the mark; 0 = off
    double max_loss = INFINITY;  // no new orders once PnL <= -max_loss
};

// Pre-trade risk for one account. Every check is O(1) arithmetic on running
// totals: no locks, no allocation, no scan over orders. The OMS asks check()
// before anything leaves, then reports open-quantity changes and fills back
// so the totals stay exact. mark() feeds prices from market data.
class Risk {
public:
    Risk(size_t symbols, RiskLimits limits) : lim_(limits), syms_(symbols) {}

    // old_qty: the open quantity being replaced, 0 for a new order. Position
    // is checked worst case, as if every open order on that side filled.
    Reject check(SymbolId sym, Side side, OrdType type, Price px, Qty qty, Qty old_qty = 0) const {
        if (killed()) return Reject::KillSwitch;
        if (sym >= syms_.size()) return Reject::UnknownSymbol;
        if (qty > lim_.max_order_qty) return Reject::MaxOrderQty;
        const Sym& s = syms_[sym];
        if (!s.has_mark) return Reject::NoReferencePrice;
        // ponytail: a market order is valued at the mark; a sweep can print worse.
        if (type == OrdType::Market) px = s.mark;
        else if (lim_.collar_bps &&
                 double(std::llabs(px - s.mark)) * 10000.0 > double(std::llabs(s.mark)) * lim_.collar_bps)
            return Reject::FatFinger;
        if (std::fabs(double(px)) * double(qty) > lim_.max_order_notional) return Reject::MaxNotional;
        double worst = side == Side::Buy ? double(s.pos) + double(s.open_buy)
                                         : double(s.open_sell) - double(s.pos);
        if (worst - double(old_qty) + double(qty) > double(lim_.max_position)) return Reject::MaxPosition;
        if (!old_qty && open_orders_ >= lim_.max_open_orders) return Reject::MaxOpenOrders;
        if (pnl_ <= -lim_.max_loss) return Reject::LossLimit;  // blocks reducing orders too: flattening is a human call
        return Reject::None;
    }

    // Order accepted and sent.
    void on_new(SymbolId sym, Side side, Qty qty) {
        open(syms_[sym], side) += qty;
        ++open_orders_;
    }

    // Open quantity moved (fill, replace, cancel, expiry); done = order reached a terminal state.
    void on_leaves(SymbolId sym, Side side, Qty from, Qty to, bool done) {
        Qty& o = open(syms_[sym], side);
        o = o - from + to;
        open_orders_ -= done;
    }

    // Buying q at px when the mark is m is worth q*(m - px) right now; selling is the negative.
    void on_fill(SymbolId sym, Side side, Price px, Qty qty) {
        Sym& s = syms_[sym];
        if (!s.has_mark) set_mark(s, px);
        int64_t signed_qty = side == Side::Buy ? int64_t(qty) : -int64_t(qty);
        s.pos += signed_qty;
        pnl_ += double(signed_qty) * double(s.mark - px);
    }

    // Mark to market: a position of p gains p*(new - old).
    void mark(SymbolId sym, Price px) { set_mark(syms_[sym], px); }

    // Callable from any thread. Relaxed: the flag publishes no other data.
    void kill() { killed_.store(true, std::memory_order_relaxed); }
    bool killed() const { return killed_.load(std::memory_order_relaxed); }

    int64_t position(SymbolId sym) const { return syms_[sym].pos; }
    Qty open_qty(SymbolId sym, Side side) const {
        return side == Side::Buy ? syms_[sym].open_buy : syms_[sym].open_sell;
    }
    size_t open_orders() const { return open_orders_; }
    double pnl() const { return pnl_; }  // realized + unrealized, since construction

private:
    struct Sym {
        int64_t pos = 0;
        Qty open_buy = 0;
        Qty open_sell = 0;
        Price mark = 0;
        bool has_mark = false;
    };

    static Qty& open(Sym& s, Side side) { return side == Side::Buy ? s.open_buy : s.open_sell; }

    void set_mark(Sym& s, Price px) {
        if (s.has_mark) pnl_ += double(s.pos) * double(px - s.mark);
        s.mark = px;
        s.has_mark = true;
    }

    RiskLimits lim_;
    std::vector<Sym> syms_;
    size_t open_orders_ = 0;
    double pnl_ = 0;
    std::atomic<bool> killed_{false};
};
