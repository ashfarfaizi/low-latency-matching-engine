#pragma once
// Single-instrument limit order book with price-time priority.
//
// Design (all choices are about predictable latency):
//  * Price ladder: one Level per tick in [min_price, max_price], stored in a
//    flat vector, so finding a level is an index, not a tree lookup.
//  * Each Level is an intrusive doubly-linked FIFO of orders, so appending at
//    the tail and unlinking from the middle are both O(1).
//  * Orders live in a pre-allocated pool with a free list: no heap allocation
//    on the hot path once the book is constructed.
//  * id -> pool-slot table gives O(1) cancel by order id. It is pre-sized and
//    pre-touched at construction so the hot path never takes a page fault.
//  * best_bid_ / best_ask_ are cached; when the best level empties we scan the
//    ladder to the next non-empty level (cheap when the book is dense near the
//    touch, which is the normal case).

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "lob/types.hpp"

namespace lob {

class OrderBook {
 public:
  static constexpr std::uint32_t kNil = UINT32_MAX;

  /// `expected_order_ids`: how many orders you expect to submit in total.
  /// The id table is pre-allocated for that many (it still grows if exceeded).
  OrderBook(Price min_price, Price max_price, std::size_t max_resting_orders, std::size_t expected_order_ids = 0)
      : min_price_(min_price),
        max_price_(max_price),
        bids_(static_cast<std::size_t>(max_price - min_price + 1)),
        asks_(static_cast<std::size_t>(max_price - min_price + 1)),
        pool_(max_resting_orders),
        best_bid_(min_price - 1),
        best_ask_(max_price + 1) {
    if (max_price < min_price) throw std::invalid_argument("max_price < min_price");
    free_.reserve(max_resting_orders);
    for (std::size_t i = max_resting_orders; i-- > 0;) free_.push_back(static_cast<std::uint32_t>(i));
    // Value-initialising touches every page now, instead of on the hot path.
    id_to_slot_.assign(std::max<std::size_t>(expected_order_ids, max_resting_orders) + 1, kNil);
    next_id_ = 1;  // id 0 is reserved (kNoOrder)
  }

  /// Submit a new order. `on_trade(const Trade&)` is called for every fill,
  /// in execution order. Returns the assigned id and fill summary.
  template <class OnTrade>
  SubmitResult submit(Side side, Price price, Qty qty, OrderType type, OnTrade&& on_trade) {
    if (qty == 0) throw std::invalid_argument("qty must be > 0");
    if (price < min_price_ || price > max_price_) throw std::out_of_range("price outside book range");

    const OrderId id = next_id_++;
    if (id >= id_to_slot_.size()) id_to_slot_.resize(id_to_slot_.size() * 2, kNil);  // rare, amortised

    Qty remaining = qty;
    if (side == Side::Buy) {
      while (remaining && best_ask_ <= price) remaining = match_level(asks_[index(best_ask_)], id, remaining, on_trade, Side::Sell);
    } else {
      while (remaining && best_bid_ >= price) remaining = match_level(bids_[index(best_bid_)], id, remaining, on_trade, Side::Buy);
    }

    const Qty filled = qty - remaining;
    if (remaining == 0 || type == OrderType::IOC) return {id, filled, false};

    rest(id, side, price, remaining);
    return {id, filled, true};
  }

  /// Cancel a resting order. O(1). Returns false if the id is unknown,
  /// already fully filled, or already cancelled.
  bool cancel(OrderId id) {
    if (id == kNoOrder || id >= next_id_) return false;
    const std::uint32_t slot = id_to_slot_[id];
    if (slot == kNil) return false;
    Order& o = pool_[slot];
    Level& lvl = level_of(o.side, o.price);
    unlink(lvl, slot);
    lvl.total -= o.qty;
    id_to_slot_[id] = kNil;
    free_.push_back(slot);
    if (lvl.head == kNil) refresh_best_after_empty(o.side, o.price);
    return true;
  }

  // ── Read-only views ────────────────────────────────────────────────────
  bool has_bid() const { return best_bid_ >= min_price_; }
  bool has_ask() const { return best_ask_ <= max_price_; }
  Price best_bid() const { return best_bid_; }
  Price best_ask() const { return best_ask_; }
  Qty depth_at(Side side, Price p) const { return level_of(side, p).total; }
  std::size_t resting_orders() const { return pool_.size() - free_.size(); }
  bool is_resting(OrderId id) const { return id != kNoOrder && id < next_id_ && id_to_slot_[id] != kNil; }
  Qty open_qty(OrderId id) const { return is_resting(id) ? pool_[id_to_slot_[id]].qty : 0; }

  /// Order ids at a price level, front of the queue first (for tests/tools).
  std::vector<OrderId> queue_at(Side side, Price p) const {
    std::vector<OrderId> out;
    for (std::uint32_t s = level_of(side, p).head; s != kNil; s = pool_[s].next) out.push_back(pool_[s].id);
    return out;
  }

 private:
  struct Order {
    OrderId id;
    Price price;
    Qty qty;
    Side side;
    std::uint32_t prev;
    std::uint32_t next;
  };

  struct Level {
    std::uint32_t head = kNil;
    std::uint32_t tail = kNil;
    Qty total = 0;
  };

  std::size_t index(Price p) const { return static_cast<std::size_t>(p - min_price_); }
  Level& level_of(Side s, Price p) { return s == Side::Buy ? bids_[index(p)] : asks_[index(p)]; }
  const Level& level_of(Side s, Price p) const { return s == Side::Buy ? bids_[index(p)] : asks_[index(p)]; }

  void unlink(Level& lvl, std::uint32_t slot) {
    Order& o = pool_[slot];
    if (o.prev != kNil) pool_[o.prev].next = o.next; else lvl.head = o.next;
    if (o.next != kNil) pool_[o.next].prev = o.prev; else lvl.tail = o.prev;
  }

  void rest(OrderId id, Side side, Price price, Qty qty) {
    if (free_.empty()) throw std::length_error("order pool exhausted");
    const std::uint32_t slot = free_.back();
    free_.pop_back();
    Level& lvl = level_of(side, price);
    pool_[slot] = Order{id, price, qty, side, lvl.tail, kNil};
    if (lvl.tail != kNil) pool_[lvl.tail].next = slot; else lvl.head = slot;
    lvl.tail = slot;
    lvl.total += qty;
    id_to_slot_[id] = slot;
    if (side == Side::Buy) { if (price > best_bid_) best_bid_ = price; }
    else { if (price < best_ask_) best_ask_ = price; }
  }

  // Fill `remaining` against the FIFO at `lvl` (the current best level of the
  // opposite side). Returns what is still unfilled.
  template <class OnTrade>
  Qty match_level(Level& lvl, OrderId taker, Qty remaining, OnTrade& on_trade, Side maker_side) {
    while (remaining && lvl.head != kNil) {
      const std::uint32_t slot = lvl.head;
      Order& maker = pool_[slot];
      const Qty q = maker.qty < remaining ? maker.qty : remaining;
      on_trade(Trade{maker.id, taker, maker.price, q});
      maker.qty -= q;
      lvl.total -= q;
      remaining -= q;
      if (maker.qty == 0) {
        const Price p = maker.price;
        unlink(lvl, slot);
        id_to_slot_[maker.id] = kNil;
        free_.push_back(slot);
        if (lvl.head == kNil) refresh_best_after_empty(maker_side, p);
      }
    }
    return remaining;
  }

  void refresh_best_after_empty(Side side, Price p) {
    if (side == Side::Buy) {
      if (p != best_bid_) return;
      Price q = p - 1;
      while (q >= min_price_ && bids_[index(q)].head == kNil) --q;
      best_bid_ = q;  // min_price_-1 means "no bids"
    } else {
      if (p != best_ask_) return;
      Price q = p + 1;
      while (q <= max_price_ && asks_[index(q)].head == kNil) ++q;
      best_ask_ = q;  // max_price_+1 means "no asks"
    }
  }

  Price min_price_;
  Price max_price_;
  std::vector<Level> bids_;
  std::vector<Level> asks_;
  std::vector<Order> pool_;
  std::vector<std::uint32_t> free_;
  std::vector<std::uint32_t> id_to_slot_;
  OrderId next_id_ = 1;
  Price best_bid_;
  Price best_ask_;
};

}  // namespace lob
