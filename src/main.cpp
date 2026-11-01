// ---------------------------------------------------------------------------
// main.cpp — a small, readable demo of the engine. Run it first to see what
// the matching rules do before looking at tests and benchmarks.
// ---------------------------------------------------------------------------
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "me/latency_recorder.hpp"
#include "me/matching_engine.hpp"

using namespace me;

static void print_book(const OrderBook& book) {
    std::printf("    ---- ASKS ----\n");
    // Print asks highest-first so the book reads top-down like a ladder.
    std::vector<const PriceLevel*> asks;
    book.for_each_level(Side::Sell, [&](const PriceLevel& l) { asks.push_back(&l); });
    for (auto it = asks.rbegin(); it != asks.rend(); ++it)
        std::printf("    %6lld  x %-6llu (%u orders)\n", (long long)(*it)->price,
                    (unsigned long long)(*it)->total_qty, (*it)->order_count);
    std::printf("    ---- BIDS ----\n");
    book.for_each_level(Side::Buy, [](const PriceLevel& l) {
        std::printf("    %6lld  x %-6llu (%u orders)\n", (long long)l.price,
                    (unsigned long long)l.total_qty, l.order_count);
    });
}

static void send(MatchingEngine& eng, MsgType type, Side side, OrderId id, Price px, Qty qty) {
    OrderRequest req{type, side, id, px, qty, now_ns()};
    ExecReport   rep = eng.process(req);

    if (type == MsgType::Cancel)
        std::printf("\n> CANCEL #%llu\n", (unsigned long long)id);
    else
        std::printf("\n> %s #%llu  %u @ %lld\n", side == Side::Buy ? "BUY " : "SELL",
                    (unsigned long long)id, qty, (long long)px);

    std::printf("  status=%s filled=%u\n", to_string(rep.status), rep.filled_qty);
    for (const Trade& t : eng.last_trades())
        std::printf("  TRADE buy#%llu sell#%llu  %u @ %lld\n", (unsigned long long)t.buy_id,
                    (unsigned long long)t.sell_id, t.qty, (long long)t.price);
}

int main() {
    std::printf("=== Part 1: matching rules (single thread) ===\n");
    MatchingEngine eng(1024);

    send(eng, MsgType::NewOrder, Side::Sell, 1, 101, 50);
    send(eng, MsgType::NewOrder, Side::Sell, 2, 101, 30);   // same price, behind #1
    send(eng, MsgType::NewOrder, Side::Sell, 3, 102, 100);
    send(eng, MsgType::NewOrder, Side::Buy,  4,  99, 40);
    send(eng, MsgType::NewOrder, Side::Buy,  5, 100, 20);
    std::printf("\n  Book now:\n");
    print_book(eng.book());

    // Aggressive buy: sweeps 101 (#1 then #2 — time priority), then 102.
    send(eng, MsgType::NewOrder, Side::Buy, 6, 102, 120);
    send(eng, MsgType::Cancel,   Side::Buy, 4, 0, 0);
    send(eng, MsgType::Cancel,   Side::Buy, 4, 0, 0);       // already gone
    std::printf("\n  Book now:\n");
    print_book(eng.book());

    std::printf("\n=== Part 2: same engine on its own thread, via SPSC queues ===\n");
    auto in  = std::make_unique<InboundQueue>();   // queues are big: heap, not stack
    auto out = std::make_unique<OutboundQueue>();
    MatchingEngine threaded(1024);
    std::thread engine_thread([&] { threaded.run(*in, *out); });

    const OrderRequest reqs[] = {
        {MsgType::NewOrder, Side::Sell, 10, 200, 10, 0},
        {MsgType::NewOrder, Side::Buy,  11, 200,  4, 0},
        {MsgType::NewOrder, Side::Buy,  12, 201, 10, 0},
    };
    for (auto r : reqs) {
        r.ts_ns = now_ns();
        while (!in->try_push(r)) cpu_relax();
    }
    for (int received = 0; received < 3;) {
        ExecReport rep;
        if (out->try_pop(rep)) {
            ++received;
            std::printf("  report #%llu status=%-15s filled=%u  round-trip=%llu ns\n",
                        (unsigned long long)rep.id, to_string(rep.status), rep.filled_qty,
                        (unsigned long long)(now_ns() - rep.ts_in_ns));
        }
    }
    threaded.request_stop();
    engine_thread.join();
    std::printf("  (these first messages are COLD: fresh thread, empty caches.\n"
                "   bench_pipeline measures the warm, steady-state number.)\n");
    std::printf("\nDone. Run the tests and benchmarks next.\n");
}
