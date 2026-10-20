// Latency + throughput benchmark for the order book.
//
// Workload: the book is pre-filled with resting orders around a mid price,
// then a random mix of operations is timed one at a time:
//   ~55% passive limit adds (rest in the book)
//   ~30% cancels of a random live order
//   ~15% aggressive orders that cross the spread (IOC, 1-3 levels)
// Each op is timed with the CPU timestamp counter (rdtsc), converted to ns,
// and reported as p50 / p90 / p99 / p99.9 / max, per op type.
//
// Usage: ./build/lob_bench [ops=2000000] [cpu=-1]

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
static inline std::uint64_t ticks() {
  unsigned aux;
  return __rdtscp(&aux);
}
#else
static inline std::uint64_t ticks() {
  return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
}
#endif

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

#include "lob/order_book.hpp"

using namespace lob;

static double ns_per_tick() {
  auto t0 = std::chrono::steady_clock::now();
  std::uint64_t c0 = ticks();
  while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(200)) {
  }
  std::uint64_t c1 = ticks();
  auto t1 = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(c1 - c0);
}

static void pin_to_cpu(int cpu) {
#ifdef __linux__
  if (cpu < 0) return;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) std::fprintf(stderr, "warning: could not pin to cpu %d\n", cpu);
#else
  (void)cpu;
#endif
}

struct Stats {
  std::vector<std::uint32_t> samples;
  std::size_t n = 0;
  void prepare(std::size_t cap) { samples.assign(cap, 0); }  // pre-touch: no page faults while timing
  void add(std::uint32_t v) { samples[n++] = v; }
  void report(const char* name, double nspt) {
    samples.resize(n);
    if (samples.empty()) return;
    std::sort(samples.begin(), samples.end());
    auto pct = [&](double p) { return samples[std::min(samples.size() - 1, static_cast<std::size_t>(p * samples.size()))] * nspt; };
    std::printf("%-12s %9zu  p50 %6.0f  p90 %6.0f  p99 %6.0f  p99.9 %7.0f  max %8.0f ns\n", name, samples.size(), pct(0.50), pct(0.90),
                pct(0.99), pct(0.999), samples.back() * nspt);
  }
};

int main(int argc, char** argv) {
  const std::size_t kOps = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 2000000;
  const int cpu = argc > 2 ? std::atoi(argv[2]) : -1;
  pin_to_cpu(cpu);

  constexpr Price kMin = 1, kMax = 20000, kMid = 10000;
  constexpr std::size_t kPool = 1 << 20;
  OrderBook book(kMin, kMax, kPool, kOps + 200000);

  std::mt19937_64 rng(7);
  std::vector<OrderId> live;  // ids we believe are resting (may have been filled)
  live.reserve(kPool);
  std::uint64_t trades = 0;
  auto on_trade = [&](const Trade&) { ++trades; };

  // Pre-fill: 50k orders on each side within 200 ticks of the mid.
  for (int i = 0; i < 100000; ++i) {
    bool buy = i & 1;
    Price p = buy ? kMid - 1 - static_cast<Price>(rng() % 200) : kMid + 1 + static_cast<Price>(rng() % 200);
    live.push_back(book.submit(buy ? Side::Buy : Side::Sell, p, 1 + rng() % 100, OrderType::Limit, on_trade).id);
  }

  // Pre-generate the workload so RNG cost is not measured.
  struct Op { std::uint8_t kind; Side side; Price price; Qty qty; std::uint32_t pick; };
  std::vector<Op> ops(kOps);
  for (auto& op : ops) {
    std::uint32_t r = static_cast<std::uint32_t>(rng() % 100);
    op.side = (rng() & 1) ? Side::Buy : Side::Sell;
    op.pick = static_cast<std::uint32_t>(rng());
    if (r < 55) {
      op.kind = 0;
      op.price = op.side == Side::Buy ? kMid - 1 - static_cast<Price>(rng() % 200) : kMid + 1 + static_cast<Price>(rng() % 200);
      op.qty = 1 + static_cast<Qty>(rng() % 100);
    } else if (r < 85) {
      op.kind = 1;
    } else {
      op.kind = 2;
      op.qty = 50 + static_cast<Qty>(rng() % 250);
    }
  }

  Stats add_s, cancel_s, cross_s;
  add_s.prepare(kOps);
  cancel_s.prepare(kOps);
  cross_s.prepare(kOps);

  const double nspt = ns_per_tick();
  auto wall0 = std::chrono::steady_clock::now();
  for (const auto& op : ops) {
    if (op.kind == 0) {
      std::uint64_t t0 = ticks();
      auto res = book.submit(op.side, op.price, op.qty, OrderType::Limit, on_trade);
      std::uint64_t t1 = ticks();
      add_s.add(static_cast<std::uint32_t>(t1 - t0));
      if (res.resting) live.push_back(res.id);
    } else if (op.kind == 1) {
      if (live.empty()) continue;
      std::size_t i = op.pick % live.size();
      OrderId id = live[i];
      std::uint64_t t0 = ticks();
      book.cancel(id);
      std::uint64_t t1 = ticks();
      cancel_s.add(static_cast<std::uint32_t>(t1 - t0));
      live[i] = live.back();
      live.pop_back();
    } else {
      // Aggressive: cross the spread by a few ticks so it sweeps 1-3 levels.
      Price p = op.side == Side::Buy ? (book.has_ask() ? book.best_ask() + 2 : kMid) : (book.has_bid() ? book.best_bid() - 2 : kMid);
      p = std::clamp(p, kMin, kMax);
      std::uint64_t t0 = ticks();
      book.submit(op.side, p, op.qty, OrderType::IOC, on_trade);
      std::uint64_t t1 = ticks();
      cross_s.add(static_cast<std::uint32_t>(t1 - t0));
    }
  }
  auto wall1 = std::chrono::steady_clock::now();
  const double secs = std::chrono::duration<double>(wall1 - wall0).count();

  std::printf("ops %zu, trades %llu, resting at end %zu, cpu pin %d\n", kOps, static_cast<unsigned long long>(trades),
              book.resting_orders(), cpu);
  std::printf("throughput: %.2f M ops/s (includes timing overhead)\n\n", kOps / secs / 1e6);
  add_s.report("add (rest)", nspt);
  cancel_s.report("cancel", nspt);
  cross_s.report("cross (IOC)", nspt);

  // Timer overhead, so readers can judge the floor.
  Stats overhead;
  overhead.prepare(100000);
  for (int i = 0; i < 100000; ++i) {
    std::uint64_t t0 = ticks();
    std::uint64_t t1 = ticks();
    overhead.add(static_cast<std::uint32_t>(t1 - t0));
  }
  overhead.report("timer only", nspt);
  return 0;
}
