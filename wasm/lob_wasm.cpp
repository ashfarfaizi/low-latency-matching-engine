// A small C API over the real lob::OrderBook, compiled to WebAssembly with
// Emscripten for the browser demo (and natively for tests). Same header, same
// matching logic as the native engine; only this thin binding is new.
//
// JavaScript numbers are doubles, so order ids cross the boundary as double
// (exact up to 2^53) and prices/quantities as int32.

#include <chrono>
#include <cstdint>
#include <random>
#include <vector>

#include "lob/order_book.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define LOB_EXPORT extern "C" EMSCRIPTEN_KEEPALIVE
static double now_ms() { return emscripten_get_now(); }
#else
#define LOB_EXPORT extern "C"
static double now_ms() {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
#endif

using namespace lob;

namespace {
OrderBook* g_book = nullptr;
Price g_min = 1, g_max = 1;
std::vector<Trade> g_trades;     // trades from the most recent submit
SubmitResult g_last{};
std::int32_t g_depth[2 * 64];    // [price, qty] pairs for lob_depth
double g_bench[8];               // results of lob_bench
}  // namespace

LOB_EXPORT int lob_init(int min_price, int max_price, int max_orders) {
  if (max_price < min_price || max_orders <= 0) return 0;
  delete g_book;
  g_min = min_price;
  g_max = max_price;
  g_book = new OrderBook(min_price, max_price, static_cast<std::size_t>(max_orders), static_cast<std::size_t>(max_orders) * 4);
  g_trades.clear();
  g_trades.reserve(1024);
  return 1;
}

/// side: 0 = buy, 1 = sell. ioc: 0 = limit, 1 = immediate-or-cancel.
/// Returns the new order id, or 0 if the order was invalid / the pool is full.
LOB_EXPORT double lob_submit(int side, int price, int qty, int ioc) {
  g_trades.clear();
  g_last = SubmitResult{};
  if (!g_book || qty <= 0 || price < g_min || price > g_max) return 0;
  if (!ioc && g_book->resting_orders() >= static_cast<std::size_t>(g_max - g_min + 1) * 64) return 0;
  try {
    g_last = g_book->submit(side ? Side::Sell : Side::Buy, price, static_cast<Qty>(qty), ioc ? OrderType::IOC : OrderType::Limit,
                            [](const Trade& t) { g_trades.push_back(t); });
  } catch (...) {  // pool exhausted (exceptions abort under -fno-exceptions builds; we pre-check above)
    return 0;
  }
  return static_cast<double>(g_last.id);
}

LOB_EXPORT int lob_last_filled() { return static_cast<int>(g_last.filled); }
LOB_EXPORT int lob_last_resting() { return g_last.resting ? 1 : 0; }
LOB_EXPORT int lob_trade_count() { return static_cast<int>(g_trades.size()); }
LOB_EXPORT double lob_trade_maker(int i) { return static_cast<double>(g_trades[i].maker); }
LOB_EXPORT int lob_trade_price(int i) { return static_cast<int>(g_trades[i].price); }
LOB_EXPORT int lob_trade_qty(int i) { return static_cast<int>(g_trades[i].qty); }

LOB_EXPORT int lob_cancel(double id) { return g_book && g_book->cancel(static_cast<OrderId>(id)) ? 1 : 0; }
LOB_EXPORT int lob_open_qty(double id) { return g_book ? static_cast<int>(g_book->open_qty(static_cast<OrderId>(id))) : 0; }
LOB_EXPORT int lob_best_bid() { return g_book && g_book->has_bid() ? static_cast<int>(g_book->best_bid()) : 0; }
LOB_EXPORT int lob_best_ask() { return g_book && g_book->has_ask() ? static_cast<int>(g_book->best_ask()) : 0; }
LOB_EXPORT int lob_resting() { return g_book ? static_cast<int>(g_book->resting_orders()) : 0; }

/// Top `levels` non-empty levels of a side (0 = bids, 1 = asks), best first,
/// written as [price, qty] pairs into the buffer returned by lob_depth_ptr().
LOB_EXPORT int lob_depth(int side, int levels) {
  if (!g_book) return 0;
  if (levels > 64) levels = 64;
  int n = 0;
  if (side == 0) {
    if (!g_book->has_bid()) return 0;
    for (Price p = g_book->best_bid(); p >= g_min && n < levels; --p)
      if (Qty q = g_book->depth_at(Side::Buy, p)) {
        g_depth[2 * n] = static_cast<std::int32_t>(p);
        g_depth[2 * n + 1] = static_cast<std::int32_t>(q);
        ++n;
      }
  } else {
    if (!g_book->has_ask()) return 0;
    for (Price p = g_book->best_ask(); p <= g_max && n < levels; ++p)
      if (Qty q = g_book->depth_at(Side::Sell, p)) {
        g_depth[2 * n] = static_cast<std::int32_t>(p);
        g_depth[2 * n + 1] = static_cast<std::int32_t>(q);
        ++n;
      }
  }
  return n;
}
LOB_EXPORT std::int32_t* lob_depth_ptr() { return g_depth; }

/// Runs the same kind of workload as bench/bench_order_book.cpp on a private
/// book: prefill 100k orders, then time `n` passive adds, n/2 cancels of random
/// live orders and n/5 aggressive IOCs. Browsers coarsen timers, so we time
/// whole phases and report the average ns per operation.
/// Results via lob_bench_result(i): 0 add ns/op, 1 cancel ns/op, 2 IOC ns/op,
/// 3 total ops, 4 total ms, 5 trades.
LOB_EXPORT int lob_bench(int n) {
  if (n < 1000) n = 1000;
  constexpr Price kMin = 1, kMax = 20000, kMid = 10000;
  OrderBook book(kMin, kMax, static_cast<std::size_t>(n) + 200000, static_cast<std::size_t>(n) * 2 + 400000);
  std::mt19937_64 rng(7);
  std::vector<OrderId> live;
  live.reserve(static_cast<std::size_t>(n) + 100000);
  std::uint64_t trades = 0;
  auto on_trade = [&](const Trade&) { ++trades; };
  for (int i = 0; i < 100000; ++i) {
    const bool buy = i & 1;
    const Price p = buy ? kMid - 1 - static_cast<Price>(rng() % 200) : kMid + 1 + static_cast<Price>(rng() % 200);
    live.push_back(book.submit(buy ? Side::Buy : Side::Sell, p, 1 + rng() % 100, OrderType::Limit, on_trade).id);
  }
  // Pre-generate inputs so random number generation is not timed.
  struct Add { Side s; Price p; Qty q; };
  std::vector<Add> adds(static_cast<std::size_t>(n));
  for (auto& a : adds) {
    a.s = (rng() & 1) ? Side::Buy : Side::Sell;
    a.p = a.s == Side::Buy ? kMid - 1 - static_cast<Price>(rng() % 200) : kMid + 1 + static_cast<Price>(rng() % 200);
    a.q = 1 + static_cast<Qty>(rng() % 100);
  }
  const int n_cancel = n / 2, n_cross = n / 5;
  std::vector<std::uint32_t> picks(static_cast<std::size_t>(n_cancel));
  for (auto& x : picks) x = static_cast<std::uint32_t>(rng());

  const double t0 = now_ms();
  for (const auto& a : adds) {
    auto r = book.submit(a.s, a.p, a.q, OrderType::Limit, on_trade);
    if (r.resting) live.push_back(r.id);
  }
  const double t1 = now_ms();
  for (int i = 0; i < n_cancel; ++i) {
    const std::size_t k = picks[static_cast<std::size_t>(i)] % live.size();
    book.cancel(live[k]);
    live[k] = live.back();
    live.pop_back();
  }
  const double t2 = now_ms();
  for (int i = 0; i < n_cross; ++i) {
    const bool buy = i & 1;
    const Price p = buy ? (book.has_ask() ? book.best_ask() + 2 : kMid) : (book.has_bid() ? book.best_bid() - 2 : kMid);
    book.submit(buy ? Side::Buy : Side::Sell, p < kMin ? kMin : (p > kMax ? kMax : p), 50 + static_cast<Qty>(i % 250), OrderType::IOC,
                on_trade);
  }
  const double t3 = now_ms();
  g_bench[0] = (t1 - t0) * 1e6 / n;
  g_bench[1] = (t2 - t1) * 1e6 / n_cancel;
  g_bench[2] = (t3 - t2) * 1e6 / n_cross;
  g_bench[3] = n + n_cancel + n_cross;
  g_bench[4] = t3 - t0;
  g_bench[5] = static_cast<double>(trades);
  return 1;
}
LOB_EXPORT double lob_bench_result(int i) { return (i >= 0 && i < 8) ? g_bench[i] : 0; }
