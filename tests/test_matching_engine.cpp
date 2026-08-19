#include "lob/matching_engine.hpp"

#include <atomic>
#include <thread>
#include <vector>

#include "test_framework.hpp"

using namespace lob;

namespace {

void test_symbols_are_independent_books() {
    std::vector<std::pair<std::string, Trade>> trades;
    MatchingEngine engine([&](const std::string& sym, const Trade& t) { trades.emplace_back(sym, t); });

    engine.submit_limit_order("AAPL", Side::Buy, 100, 10);
    engine.submit_limit_order("MSFT", Side::Sell, 50, 10);

    CHECK(engine.symbol_count() == 2, "two distinct symbols get two shards");

    Price aapl_bid = engine.with_book("AAPL", [](const OrderBook& b) { return *b.best_bid(); });
    Price msft_ask = engine.with_book("MSFT", [](const OrderBook& b) { return *b.best_ask(); });
    CHECK(aapl_bid == 100, "AAPL's resting order doesn't leak into MSFT's book");
    CHECK(msft_ask == 50, "MSFT's resting order is independent of AAPL");
    CHECK(trades.empty(), "no crossing orders, no trades");
}

void test_order_ids_are_unique_across_symbols() {
    MatchingEngine engine([](const std::string&, const Trade&) {});
    OrderId a = engine.submit_limit_order("AAPL", Side::Buy, 100, 1);
    OrderId b = engine.submit_limit_order("MSFT", Side::Buy, 100, 1);
    CHECK(a != b, "order ids are unique even across different symbols' shards");
}

void test_cancel_is_scoped_to_symbol() {
    MatchingEngine engine([](const std::string&, const Trade&) {});
    OrderId id = engine.submit_limit_order("AAPL", Side::Buy, 100, 1);
    CHECK(!engine.cancel_order("MSFT", id), "cancelling an id under the wrong symbol fails");
    CHECK(engine.cancel_order("AAPL", id), "cancelling under the correct symbol succeeds");
}

// Hammers a single symbol from many threads concurrently and checks that
// the resulting fills are internally consistent -- i.e. the shard mutex
// actually serializes access and no trade double-counts or corrupts
// quantities. This is a correctness check for the concurrency story, not
// the throughput benchmark (see bench/benchmark.cpp for numbers).
void test_concurrent_orders_on_one_symbol_stay_consistent() {
    constexpr int kThreads = 8;
    constexpr int kOrdersPerThread = 500;
    constexpr Quantity kQtyPerOrder = 3;

    std::atomic<Quantity> total_traded{0};
    MatchingEngine engine([&](const std::string&, const Trade& t) {
        total_traded.fetch_add(t.quantity, std::memory_order_relaxed);
    });

    // Seed one huge resting sell order that every buy thread will chew
    // through, so every fill is deterministically attributable.
    engine.submit_limit_order("TEST", Side::Sell, 100, kThreads * kOrdersPerThread * kQtyPerOrder);

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&engine]() {
            for (int i = 0; i < kOrdersPerThread; ++i) {
                engine.submit_limit_order("TEST", Side::Buy, 100, kQtyPerOrder);
            }
        });
    }
    for (auto& th : threads) th.join();

    Quantity expected = static_cast<Quantity>(kThreads) * kOrdersPerThread * kQtyPerOrder;
    CHECK(total_traded.load() == expected, "every concurrently submitted order fills exactly once, no lost or duplicated fills");

    bool book_empty = engine.with_book("TEST", [](const OrderBook& b) { return b.order_count() == 0; });
    CHECK(book_empty, "the seeded resting order is fully consumed with nothing left over");
}

} // namespace

int main() {
    RUN_TEST(test_symbols_are_independent_books);
    RUN_TEST(test_order_ids_are_unique_across_symbols);
    RUN_TEST(test_cancel_is_scoped_to_symbol);
    RUN_TEST(test_concurrent_orders_on_one_symbol_stay_consistent);
    TEST_MAIN_EXIT();
}
