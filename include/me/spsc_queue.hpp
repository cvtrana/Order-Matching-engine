#pragma once
// ---------------------------------------------------------------------------
// spsc_queue.hpp — a bounded, lock-free, wait-free Single-Producer /
// Single-Consumer ring buffer.
//
//   producer thread ──try_push──▶ [ ring buffer ] ──try_pop──▶ consumer thread
//
// Why it can be lock-free: exactly ONE thread ever writes `head_` (producer)
// and exactly ONE thread ever writes `tail_` (consumer). No two threads ever
// write the same variable, so we never need a mutex or a compare-and-swap —
// just atomic loads and stores with the right memory ordering.
//
// Indices grow forever (size_t won't overflow in practice: 2^64 messages)
// and are mapped onto the buffer with `index & kMask`. Because Capacity is a
// power of two, `& kMask` is the same as `% Capacity` but costs one cycle.
//
//   size = head - tail     empty: head == tail     full: head - tail == Capacity
// ---------------------------------------------------------------------------
#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

#include "me/platform.hpp"

namespace me {

template <typename T, std::size_t Capacity>
class SpscQueue {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>,
                  "keep queue payloads plain data (no allocations, no dtors)");

    static constexpr std::size_t kMask = Capacity - 1;

public:
    SpscQueue() = default;
    SpscQueue(const SpscQueue&) = delete;             // the atomics and the
    SpscQueue& operator=(const SpscQueue&) = delete;  // buffer must not move

    // PRODUCER ONLY. Returns false if the queue is full (caller decides
    // whether to spin, drop, or apply back-pressure).
    bool try_push(const T& value) noexcept {
        // Our own index: only we write it, so a relaxed load is enough.
        const std::size_t head = head_.load(std::memory_order_relaxed);

        // Is there space? First check our CACHED copy of the consumer's tail.
        // Only if that says "full" do we pay for reading the real tail_,
        // which lives on a cache line owned by the other core.
        if (head - tail_cache_ == Capacity) {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (head - tail_cache_ == Capacity) return false;  // truly full
        }

        buffer_[head & kMask] = value;  // 1) write the data

        // 2) publish it. RELEASE guarantees the data write above becomes
        //    visible to the consumer BEFORE the new head value does.
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // CONSUMER ONLY. Returns false if the queue is empty.
    bool try_pop(T& out) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);

        if (tail == head_cache_) {
            // ACQUIRE pairs with the producer's RELEASE: once we see the new
            // head, we are guaranteed to also see the data written before it.
            head_cache_ = head_.load(std::memory_order_acquire);
            if (tail == head_cache_) return false;  // truly empty
        }

        out = buffer_[tail & kMask];  // 1) read the data

        // 2) hand the slot back. RELEASE guarantees our read of the slot has
        //    finished before the producer can see it is free and overwrite it.
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Approximate (may be stale by the time you use it). Fine for stats.
    std::size_t size_approx() const noexcept {
        return head_.load(std::memory_order_acquire) -
               tail_.load(std::memory_order_acquire);
    }

    static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    // Each field sits on its own cache line to prevent FALSE SHARING:
    //  - head_       written by producer, read by consumer
    //  - tail_cache_ producer-private
    //  - tail_       written by consumer, read by producer
    //  - head_cache_ consumer-private
    // If head_ and tail_ shared a line, every push would invalidate the
    // consumer's copy of the line and vice-versa (cache-line ping-pong).
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};
    alignas(kCacheLine) std::size_t tail_cache_{0};
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
    alignas(kCacheLine) std::size_t head_cache_{0};
    alignas(kCacheLine) std::array<T, Capacity> buffer_{};
};

}  // namespace me
