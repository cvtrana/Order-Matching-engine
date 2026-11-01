// ---------------------------------------------------------------------------
// bench_engine — how long does the MATCHING LOGIC itself take?
// Single thread, no queues: time each process() call individually.
// This is the "tick-to-trade inside the book" number.
// ---------------------------------------------------------------------------
#include <cstdio>
#include <cstdlib>

#include "me/latency_recorder.hpp"
#include "me/matching_engine.hpp"
#include "me/platform.hpp"
#include "order_generator.hpp"

using namespace me;

int main(int argc, char** argv) {
    const std::size_t measured = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 2'000'000;
    const std::size_t warmup   = 200'000;

    pin_current_thread(0);
    std::printf("Generating %zu orders...\n", warmup + measured);
    const auto orders = bench::generate_orders(warmup + measured);

    // --- 1) per-order latency -------------------------------------------
    {
        MatchingEngine  eng(orders.size());
        LatencyRecorder rec(measured);

        // Warm-up: fault in memory pages, train the branch predictor, fill
        // caches, grow internal containers to steady state. Not recorded.
        for (std::size_t i = 0; i < warmup; ++i) eng.process(orders[i]);

        std::uint64_t sink = 0;  // stops the compiler deleting "unused" work
        for (std::size_t i = warmup; i < orders.size(); ++i) {
            const auto t0  = now_ns();
            const auto rep = eng.process(orders[i]);
            const auto t1  = now_ns();
            rec.record(t1 - t0);
            sink += rep.filled_qty;
        }
        rec.print("Engine process() latency per message");
        std::printf("  (clock overhead ~%llu ns per measurement, included above)\n",
                    (unsigned long long)measure_clock_overhead_ns());
        std::printf("  trades=%llu  resting orders at end=%zu  [sink %llu]\n",
                    (unsigned long long)eng.total_trades(), eng.book().order_count(),
                    (unsigned long long)sink);
    }

    // --- 2) throughput (no per-message timing overhead) -------------------
    {
        MatchingEngine eng(orders.size());
        for (std::size_t i = 0; i < warmup; ++i) eng.process(orders[i]);
        std::uint64_t sink = 0;
        const auto    t0   = now_ns();
        for (std::size_t i = warmup; i < orders.size(); ++i) sink += eng.process(orders[i]).filled_qty;
        const auto t1 = now_ns();
        const double secs = static_cast<double>(t1 - t0) / 1e9;
        std::printf("\nEngine throughput: %.2f million msgs/sec  (%.1f ns/msg avg)  [sink %llu]\n",
                    static_cast<double>(measured) / secs / 1e6,
                    static_cast<double>(t1 - t0) / static_cast<double>(measured), (unsigned long long)sink);
    }
}
