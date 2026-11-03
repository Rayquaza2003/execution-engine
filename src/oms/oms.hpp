#pragma once
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

#include "core/id_index.hpp"
#include "core/types.hpp"
#include "exchange/venue.hpp"
#include "risk/risk.hpp"

// Order lifecycle as seen by the trading firm.
//
//   PendingNew -> Acknowledged -> PartiallyFilled -> Filled
//        |             |   \            |
//     Rejected         |    +--> CancelPending --> Cancelled
//                      +-------> ReplacePending --> back to Acknowledged/PartiallyFilled
//
// Plus Expired (Day orders at the close). Requests and replies cross on the
// wire, so pending states must accept fills that were already in flight.
enum class OrdState : uint8_t {
    PendingNew,      // sent, no reply yet
    Acknowledged,    // live at the venue, nothing filled
    PartiallyFilled,
    Filled,          // terminal
    CancelPending,   // cancel sent, no reply yet
    ReplacePending,  // replace sent, no reply yet
    Cancelled,       // terminal
    Rejected,        // terminal
    Expired,         // terminal
};
constexpr int kOrdStates = 9;

enum class OrdEvent : uint8_t {
    CancelReq,        // local: client asks to cancel
    ReplaceReq,       // local: client asks to replace
    Ack,              // venue: New
    PartialFill,      // venue: Trade with leaves > 0
    Fill,             // venue: Trade with leaves == 0
    Cancelled,        // venue: Cancelled (requested, IOC/FOK/market remainder, STP)
    Replaced,         // venue: Replaced
    Rejected,         // venue: Rejected (new order, or a triggered stop the book refused)
    Expired,          // venue: Expired
    CancelRejected,   // venue: cancel refused
    ReplaceRejected,  // venue: replace refused
    Triggered,        // venue: stop-limit fired
};
constexpr int kOrdEvents = 12;

constexpr bool is_terminal(OrdState s) {
    return s == OrdState::Filled || s == OrdState::Cancelled || s == OrdState::Rejected ||
           s == OrdState::Expired;
}

// The whole state machine. nullopt = invalid transition (the OMS refuses a
// local request, or counts and ignores a venue message). `filled_any` picks
// where a refused cancel/replace returns to.
constexpr std::optional<OrdState> transition(OrdState s, OrdEvent e, bool filled_any) {
    using S = OrdState;
    using E = OrdEvent;
    const S live = filled_any ? S::PartiallyFilled : S::Acknowledged;
    switch (s) {
        case S::PendingNew:
            switch (e) {
                case E::Ack: return S::Acknowledged;
                case E::Rejected: return S::Rejected;
                case E::CancelReq: return S::CancelPending;  // cancel may chase an unacked new
                default: return std::nullopt;
            }
        case S::Acknowledged:
        case S::PartiallyFilled:
            switch (e) {
                case E::PartialFill: return S::PartiallyFilled;
                case E::Fill: return S::Filled;
                case E::Cancelled: return S::Cancelled;
                case E::Expired: return S::Expired;
                case E::Rejected: return S::Rejected;  // stop triggered, then refused by the book
                case E::Triggered: return s;
                case E::CancelReq: return S::CancelPending;
                case E::ReplaceReq: return S::ReplacePending;
                default: return std::nullopt;
            }
        case S::CancelPending:
            switch (e) {
                case E::Ack:          // cancel was sent before the ack arrived
                case E::Triggered:
                case E::PartialFill:  // fill was already in flight; apply it, keep waiting
                    return S::CancelPending;
                case E::Fill: return S::Filled;  // fill won the race; the cancel reject comes later
                case E::Cancelled: return S::Cancelled;
                case E::Expired: return S::Expired;
                case E::Rejected: return S::Rejected;  // the new order itself was refused
                case E::Replaced:        // a replace was in flight when the cancel went out
                case E::ReplaceRejected: return S::CancelPending;
                case E::CancelRejected: return live;
                default: return std::nullopt;
            }
        case S::ReplacePending:
            switch (e) {
                case E::Replaced:
                case E::ReplaceRejected: return live;
                case E::Triggered:
                case E::PartialFill: return S::ReplacePending;
                case E::Fill: return S::Filled;
                case E::Cancelled: return S::Cancelled;
                case E::Expired: return S::Expired;
                case E::CancelReq: return S::CancelPending;  // the venue answers the replace first
                default: return std::nullopt;
            }
        case S::Filled:
        case S::Cancelled:
        case S::Rejected:
        case S::Expired:
            switch (e) {
                case E::CancelRejected:
                case E::ReplaceRejected: return s;  // late reply to a request that lost the race
                default: return std::nullopt;
            }
    }
    return std::nullopt;
}

using ClOrdId = uint64_t;  // the client's own key for an order, unique within one OMS

struct ClientOrder {
    ClOrdId cl;
    SymbolId sym;
    Side side;
    OrdType type;
    Tif tif;
    Price px;
    Qty qty;
    Price stop_px = 0;
};

struct OmsOrder {
    ClOrdId cl;
    OrderId id;  // venue order id, assigned by the OMS from its own range
    SymbolId sym;
    Side side;
    OrdType type;
    Tif tif;
    Price px;
    Price stop_px;
    Qty leaves;           // open quantity at the venue
    Qty cum = 0;          // filled so far
    double notional = 0;  // sum of fill price * qty, for the average price
    OrdState state = OrdState::PendingNew;
    Reject last_reject = Reject::None;
    Price pending_px = 0;  // replace in flight
    Qty pending_qty = 0;

    double avg_px() const { return cum ? notional / double(cum) : 0.0; }
};

struct OmsFill {
    Nanos time;
    ClOrdId cl;
    OrderId id;
    SymbolId sym;
    Side side;
    Price px;
    Qty qty;
};

// Order management for one trading account. Turns client intents into venue
// requests and venue execution reports into order state, enforcing the state
// machine above. Pure: requests go out through a vector, reports come in
// through on_report, and the caller owns transport and timing.
//
// With a Risk attached, every new order and replace is checked before it
// leaves. A refusal comes back as a Rejected / ReplaceRejected report with the
// risk reason, through the same path as a venue reply.
class Oms {
public:
    Oms(AccountId account, OrderId id_base, Risk* risk = nullptr, size_t expected = 1 << 16)
        : account_(account), base_(id_base), risk_(risk), by_cl_(expected) {}

    void set_time(Nanos now) { now_ = now; }

    struct Submitted {
        const OmsOrder& order;
        bool duplicate;  // ClOrdId seen before: nothing was sent
    };

    // Idempotent: resubmitting a ClOrdId (say, after a timeout) returns the
    // existing order and sends nothing, so a retry can never double the position.
    Submitted submit(const ClientOrder& c, std::vector<VenueRequest>& out) {
        uint32_t slot = by_cl_.find(c.cl);
        if (slot != IdIndex::kMissing) return {orders_[slot], true};
        slot = uint32_t(orders_.size());
        OrderId id = base_ + slot;
        orders_.push_back({c.cl, id, c.sym, c.side, c.type, c.tif, c.px, c.stop_px, c.qty});
        by_cl_.insert(c.cl, slot);
        OmsOrder& o = orders_.back();
        if (risk_) {
            Reject why = risk_->check(c.sym, c.side, c.type, c.px, c.qty);
            if (why != Reject::None) {
                apply(o, {id, c.sym, ExecType::Rejected, why, 0, 0, 0});
                return {o, false};
            }
            risk_->on_new(c.sym, c.side, c.qty);
        }
        out.push_back(NewOrder{id, account_, c.sym, c.side, c.type, c.tif, c.px, c.qty, c.stop_px});
        return {o, false};
    }

    // False if the order is unknown or its state forbids it (terminal, or
    // another cancel/replace is already in flight).
    bool cancel(ClOrdId cl, std::vector<VenueRequest>& out) {
        OmsOrder* o = find_mut(cl);
        if (!o || !step(*o, OrdEvent::CancelReq)) return false;
        out.push_back(CancelRequest{o->id, o->sym});
        return true;
    }

    // qty is the new open quantity (the venue's replace semantics). False also
    // when risk refuses it; the order stays as it was, with last_reject set.
    // ponytail: FIX replaces carry total OrderQty; convert at the gateway in S10.
    // ponytail: risk keeps the old open qty until Replaced arrives, so an upsize
    // in flight is briefly undercounted; reserve max(old, new) if that matters.
    bool replace(ClOrdId cl, Price px, Qty qty, std::vector<VenueRequest>& out) {
        OmsOrder* o = find_mut(cl);
        if (!o || !step(*o, OrdEvent::ReplaceReq)) return false;
        if (risk_) {
            Reject why = risk_->check(o->sym, o->side, o->type, px, qty, o->leaves);
            if (why != Reject::None) {
                apply(*o, {o->id, o->sym, ExecType::ReplaceRejected, why, 0, 0, 0});
                return false;
            }
        }
        o->pending_px = px;
        o->pending_qty = qty;
        out.push_back(ReplaceRequest{o->id, o->sym, px, qty});
        return true;
    }

    void on_report(const ExecReport& r) {
        if (r.id < base_ || r.id - base_ >= orders_.size()) {  // another client's order
            ++foreign_;
            return;
        }
        OmsOrder& o = orders_[r.id - base_];
        Qty before = o.leaves;
        bool was_open = !is_terminal(o.state);
        if (!apply(o, r)) {
            ++invalid_;
            return;
        }
        if (risk_) {
            if (r.type == ExecType::Trade) risk_->on_fill(o.sym, o.side, r.last_px, r.last_qty);
            risk_->on_leaves(o.sym, o.side, before, o.leaves, was_open && is_terminal(o.state));
        }
    }

    const OmsOrder* find(ClOrdId cl) const {
        uint32_t slot = by_cl_.find(cl);
        return slot == IdIndex::kMissing ? nullptr : &orders_[slot];
    }

    const std::vector<OmsFill>& fills() const { return fills_; }
    size_t order_count() const { return orders_.size(); }
    size_t invalid_transitions() const { return invalid_; }  // venue messages the state machine refused
    size_t foreign_reports() const { return foreign_; }

private:
    static OrdEvent event_of(const ExecReport& r) {
        switch (r.type) {
            case ExecType::New: return OrdEvent::Ack;
            case ExecType::Trade: return r.leaves ? OrdEvent::PartialFill : OrdEvent::Fill;
            case ExecType::Cancelled: return OrdEvent::Cancelled;
            case ExecType::Replaced: return OrdEvent::Replaced;
            case ExecType::Rejected: return OrdEvent::Rejected;
            case ExecType::Expired: return OrdEvent::Expired;
            case ExecType::CancelRejected: return OrdEvent::CancelRejected;
            case ExecType::ReplaceRejected: return OrdEvent::ReplaceRejected;
            case ExecType::Triggered: return OrdEvent::Triggered;
        }
        return OrdEvent::Rejected;
    }

    bool step(OmsOrder& o, OrdEvent e) {
        std::optional<OrdState> next = transition(o.state, e, o.cum > 0);
        if (!next) return false;
        o.state = *next;
        return true;
    }

    // State transition plus its effects. False (nothing changed) if the state
    // machine refuses the report.
    bool apply(OmsOrder& o, const ExecReport& r) {
        if (!step(o, event_of(r))) return false;
        switch (r.type) {
            case ExecType::Trade:
                o.cum += r.last_qty;
                o.leaves = r.leaves;
                o.notional += double(r.last_px) * double(r.last_qty);
                fills_.push_back({now_, o.cl, o.id, o.sym, o.side, r.last_px, r.last_qty});
                break;
            case ExecType::Replaced:
                o.px = o.pending_px;
                o.leaves = r.leaves;
                break;
            case ExecType::Cancelled:
            case ExecType::Expired:
            case ExecType::Rejected:
                o.leaves = 0;
                o.last_reject = r.reason;
                break;
            case ExecType::CancelRejected:
            case ExecType::ReplaceRejected:
                o.last_reject = r.reason;
                break;
            case ExecType::New:
            case ExecType::Triggered:
                break;
        }
        return true;
    }

    OmsOrder* find_mut(ClOrdId cl) {
        uint32_t slot = by_cl_.find(cl);
        return slot == IdIndex::kMissing ? nullptr : &orders_[slot];
    }

    AccountId account_;
    OrderId base_;
    Risk* risk_;  // optional, not owned
    Nanos now_ = 0;
    std::deque<OmsOrder> orders_;  // deque: references stay valid as orders are added
    IdIndex by_cl_;                // ClOrdId -> slot in orders_
    std::vector<OmsFill> fills_;
    size_t invalid_ = 0;
    size_t foreign_ = 0;
};
