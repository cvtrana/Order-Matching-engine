#include "me/matching_engine.hpp"

#include "me/platform.hpp"

namespace me {

MatchingEngine::MatchingEngine(std::size_t max_orders) : book_(max_orders) {
    trades_.reserve(4096);
}

ExecReport MatchingEngine::process(const OrderRequest& req) {
    trades_.clear();  // keeps capacity: no free/malloc
    ExecReport rep{req.id, req.ts_ns, OrderStatus::Rejected, 0, 0};

    switch (req.type) {
        case MsgType::NewOrder: {
            rep.status = book_.add_limit(req.id, req.side, req.price, req.qty, trades_);
            Qty filled = 0;
            for (const Trade& t : trades_) filled += t.qty;
            rep.trade_count = static_cast<std::uint32_t>(trades_.size());
            rep.filled_qty  = filled;
            total_trades_  += trades_.size();
            break;
        }
        case MsgType::Cancel:
            rep.status = book_.cancel(req.id) ? OrderStatus::Cancelled
                                              : OrderStatus::Rejected;
            break;
    }
    return rep;
}

void MatchingEngine::run(InboundQueue& in, OutboundQueue& out) {
    OrderRequest req;
    // relaxed is fine for a stop flag: we only need to see it "eventually".
    while (running_.load(std::memory_order_relaxed)) {
        if (in.try_pop(req)) {
            const ExecReport rep = process(req);
            // Back-pressure: if the gateway isn't draining reports, wait.
            while (!out.try_push(rep)) cpu_relax();
        } else {
            cpu_relax();
        }
    }
}

}  // namespace me
