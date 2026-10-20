// Tests for the SPSC queue and the threaded matching engine.
// Build with -DLOB_TSAN=ON to run these under ThreadSanitizer.

#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>

#include "lob/engine.hpp"
#include "lob/spsc_queue.hpp"

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

static void test_spsc_basic() {
  SpscQueue<int> q(4);
  int v = 0;
  CHECK(!q.try_pop(v));
  CHECK(q.try_push(1) && q.try_push(2) && q.try_push(3) && q.try_push(4));
  CHECK(!q.try_push(5));  // full
  CHECK(q.try_pop(v) && v == 1);
  CHECK(q.try_push(5));   // wraps around
  for (int want = 2; want <= 5; ++want) CHECK(q.try_pop(v) && v == want);
  CHECK(!q.try_pop(v));
  bool threw = false;
  try {
    SpscQueue<int> bad(6);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
}

static void test_spsc_two_threads() {
  constexpr std::uint64_t kN = 5'000'000;
  SpscQueue<std::uint64_t> q(1024);
  std::uint64_t bad_order = 0, sum = 0;
  std::thread consumer([&] {
    std::uint64_t expect = 0, v;
    while (expect < kN) {
      if (q.try_pop(v)) {
        if (v != expect) ++bad_order;
        sum += v;
        ++expect;
      }
    }
  });
  for (std::uint64_t i = 0; i < kN; ++i)
    while (!q.try_push(i)) {
    }
  consumer.join();
  CHECK(bad_order == 0);
  CHECK(sum == kN * (kN - 1) / 2);
  std::printf("  spsc: %llu items across threads, in order\n", static_cast<unsigned long long>(kN));
}

// A deterministic command stream. Every New is valid, so the engine assigns
// ids 1, 2, 3, ... in order, which lets us pre-compute cancel targets.
static std::vector<Command> make_commands(std::size_t n, std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::vector<Command> cmds;
  cmds.reserve(n);
  std::uint64_t news = 0;
  for (std::size_t i = 0; i < n; ++i) {
    Command c{};
    c.client_seq = i + 1;
    if (news > 0 && rng() % 10 < 3) {
      c.type = Command::Type::Cancel;
      c.cancel_id = 1 + rng() % news;  // may already be filled/cancelled
    } else {
      c.type = Command::Type::New;
      c.side = (rng() & 1) ? Side::Buy : Side::Sell;
      c.order_type = (rng() % 8 == 0) ? OrderType::IOC : OrderType::Limit;
      c.price = 900 + static_cast<Price>(rng() % 200);
      c.qty = 1 + static_cast<Qty>(rng() % 50);
      ++news;
    }
    cmds.push_back(c);
  }
  return cmds;
}

static bool same_event(const Event& a, const Event& b) {
  return a.type == b.type && a.side == b.side && a.resting == b.resting && a.client_seq == b.client_seq &&
         a.order_id == b.order_id && a.maker_id == b.maker_id && a.price == b.price && a.qty == b.qty &&
         a.md_seq == b.md_seq;
}

static void test_threaded_engine_matches_synchronous() {
  EngineConfig cfg;
  cfg.min_price = 1;
  cfg.max_price = 2000;
  cfg.max_resting_orders = 1 << 18;
  cfg.expected_order_ids = 1 << 18;
  cfg.queue_capacity = 1 << 12;
  const auto cmds = make_commands(100000, 99);

  // Reference: run every command synchronously on this thread.
  std::vector<Event> expected;
  {
    MatchingEngine eng(cfg);
    Event e;
    for (const auto& c : cmds) {
      eng.handle(c);
      while (eng.poll(e)) expected.push_back(e);
    }
  }

  // Under test: engine on its own thread, gateway here, queues in between.
  std::vector<Event> got;
  got.reserve(expected.size());
  {
    MatchingEngine eng(cfg);
    std::thread engine_thread([&] { eng.run(); });
    Event e;
    for (const auto& c : cmds) {
      while (!eng.try_send(c))
        while (eng.poll(e)) got.push_back(e);  // drain while waiting, avoids deadlock
      while (eng.poll(e)) got.push_back(e);
    }
    while (got.size() < expected.size())
      if (eng.poll(e)) got.push_back(e);
    Command stop{};
    stop.type = Command::Type::Stop;
    eng.send(stop);
    engine_thread.join();
  }

  CHECK(got.size() == expected.size());
  bool same = got.size() == expected.size();
  for (std::size_t i = 0; same && i < got.size(); ++i) same = same_event(got[i], expected[i]);
  CHECK(same);
  std::printf("  engine: %zu commands -> %zu events, threaded == synchronous\n", cmds.size(), got.size());
}

static void test_market_data_sequence_is_gapless() {
  EngineConfig cfg;
  cfg.min_price = 1;
  cfg.max_price = 2000;
  cfg.max_resting_orders = 1 << 16;
  cfg.expected_order_ids = 1 << 16;
  MatchingEngine eng(cfg);
  Event e;
  std::uint64_t last = 0;
  bool gapless = true;
  for (const auto& c : make_commands(20000, 5)) {
    eng.handle(c);
    while (eng.poll(e))
      if (e.type == Event::Type::LevelUpdate) {
        gapless = gapless && e.md_seq == last + 1;
        last = e.md_seq;
        gapless = gapless && e.qty == eng.book().depth_at(e.side, e.price);
      }
  }
  CHECK(gapless);
  CHECK(last > 0);
}

static void test_rejects_invalid_orders() {
  EngineConfig cfg;
  cfg.min_price = 10;
  cfg.max_price = 20;
  cfg.max_resting_orders = 16;
  cfg.expected_order_ids = 16;
  MatchingEngine eng(cfg);
  Event e;
  Command c{};
  c.type = Command::Type::New;
  c.side = Side::Buy;
  c.price = 5;  // below range
  c.qty = 1;
  c.client_seq = 7;
  eng.handle(c);
  CHECK(eng.poll(e) && e.type == Event::Type::Reject && e.client_seq == 7);
  c.price = 15;
  c.qty = 0;  // zero qty
  eng.handle(c);
  CHECK(eng.poll(e) && e.type == Event::Type::Reject);
  Command x{};
  x.type = Command::Type::Cancel;
  x.cancel_id = 12345;
  eng.handle(x);
  CHECK(eng.poll(e) && e.type == Event::Type::CancelReject);
}

int main() {
  struct { const char* name; void (*fn)(); } tests[] = {
      {"spsc_basic", test_spsc_basic},
      {"spsc_two_threads", test_spsc_two_threads},
      {"threaded_engine_matches_synchronous", test_threaded_engine_matches_synchronous},
      {"market_data_sequence_is_gapless", test_market_data_sequence_is_gapless},
      {"rejects_invalid_orders", test_rejects_invalid_orders},
  };
  for (auto& t : tests) {
    int before = g_failed;
    t.fn();
    std::printf("%s %s\n", g_failed == before ? "PASS" : "FAIL", t.name);
  }
  std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
