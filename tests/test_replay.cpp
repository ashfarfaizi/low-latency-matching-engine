// Journal + deterministic replay: record what a threaded engine was fed,
// replay it single-threaded, and require an identical event digest.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "lob/engine.hpp"
#include "lob/journal.hpp"

using namespace lob;

static int g_failed = 0;
static int g_checks = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    ++g_checks;                                                              \
    if (!(cond)) {                                                           \
      ++g_failed;                                                            \
      std::fprintf(stderr, "  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    }                                                                        \
  } while (0)

static EngineConfig small_cfg() {
  EngineConfig cfg;
  cfg.min_price = 1;
  cfg.max_price = 2000;
  cfg.max_resting_orders = 1 << 18;
  cfg.expected_order_ids = 1 << 18;
  cfg.queue_capacity = 1 << 12;
  return cfg;
}

static void test_live_vs_replay_digest() {
  const std::string path = "lob_test_journal.bin";
  std::mt19937_64 rng(2024);
  std::uint64_t live_digest = 0, live_events = 0, journaled = 0;
  {
    MatchingEngine eng(small_cfg());
    std::thread t([&] { eng.run(); });
    JournalWriter journal(path);
    EventDigest digest;
    Event e;
    std::uint64_t news = 0;
    for (std::uint64_t i = 1; i <= 100000; ++i) {
      Command c{};
      c.client_seq = i;
      c.session = 1 + rng() % 3;
      if (news && rng() % 10 < 3) {
        c.type = Command::Type::Cancel;
        c.cancel_id = 1 + rng() % news;
      } else {
        c.type = Command::Type::New;
        c.side = (rng() & 1) ? Side::Buy : Side::Sell;
        c.order_type = rng() % 8 ? OrderType::Limit : OrderType::IOC;
        c.price = 900 + static_cast<Price>(rng() % 200);
        c.qty = 1 + static_cast<Qty>(rng() % 50);
        ++news;
      }
      c.ts = rng();  // live timestamps differ from replay; must not matter
      journal.append(c);
      while (!eng.try_send(c))
        while (eng.poll(e)) digest.add(e);
      while (eng.poll(e)) digest.add(e);
    }
    journaled = journal.count();
    // Drain everything the engine still owes us, then stop it.
    auto idle = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - idle < std::chrono::milliseconds(100))
      if (eng.poll(e)) {
        digest.add(e);
        idle = std::chrono::steady_clock::now();
      }
    Command stop{};
    stop.type = Command::Type::Stop;
    eng.send(stop);
    t.join();
    live_digest = digest.value();
    live_events = digest.count();
  }

  auto replay = [&](std::uint64_t& events, std::uint64_t& commands) {
    MatchingEngine eng(small_cfg());
    JournalReader in(path);
    EventDigest d;
    Command c;
    Event e;
    commands = 0;
    while (in.next(c)) {
      eng.handle(c);
      ++commands;
      while (eng.poll(e)) d.add(e);
    }
    events = d.count();
    return d.value();
  };
  std::uint64_t ev1, ev2, cmd1, cmd2;
  const std::uint64_t r1 = replay(ev1, cmd1);
  const std::uint64_t r2 = replay(ev2, cmd2);
  CHECK(cmd1 == journaled);
  CHECK(ev1 == live_events);
  CHECK(r1 == live_digest);  // replay reproduces the live run exactly
  CHECK(r1 == r2);           // and replay is itself deterministic
  std::printf("  %llu commands, %llu events, digest %016llx (live == replay)\n", (unsigned long long)journaled,
              (unsigned long long)live_events, (unsigned long long)live_digest);
  std::remove(path.c_str());
}

static void test_digest_detects_difference() {
  Event a{};
  a.type = Event::Type::Fill;
  a.qty = 5;
  Event b = a;
  b.qty = 6;
  EventDigest d1, d2, d3;
  d1.add(a);
  d2.add(b);
  d3.add(a);
  CHECK(d1.value() != d2.value());
  CHECK(d1.value() == d3.value());
  Event c = a;
  c.ts = 999;  // timestamps are ignored
  EventDigest d4;
  d4.add(c);
  CHECK(d4.value() == d1.value());
}

static void test_rejects_non_journal() {
  const char* path = "lob_not_a_journal.bin";
  if (std::FILE* f = std::fopen(path, "wb")) {
    std::fputs("hello", f);
    std::fclose(f);
  }
  bool threw = false;
  try {
    JournalReader r(path);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  CHECK(threw);
  std::remove(path);
}

int main() {
  struct { const char* name; void (*fn)(); } tests[] = {
      {"live_vs_replay_digest", test_live_vs_replay_digest},
      {"digest_detects_difference", test_digest_detects_difference},
      {"rejects_non_journal", test_rejects_non_journal},
  };
  for (auto& t : tests) {
    int before = g_failed;
    t.fn();
    std::printf("%s %s\n", g_failed == before ? "PASS" : "FAIL", t.name);
  }
  std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
