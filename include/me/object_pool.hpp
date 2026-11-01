#pragma once
// ---------------------------------------------------------------------------
// object_pool.hpp — pre-allocated pool of T with an O(1) free list.
//
// `new`/`delete` (malloc/free) on the hot path are a classic latency killer:
// they can take a lock, walk free lists, or even ask the OS for memory —
// sometimes 50ns, sometimes 50µs. A pool does ALL allocation once at start-up;
// afterwards acquire()/release() are just a vector pop/push of a pointer.
//
// Bonus: all orders live in one contiguous block, which is friendlier to the
// CPU cache and the TLB than objects scattered across the heap.
// ---------------------------------------------------------------------------
#include <cstddef>
#include <vector>

namespace me {

template <typename T>
class ObjectPool {
public:
    explicit ObjectPool(std::size_t capacity) : storage_(capacity) {
        free_.reserve(capacity);  // reserve => push_back never reallocates
        // Push in reverse so the first acquire() returns &storage_[0]
        // (sequential addresses = nicer for the hardware prefetcher).
        for (std::size_t i = capacity; i > 0; --i) free_.push_back(&storage_[i - 1]);
    }

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    // Returns nullptr when exhausted — the caller must handle it (we reject
    // the order) rather than silently allocating.
    T* acquire() noexcept {
        if (free_.empty()) return nullptr;
        T* p = free_.back();
        free_.pop_back();
        return p;
    }

    void release(T* p) noexcept { free_.push_back(p); }

    std::size_t available() const noexcept { return free_.size(); }
    std::size_t capacity() const noexcept { return storage_.size(); }

private:
    std::vector<T>  storage_;  // the objects themselves, allocated once
    std::vector<T*> free_;     // stack of currently unused slots
};

}  // namespace me
