// End-to-end benchmark: gateway thread -> SPSC -> engine thread -> SPSC -> gateway.
//
// 1. Round trip ("ping-pong"): send one command, wait for its Ack/Cancelled,
//    record the round-trip time. This is the latency an order sees crossing
//    two lock-free queues and the matching engine. One order in flight.
// 2. Throughput: stream commands as fast as possible while draining events.
//
// Usage: ./build/lob_bench_engine [roundtrips=500000] [stream=5000000] [gateway_cpu=0] [engine_cpu=1]

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>

#include "lob/engine.hpp"

#if defined(__x86_64__)
#include <x86intrin.h>
static inline std::uint64_t ticks() {
  unsigned aux;
  return __rdtscp(&aux);
}
#else
static inline std::uint64_t ticks() { return std::chrono::steady_clock::now().time_since_epoch().count(); }
#endif

using namespace lob;

static double ns_per_tick() {
  auto t0 = std::chrono::steady_clock::now();
  auto c0 = ticks();
  while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(200)) {
  }
  auto c1 = ticks();
  auto t1 = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(c1 - c0);
}

static void report(const char* name, std::vector<std::uint64_t>& s, double nspt) {
  std::sort(s.begin(), s.end());
  auto pct = [&](double p) { return s[std::min(s.size() - 1, static_cast<std::size_t>(p * s.size()))] * nspt; };
  std::printf("%-16s %8zu  p50 %6.0f  p90 %6.0f  p99 %6.0f  p99.9 %7.0f ns\n", name, s.size(), pct(0.5), pct(0.9), pct(0.99), pct(0.999));
}

int main(int argc, char** argv) {
  const std::size_t kRoundTrips = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 500000;
  const std::size_t kStream = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 5000000;
  const int gw_cpu = argc > 3 ? std::atoi(argv[3]) : 0;
  const int eng_cpu = argc > 4 ? std::atoi(argv[4]) : 1;

  EngineConfig cfg;
  cfg.min_price = 1;
  cfg.max_price = 20000;
  cfg.max_resting_orders = 1 << 21;
  cfg.expected_order_ids = kRoundTrips + kStream + 200000;
  cfg.queue_capacity = 1 << 16;
  MatchingEngine eng(cfg);
  std::thread engine_thread([&] { eng.run(eng_cpu); });
  pin_current_thread(gw_cpu);

  const double nspt = ns_per_tick();
  std::mt19937_64 rng(11);
  constexpr Price kMid = 10000;
  std::vector<OrderId> live;
  live.reserve(cfg.expected_order_ids);
  Event e;
  std::uint64_t seq = 0;

  auto new_order = [&](bool aggressive) {
    Command c{};
    c.type = Command::Type::New;
    c.client_seq = ++seq;
    c.side = (rng() & 1) ? Side::Buy : Side::Sell;
    if (aggressive) {
      c.order_type = OrderType::IOC;
      c.price = c.side == Side::Buy ? kMid + 210 : kMid - 210;
      c.qty = 50 + rng() % 250;
    } else {
      c.order_type = OrderType::Limit;
      c.price = c.side == Side::Buy ? kMid - 1 - static_cast<Price>(rng() % 200) : kMid + 1 + static_cast<Price>(rng() % 200);
      c.qty = 1 + rng() % 100;
    }
    return c;
  };

  // Warm the book (and the queues) with 100k resting orders.
  for (int i = 0; i < 100000; ++i) {
    eng.send(new_order(false));
    while (eng.poll(e))
      if (e.type == Event::Type::Ack && e.resting) live.push_back(e.order_id);
  }
  while (live.size() < 100000)
    if (eng.poll(e) && e.type == Event::Type::Ack && e.resting) live.push_back(e.order_id);

  // ── 1. Round trip, one order in flight ───────────────────────────────
  std::vector<std::uint64_t> rt_new, rt_cancel, rt_cross;
  rt_new.reserve(kRoundTrips);
  rt_cancel.reserve(kRoundTrips);
  rt_cross.reserve(kRoundTrips);
  for (std::size_t i = 0; i < kRoundTrips; ++i) {
    const unsigned r = rng() % 100;
    Command c;
    if (r < 30 && !live.empty()) {
      std::size_t k = rng() % live.size();
      c = Command{};
      c.type = Command::Type::Cancel;
      c.client_seq = ++seq;
      c.cancel_id = live[k];
      live[k] = live.back();
      live.pop_back();
    } else {
      c = new_order(r >= 85);
    }
    c.ts = ticks();
    eng.send(c);
    for (;;) {  // wait for the reply to *this* command (drain fills / md on the way)
      if (!eng.poll(e)) continue;
      if (e.client_seq != c.client_seq) continue;
      const bool done = e.type == Event::Type::Ack || e.type == Event::Type::Cancelled || e.type == Event::Type::CancelReject ||
                        e.type == Event::Type::Reject;
      if (!done) continue;
      const std::uint64_t dt = ticks() - e.ts;
      if (c.type == Command::Type::Cancel) rt_cancel.push_back(dt);
      else if (c.order_type == OrderType::IOC) rt_cross.push_back(dt);
      else rt_new.push_back(dt);
      if (e.type == Event::Type::Ack && e.resting) live.push_back(e.order_id);
      break;
    }
    while (eng.poll(e)) {  // market data that followed the ack
    }
  }

  std::printf("round trip: gateway -> SPSC -> engine -> SPSC -> gateway (gateway cpu %d, engine cpu %d)\n", gw_cpu, eng_cpu);
  report("new (rests)", rt_new, nspt);
  report("cancel", rt_cancel, nspt);
  report("cross (IOC)", rt_cross, nspt);

  // ── 2. Streaming throughput ──────────────────────────────────────────
  std::uint64_t events = 0;
  auto t0 = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < kStream; ++i) {
    Command c = new_order(rng() % 100 >= 85);
    while (!eng.try_send(c))
      while (eng.poll(e)) ++events;
    while (eng.poll(e)) ++events;
  }
  // Wait until the engine has caught up (queue empty and nothing more arriving).
  auto idle_since = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - idle_since < std::chrono::milliseconds(50))
    if (eng.poll(e)) {
      ++events;
      idle_since = std::chrono::steady_clock::now();
    }
  auto t1 = std::chrono::steady_clock::now();
  const double secs = std::chrono::duration<double>(t1 - t0).count() - 0.05;
  std::printf("\nstreaming: %zu commands, %llu events in %.2fs -> %.2f M commands/s, %.2f M events/s\n", kStream,
              static_cast<unsigned long long>(events), secs, kStream / secs / 1e6, events / secs / 1e6);

  Command stop{};
  stop.type = Command::Type::Stop;
  eng.send(stop);
  engine_thread.join();
  return 0;
}
