#pragma once
// The matching engine: owns one OrderBook and runs on its own (ideally pinned)
// thread. Commands arrive on an SPSC queue from the gateway; results go back
// on another SPSC queue. The engine thread is the only thread that ever
// touches the book, so the book itself needs no locks.
//
//   gateway thread ──Command──▶ [SPSC in] ──▶ engine thread ──▶ [SPSC out] ──Event──▶ gateway thread

#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <vector>

#include "lob/messages.hpp"
#include "lob/order_book.hpp"
#include "lob/spsc_queue.hpp"

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

namespace lob {

inline bool pin_current_thread(int cpu) {
#ifdef __linux__
  if (cpu < 0) return false;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
  (void)cpu;
  return false;
#endif
}

inline void cpu_relax() {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  asm volatile("yield");
#endif
}

struct EngineConfig {
  Price min_price = 1;
  Price max_price = 100000;
  std::size_t max_resting_orders = 1 << 20;
  std::size_t expected_order_ids = 1 << 22;
  std::size_t queue_capacity = 1 << 16;  // per direction, power of two
  bool publish_market_data = true;       // emit LevelUpdate events
};

class MatchingEngine {
 public:
  explicit MatchingEngine(const EngineConfig& cfg)
      : cfg_(cfg),
        book_(cfg.min_price, cfg.max_price, cfg.max_resting_orders, cfg.expected_order_ids),
        in_(cfg.queue_capacity),
        out_(cfg.queue_capacity) {
    touched_.reserve(256);
  }

  /// Gateway side: enqueue a command. Spins (with pause) if the queue is full.
  void send(const Command& c) {
    while (!in_.try_push(c)) cpu_relax();
  }
  bool try_send(const Command& c) { return in_.try_push(c); }

  /// Gateway side: non-blocking read of the next event.
  bool poll(Event& e) { return out_.try_pop(e); }

  /// Engine thread body: busy-polls until a Stop command arrives.
  void run(int cpu = -1) {
    pin_current_thread(cpu);
    Command c;
    for (;;) {
      if (!in_.try_pop(c)) {
        cpu_relax();
        continue;
      }
      if (c.type == Command::Type::Stop) break;
      handle(c);
    }
  }

  /// Process one command synchronously (used by run(), tests and replay).
  void handle(const Command& c) {
    if (c.type == Command::Type::New) {
      handle_new(c);
    } else if (c.type == Command::Type::Cancel) {
      handle_cancel(c);
    }
  }

  const OrderBook& book() const { return book_; }
  std::uint64_t md_seq() const { return md_seq_; }

 private:
  void emit(const Event& e) {
    while (!out_.try_push(e)) cpu_relax();  // back-pressure: wait for the gateway
  }

  void touch(Side side, Price p) {
    for (auto& t : touched_)
      if (t.side == side && t.price == p) return;
    touched_.push_back({side, p});
  }

  void publish_levels() {
    if (!cfg_.publish_market_data) {
      touched_.clear();
      return;
    }
    for (auto& t : touched_) {
      Event e{};
      e.type = Event::Type::LevelUpdate;
      e.side = t.side;
      e.price = t.price;
      e.qty = book_.depth_at(t.side, t.price);
      e.md_seq = ++md_seq_;
      emit(e);
    }
    touched_.clear();
  }

  void handle_new(const Command& c) {
    if (c.qty == 0 || c.price < cfg_.min_price || c.price > cfg_.max_price) {
      reject(c);
      return;
    }
    const Side maker_side = c.side == Side::Buy ? Side::Sell : Side::Buy;
    SubmitResult r;
    try {
      r = book_.submit(c.side, c.price, c.qty, c.order_type, [&](const Trade& t) {
        Event f{};
        f.type = Event::Type::Fill;
        f.side = c.side;
        f.client_seq = c.client_seq;
        f.session = c.session;
        f.order_id = t.taker;
        f.maker_id = t.maker;
        f.price = t.price;
        f.qty = t.qty;
        f.ts = c.ts;
        emit(f);
        touch(maker_side, t.price);
      });
    } catch (const std::exception&) {  // pool exhausted
      reject(c);
      return;
    }
    if (r.resting) touch(c.side, c.price);
    Event a{};
    a.type = Event::Type::Ack;
    a.side = c.side;
    a.resting = r.resting ? 1 : 0;
    a.client_seq = c.client_seq;
    a.session = c.session;
    a.order_id = r.id;
    a.price = c.price;
    a.qty = r.filled;
    a.ts = c.ts;
    emit(a);
    publish_levels();
  }

  void handle_cancel(const Command& c) {
    Side side{};
    Price price = 0;
    const bool live = book_.is_resting(c.cancel_id);
    if (live) {
      side = book_.side_of(c.cancel_id);
      price = book_.price_of(c.cancel_id);
    }
    const bool ok = live && book_.cancel(c.cancel_id);
    Event e{};
    e.type = ok ? Event::Type::Cancelled : Event::Type::CancelReject;
    e.client_seq = c.client_seq;
    e.session = c.session;
    e.order_id = c.cancel_id;
    e.ts = c.ts;
    emit(e);
    if (ok) {
      touch(side, price);
      publish_levels();
    }
  }

  void reject(const Command& c) {
    Event e{};
    e.type = Event::Type::Reject;
    e.client_seq = c.client_seq;
    e.session = c.session;
    e.ts = c.ts;
    emit(e);
  }

  struct Touched {
    Side side;
    Price price;
  };

  EngineConfig cfg_;
  OrderBook book_;
  SpscQueue<Command> in_;
  SpscQueue<Event> out_;
  std::vector<Touched> touched_;
  std::uint64_t md_seq_ = 0;
};

}  // namespace lob
