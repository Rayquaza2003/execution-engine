#pragma once
#include <cstdint>

// Shared by the exchange side and the execution side.
using OrderId = uint64_t;
using Price = int64_t;  // integer ticks, never floating point
using Qty = uint64_t;
using Nanos = uint64_t;  // simulated time since session start
using AccountId = uint32_t;  // 0 = anonymous (no self-trade prevention)

enum class Side : uint8_t { Buy, Sell };

// Time in force. Day rests until end_of_day; Gtc rests until cancelled;
// Ioc fills what it can now and drops the rest; Fok fills completely or not at all.
enum class Tif : uint8_t { Day, Gtc, Ioc, Fok };

// Why an order or request was refused. Shared so every layer reports the same codes.
enum class Reject : uint8_t {
    None,
    ZeroQty,
    DuplicateId,
    PriceBand,      // outside the price domain or the side's band
    UnknownOrder,   // cancel/replace for an id that is not live
    UnknownSymbol,
    Halted,         // symbol halted by the circuit breaker
    SelfTrade,      // cancel reason: remainder would have traded with the same account
    // Pre-trade risk (the firm's own checks; the order never reaches the venue)
    KillSwitch,
    MaxOrderQty,
    MaxNotional,
    MaxPosition,       // worst case: position plus every open order on that side
    MaxOpenOrders,
    FatFinger,         // limit price too far from the mark
    LossLimit,         // mark-to-market PnL at or below the loss limit
    NoReferencePrice,  // no mark yet for the symbol, so price checks can't run
};
