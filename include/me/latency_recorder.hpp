#pragma once
// ---------------------------------------------------------------------------
// latency_recorder.hpp — timing helpers and percentile reporting.
//
// Why percentiles and not the average? Latency distributions have long tails.
// A mean of 200ns can hide 1 order in 1,000 taking 20µs — and that one order
// is the one that missed the market. Trading firms quote p50 / p99 / p99.9.
// ---------------------------------------------------------------------------
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace me {

// Monotonic nanosecond clock. steady_clock never jumps backwards (unlike the
// wall clock). On Apple Silicon this costs ~20–40ns per call; benchmarks
// measure and print that overhead so you can account for it.
inline std::uint64_t now_ns() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

class LatencyRecorder {
public:
    explicit LatencyRecorder(std::size_t expected_samples) {
        samples_.reserve(expected_samples);  // no allocation while recording
    }

    void record(std::uint64_t ns) noexcept { samples_.push_back(ns); }
    std::size_t count() const noexcept { return samples_.size(); }

    // Sorts in place; call once at the end, never on the hot path.
    void print(const char* title) {
        if (samples_.empty()) { std::printf("%s: no samples\n", title); return; }
        std::sort(samples_.begin(), samples_.end());
        long double sum = 0;
        for (auto s : samples_) sum += s;

        std::printf("\n%s  (%zu samples)\n", title, samples_.size());
        std::printf("  %-8s %10.0Lf ns\n", "mean", sum / static_cast<long double>(samples_.size()));
        std::printf("  %-8s %10llu ns\n", "min",    ull(samples_.front()));
        std::printf("  %-8s %10llu ns\n", "p50",    ull(pct(50.0)));
        std::printf("  %-8s %10llu ns\n", "p90",    ull(pct(90.0)));
        std::printf("  %-8s %10llu ns\n", "p99",    ull(pct(99.0)));
        std::printf("  %-8s %10llu ns\n", "p99.9",  ull(pct(99.9)));
        std::printf("  %-8s %10llu ns\n", "p99.99", ull(pct(99.99)));
        std::printf("  %-8s %10llu ns\n", "max",    ull(samples_.back()));
    }

    // Valid only after print() (which sorts).
    std::uint64_t pct(double p) const noexcept {
        std::size_t idx = static_cast<std::size_t>(p / 100.0 * static_cast<double>(samples_.size() - 1));
        return samples_[idx];
    }

private:
    static unsigned long long ull(std::uint64_t v) { return static_cast<unsigned long long>(v); }
    std::vector<std::uint64_t> samples_;
};

// Rough cost of one now_ns() pair, so benchmark readers can subtract it.
inline std::uint64_t measure_clock_overhead_ns(int iters = 1'000'000) {
    std::uint64_t best = ~0ull;
    for (int i = 0; i < iters; ++i) {
        const auto a = now_ns();
        const auto b = now_ns();
        best = std::min<std::uint64_t>(best, b - a);
    }
    return best;
}

}  // namespace me
