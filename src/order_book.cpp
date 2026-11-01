#include "me/order_book.hpp"

#include <algorithm>

namespace me {

// ---------------------------------------------------------------- PriceLevel

void PriceLevel::push_back(Order* o) noexcept {
    o->level = this;
    o->next  = nullptr;
    o->prev  = tail;
    if (tail) tail->next = o;
    else      head = o;
    tail = o;
    total_qty += o->qty;
    ++order_count;
}

// O(1) unlink from anywhere in the FIFO — this is why cancel is fast.
void PriceLevel::remove(Order* o) noexcept {
    if (o->prev) o->prev->next = o->next;
    else         head = o->next;
    if (o->next) o->next->prev = o->prev;
    else         tail = o->prev;
    total_qty -= o->qty;
    --order_count;
    o->prev = o->next = nullptr;
    o->level = nullptr;
}

// ----------------------------------------------------------------- OrderBook

OrderBook::OrderBook(std::size_t max_orders) : pool_(max_orders) {
    // Reserve buckets up front so the hash table never rehashes on the hot
    // path (a rehash of a big table is a multi-millisecond latency spike).
    orders_.reserve(max_orders);
}

// Walk the opposite side best-price-first, filling against each level's FIFO
// until the aggressor is done or prices no longer cross.
//
// One template serves both sides. The trick is the map's comparator:
//   asks (less):    crosses while level_px <= limit   ==  !(limit <  level_px)
//   bids (greater): crosses while level_px >= limit   ==  !(limit >  level_px)
// i.e. "crosses" is exactly  !comp(limit, level_px)  for both.
template <typename Book>
Qty OrderBook::match(Book& book, Side aggressor, OrderId id, Price limit, Qty qty,
                     std::vector<Trade>& trades) {
    const auto comp = book.key_comp();

    while (qty > 0 && !book.empty()) {
        auto        it    = book.begin();  // best opposite price
        PriceLevel& level = it->second;
        if (comp(limit, level.price)) break;  // no longer crosses

        Order* resting = level.head;
        while (qty > 0 && resting) {
            const Qty fill = std::min(qty, resting->qty);

            // Trade happens at the RESTING order's price: the passive side
            // set the price, the aggressor gets any price improvement.
            if (aggressor == Side::Buy) trades.push_back({id, resting->id, level.price, fill});
            else                        trades.push_back({resting->id, id, level.price, fill});

            qty             -= fill;
            resting->qty    -= fill;
            level.total_qty -= fill;

            Order* next = resting->next;
            if (resting->qty == 0) {  // fully filled → leaves the book
                level.remove(resting);
                orders_.erase(resting->id);
                pool_.release(resting);
            }
            resting = next;
        }
        if (level.empty()) book.erase(it);
    }
    return qty;  // whatever is left over
}

template <typename Book>
void OrderBook::rest(Book& book, Order* order) {
    // try_emplace: find the level, or create it if this is a new price.
    auto [it, inserted] = book.try_emplace(order->price);
    if (inserted) it->second.price = order->price;
    it->second.push_back(order);
    orders_.emplace(order->id, order);
}

OrderStatus OrderBook::add_limit(OrderId id, Side side, Price price, Qty qty,
                                 std::vector<Trade>& trades) {
    if (qty == 0 || price <= 0) return OrderStatus::Rejected;
    if (orders_.find(id) != orders_.end()) return OrderStatus::Rejected;  // dup id

    const Qty remaining = (side == Side::Buy)
                              ? match(asks_, side, id, price, qty, trades)
                              : match(bids_, side, id, price, qty, trades);
    const Qty filled = qty - remaining;

    if (remaining == 0) return OrderStatus::Filled;

    Order* o = pool_.acquire();
    if (!o) {
        // Book is at capacity. We already traded what we could; the rest is
        // cancelled rather than allocating on the hot path.
        return filled > 0 ? OrderStatus::Cancelled : OrderStatus::Rejected;
    }
    *o = Order{id, price, remaining, side, nullptr, nullptr, nullptr};
    if (side == Side::Buy) rest(bids_, o);
    else                   rest(asks_, o);

    return filled > 0 ? OrderStatus::PartiallyFilled : OrderStatus::New;
}

bool OrderBook::cancel(OrderId id) {
    auto it = orders_.find(id);
    if (it == orders_.end()) return false;

    Order*      o     = it->second;
    PriceLevel* level = o->level;
    const Price px    = o->price;
    const Side  side  = o->side;

    level->remove(o);
    if (level->empty()) {
        if (side == Side::Buy) bids_.erase(px);
        else                   asks_.erase(px);
    }
    orders_.erase(it);
    pool_.release(o);
    return true;
}

std::optional<Price> OrderBook::best_bid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

std::uint64_t OrderBook::volume_at(Side side, Price price) const {
    if (side == Side::Buy) {
        auto it = bids_.find(price);
        return it == bids_.end() ? 0 : it->second.total_qty;
    }
    auto it = asks_.find(price);
    return it == asks_.end() ? 0 : it->second.total_qty;
}

std::size_t OrderBook::level_count(Side side) const noexcept {
    return side == Side::Buy ? bids_.size() : asks_.size();
}

std::optional<Qty> OrderBook::open_qty(OrderId id) const {
    auto it = orders_.find(id);
    if (it == orders_.end()) return std::nullopt;
    return it->second->qty;
}

}  // namespace me
