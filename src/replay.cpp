// lob_replay: replays a command journal through a fresh engine, single
// threaded, and prints the event digest. It must equal the digest the live
// server printed for the same session (deterministic replay).
//
//   lob_replay journal.bin

#include <chrono>
#include <cstdio>
#include <string>

#include "lob/engine.hpp"
#include "lob/journal.hpp"

using namespace lob;

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: lob_replay <journal>\n");
    return 2;
  }
  EngineConfig cfg;  // must match the server's config
  cfg.min_price = 1;
  cfg.max_price = 100000;
  cfg.max_resting_orders = 1 << 21;
  cfg.expected_order_ids = 1 << 23;
  cfg.queue_capacity = 1 << 16;
  MatchingEngine engine(cfg);

  JournalReader in(argv[1]);
  EventDigest digest;
  Command c;
  Event e;
  std::uint64_t commands = 0, fills = 0;
  const auto t0 = std::chrono::steady_clock::now();
  while (in.next(c)) {
    engine.handle(c);
    ++commands;
    while (engine.poll(e)) {
      digest.add(e);
      if (e.type == Event::Type::Fill) ++fills;
    }
  }
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const auto& b = engine.book();
  std::printf("replayed %llu commands -> %llu events (%llu fills) in %.3fs (%.2f M commands/s)\n", (unsigned long long)commands,
              (unsigned long long)digest.count(), (unsigned long long)fills, secs, commands / secs / 1e6);
  std::printf("final book: %zu resting orders, best bid %lld, best ask %lld\n", b.resting_orders(),
              b.has_bid() ? (long long)b.best_bid() : -1LL, b.has_ask() ? (long long)b.best_ask() : -1LL);
  std::printf("event digest %016llx\n", (unsigned long long)digest.value());
  return 0;
}
