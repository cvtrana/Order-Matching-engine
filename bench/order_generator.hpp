#pragma once
// ---------------------------------------------------------------------------
// Synthetic order flow shared by the benchmarks.
//
// Everything is generated BEFORE the timed loop, so random-number generation
// and vector growth never pollute the measurement.
//
// Mix (roughly what a liquid book sees):
//   ~45% cancels of a previously sent order (some will already be filled)
//   ~40% passive limit orders 1–20 ticks away from mid (they rest)
//   ~15% aggressive orders crossing up to 5 ticks (they trade)
// ---------------------------------------------------------------------------
#include <random>
#include <vector>

#include "me/types.hpp"

namespace me::bench {

inline std::vector<OrderRequest> generate_orders(std::size_t n, std::uint64_t seed = 42) {
    std::mt19937_64                      rng(seed);
    std::uniform_int_distribution<int>   pct(0, 99);
    std::uniform_int_distribution<Price> passive_off(1, 20);
    std::uniform_int_distribution<Price> aggr_off(0, 5);
    std::uniform_int_distribution<Qty>   qty(1, 100);

    constexpr Price mid = 10'000;
    std::vector<OrderRequest> out;
    out.reserve(n);
    std::vector<OrderId> cancel_candidates;
    cancel_candidates.reserve(n);
    OrderId next_id = 1;

    while (out.size() < n) {
        const int r = pct(rng);
        if (r < 45 && !cancel_candidates.empty()) {
            // pick a random earlier order; swap-remove so it's cancelled once
            std::size_t i = rng() % cancel_candidates.size();
            OrderId id = cancel_candidates[i];
            cancel_candidates[i] = cancel_candidates.back();
            cancel_candidates.pop_back();
            out.push_back({MsgType::Cancel, Side::Buy, id, 0, 0, 0});
            continue;
        }
        const Side side       = (rng() & 1) ? Side::Buy : Side::Sell;
        const bool aggressive = r >= 85;
        Price px;
        if (aggressive) px = side == Side::Buy ? mid + aggr_off(rng) : mid - aggr_off(rng);
        else            px = side == Side::Buy ? mid - passive_off(rng) : mid + passive_off(rng);

        out.push_back({MsgType::NewOrder, side, next_id, px, qty(rng), 0});
        cancel_candidates.push_back(next_id);
        ++next_id;
    }
    return out;
}

}  // namespace me::bench
