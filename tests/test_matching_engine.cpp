#include <memory>
#include <thread>
#include <vector>

#include "me/matching_engine.hpp"
#include "test_framework.hpp"

using namespace me;

TEST(engine_reports_fills_and_cancels) {
    MatchingEngine eng(100);
    auto r1 = eng.process({MsgType::NewOrder, Side::Sell, 1, 100, 10, 0});
    CHECK(r1.status == OrderStatus::New && r1.trade_count == 0);

    auto r2 = eng.process({MsgType::NewOrder, Side::Buy, 2, 100, 4, 0});
    CHECK(r2.status == OrderStatus::Filled && r2.filled_qty == 4 && r2.trade_count == 1);
    CHECK(eng.last_trades().size() == 1);

    auto r3 = eng.process({MsgType::Cancel, Side::Sell, 1, 0, 0, 0});
    CHECK(r3.status == OrderStatus::Cancelled);
    auto r4 = eng.process({MsgType::Cancel, Side::Sell, 1, 0, 0, 0});
    CHECK(r4.status == OrderStatus::Rejected);
    CHECK(eng.total_trades() == 1);
}

// End-to-end through the real threaded path: gateway → queue → engine
// thread → queue → gateway. Reports must come back in request order.
TEST(engine_thread_round_trip_preserves_order) {
    auto           in  = std::make_unique<InboundQueue>();
    auto           out = std::make_unique<OutboundQueue>();
    MatchingEngine eng(100'000);
    std::thread    t([&] { eng.run(*in, *out); });

    constexpr int N = 50'000;
    int sent = 0, received = 0;
    bool ordered = true;
    while (received < N) {
        if (sent < N) {
            OrderRequest r{MsgType::NewOrder, sent % 2 ? Side::Buy : Side::Sell,
                           static_cast<OrderId>(sent + 1), 100, 1, 0};
            if (in->try_push(r)) ++sent;
        }
        ExecReport rep;
        if (out->try_pop(rep)) {
            if (rep.id != static_cast<OrderId>(received + 1)) ordered = false;
            ++received;
        }
    }
    eng.request_stop();
    t.join();
    CHECK(ordered);
    CHECK(eng.total_trades() == N / 2);  // every buy hits the sell before it
    CHECK(eng.book().order_count() == 0);
}
