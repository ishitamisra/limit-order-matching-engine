#include "lob/order_book.hpp"

#include <vector>

#include "test_framework.hpp"

using namespace lob;

namespace {

void test_rests_when_no_cross() {
    OrderBook book;
    std::vector<Trade> trades;
    auto on_trade = [&](const Trade& t) { trades.push_back(t); };

    book.add_limit_order(1, Side::Buy, 100, 10, 1, on_trade);

    CHECK(trades.empty(), "no trade when book is empty");
    CHECK(book.best_bid() == std::optional<Price>(100), "resting order becomes best bid");
    CHECK(book.order_count() == 1, "order is tracked in the book");
}

void test_full_match() {
    OrderBook book;
    std::vector<Trade> trades;
    auto on_trade = [&](const Trade& t) { trades.push_back(t); };

    book.add_limit_order(1, Side::Sell, 100, 10, 1, on_trade);
    book.add_limit_order(2, Side::Buy, 100, 10, 2, on_trade);

    CHECK(trades.size() == 1, "crossing orders produce exactly one trade");
    CHECK(trades[0].price == 100, "trade executes at the resting order's price");
    CHECK(trades[0].quantity == 10, "trade quantity matches the fully-filled size");
    CHECK(trades[0].resting_order_id == 1, "resting side is the earlier order");
    CHECK(trades[0].aggressor_order_id == 2, "aggressor side is the incoming order");
    CHECK(book.order_count() == 0, "fully filled orders are removed from the book");
    CHECK(!book.best_bid().has_value(), "no resting bid remains");
    CHECK(!book.best_ask().has_value(), "no resting ask remains");
}

void test_partial_match_rests_remainder() {
    OrderBook book;
    std::vector<Trade> trades;
    auto on_trade = [&](const Trade& t) { trades.push_back(t); };

    book.add_limit_order(1, Side::Sell, 100, 10, 1, on_trade);
    book.add_limit_order(2, Side::Buy, 100, 4, 2, on_trade);

    CHECK(trades.size() == 1, "one trade for the overlapping quantity");
    CHECK(trades[0].quantity == 4, "trade size is the smaller of the two orders");
    CHECK(book.order_count() == 1, "the partially filled resting order stays in the book");
    CHECK(book.best_ask() == std::optional<Price>(100), "remainder still rests at its price");

    auto levels = book.ask_levels(1);
    CHECK(levels.size() == 1 && levels[0].quantity == 6, "remaining resting quantity is 10-4=6");
}

void test_time_priority_at_same_price() {
    OrderBook book;
    std::vector<Trade> trades;
    auto on_trade = [&](const Trade& t) { trades.push_back(t); };

    book.add_limit_order(1, Side::Sell, 100, 5, 1, on_trade);  // arrives first
    book.add_limit_order(2, Side::Sell, 100, 5, 2, on_trade);  // arrives second, same price
    book.add_limit_order(3, Side::Buy, 100, 5, 3, on_trade);   // should match order 1, not 2

    CHECK(trades.size() == 1, "only enough quantity for one resting order trades");
    CHECK(trades[0].resting_order_id == 1, "earlier order at the same price fills first (time priority)");
    CHECK(book.order_count() == 1, "order #2 still rests, untouched");
}

void test_price_priority_beats_time() {
    OrderBook book;
    std::vector<Trade> trades;
    auto on_trade = [&](const Trade& t) { trades.push_back(t); };

    book.add_limit_order(1, Side::Sell, 101, 5, 1, on_trade); // worse price, arrives first
    book.add_limit_order(2, Side::Sell, 100, 5, 2, on_trade); // better price, arrives second
    book.add_limit_order(3, Side::Buy, 101, 5, 3, on_trade);  // crosses both; should take best price

    CHECK(trades.size() == 1, "one trade");
    CHECK(trades[0].resting_order_id == 2, "better price fills before an earlier, worse-priced order");
}

void test_cancel_removes_order() {
    OrderBook book;
    auto on_trade = [](const Trade&) {};

    book.add_limit_order(1, Side::Buy, 100, 10, 1, on_trade);
    CHECK(book.cancel_order(1), "cancel succeeds for a known resting order");
    CHECK(!book.best_bid().has_value(), "book is empty after the only order is cancelled");
    CHECK(!book.cancel_order(1), "cancelling the same id twice fails");
    CHECK(!book.cancel_order(999), "cancelling an unknown id fails");
}

void test_cancelled_order_does_not_match() {
    OrderBook book;
    std::vector<Trade> trades;
    auto on_trade = [&](const Trade& t) { trades.push_back(t); };

    book.add_limit_order(1, Side::Buy, 100, 10, 1, on_trade);
    book.cancel_order(1);
    book.add_limit_order(2, Side::Sell, 100, 10, 2, on_trade);

    CHECK(trades.empty(), "a cancelled order cannot be matched against later");
}

void test_market_order_consumes_best_prices_first() {
    OrderBook book;
    std::vector<Trade> trades;
    auto on_trade = [&](const Trade& t) { trades.push_back(t); };

    book.add_limit_order(1, Side::Sell, 102, 5, 1, on_trade);
    book.add_limit_order(2, Side::Sell, 100, 5, 2, on_trade); // best ask
    book.add_limit_order(3, Side::Sell, 101, 5, 3, on_trade);

    Quantity unfilled = book.add_market_order(4, Side::Buy, 12, 4, on_trade);

    CHECK(trades.size() == 3, "market order walks through every price level it needs");
    CHECK(trades[0].price == 100, "cheapest ask fills first");
    CHECK(trades[1].price == 101, "then the next best price");
    CHECK(trades[2].price == 102, "then the worst price, for the remainder");
    CHECK(unfilled == 0, "12 requested == 5+5+2 filled, nothing left over");
}

void test_market_order_unfilled_remainder_is_dropped() {
    OrderBook book;
    auto on_trade = [](const Trade&) {};

    book.add_limit_order(1, Side::Sell, 100, 5, 1, on_trade);
    Quantity unfilled = book.add_market_order(2, Side::Buy, 20, 2, on_trade);

    CHECK(unfilled == 15, "market order reports what it couldn't fill");
    CHECK(book.order_count() == 0, "market orders never rest -- no order left in the book");
}

} // namespace

int main() {
    RUN_TEST(test_rests_when_no_cross);
    RUN_TEST(test_full_match);
    RUN_TEST(test_partial_match_rests_remainder);
    RUN_TEST(test_time_priority_at_same_price);
    RUN_TEST(test_price_priority_beats_time);
    RUN_TEST(test_cancel_removes_order);
    RUN_TEST(test_cancelled_order_does_not_match);
    RUN_TEST(test_market_order_consumes_best_prices_first);
    RUN_TEST(test_market_order_unfilled_remainder_is_dropped);
    TEST_MAIN_EXIT();
}
