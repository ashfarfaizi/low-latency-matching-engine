#pragma once
#include <cstdint>

namespace lob {

using OrderId = std::uint64_t;
using Price = std::int64_t;   // integer ticks: never floating point on the hot path
using Qty = std::uint32_t;

enum class Side : std::uint8_t { Buy, Sell };

// Limit: match what crosses, rest the remainder.
// IOC (immediate-or-cancel): match what crosses, drop the remainder.
enum class OrderType : std::uint8_t { Limit, IOC };

struct Trade {
  OrderId maker;   // resting order
  OrderId taker;   // incoming order
  Price price;     // always the maker's price
  Qty qty;
};

struct SubmitResult {
  OrderId id;      // id assigned to the incoming order
  Qty filled;      // quantity traded immediately
  bool resting;    // true if a remainder now rests in the book
};

inline constexpr OrderId kNoOrder = 0;

}  // namespace lob
