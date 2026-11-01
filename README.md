# Low-Latency Order Matching Engine (C++20)

A single-instrument limit order book with price-time priority, running on a
dedicated busy-spinning thread and fed through **lock-free SPSC ring buffers**.
Built to learn — and measure — the techniques used in low-latency trading
systems: lock-free queues, memory ordering, cache-line alignment, allocation-free
hot paths, and tail-latency benchmarking.

## Architecture

```
  Gateway thread                                   Engine thread (pinned, busy-spin)
 ┌───────────────┐   InboundQueue (SPSC, lock-free)   ┌──────────────────────────────┐
 │ stamp ts      │ ─────── OrderRequest ────────────▶ │ MatchingEngine::run()        │
 │ try_push()    │                                    │   try_pop → process()        │
 │               │                                    │     └─ OrderBook::add_limit  │
 │ try_pop()     │ ◀────── ExecReport ─────────────── │     └─ OrderBook::cancel     │
 │ latency =     │   OutboundQueue (SPSC, lock-free)  │   try_push(report)           │
 │  now - ts     │                                    └──────────────────────────────┘
 └───────────────┘

 OrderBook (owned by ONE thread → no locks)
   bids: std::map<Price, PriceLevel, greater>   asks: std::map<Price, PriceLevel, less>
   PriceLevel = intrusive doubly-linked FIFO of Order*   (O(1) append, O(1) cancel)
   orders_: unordered_map<OrderId, Order*>               (O(1) cancel lookup)
   pool_:   ObjectPool<Order>                            (no malloc per order)
```

## Features

- Limit orders + cancels, price-time (FIFO) priority, trades at the resting price
- Integer tick prices (no floating point on the matching path)
- Wait-free SPSC ring buffer: acquire/release ordering, cached indices,
  cache-line padding against false sharing, power-of-two masking
- Pre-allocated object pool, reserved hash map and trade buffer — no heap
  allocation per message for orders at existing price levels
- 21 unit tests, including a **differential fuzz test** against a brute-force
  reference model (20k random ops, identical trades required at every step)
- Clean under AddressSanitizer, UndefinedBehaviorSanitizer and ThreadSanitizer
- Three benchmarks reporting p50/p90/p99/p99.9/p99.99/max

## Build & run (macOS / Linux, needs only clang++ and make)

```bash
make test        # build + run unit tests
make run         # demo: matching rules + threaded round trip
make bench       # all benchmarks (release, -O3)
make asan        # tests under Address + UB sanitizer
make tsan        # tests under ThreadSanitizer
```

CMake also works: `cmake -S . -B build/cmake && cmake --build build/cmake && ctest --test-dir build/cmake`.

In VS Code: **Cmd+Shift+B** builds, *Tasks: Run Task* runs tests/benchmarks,
**F5** debugs (install the recommended CodeLLDB extension).

## Results

Measured on an Apple M5 Mac, macOS, Apple clang, `-O3`.
Synthetic flow: ~45% cancels, ~40% passive limits, ~15% aggressive orders;
2M measured messages after 200k warm-up.

| Benchmark | p50 | p99 | p99.9 | Throughput |
|---|---|---|---|---|
| Matching logic only (`bench_engine`) | 41 ns | 125 ns | 250 ns | **29.0 M msg/s** (34.5 ns/msg avg) |
| End-to-end round trip, 2 threads (`bench_pipeline`) | **208 ns** | **375 ns** | 875 ns | **12.5 M msg/s** |
| SPSC queue cross-core round trip (`bench_spsc`) | **125 ns** | **167 ns** | 292 ns | 20.5 M msg/s |
| Mutex queue cross-core round trip (`bench_spsc`, baseline) | 3,209 ns | 19,987,583 ns (~20 ms) | 36,079,458 ns (~36 ms) | 10.9 M msg/s |

**Reading the numbers**

- **Timer resolution.** Apple Silicon's `steady_clock` ticks every ~41.7 ns,
  so per-message timings are quantized to multiples of ~42 ns (0, 41, 83,
  125, ...). The engine's p50 sits at one tick, i.e. the matching itself is at
  or below the timer's resolution. The throughput run (clock read only at start
  and end) gives the precise average: **34.5 ns per message**.
- **Lock-free vs mutex.** The SPSC queue hands a message to another core and
  back in 125 ns at the median and 167 ns at p99. The mutex queue's median is
  ~26x slower, and its p99 is ~20 ms: under contention macOS puts the waiting
  thread to sleep in the kernel, and waking it costs milliseconds. That tail is
  the reason trading systems avoid locks on the hot path.
- **Bulk throughput is noisy.** Queue throughput varied between runs (the
  mutex queue benefits from unfair lock hand-offs that accidentally batch
  work). Latency percentiles are the stable, meaningful comparison.
- **End-to-end ≈ queue hops + matching.** 208 ns round trip ≈ 125 ns of
  cross-core hand-off + ~35-80 ns of matching.
- `max` values (16-91 µs) are OS interruptions on a laptop without core
  isolation and are not representative.

## Possible next steps which can be added to improve the performance

1. Replace `std::map` price levels with a flat array indexed by tick
   (O(1) level access, no allocation on new level).
2. Replace `unordered_map` with an open-addressing hash map (no node allocation).
3. Add a market-data publisher thread (third SPSC queue) emitting L2 updates.
4. IOC / FOK / market orders; multiple instruments (one book per symbol).
5. Run on Linux with `isolcpus` + core pinning and compare tail latency.
