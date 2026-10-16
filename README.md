# Low-Latency Matching Engine (C++20, Linux)

**Try it live: [low-latency-matching-engine.vercel.app](https://low-latency-matching-engine.vercel.app).** The order book is compiled to WebAssembly, so you can place orders against the real engine and benchmark it in your browser.

A limit-order-book exchange core written for predictable low latency. Clients send orders over **TCP** in a binary protocol. A **lock-free** queue hands them to a **pinned matching thread**. Fills come back over TCP, and book changes are published as **UDP market data** with gapless sequence numbers. Every command is **journaled**, and a replay tool proves the live run can be reproduced exactly.

About 3,000 lines of C++20, with no dependencies beyond the standard library and POSIX.

```
                      ┌──────────────── lob_server ─────────────────┐
 lob_client ──TCP──▶  │ gateway thread (epoll)                      │
 (orders)   ◀──TCP──  │   framing · sessions · ownership checks     │
                      │   journal ──▶ journal.bin                   │
                      │        │ SPSC ring          ▲ SPSC ring     │
                      │        ▼                    │               │
                      │ engine thread (pinned core, busy-poll)      │
                      │   OrderBook: price-time priority, O(1) ops  │
                      └───────────────┬─────────────────────────────┘
                                      └──UDP──▶ lob_md_listen (gap detection, book rebuild)
 lob_replay journal.bin  ──▶ same events, same digest as the live run
```

## Features by version

| Version | What | How it's verified |
|---|---|---|
| **v1: order book** | Matching by price, then arrival time (FIFO). Limit and IOC orders, trades at the resting order's price. Flat price-ladder array with an intrusive FIFO per level and an `id→slot` table, so add-to-level and cancel are both O(1). Orders come from a pre-allocated pool with a free list, so there is no allocation while matching. | 1,035 checks, including a **differential fuzz test**: 200k random operations against a simple `std::map` reference book, requiring identical trades and depth. |
| **v2: lock-free gateway** | Single-producer/single-consumer ring buffers (head and tail on separate cache lines, each side caching the other's index) between the gateway thread and the engine thread, pinned to separate cores. Market-data level updates carry gapless sequence numbers. | 5M items across threads arrive in order. The **threaded engine's events are identical to a single-threaded run**. Clean under **ThreadSanitizer**. |
| **v3: networking** | Binary wire protocol, encoded with `memcpy` and no packed structs, plus a stream reassembler for frames split across reads. epoll gateway with adaptive polling: it spins while orders are in flight and blocks when idle. Per-session routing, **order ownership** (a client can't cancel another's order), maker fill notifications, and UDP market data batched up to 100 updates per datagram. | Wire tests (including byte-by-byte delivery and malformed frames). `lob_client selftest` checks ownership, fills and cancels end to end. The listener reports **0 sequence gaps** and rebuilds the book. |
| **v4: journal and replay** | Append-only command journal written by the single producer, so journal order is engine order. A 64-bit FNV-1a digest is computed over every event, and `lob_replay` re-runs the journal single-threaded. | **The replay digest equals the live server's digest** (for example, 550k commands produced 1.68M events, both with digest `f65078e7459095f4`). The replayed final book matches the book the UDP listener rebuilt. |
| **Browser demo** | `order_book.hpp` compiled to WebAssembly with Emscripten behind a thin C API (`wasm/lob_wasm.cpp`). The page offers order entry, a live price ladder, a trade tape, a simulated market, and the benchmark workload running in the browser. Vercel installs Emscripten and builds it on every push. | Native tests of the C API (`tests/test_wasm_api.cpp`); verified in a browser on the deployed site. |
| **CI** | GitHub Actions: GCC and clang × Release, ASan+UBSan and TSan, warnings treated as errors, plus the end-to-end script. | `.github/workflows/ci.yml` |

## Measured performance

Machine: a **2-vCPU shared cloud VM** (Intel Xeon @ 2.1 GHz). Expect jitter, and expect better numbers on dedicated hardware. Latencies use `rdtscp` on pinned cores and include about 22 ns of timer overhead. Ranges cover 3 runs.

**Order book alone** (single thread, 2M timed operations on a book of about 100k orders):

| Operation | p50 | p99 | p99.9 |
|---|---|---|---|
| Add (rests in book) | 43–44 ns | 85–116 ns | 228–389 ns |
| Cancel (random live order) | 116–129 ns | 427–490 ns | 569–708 ns |
| Aggressive IOC (sweeps 1–3 levels) | 57–60 ns | 338–379 ns | 714–862 ns |

That's 6–7.5M operations per second on one core.

**Through the lock-free gateway** (gateway → SPSC → engine → SPSC → gateway, one order in flight):

| | p50 | p99 |
|---|---|---|
| New order round trip | 640–665 ns | 918–968 ns |
| Cancel round trip | 764–821 ns | 1.28–1.38 µs |
| Queues only, no engine (core-to-core ping-pong baseline) | 403–457 ns | 531–589 ns |

So about two-thirds of the engine round trip on this VM is moving cache lines between cores; matching adds roughly 200 ns. Streaming throughput is **2.1–2.4M commands per second, which produce 6.6–7.5M events per second**.

**Over TCP loopback** (client process → kernel → gateway → engine → back):

- Round trip: p50 about 13 µs and p99 about 40 µs. This is dominated by the kernel TCP stack; kernel-bypass networking is listed under "Next".
- One pipelined TCP connection: **1.3–2.9M orders per second**, because the load generator shares the VM's 2 cores with the server's two busy threads.
- Journaling costs less than the run-to-run noise.

**Tail-latency fix:** in v1, the id table grew on demand and page-faulted about every 1,024 orders, which put add p99.9 at about 1.5 µs. Pre-sizing and pre-touching it cut p99.9 to 230–390 ns, about 5× better.

## Build, test, run

```bash
cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure   # all unit / concurrency / replay tests
./scripts/e2e.sh                              # full system: TCP, UDP, journal, replay digest

# run it yourself
./build/lob_server --port 9000 --md 127.0.0.1:9001 --journal session.bin
./build/lob_md_listen 127.0.0.1 9001
./build/lob_client latency 100000
./build/lob_client flood 1000000
./build/lob_replay session.bin                # digest must equal the server's

# benchmarks
./build/lob_bench 2000000 1          # order book, pinned to cpu 1
./build/lob_bench_spsc 1000000 0 1   # queue-only baseline
./build/lob_bench_engine             # gateway <-> engine round trip + streaming

# sanitizers
cmake -S . -B build-asan -DLOB_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug && cmake --build build-asan -j && ctest --test-dir build-asan
cmake -S . -B build-tsan -DLOB_TSAN=ON -DCMAKE_BUILD_TYPE=Debug && cmake --build build-tsan -j && ctest --test-dir build-tsan
```

UDP market data is sent to any IPv4 address. Use a multicast group (for example `--md 239.1.1.1:9001`) and the listener joins it automatically.

## Layout

```
include/lob/types.hpp        ids, integer-tick prices, trades
include/lob/order_book.hpp   order book and matching (header-only)
include/lob/spsc_queue.hpp   lock-free single-producer/single-consumer ring buffer
include/lob/messages.hpp     fixed-size Command / Event structs crossing threads
include/lob/engine.hpp       matching engine thread, market-data events
include/lob/wire.hpp         TCP binary protocol, frame reassembly, UDP market-data format
include/lob/journal.hpp      command journal, event digest
src/server.cpp               epoll gateway + engine thread + UDP publisher
src/client.cpp               latency / flood / selftest client
src/md_listener.cpp          market-data listener with gap detection
src/replay.cpp               deterministic journal replay
wasm/lob_wasm.cpp            C API over the order book for the WebAssembly build
web/                         browser demo (order entry, ladder, trades, benchmark)
scripts/build-wasm.sh        Emscripten build used by Vercel
tests/                       order book, engine/SPSC, wire, replay
bench/                       order book, SPSC baseline, engine round trip
scripts/e2e.sh               full-system test used by CI
```

## Next

- [ ] Kernel bypass or `io_uring` for order entry, and busy-poll sockets (`SO_BUSY_POLL`), to cut the ~13 µs TCP floor.
- [ ] Market-data snapshots plus a recovery/retransmit channel for listeners that detect a gap.
- [ ] Multiple instruments: one engine per symbol, sharded across cores.
- [ ] Replay real public L2/L3 exchange data, and a Python harness for a simple market-making strategy.
- [ ] `perf stat` counters (cache misses, branch misses) per operation, and a `std::map` baseline for comparison.
