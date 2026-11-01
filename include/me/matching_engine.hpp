#pragma once
// ---------------------------------------------------------------------------
// matching_engine.hpp — wraps an OrderBook and connects it to the outside
// world through two SPSC queues:
//
//   gateway ──[InboundQueue: OrderRequest]──▶ ENGINE ──[OutboundQueue: ExecReport]──▶ gateway
//
// The engine thread busy-spins on the inbound queue. Busy-spinning burns a
// whole core at 100% but avoids the 5–50µs it takes the OS to wake a sleeping
// thread — that trade is standard in low-latency trading.
// ---------------------------------------------------------------------------
#include <atomic>
#include <cstdint>
#include <span>
#include <vector>

#include "me/order_book.hpp"
#include "me/spsc_queue.hpp"
#include "me/types.hpp"

namespace me {

inline constexpr std::size_t kQueueCapacity = 1u << 16;  // 65,536 messages
using InboundQueue  = SpscQueue<OrderRequest, kQueueCapacity>;
using OutboundQueue = SpscQueue<ExecReport, kQueueCapacity>;

class MatchingEngine {
public:
    explicit MatchingEngine(std::size_t max_orders);

    // Synchronous: handle one request and return its report. Used by the
    // threaded loop below, by the tests, and by the single-thread benchmark.
    ExecReport process(const OrderRequest& req);

    // Threaded: spin until request_stop(), moving messages in → book → out.
    // Run this on a dedicated thread.
    void run(InboundQueue& in, OutboundQueue& out);
    void request_stop() noexcept { running_.store(false, std::memory_order_release); }

    // Fills produced by the most recent process() call.
    std::span<const Trade> last_trades() const noexcept { return trades_; }

    const OrderBook& book() const noexcept { return book_; }
    std::uint64_t    total_trades() const noexcept { return total_trades_; }

private:
    OrderBook          book_;
    std::vector<Trade> trades_;  // reused scratch buffer: never reallocates
                                 // after warm-up (capacity is kept on clear())
    std::uint64_t      total_trades_{0};
    std::atomic<bool>  running_{true};
};

}
