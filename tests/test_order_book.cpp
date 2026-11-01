#include <algorithm>
#include <random>
#include <vector>

#include "me/order_book.hpp"
#include "test_framework.hpp"

using namespace me;

namespace {
struct Fixture {
    OrderBook          book{1000};
    std::vector<Trade> trades;
    OrderStatus add(OrderId id, Side s, Price p, Qty q) {
        trades.clear();
        return book.add_limit(id, s, p, q, trades);
    }
};
}  // namespace

TEST(empty_book_has_no_prices) {
    OrderBook b(10);
    CHECK(!b.best_bid());
    CHECK(!b.best_ask());
    CHECK(b.order_count() == 0);
}

TEST(non_crossing_orders_rest) {
    Fixture f;
    CHECK(f.add(1, Side::Buy, 99, 10) == OrderStatus::New);
    CHECK(f.add(2, Side::Sell, 101, 10) == OrderStatus::New);
    CHECK(f.trades.empty());
    CHECK(*f.book.best_bid() == 99);
    CHECK(*f.book.best_ask() == 101);
    CHECK(f.book.order_count() == 2);
}

TEST(exact_match_fills_both) {
    Fixture f;
    f.add(1, Side::Sell, 100, 10);
    CHECK(f.add(2, Side::Buy, 100, 10) == OrderStatus::Filled);
    CHECK(f.trades.size() == 1);
    CHECK((f.trades[0] == Trade{2, 1, 100, 10}));
    CHECK(f.book.order_count() == 0);
    CHECK(!f.book.best_ask());
}

TEST(aggressor_remainder_rests) {
    Fixture f;
    f.add(1, Side::Sell, 100, 10);
    CHECK(f.add(2, Side::Buy, 100, 25) == OrderStatus::PartiallyFilled);
    CHECK(f.trades.size() == 1 && f.trades[0].qty == 10);
    CHECK(*f.book.best_bid() == 100);
    CHECK(*f.book.open_qty(2) == 15);
    CHECK(!f.book.best_ask());
}

TEST(resting_order_partially_filled_keeps_priority) {
    Fixture f;
    f.add(1, Side::Sell, 100, 50);
    f.add(2, Side::Sell, 100, 50);
    f.add(3, Side::Buy, 100, 20);
    CHECK(*f.book.open_qty(1) == 30);  // #1 still first in line
    f.add(4, Side::Buy, 100, 30);
    CHECK(f.trades.size() == 1 && f.trades[0].sell_id == 1);
    CHECK(!f.book.open_qty(1));
    CHECK(*f.book.open_qty(2) == 50);
}

TEST(price_priority_best_level_first) {
    Fixture f;
    f.add(1, Side::Sell, 103, 10);
    f.add(2, Side::Sell, 101, 10);
    f.add(3, Side::Sell, 102, 10);
    f.add(4, Side::Buy, 105, 30);
    CHECK(f.trades.size() == 3);
    CHECK(f.trades[0].price == 101);
    CHECK(f.trades[1].price == 102);
    CHECK(f.trades[2].price == 103);
}

TEST(time_priority_fifo_within_level) {
    Fixture f;
    f.add(1, Side::Buy, 100, 10);
    f.add(2, Side::Buy, 100, 10);
    f.add(3, Side::Buy, 100, 10);
    f.add(4, Side::Sell, 100, 25);
    CHECK(f.trades.size() == 3);
    CHECK(f.trades[0].buy_id == 1);
    CHECK(f.trades[1].buy_id == 2);
    CHECK(f.trades[2].buy_id == 3 && f.trades[2].qty == 5);
}

TEST(trade_executes_at_resting_price) {
    Fixture f;
    f.add(1, Side::Sell, 100, 10);
    f.add(2, Side::Buy, 110, 10);  // willing to pay 110, gets 100
    CHECK(f.trades[0].price == 100);
}

TEST(limit_price_stops_the_sweep) {
    Fixture f;
    f.add(1, Side::Sell, 100, 10);
    f.add(2, Side::Sell, 102, 10);
    CHECK(f.add(3, Side::Buy, 101, 30) == OrderStatus::PartiallyFilled);
    CHECK(f.trades.size() == 1);
    CHECK(*f.book.best_bid() == 101);
    CHECK(*f.book.best_ask() == 102);
}

TEST(cancel_removes_order_and_empty_level) {
    Fixture f;
    f.add(1, Side::Buy, 100, 10);
    CHECK(f.book.cancel(1));
    CHECK(!f.book.best_bid());
    CHECK(f.book.level_count(Side::Buy) == 0);
    CHECK(!f.book.cancel(1));   // second cancel fails
    CHECK(!f.book.cancel(42));  // unknown id fails
}

TEST(cancel_middle_of_queue_keeps_fifo) {
    Fixture f;
    f.add(1, Side::Sell, 100, 10);
    f.add(2, Side::Sell, 100, 10);
    f.add(3, Side::Sell, 100, 10);
    CHECK(f.book.cancel(2));
    CHECK(f.book.volume_at(Side::Sell, 100) == 20);
    f.add(4, Side::Buy, 100, 20);
    CHECK(f.trades.size() == 2);
    CHECK(f.trades[0].sell_id == 1 && f.trades[1].sell_id == 3);
}

TEST(rejects_invalid_and_duplicate) {
    Fixture f;
    CHECK(f.add(1, Side::Buy, 100, 0) == OrderStatus::Rejected);
    CHECK(f.add(1, Side::Buy, 0, 10) == OrderStatus::Rejected);
    CHECK(f.add(1, Side::Buy, 100, 10) == OrderStatus::New);
    CHECK(f.add(1, Side::Buy, 100, 10) == OrderStatus::Rejected);  // dup id
}

TEST(full_book_rejects_instead_of_allocating) {
    OrderBook          b(2);
    std::vector<Trade> t;
    CHECK(b.add_limit(1, Side::Buy, 100, 1, t) == OrderStatus::New);
    CHECK(b.add_limit(2, Side::Buy, 100, 1, t) == OrderStatus::New);
    CHECK(b.add_limit(3, Side::Buy, 100, 1, t) == OrderStatus::Rejected);
    CHECK(b.cancel(1));
    CHECK(b.add_limit(3, Side::Buy, 100, 1, t) == OrderStatus::New);  // slot reused
}

// ---------------------------------------------------------------------------
// Differential ("fuzz") test: run 20,000 random operations through the real
// book AND a deliberately dumb, obviously-correct reference model, and
// require identical trades and top-of-book after every single step.
// This catches the subtle bugs that hand-written cases miss.
// ---------------------------------------------------------------------------
namespace {
struct RefOrder { OrderId id; Side side; Price px; Qty qty; std::uint64_t seq; };

struct ReferenceBook {
    std::vector<RefOrder> orders;
    std::uint64_t         seq = 0;

    void add(OrderId id, Side side, Price px, Qty qty, std::vector<Trade>& out) {
        for (;;) {
            if (qty == 0) return;
            // Linear scan for the best crossing order on the other side.
            RefOrder* best = nullptr;
            for (auto& o : orders) {
                if (o.side == side) continue;
                bool crosses = side == Side::Buy ? o.px <= px : o.px >= px;
                if (!crosses) continue;
                if (!best) { best = &o; continue; }
                bool better = side == Side::Buy ? o.px < best->px : o.px > best->px;
                if (better || (o.px == best->px && o.seq < best->seq)) best = &o;
            }
            if (!best) break;
            Qty fill = std::min(qty, best->qty);
            out.push_back(side == Side::Buy ? Trade{id, best->id, best->px, fill}
                                            : Trade{best->id, id, best->px, fill});
            qty -= fill;
            best->qty -= fill;
            if (best->qty == 0)
                orders.erase(orders.begin() + (best - orders.data()));
        }
        orders.push_back({id, side, px, qty, seq++});
    }
    bool cancel(OrderId id) {
        auto it = std::find_if(orders.begin(), orders.end(),
                               [&](auto& o) { return o.id == id; });
        if (it == orders.end()) return false;
        orders.erase(it);
        return true;
    }
    std::optional<Price> best(Side s) const {
        std::optional<Price> b;
        for (auto& o : orders)
            if (o.side == s && (!b || (s == Side::Buy ? o.px > *b : o.px < *b))) b = o.px;
        return b;
    }
};
}  // namespace

TEST(randomized_matches_reference_model) {
    OrderBook          book(100'000);
    ReferenceBook      ref;
    std::mt19937_64    rng(12345);
    std::vector<Trade> got, want;
    OrderId            next_id = 1;
    bool               all_ok  = true;

    for (int step = 0; step < 20'000 && all_ok; ++step) {
        got.clear();
        want.clear();
        if (rng() % 4 == 0 && next_id > 1) {
            OrderId id = 1 + rng() % (next_id - 1);
            all_ok &= book.cancel(id) == ref.cancel(id);
        } else {
            Side  s  = rng() % 2 ? Side::Buy : Side::Sell;
            Price px = 95 + static_cast<Price>(rng() % 11);
            Qty   q  = 1 + static_cast<Qty>(rng() % 50);
            book.add_limit(next_id, s, px, q, got);
            ref.add(next_id, s, px, q, want);
            ++next_id;
        }
        all_ok &= got == want;
        all_ok &= book.best_bid() == ref.best(Side::Buy);
        all_ok &= book.best_ask() == ref.best(Side::Sell);
        all_ok &= book.order_count() == ref.orders.size();
    }
    CHECK(all_ok);
}
