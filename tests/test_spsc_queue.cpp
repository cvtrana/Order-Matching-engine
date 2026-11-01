#include <cstdint>
#include <memory>
#include <thread>

#include "me/spsc_queue.hpp"
#include "test_framework.hpp"

using me::SpscQueue;

TEST(spsc_pop_on_empty_fails) {
    SpscQueue<int, 4> q;
    int v = 0;
    CHECK(!q.try_pop(v));
}

TEST(spsc_is_fifo) {
    SpscQueue<int, 8> q;
    for (int i = 0; i < 5; ++i) CHECK(q.try_push(i));
    for (int i = 0; i < 5; ++i) {
        int v = -1;
        CHECK(q.try_pop(v));
        CHECK(v == i);
    }
}

TEST(spsc_holds_exactly_capacity_items) {
    SpscQueue<int, 4> q;
    for (int i = 0; i < 4; ++i) CHECK(q.try_push(i));
    CHECK(!q.try_push(99));  // full
    int v;
    CHECK(q.try_pop(v) && v == 0);
    CHECK(q.try_push(4));    // one slot freed
    CHECK(!q.try_push(5));
}

TEST(spsc_wraps_around_many_times) {
    SpscQueue<std::uint64_t, 4> q;
    for (std::uint64_t i = 0; i < 1000; ++i) {
        CHECK(q.try_push(i));
        std::uint64_t v = 0;
        CHECK(q.try_pop(v) && v == i);
    }
    CHECK(q.size_approx() == 0);
}

// The real test of a concurrent queue: two threads hammering it. Every value
// must arrive exactly once and in order. Run under ThreadSanitizer too
// (`make tsan`) — it will flag any data race in the memory ordering.
TEST(spsc_two_threads_no_loss_no_reorder) {
    constexpr std::uint64_t N = 2'000'000;
    auto q = std::make_unique<SpscQueue<std::uint64_t, 1024>>();

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < N; ++i)
            while (!q->try_push(i)) me::cpu_relax();
    });

    std::uint64_t expected = 0;
    bool          in_order = true;
    while (expected < N) {
        std::uint64_t v;
        if (q->try_pop(v)) {
            if (v != expected) in_order = false;
            ++expected;
        }
    }
    producer.join();
    CHECK(in_order);
    CHECK(expected == N);
}
