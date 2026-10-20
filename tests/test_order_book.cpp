// Unit tests + a randomized differential test against a simple reference book.
// No external framework: build and run with `ctest` or ./build/lob_tests.

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "lob/order_book.hpp"

using namespace lob;

static int g_failed = 0;
static int g_checks = 0;
#define CHECK(cond)                                                              \
  do {                                                                           \
    ++g_checks;                                                                  \
    if (!(cond)) {                                                               \
      ++g_failed;                                                                \
      std::fprintf(stderr, "  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);     \
    }                                                                            \
  } while (0)

struct Recorder {
  std::vector<Trade> trades;
  void operator()(const Trade& t) { trades.push_back(t); }
};

static void test_rest_and_best_prices() {
  OrderBook b(1, 1000, 64);
  Recorder r;
  CHECK(!b.has_bid() && !b.has_ask());
  b.submit(Side::Buy, 100, 10, OrderType::Limit, r);
  b.submit(Side::Buy, 101, 5, OrderType::Limit, r);
  b.submit(Side::Sell, 105, 7, OrderType::Limit, r);
  CHECK(b.best_bid() == 101 && b.best_ask() == 105);
  CHECK(b.depth_at(Side::Buy, 100) == 10);
  CHECK(r.trades.empty());
  CHECK(b.resting_orders() == 3);
}

static void test_price_time_priority() {
  OrderBook b(1, 1000, 64);
  Recorder r;
  auto a = b.submit(Side::Sell, 100, 5, OrderType::Limit, r).id;  // first in queue
  auto c = b.submit(Side::Sell, 100, 5, OrderType::Limit, r).id;  // second
  auto d = b.submit(Side::Sell, 99, 5, OrderType::Limit, r).id;   // better price
  auto res = b.submit(Side::Buy, 100, 8, OrderType::Limit, r);
  CHECK(res.filled == 8 && !res.resting);
  CHECK(r.trades.size() == 2);
  CHECK(r.trades[0].maker == d && r.trades[0].price == 99 && r.trades[0].qty == 5);   // best price first
  CHECK(r.trades[1].maker == a && r.trades[1].price == 100 && r.trades[1].qty == 3);  // then earliest at 100
  CHECK(b.open_qty(a) == 2 && b.open_qty(c) == 5);
  CHECK((b.queue_at(Side::Sell, 100) == std::vector<OrderId>{a, c}));
}

static void test_partial_fill_rests_remainder() {
  OrderBook b(1, 1000, 64);
  Recorder r;
  b.submit(Side::Sell, 100, 4, OrderType::Limit, r);
  auto res = b.submit(Side::Buy, 102, 10, OrderType::Limit, r);
  CHECK(res.filled == 4 && res.resting);
  CHECK(b.best_bid() == 102 && b.depth_at(Side::Buy, 102) == 6);
  CHECK(!b.has_ask());
  CHECK(r.trades.size() == 1 && r.trades[0].price == 100);  // trades at maker price
}

static void test_sweep_multiple_levels() {
  OrderBook b(1, 1000, 64);
  Recorder r;
  for (Price p = 100; p < 105; ++p) b.submit(Side::Buy, p, 2, OrderType::Limit, r);
  auto res = b.submit(Side::Sell, 101, 100, OrderType::IOC, r);
  CHECK(res.filled == 8 && !res.resting);  // 104,103,102,101 swept; 100 not crossed
  CHECK(r.trades.size() == 4 && r.trades.front().price == 104 && r.trades.back().price == 101);
  CHECK(b.best_bid() == 100);
  CHECK(!b.has_ask());  // IOC remainder never rests
}

static void test_cancel() {
  OrderBook b(1, 1000, 64);
  Recorder r;
  auto x = b.submit(Side::Buy, 100, 1, OrderType::Limit, r).id;
  auto y = b.submit(Side::Buy, 100, 2, OrderType::Limit, r).id;
  auto z = b.submit(Side::Buy, 100, 3, OrderType::Limit, r).id;
  CHECK(b.cancel(y));                       // middle of the FIFO
  CHECK(!b.cancel(y));                      // double cancel
  CHECK(!b.cancel(9999) && !b.cancel(kNoOrder));
  CHECK((b.queue_at(Side::Buy, 100) == std::vector<OrderId>{x, z}));
  CHECK(b.depth_at(Side::Buy, 100) == 4);
  CHECK(b.cancel(x) && b.cancel(z));
  CHECK(!b.has_bid());                      // best price recovered after emptying
}

static void test_best_price_recovery_after_cancel() {
  OrderBook b(1, 1000, 64);
  Recorder r;
  b.submit(Side::Sell, 110, 1, OrderType::Limit, r);
  auto best = b.submit(Side::Sell, 105, 1, OrderType::Limit, r).id;
  CHECK(b.best_ask() == 105);
  b.cancel(best);
  CHECK(b.best_ask() == 110);
}

static void test_filled_order_cannot_be_cancelled() {
  OrderBook b(1, 1000, 64);
  Recorder r;
  auto m = b.submit(Side::Sell, 100, 5, OrderType::Limit, r).id;
  b.submit(Side::Buy, 100, 5, OrderType::Limit, r);
  CHECK(!b.cancel(m));
  CHECK(b.resting_orders() == 0);
}

static void test_pool_slots_are_reused() {
  OrderBook b(1, 1000, 4);  // tiny pool
  Recorder r;
  for (int i = 0; i < 1000; ++i) {
    auto id = b.submit(Side::Buy, 100, 1, OrderType::Limit, r).id;
    CHECK(b.cancel(id));
  }
  CHECK(b.resting_orders() == 0);
}

// ── Differential test: compare against a slow, obviously-correct book ──────
struct RefBook {
  struct O { OrderId id; Qty qty; };
  std::map<Price, std::deque<O>, std::greater<Price>> bids;  // best (highest) first
  std::map<Price, std::deque<O>> asks;                       // best (lowest) first
  std::vector<Trade> trades;
  OrderId next_id = 1;

  template <class Book>
  Qty match(Book& book, OrderId taker, Qty rem, std::function<bool(Price)> crosses) {
    while (rem && !book.empty() && crosses(book.begin()->first)) {
      auto& q = book.begin()->second;
      auto& m = q.front();
      Qty f = std::min(m.qty, rem);
      trades.push_back({m.id, taker, book.begin()->first, f});
      m.qty -= f;
      rem -= f;
      if (!m.qty) q.pop_front();
      if (q.empty()) book.erase(book.begin());
    }
    return rem;
  }
  OrderId submit(Side s, Price p, Qty qty, OrderType t) {
    OrderId id = next_id++;
    Qty rem = s == Side::Buy ? match(asks, id, qty, [p](Price a) { return a <= p; })
                             : match(bids, id, qty, [p](Price b) { return b >= p; });
    if (rem && t == OrderType::Limit) (s == Side::Buy ? bids[p] : asks[p]).push_back({id, rem});
    return id;
  }
  template <class Book>
  static bool erase_from(Book& book, OrderId id) {
    for (auto it = book.begin(); it != book.end(); ++it)
      for (auto o = it->second.begin(); o != it->second.end(); ++o)
        if (o->id == id) {
          it->second.erase(o);
          if (it->second.empty()) book.erase(it);
          return true;
        }
    return false;
  }
  bool cancel(OrderId id) { return erase_from(bids, id) || erase_from(asks, id); }
};

static void test_differential_fuzz() {
  constexpr int kOps = 200000;
  OrderBook b(1, 2000, 200000);
  RefBook ref;
  std::vector<Trade> got;
  auto rec = [&](const Trade& t) { got.push_back(t); };
  std::mt19937_64 rng(42);
  std::vector<OrderId> ids;
  int mismatches = 0;

  for (int i = 0; i < kOps; ++i) {
    int kind = static_cast<int>(rng() % 10);
    if (kind < 3 && !ids.empty()) {  // cancel a random known id (may already be gone)
      OrderId id = ids[rng() % ids.size()];
      if (b.cancel(id) != ref.cancel(id)) ++mismatches;
    } else {
      Side s = (rng() & 1) ? Side::Buy : Side::Sell;
      Price p = 950 + static_cast<Price>(rng() % 101);  // tight band => lots of crossing
      Qty q = 1 + static_cast<Qty>(rng() % 20);
      OrderType t = (rng() % 8 == 0) ? OrderType::IOC : OrderType::Limit;
      OrderId a = b.submit(s, p, q, t, rec).id;
      OrderId e = ref.submit(s, p, q, t);
      if (a != e) ++mismatches;
      ids.push_back(a);
    }
    if (i % 997 == 0) {  // periodically compare the visible book
      Price rb = ref.bids.empty() ? 0 : ref.bids.begin()->first;
      Price ra = ref.asks.empty() ? 0 : ref.asks.begin()->first;
      if ((b.has_bid() ? b.best_bid() : 0) != rb) ++mismatches;
      if ((b.has_ask() ? b.best_ask() : 0) != ra) ++mismatches;
    }
  }
  CHECK(mismatches == 0);
  CHECK(got.size() == ref.trades.size());
  bool same = got.size() == ref.trades.size();
  for (std::size_t i = 0; same && i < got.size(); ++i)
    same = got[i].maker == ref.trades[i].maker && got[i].taker == ref.trades[i].taker &&
           got[i].price == ref.trades[i].price && got[i].qty == ref.trades[i].qty;
  CHECK(same);
  // Full depth comparison at the end.
  bool depth_ok = true;
  for (Price p = 1; p <= 2000; ++p) {
    Qty rb = 0, ra = 0;
    if (auto it = ref.bids.find(p); it != ref.bids.end()) for (auto& o : it->second) rb += o.qty;
    if (auto it = ref.asks.find(p); it != ref.asks.end()) for (auto& o : it->second) ra += o.qty;
    depth_ok = depth_ok && b.depth_at(Side::Buy, p) == rb && b.depth_at(Side::Sell, p) == ra;
  }
  CHECK(depth_ok);
  std::printf("  fuzz: %d ops, %zu trades, identical to reference\n", kOps, got.size());
}

int main() {
  struct { const char* name; void (*fn)(); } tests[] = {
      {"rest_and_best_prices", test_rest_and_best_prices},
      {"price_time_priority", test_price_time_priority},
      {"partial_fill_rests_remainder", test_partial_fill_rests_remainder},
      {"sweep_multiple_levels", test_sweep_multiple_levels},
      {"cancel", test_cancel},
      {"best_price_recovery_after_cancel", test_best_price_recovery_after_cancel},
      {"filled_order_cannot_be_cancelled", test_filled_order_cannot_be_cancelled},
      {"pool_slots_are_reused", test_pool_slots_are_reused},
      {"differential_fuzz", test_differential_fuzz},
  };
  for (auto& t : tests) {
    int before = g_failed;
    t.fn();
    std::printf("%s %s\n", g_failed == before ? "PASS" : "FAIL", t.name);
  }
  std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
