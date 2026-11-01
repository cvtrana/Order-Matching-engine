#pragma once
// ---------------------------------------------------------------------------
// order_book.hpp — a single-instrument limit order book with
// PRICE-TIME priority (best price first; at the same price, first come first
// served — "FIFO").
//
// Data structures:
//
//   bids_ : std::map<Price, PriceLevel, greater>   (highest price first)
//   asks_ : std::map<Price, PriceLevel, less>      (lowest price first)
//              │
//              └─ PriceLevel: intrusive doubly-linked FIFO of Order*
//                      head ⇄ order ⇄ order ⇄ tail
//
//   orders_ : unordered_map<OrderId, Order*>   O(1) lookup for cancel
//   pool_   : ObjectPool<Order>                no malloc per order
//
// Complexity: best price O(1) (map.begin()), new price level O(log L),
// add to existing level O(1), cancel O(1) + O(log L) only if level empties.
//
// The book is NOT thread-safe — by design. Exactly one thread (the engine)
// owns it; other threads talk to it only through SPSC queues. This
// "single-writer principle" is why no locks are needed anywhere.
// ---------------------------------------------------------------------------
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "me/object_pool.hpp"
#include "me/types.hpp"

namespace me {

struct PriceLevel;

// "Intrusive" list node: the prev/next links live INSIDE the order, so adding
// an order to a level needs no extra node allocation (unlike std::list).
struct Order {
    OrderId     id{};
    Price       price{};
    Qty         qty{};  // remaining (open) quantity
    Side        side{};
    Order*      prev{};
    Order*      next{};
    PriceLevel* level{};
};

struct PriceLevel {
    Price         price{};
    std::uint64_t total_qty{};  // sum of open qty — handy for market data
    std::uint32_t order_count{};
    Order*        head{};  // oldest order = first to be matched
    Order*        tail{};  // newest order

    void push_back(Order* o) noexcept;
    void remove(Order* o) noexcept;
    bool empty() const noexcept { return head == nullptr; }
};

class OrderBook {
public:
    explicit OrderBook(std::size_t max_orders);

    // Match `qty` against the opposite side up to `price`; any remainder
    // rests on the book. Fills are APPENDED to `trades` (caller clears it).
    OrderStatus add_limit(OrderId id, Side side, Price price, Qty qty,
                          std::vector<Trade>& trades);

    // Remove a resting order. Returns false if the id is not on the book.
    bool cancel(OrderId id);

    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;
    std::uint64_t        volume_at(Side side, Price price) const;
    std::size_t          order_count() const noexcept { return orders_.size(); }
    std::size_t          level_count(Side side) const noexcept;
    std::optional<Qty>   open_qty(OrderId id) const;

    // Visit levels best-first (for printing / market data snapshots).
    template <typename Fn>
    void for_each_level(Side side, Fn&& fn) const {
        if (side == Side::Buy)
            for (const auto& [px, lvl] : bids_) fn(lvl);
        else
            for (const auto& [px, lvl] : asks_) fn(lvl);
    }

private:
    using Bids = std::map<Price, PriceLevel, std::greater<Price>>;
    using Asks = std::map<Price, PriceLevel, std::less<Price>>;

    template <typename Book>
    Qty match(Book& book, Side aggressor, OrderId id, Price limit, Qty qty,
              std::vector<Trade>& trades);

    template <typename Book>
    void rest(Book& book, Order* order);

    Bids                                bids_;
    Asks                                asks_;
    ObjectPool<Order>                   pool_;
    std::unordered_map<OrderId, Order*> orders_;
};

}  // namespace me
