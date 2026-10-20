#pragma once
// Fixed-size messages that cross thread boundaries (and later the wire).
// Trivially copyable, no pointers, no heap: they can be memcpy'd into a ring
// buffer slot or a network packet as-is.

#include <cstdint>

#include "lob/types.hpp"

namespace lob {

struct Command {
  enum class Type : std::uint8_t { New = 1, Cancel = 2, Stop = 3 };
  Type type{};
  Side side{};
  OrderType order_type{};
  std::uint8_t pad_[5]{};
  std::uint64_t client_seq = 0;  // chosen by the sender, echoed back in events
  std::uint64_t session = 0;     // which connection sent it (for routing replies)
  Price price = 0;
  Qty qty = 0;
  std::uint32_t pad2_ = 0;
  OrderId cancel_id = kNoOrder;  // for Cancel
  std::uint64_t ts = 0;          // sender timestamp (ticks), echoed for latency measurement
};

struct Event {
  enum class Type : std::uint8_t {
    Ack = 1,           // New accepted: order_id assigned, filled/resting summary
    Fill = 2,          // one trade (sent for taker; maker_id/taker_id both set)
    Cancelled = 3,     // cancel succeeded
    CancelReject = 4,  // unknown / already filled / already cancelled
    Reject = 5,        // invalid New (bad price/qty, pool full)
    LevelUpdate = 6,   // market data: new total qty at (side, price)
  };
  Type type{};
  Side side{};
  std::uint8_t resting = 0;
  std::uint8_t pad_[5]{};
  std::uint64_t client_seq = 0;
  std::uint64_t session = 0;
  OrderId order_id = kNoOrder;   // Ack / Cancelled / CancelReject: the order; Fill: taker
  OrderId maker_id = kNoOrder;   // Fill only
  Price price = 0;               // Fill price / level price
  Qty qty = 0;                   // Fill qty / Ack filled qty / level total qty
  std::uint32_t pad2_ = 0;
  std::uint64_t ts = 0;          // echoed from Command::ts
  std::uint64_t md_seq = 0;      // LevelUpdate: market-data sequence number
};

static_assert(sizeof(Command) == 56);
static_assert(sizeof(Event) == 72);

}  // namespace lob
