// ---------------------------------------------------------------------------
// bench_pipeline — the headline number: END-TO-END latency through the whole
// system, exactly as a client order would experience it inside the process:
//
//   gateway thread                      engine thread
//   ──────────────                      ─────────────
//   stamp ts, push ──▶ InboundQueue ──▶ pop, match, push ──▶ OutboundQueue
//   pop report, latency = now - ts ◀───────────────────────────────┘
//
// Mode 1 "latency": send one order, wait for its report, repeat. No queueing
//        delay — this is the pure cost of the path (the honest number to
//        quote as "round-trip latency").
// Mode 2 "throughput": keep the pipe full; reports how many msgs/sec the
//        pipeline sustains.
// ---------------------------------------------------------------------------
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

#include "me/latency_recorder.hpp"
#include "me/matching_engine.hpp"
#include "me/platform.hpp"
#include "order_generator.hpp"

using namespace me;

int main(int argc, char** argv) {
    const std::size_t measured = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1'000'000;
    const std::size_t warmup   = 100'000;
    const auto orders = bench::generate_orders(2 * (warmup + measured));

    auto in  = std::make_unique<InboundQueue>();
    auto out = std::make_unique<OutboundQueue>();
    MatchingEngine eng(orders.size());
    std::thread engine_thread([&] {
        pin_current_thread(1);
        eng.run(*in, *out);
    });
    pin_current_thread(0);

    std::size_t next = 0;
    auto round_trip = [&]() -> std::uint64_t {
        OrderRequest req = orders[next++];
        req.ts_ns = now_ns();
        while (!in->try_push(req)) cpu_relax();
        ExecReport rep;
        while (!out->try_pop(rep)) cpu_relax();
        return now_ns() - rep.ts_in_ns;
    };

    // --- Mode 1: latency --------------------------------------------------
    for (std::size_t i = 0; i < warmup; ++i) round_trip();
    LatencyRecorder rec(measured);
    for (std::size_t i = 0; i < measured; ++i) rec.record(round_trip());
    rec.print("End-to-end round trip: gateway -> SPSC -> engine -> SPSC -> gateway");

    // --- Mode 2: throughput ----------------------------------------------
    const std::size_t total = warmup + measured;
    std::size_t sent = 0, received = 0;
    const auto t0 = now_ns();
    while (received < total) {
        // Interleave sending and receiving on the same thread (like a real
        // gateway session), so only two threads ever spin.
        if (sent < total) {
            OrderRequest req = orders[next];
            req.ts_ns = 0;
            if (in->try_push(req)) { ++sent; ++next; }
        }
        ExecReport rep;
        if (out->try_pop(rep)) ++received;
    }
    const auto t1 = now_ns();
    std::printf("\nPipeline throughput: %.2f million msgs/sec\n", static_cast<double>(total) / (static_cast<double>(t1 - t0) / 1e9) / 1e6);

    eng.request_stop();
    engine_thread.join();
    std::printf("trades executed: %llu\n", (unsigned long long)eng.total_trades());
}
