# File-by-file walkthrough

Read the files in this order — each builds on the previous one. Every file
also has comments explaining the *why* inline.

```
include/me/
  types.hpp            1. the messages
  platform.hpp         2. cache line size, cpu_relax, thread pinning
  spsc_queue.hpp       3. the lock-free queue          ← the core concept
  object_pool.hpp      4. allocation-free order storage
  order_book.hpp       5. book data structures
  matching_engine.hpp  7. engine + queue wiring
  latency_recorder.hpp 9. timing + percentiles
src/
  order_book.cpp       6. the matching algorithm
  matching_engine.cpp  8. the busy-spin loop
  main.cpp             10. demo
tests/                 11. unit, fuzz and stress tests
bench/                 12. three benchmarks
```

---

### 1. `types.hpp` — the messages

- `Price` is `int64_t` ticks, `Qty` is `uint32_t`, `OrderId` is `uint64_t`.
- `OrderRequest` (in) and `ExecReport` (out) are the only things that cross
  threads. They're plain structs, and `static_assert(is_trivially_copyable)`
  proves at compile time they can be copied as raw bytes through the queue.
- `ExecReport::ts_in_ns` carries the request's send timestamp back, which is
  how the gateway computes round-trip latency without any shared state.

**Try it:** add a `std::string` field to `OrderRequest` and watch the
`static_assert` refuse to compile. That's the point.

### 2. `platform.hpp` — hardware details

- `kCacheLine`: 128 on Apple Silicon, 64 elsewhere.
- `cpu_relax()`: `pause`/`yield` inside spin loops.
- `pin_current_thread()`: hard pin on Linux, QoS hint on macOS.

### 3. `spsc_queue.hpp` — the lock-free queue

Read `try_push` line by line with CONCEPTS §5–7 next to it:

1. Load own `head_` *relaxed* (only we write it).
2. Check space using the private `tail_cache_`; only on "looks full" do an
   *acquire* load of the real `tail_`.
3. Write the item into `buffer_[head & kMask]`.
4. *Release*-store `head + 1` — this "publishes" the item.

`try_pop` is the mirror image. The member layout at the bottom is as
important as the code: four `alignas(kCacheLine)` fields, so producer-written
and consumer-written data never share a cache line.

**Try it:** change the release in `try_push` to `relaxed` and run `make tsan`.

### 4. `object_pool.hpp` — no `new` per order

A `std::vector<T>` holding every object, plus a `std::vector<T*>` used as a
stack of free slots. `acquire()` = pop a pointer, `release()` = push it back.
Returns `nullptr` when empty instead of allocating — the book then rejects
the order, which is how real engines behave at capacity.

### 5. `order_book.hpp` — data structures

- `Order` contains `prev/next` (intrusive list) and a back-pointer to its
  `PriceLevel`, so cancel is O(1): find order by id → unlink → done.
- `PriceLevel` holds the FIFO head/tail plus aggregate `total_qty` (what an L2
  market-data feed would publish).
- `bids_` uses `std::greater` so `begin()` is the highest bid; `asks_` uses
  `std::less` so `begin()` is the lowest ask. Best price is always `begin()`.

### 6. `order_book.cpp` — the matching algorithm

`add_limit`:
1. Validate (qty > 0, price > 0, id not already live).
2. `match()` against the opposite side.
3. If anything remains, take an `Order` from the pool and `rest()` it.
4. Return New / PartiallyFilled / Filled / Rejected.

`match()` is one template used for both sides. The neat part is the cross
test `comp(limit, level.price)` using the map's own comparator — see the
comment in the file for why that works for both bids and asks. Inside a
level it walks the FIFO from `head`, fills `min(remaining, resting.qty)`,
records a `Trade` at the **resting** price, and removes fully filled orders
(back to the pool). Empty levels are erased from the map.

`cancel()`: hash lookup → unlink from its level → erase level if empty →
release to pool.

### 7–8. `matching_engine.hpp/.cpp` — wiring it to threads

- `process(req)` is the synchronous core used by everything: tests, the
  single-thread benchmark, and the threaded loop.
- `run(in, out)` is the engine thread: pop → process → push report, forever,
  never sleeping. If the outbound queue is full it spins (back-pressure)
  rather than dropping reports.
- `trades_` is a reused buffer: `clear()` each message, capacity kept.

### 9. `latency_recorder.hpp` — measuring

`now_ns()` wraps `steady_clock` (monotonic). `LatencyRecorder` stores raw
samples in a pre-reserved vector, sorts once at the end and prints
percentiles. `measure_clock_overhead_ns()` tells you how much of each sample
is the clock itself.

### 10. `main.cpp` — demo

Part 1 replays a small scenario and prints the book so you can *see*
price-time priority. Part 2 runs the same engine on a thread behind the SPSC
queues. (Those first threaded round trips are slow because everything is
cold — that's itself a lesson in why warm-up matters.)

### 11. `tests/`

- `test_framework.hpp` — a tiny `TEST`/`CHECK` framework so there's nothing
  to install. Tests self-register through static constructors.
- `test_spsc_queue.cpp` — FIFO, full/empty edges, wraparound, and a 2-thread
  2M-message stress test.
- `test_order_book.cpp` — each matching rule, plus the differential fuzz test
  against `ReferenceBook`.
- `test_matching_engine.cpp` — reports, and a real threaded round trip of 50k
  messages checking ordering.

### 12. `bench/`

- `order_generator.hpp` — deterministic (seeded) synthetic flow: ~45% cancels,
  ~40% passive, ~15% aggressive.
- `bench_engine.cpp` — matching cost only, single thread, per-message timing,
  then a throughput run without timing overhead.
- `bench_spsc.cpp` — SPSC vs mutex queue, throughput and ping-pong.
- `bench_pipeline.cpp` — the headline: full gateway → engine → gateway round
  trip, plus sustained throughput.

---

## How to talk about it in an interview

> "I built a price-time priority matching engine in C++20. The book is owned by
> a single busy-spinning thread and fed by wait-free SPSC ring buffers using
> acquire/release ordering, cache-line-padded indices with cached copies to
> avoid false sharing. Orders come from a pre-allocated pool with intrusive
> FIFO lists, so cancel is O(1) and there's no per-order malloc. Matching
> logic runs at around X ns p50 / Y ns p99, and the full cross-thread round
> trip is Z ns p50. I verified it with a differential fuzz test against a
> reference model and ran it clean under ASan, UBSan and TSan. Next I'd swap
> std::map price levels for a tick-indexed array to remove the remaining
> allocations."

Be ready for: *why acquire/release and not seq_cst?*, *what is false sharing?*,
*why not a mutex?*, *what happens when the queue is full?*, *why is p99.9 so
much worse than p50 on a laptop?* (OS interrupts, no core isolation, frequency
scaling), *how would you support multiple producers?* (one SPSC per producer).

## Resume line (fill in your numbers)

**Low-latency order matching engine (C++20)** — Price-time priority limit order
book on a dedicated thread fed by wait-free SPSC ring buffers
(acquire/release atomics, cache-line padding); allocation-free hot path via
object pool and intrusive lists. **X ns p50 / Y ns p99** matching latency,
**Z ns p50** end-to-end cross-thread round trip, **N M msgs/sec**; verified
with differential fuzz testing and ASan/UBSan/TSan.
