#pragma once
#include <cstdint>

// Shared by the exchange side and the execution side.
using OrderId = uint64_t;
using Price = int64_t;  // integer ticks, never floating point
using Qty = uint64_t;
using Nanos = uint64_t;  // simulated time since session start

enum class Side : uint8_t { Buy, Sell };

// Why an order or request was refused. Shared so every layer reports the same codes.
enum class Reject : uint8_t {
    None,
    ZeroQty,
    DuplicateId,
    PriceBand,      // outside the price domain or the side's band
    UnknownOrder,   // cancel/replace for an id that is not live
    UnknownSymbol,
    Halted,         // symbol halted by the circuit breaker
};
