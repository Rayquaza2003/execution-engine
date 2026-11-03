#pragma once
#include <cstdint>
#include <optional>

#include "core/types.hpp"

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
