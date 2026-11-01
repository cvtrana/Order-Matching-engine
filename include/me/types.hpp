#pragma once
// ---------------------------------------------------------------------------
// types.hpp — the vocabulary of the whole engine.
//
// Design rules followed here (and everywhere on the hot path):
//   * Prices are INTEGER TICKS, never double. Floating point can't represent
//     0.1 exactly, and comparing prices for equality must be exact. A bond
//     quoted in 32nds (e.g. 99-16 = 99.5) is simply stored as a tick count.
//   * Messages are small, trivially-copyable PODs so they can be memcpy'd
//     through a lock-free queue with no constructors/destructors/allocations.
// ---------------------------------------------------------------------------
#include <cstdint>
#include <type_traits>

namespace me {

using OrderId = std::uint64_t;
using Price   = std::int64_t;   // in ticks
using Qty     = std::uint32_t;

enum class Side : std::uint8_t { Buy, Sell };

enum class MsgType : std::uint8_t { NewOrder, Cancel };

enum class OrderStatus : std::uint8_t {
    New,              // rested on the book, nothing filled
    PartiallyFilled,  // some quantity traded, remainder rests on the book
    Filled,           // fully traded, nothing rests
    Cancelled,        // cancel succeeded, OR remainder dropped (book full)
    Rejected          // invalid / duplicate id / unknown id on cancel
};

// Inbound message: gateway -> engine.
struct OrderRequest {
    MsgType       type{};
    Side          side{};
    OrderId       id{};
    Price         price{};
    Qty           qty{};
    std::uint64_t ts_ns{};  // timestamp taken by the sender, used for latency
};

// One fill between an aggressor and a resting order.
struct Trade {
    OrderId buy_id{};
    OrderId sell_id{};
    Price   price{};
    Qty     qty{};

    friend bool operator==(const Trade&, const Trade&) = default;
};

// Outbound message: engine -> gateway.
struct ExecReport {
    OrderId       id{};
    std::uint64_t ts_in_ns{};     // copied from the request, so the gateway
                                  // can compute round-trip latency
    OrderStatus   status{};
    std::uint32_t trade_count{};
    Qty           filled_qty{};
};

// Compile-time guarantees: these types can travel through the SPSC queue
// as raw bytes.
static_assert(std::is_trivially_copyable_v<OrderRequest>);
static_assert(std::is_trivially_copyable_v<ExecReport>);
static_assert(std::is_trivially_copyable_v<Trade>);

inline const char* to_string(OrderStatus s) {
    switch (s) {
        case OrderStatus::New:             return "New";
        case OrderStatus::PartiallyFilled: return "PartiallyFilled";
        case OrderStatus::Filled:          return "Filled";
        case OrderStatus::Cancelled:       return "Cancelled";
        case OrderStatus::Rejected:        return "Rejected";
    }
    return "?";
}

}  // namespace me
