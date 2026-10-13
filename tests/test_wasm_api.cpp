// Native tests for the C API used by the WebAssembly build (wasm/lob_wasm.cpp).

#include <cstdio>
#include <cstdlib>

extern "C" {
int lob_init(int, int, int);
double lob_submit(int, int, int, int);
int lob_last_filled();
int lob_last_resting();
int lob_trade_count();
double lob_trade_maker(int);
int lob_trade_price(int);
int lob_trade_qty(int);
int lob_cancel(double);
int lob_open_qty(double);
int lob_best_bid();
int lob_best_ask();
int lob_resting();
int lob_depth(int, int);
int* lob_depth_ptr();
int lob_bench(int);
double lob_bench_result(int);
}

static int g_failed = 0, g_checks = 0;
#define CHECK(c)                                                          \
  do {                                                                    \
    ++g_checks;                                                           \
    if (!(c)) {                                                           \
      ++g_failed;                                                         \
      std::fprintf(stderr, "  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); \
    }                                                                     \
  } while (0)

int main() {
  CHECK(lob_init(1, 1000, 1000) == 1);
  CHECK(lob_submit(0, 0, 1, 0) == 0);     // price below range
  CHECK(lob_submit(0, 10, 0, 0) == 0);    // zero qty
  const double b1 = lob_submit(0, 100, 5, 0);
  const double b2 = lob_submit(0, 101, 3, 0);
  const double a1 = lob_submit(1, 105, 4, 0);
  CHECK(b1 > 0 && b2 > b1 && a1 > b2);
  CHECK(lob_best_bid() == 101 && lob_best_ask() == 105 && lob_resting() == 3);

  // Depth: bids best first.
  CHECK(lob_depth(0, 10) == 2);
  int* d = lob_depth_ptr();
  CHECK(d[0] == 101 && d[1] == 3 && d[2] == 100 && d[3] == 5);
  CHECK(lob_depth(1, 10) == 1 && lob_depth_ptr()[0] == 105);

  // Aggressive sell sweeps 101 then 100.
  const double s = lob_submit(1, 100, 6, 1);
  CHECK(s > 0 && lob_last_filled() == 6 && lob_last_resting() == 0);
  CHECK(lob_trade_count() == 2);
  CHECK(lob_trade_maker(0) == b2 && lob_trade_price(0) == 101 && lob_trade_qty(0) == 3);
  CHECK(lob_trade_maker(1) == b1 && lob_trade_price(1) == 100 && lob_trade_qty(1) == 3);
  CHECK(lob_open_qty(b1) == 2);

  CHECK(lob_cancel(b1) == 1 && lob_cancel(b1) == 0);
  CHECK(lob_best_bid() == 0);  // no bids left

  CHECK(lob_bench(20000) == 1);
  CHECK(lob_bench_result(0) > 0 && lob_bench_result(3) == 20000 + 10000 + 4000);
  std::printf("bench (native, via C API): add %.0f ns/op, cancel %.0f ns/op, ioc %.0f ns/op\n", lob_bench_result(0),
              lob_bench_result(1), lob_bench_result(2));
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
