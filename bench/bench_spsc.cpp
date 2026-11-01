// ---------------------------------------------------------------------------
// bench_spsc — why bother with a lock-free queue? Measure it against the
// "normal" approach: a std::mutex-protected queue.
//
//  Test 1  throughput: producer blasts N items, consumer drains them.
//  Test 2  ping-pong:  thread A sends 1 item, thread B echoes it back on a
//                      second queue; we time each round trip. This is the
//                      cost of handing a message from one core to another.
// ---------------------------------------------------------------------------
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include "me/latency_recorder.hpp"
#include "me/platform.hpp"
#include "me/spsc_queue.hpp"

using namespace me;

// The textbook thread-safe queue. Correct, simple — and slow under contention:
// every push/pop takes a lock (an atomic RMW + possible kernel futex call)
// and std::deque may allocate.
template <typename T>
class MutexQueue {
public:
    bool try_push(const T& v) {
        std::lock_guard lk(m_);
        if (q_.size() >= 65536) return false;
        q_.push_back(v);
        return true;
    }
    bool try_pop(T& out) {
        std::lock_guard lk(m_);
        if (q_.empty()) return false;
        out = q_.front();
        q_.pop_front();
        return true;
    }
private:
    std::mutex    m_;
    std::deque<T> q_;
};

template <typename Q>
double throughput_mops(std::uint64_t n) {
    auto q = std::make_unique<Q>();
    std::thread prod([&] {
        pin_current_thread(1);
        for (std::uint64_t i = 0; i < n; ++i)
            while (!q->try_push(i)) cpu_relax();
    });
    const auto    t0 = now_ns();
    std::uint64_t got = 0, v;
    while (got < n)
        if (q->try_pop(v)) ++got;
    const auto t1 = now_ns();
    prod.join();
    return static_cast<double>(n) / (static_cast<double>(t1 - t0) / 1e9) / 1e6;
}

template <typename Q>
void ping_pong(const char* name, std::uint64_t rounds) {
    auto ping = std::make_unique<Q>();
    auto pong = std::make_unique<Q>();
    std::thread echo([&] {
        pin_current_thread(1);
        std::uint64_t v;
        for (std::uint64_t i = 0; i < rounds; ++i) {
            while (!ping->try_pop(v)) cpu_relax();
            while (!pong->try_push(v)) cpu_relax();
        }
    });
    LatencyRecorder rec(rounds);
    std::uint64_t   v;
    for (std::uint64_t i = 0; i < rounds; ++i) {
        const auto t0 = now_ns();
        while (!ping->try_push(i)) cpu_relax();
        while (!pong->try_pop(v)) cpu_relax();
        rec.record(now_ns() - t0);
    }
    echo.join();
    rec.print(name);
}

int main() {
    pin_current_thread(0);
    constexpr std::uint64_t N      = 20'000'000;
    constexpr std::uint64_t ROUNDS = 1'000'000;
    constexpr std::uint64_t MUTEX_ROUNDS = 50'000;
    std::printf("Throughput (%llu x uint64, 2 threads)\n", (unsigned long long)N);
    std::printf("  SpscQueue  : %8.1f M msgs/sec\n", throughput_mops<SpscQueue<std::uint64_t, 65536>>(N));
    std::printf("  MutexQueue : %8.1f M msgs/sec\n", throughput_mops<MutexQueue<std::uint64_t>>(N));

    ping_pong<SpscQueue<std::uint64_t, 1024>>("SpscQueue round trip (2 hops)", ROUNDS);
    ping_pong<MutexQueue<std::uint64_t>>("MutexQueue round trip (2 hops)", MUTEX_ROUNDS);
}
