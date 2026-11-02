#pragma once
#include <cstdint>

// Shared by the exchange side and the execution side.
using OrderId = uint64_t;
using Price = int64_t;  // integer ticks, never floating point
using Qty = uint64_t;
using Nanos = uint64_t;  // simulated time since session start

enum class Side : uint8_t { Buy, Sell };
