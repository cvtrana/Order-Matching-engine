# Concepts you'll learn from this project

Read this top to bottom once; each section points at the file where the idea
lives in code.

---

## 1. What a matching engine does

An exchange keeps, per instrument, an **order book**: all the resting buy
orders (**bids**) and sell orders (**asks**) that haven't traded yet.

- **Best bid** = highest buy price. **Best ask** = lowest sell price.
- If a new buy's price ≥ best ask (or a sell's ≤ best bid), it **crosses**
  and trades. The incoming order is the **aggressor** (taker); the resting one
  is **passive** (maker).
- **Price-time priority**: better price trades first; at the same price, the
  order that arrived first trades first (FIFO). This is the rule on most
  exchanges, including most fixed-income electronic venues using a CLOB.
- Trades happen at the **resting** order's price. If you bid 110 and the best
  ask is 100, you pay 100.
- Whatever isn't filled **rests** on the book at its limit price.

In your job you sit on the *client* side of this — your connectivity layer
sends orders to exchange matching engines. This project builds the thing on
the other side of the wire.

→ `src/order_book.cpp`

---

## 2. Why latency, and why percentiles

Latency = time from "message in" to "response out". In trading, being a few
microseconds late means someone else got the fill.

Never quote only the **mean**. Latency distributions have long tails: GC-like
pauses, cache misses, OS interrupts, page faults. Quote:

| Percentile | Meaning |
|---|---|
| p50 (median) | the typical message |
| p99 | 1 in 100 is slower than this |
| p99.9 | 1 in 1,000 — where most real-world problems show up |
| max | worst case seen (often an OS hiccup on a laptop) |

**Benchmarking hygiene** (all applied in `bench/`):
- *Warm up* first — the first iterations fault in memory pages and train the
  branch predictor.
- *Pre-generate inputs* — don't time your random-number generator.
- *Keep a "sink"* — accumulate a result and print it, so the optimizer can't
  delete the work as dead code.
- *Measure your clock* — `steady_clock::now()` itself costs ~20ns.
- *Avoid queueing in latency tests* — if you blast messages faster than the
  system drains them, you measure time waiting in the queue, not processing
  time. `bench_pipeline` therefore measures latency with one message in flight
  ("ping-pong") and throughput separately.

→ `include/me/latency_recorder.hpp`, `bench/*.cpp`

---

## 3. Threads talking to each other: the problem

The engine runs on its own thread; the gateway on another. They must exchange
messages. The textbook way is a queue protected by a `std::mutex`. Problems:

1. **Lock contention**: when both threads want the lock, one waits.
2. **Kernel involvement**: a contended mutex may put a thread to sleep via a
   system call; waking it costs microseconds.
3. **Allocation**: `std::deque`/`std::queue` allocate as they grow.
4. **Priority inversion / convoying**: the lock holder gets descheduled, and
   everyone waits for it.

`bench_spsc` shows the result: on this design the mutex queue is ~50–70×
slower in throughput and much worse in the tail.

---

## 4. Lock-free, wait-free

- **Lock-free**: no mutexes; at any moment *some* thread is guaranteed to make
  progress. A thread being paused can't block others.
- **Wait-free** (stronger): *every* operation finishes in a bounded number of
  steps. Our SPSC `try_push`/`try_pop` are wait-free — no loops, no retries.

Lock-free does **not** mean "no synchronisation". It means we synchronise with
**atomic operations** and **memory ordering** instead of locks.

---

## 5. SPSC: Single Producer, Single Consumer

If exactly one thread pushes and exactly one thread pops, the queue becomes
dramatically simpler than a general (MPMC) queue:

```
 buffer:  [  ][ A][ B][ C][  ][  ][  ][  ]     Capacity = 8 (power of two)
               ▲           ▲
             tail=1      head=4         size = head - tail = 3
           (consumer     (producer
            owns it)      owns it)
```

- `head_` is written **only** by the producer. `tail_` **only** by the consumer.
- No variable has two writers ⇒ no compare-and-swap (CAS) needed, just plain
  atomic loads and stores.
- Indices only grow; slot = `index & (Capacity-1)`. With a power-of-two
  capacity this AND is equivalent to `%` but costs a single cycle.
- full ⇔ `head - tail == Capacity`, empty ⇔ `head == tail`.

SPSC is everywhere in trading: feed handler → strategy, strategy → order
gateway, engine → market-data publisher. When you need multiple producers, a
common trick is *one SPSC queue per producer* rather than one MPMC queue.

→ `include/me/spsc_queue.hpp`

---

## 6. Memory ordering (the hard part, made simple)

Both the compiler and the CPU may **reorder** memory operations for speed.
On a single thread you never notice. Across threads you do:

```
Producer                         Consumer
buffer[5] = order;   (1)         if (head == 6)        (3)
head = 6;            (2)             use(buffer[5]);   (4)
```

If (1) and (2) get reordered, the consumer can see `head == 6` and read a slot
that hasn't been written yet. Garbage order. Apple Silicon (ARM) reorders far
more aggressively than x86, so bugs that hide on Intel show up on your Mac.

C++ atomics let you say which reorderings are forbidden:

| Ordering | Guarantee | Used for |
|---|---|---|
| `relaxed` | atomic, but no ordering w.r.t. other memory | reading your *own* index; stop flags |
| `release` (on a store) | all writes **before** it are visible to anyone who *acquires* this value | publishing `head_` after writing the data |
| `acquire` (on a load) | all reads **after** it see everything the releaser wrote before its release | reading `head_` before reading the data |
| `seq_cst` (default) | one global order of all seq_cst ops; strongest and slowest | rarely needed here |

The **release/acquire pair** is the whole trick: producer writes data then
*release-stores* `head_`; consumer *acquire-loads* `head_` then reads data. The
data is guaranteed visible. Symmetrically, the consumer release-stores `tail_`
after reading, so the producer can't overwrite a slot that's still being read.

On x86 release/acquire compile to plain `mov` instructions (x86 is strongly
ordered), so they're free. On ARM they become `stlr`/`ldar` — still much
cheaper than `seq_cst` or a lock.

**ThreadSanitizer** (`make tsan`) checks this: change an `acquire` to
`relaxed` and TSan will report a data race.

---

## 7. Cache lines and false sharing

CPUs move memory between cores in **cache lines** (64 bytes on x86,
128 on Apple M-series). If core A writes any byte of a line, core B's copy of
that whole line is invalidated.

**False sharing**: two *independent* variables, written by different threads,
happen to sit on the same line. They don't share data, but the cores still
ping-pong the line back and forth — each write costs ~50–100ns instead of ~1ns.

Fix: `alignas(kCacheLine)` on each hot variable so each gets its own line.
In the SPSC queue, `head_` (producer writes) and `tail_` (consumer writes) are
on separate lines.

**Cached indices**: the producer needs `tail_` to check for "full", but reading
it pulls the consumer's line over. So the producer keeps a private
`tail_cache_` and only re-reads the real `tail_` when the cache says "full".
With a mostly-not-full queue, the producer almost never touches the
consumer's line. Same trick for the consumer and `head_`. This is the single
biggest speed-up in a good SPSC queue.

→ `include/me/platform.hpp` (`kCacheLine`), `include/me/spsc_queue.hpp`

---

## 8. No allocation on the hot path

`new`/`malloc` is usually fast but sometimes very slow (lock inside the
allocator, fragmentation, asking the OS for pages). Those rare slow calls are
your p99.9. So:

- **Object pool** — allocate all `Order` objects once at startup; reuse them
  through a free list. → `include/me/object_pool.hpp`
- **Intrusive list** — the `prev/next` pointers live inside `Order`, so
  queueing an order at a price level needs no list-node allocation (unlike
  `std::list<Order>`).
- **`reserve()`** — the hash map, trade vector and latency samples are sized up
  front so they never reallocate/rehash mid-run.
- **`clear()` keeps capacity** — `trades_.clear()` doesn't free memory.

Honest caveat (good interview material): `std::map` still allocates when a
*new* price level appears, and `std::unordered_map` allocates a node per
insert. Those are the next things to replace (see README "next steps").

---

## 9. Single-writer principle

The order book has **zero locks** because only the engine thread ever touches
it. Everyone else talks to it via queues. This is how real matching engines
(and LMAX Disruptor-style systems) work: shard state so each piece has one
owner thread, and move messages, not locks.

---

## 10. Busy-spinning and thread pinning

`MatchingEngine::run()` never sleeps: it loops on `try_pop` at 100% CPU.
Sleeping (condition variables) means the OS must wake the thread — 5–50µs.
Spinning makes the reaction time ~tens of nanoseconds. `cpu_relax()` (`pause`
on x86, `yield` on ARM) tells the core it's in a spin loop.

**Pinning** keeps the thread on one core so its caches stay warm. Linux:
`pthread_setaffinity_np`. macOS doesn't allow pinning; we set the highest QoS
class so it prefers performance cores. Production systems also isolate cores
from the OS scheduler (`isolcpus`) so nothing else ever runs there.

→ `include/me/platform.hpp`, `src/matching_engine.cpp`

---

## 11. Integer prices

`0.1 + 0.2 != 0.3` in floating point. Matching needs exact equality at a price
level, so prices are stored as integer **ticks**. A US Treasury quoted
99-16+ (99 and 16.5/32nds) is just an integer number of 1/64ths. Convert to a
human price only at the edges (display, FIX messages).

---

## 12. Testing concurrent and stateful code

- **Example tests** for each rule (FIFO, price priority, partial fills…).
- **Differential testing**: a 30-line brute-force reference book that is
  obviously correct but slow; run 20k random operations through both and
  require identical output at each step. This finds bugs no one thinks to
  write a test for.
- **Stress test** for the queue with two real threads and 2M messages.
- **Sanitizers**: ASan (out-of-bounds, use-after-free), UBSan (undefined
  behaviour), TSan (data races). Run them; interviewers love hearing that.
