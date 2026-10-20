// Baseline: the cost of the queues alone. Two pinned threads bounce a message
// through two SPSC queues (A -> B -> A). Subtracting this from the engine
// round trip shows how much of it is core-to-core transfer vs matching work.
//
// Usage: ./build/lob_bench_spsc [iters=1000000] [cpu_a=0] [cpu_b=1]

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "lob/engine.hpp"
#include "lob/spsc_queue.hpp"

#include <x86intrin.h>

using namespace lob;

static inline std::uint64_t ticks() {
  unsigned aux;
  return __rdtscp(&aux);
}

int main(int argc, char** argv) {
  const std::size_t kIters = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1000000;
  const int cpu_a = argc > 2 ? std::atoi(argv[2]) : 0;
  const int cpu_b = argc > 3 ? std::atoi(argv[3]) : 1;

  SpscQueue<std::uint64_t> ping(1024), pong(1024);
  std::thread echo([&] {
    pin_current_thread(cpu_b);
    std::uint64_t v;
    for (std::size_t i = 0; i < kIters + 1000; ++i) {
      while (!ping.try_pop(v)) cpu_relax();
      while (!pong.try_push(v)) cpu_relax();
    }
  });
  pin_current_thread(cpu_a);

  auto t0 = std::chrono::steady_clock::now();
  auto c0 = ticks();
  while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(200)) {
  }
  const double nspt = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / (ticks() - c0);

  std::vector<std::uint64_t> rt;
  rt.reserve(kIters);
  std::uint64_t v;
  for (std::size_t i = 0; i < kIters + 1000; ++i) {
    const std::uint64_t s = ticks();
    while (!ping.try_push(s)) cpu_relax();
    while (!pong.try_pop(v)) cpu_relax();
    if (i >= 1000) rt.push_back(ticks() - s);  // skip warm-up
  }
  echo.join();
  std::sort(rt.begin(), rt.end());
  auto pct = [&](double p) { return rt[std::min(rt.size() - 1, static_cast<std::size_t>(p * rt.size()))] * nspt; };
  std::printf("SPSC ping-pong round trip (cpu %d <-> cpu %d), %zu iters\n", cpu_a, cpu_b, rt.size());
  std::printf("p50 %.0f  p90 %.0f  p99 %.0f  p99.9 %.0f ns\n", pct(0.5), pct(0.9), pct(0.99), pct(0.999));
  return 0;
}
