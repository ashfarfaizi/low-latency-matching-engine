# Low-Latency Matching Engine (C++20, Linux)

A limit-order-book matching engine built for predictable, low latency. This is the core component of an exchange or a trading system's simulator.

**Status: in active development.** Version 1 (the order book and matching core) is done and tested. The concurrency and networking layers are next.

## What's done (v1)

- **Price-time priority matching.** The best price fills first, and orders at the same price fill in arrival order (FIFO). Trades execute at the resting order's price. Limit and IOC (immediate-or-cancel) orders.
- **O(1) add-to-level and O(1) cancel**, with no tree lookups:
  - the price ladder is a flat array, one level per tick, so finding a level is an index;
  - each level is an intrusive doubly-linked FIFO, so tail append and middle unlink are both O(1);
  - an `id → slot` table finds any order for cancel in O(1).
- **No heap allocation on the hot path.** Orders live in a pre-allocated pool with a free list. The id table is pre-sized and pre-touched, so matching never takes a page fault.
- **Cached best bid and ask.** When the best level empties, the book scans to the next non-empty level. This is cheap when the book is dense near the touch.
- **Tests:** 9 suites and 1,035 checks, including a **differential fuzz test**. It runs 200,000 random adds, cancels and IOCs against a simple `std::map`-based reference book and asserts identical trades, best prices and full depth. The suite is also clean under AddressSanitizer and UBSan.
- **Benchmark harness:** times every operation with `rdtscp`, pinned to one CPU core, and reports the 50th to 99.9th percentiles by operation type.

## Measured latency

Workload: a book pre-filled with 100k orders, then **2,000,000 timed operations**. About 55% are passive adds, 30% cancels of random live orders, and 15% aggressive IOC orders that sweep 1–3 levels. Machine: a 2-vCPU cloud VM (Intel Xeon @ 2.1 GHz), shared, so expect jitter. The numbers include about 22 ns of timer overhead per sample. Ranges cover 3 runs.

| Operation | p50 | p99 | p99.9 |
|---|---|---|---|
| Add (rests in book) | 43–44 ns | 85–116 ns | 228–389 ns |
| Cancel (random live order) | 116–129 ns | 427–490 ns | 569–708 ns |
| Aggressive IOC (sweeps levels) | 57–60 ns | 338–379 ns | 714–862 ns |

**Throughput:** about 6–7.5M operations per second on one core, including timing overhead.

**A tail-latency fix along the way:** the first version grew the id table on demand. That caused a page fault about every 1,024 orders, which put add p99.9 at about 1.5 µs. Pre-sizing and pre-touching the table brought p99.9 down to 230–390 ns, about 5× better.

Cancels are slower than adds because they hit a random order, which usually means a cache miss. Adds append near the hot top of the book.

## Roadmap

- [ ] **Lock-free gateway:** an SPSC ring buffer between the network thread and the matching thread, with core pinning, measured end to end.
- [ ] **TCP order entry:** a compact binary protocol (new, cancel, ack, fill), using epoll.
- [ ] **UDP multicast market data:** incremental book updates with sequence numbers, plus a gap-detecting client.
- [ ] **Persistence and replay:** an append-only trade log, and deterministic replay of a recorded session.
- [ ] **Real data:** replay public exchange L2/L3 data through the engine; a Python harness for a simple market-making strategy.
- [ ] **Profiling:** `perf` counters (cache misses, branch misses) per operation type; compare against a `std::map` baseline.

## Build and run

```bash
cmake -S . -B build
cmake --build build -j
./build/lob_tests                 # unit + differential fuzz tests
./build/lob_bench 2000000 1       # ops, CPU core to pin to

# sanitizers
cmake -S . -B build-asan -DLOB_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan --target lob_tests && ./build-asan/lob_tests
```

## Layout

```
include/lob/types.hpp       Order ids, prices in integer ticks, trades
include/lob/order_book.hpp  The order book and matching (header-only)
tests/test_order_book.cpp   Unit tests + differential fuzz vs reference book
bench/bench_order_book.cpp  rdtscp latency benchmark, percentiles per op type
```
