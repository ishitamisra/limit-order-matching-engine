#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "lob/order.hpp"
#include "lob/order_pool.hpp"

namespace lob {

// A FIFO queue of orders resting at a single price, in strict arrival
// order (price-time priority within the level). Implemented as an
// intrusive doubly linked list threaded through Order::prev/next: no
// per-node allocation, and removing a specific order (a fill or a cancel)
// is O(1) once you have its pointer, instead of the O(n) scan a
// std::list<Order> erase-by-value would need.
struct PriceLevel {
    Price price = 0;
    Quantity total_quantity = 0;
    Order* head = nullptr; // oldest order: next to fill
    Order* tail = nullptr; // newest order: goes to the back of the queue

    bool empty() const { return head == nullptr; }

    void push_back(Order* o) {
        o->prev = tail;
        o->next = nullptr;
        if (tail) tail->next = o; else head = o;
        tail = o;
        total_quantity += o->quantity;
    }

    // Unlink `o` from the list. Does not touch total_quantity for the
    // portion already filled -- callers adjust total_quantity themselves
    // as they reduce o->quantity, then call remove() once o is fully done
    // (filled or cancelled) to unlink it from the queue.
    void remove(Order* o) {
        if (o->prev) o->prev->next = o->next; else head = o->next;
        if (o->next) o->next->prev = o->prev; else tail = o->prev;
        o->prev = o->next = nullptr;
    }
};

// A single-symbol limit order book with price-time priority matching.
// Not thread-safe on its own -- MatchingEngine wraps one OrderBook per
// symbol in its own mutex (see matching_engine.hpp) so that concurrent
// order flow for *different* symbols never contends on the same lock.
//
// Data structures, and why:
//  - bids/asks are std::map<Price, PriceLevel>, i.e. a balanced BST kept
//    in sorted order by price. Insert/erase/best-price lookup are all
//    O(log P) in the number of distinct price levels (not the number of
//    orders), and begin() always yields the best price with no scan.
//    A hash map would be O(1) per level but can't answer "what's the best
//    price" without a scan or a second sorted structure; a flat sorted
//    vector would need O(P) shifts to insert a new price level in a
//    fast-moving book. std::map is the standard middle ground here, and
//    is what most textbook/interview implementations reach for.
//  - Each PriceLevel holds an intrusive FIFO (see above) for time
//    priority among orders at the same price.
//  - order_index is a hash map from OrderId to the order's location
//    (which side, which price, and the raw Order*), giving O(1) average
//    cancel-by-id instead of having to search every price level.
class OrderBook {
public:
    using TradeCallback = std::function<void(const Trade&)>;

    explicit OrderBook(std::size_t pool_capacity_hint = 4096)
        : pool_(pool_capacity_hint) {}

    // Places a limit order. Matches immediately against any crossing
    // orders on the opposite side (best price first, then FIFO within a
    // price), then rests any unfilled remainder in the book. `on_trade`
    // is invoked once per fill, in price-time priority order.
    void add_limit_order(OrderId id, Side side, Price price, Quantity quantity,
                          Sequence sequence, const TradeCallback& on_trade);

    // Places a market order: matches immediately against the best
    // available prices on the opposite side until fully filled or the
    // book runs out of liquidity. Never rests -- any unfilled remainder
    // is dropped (standard IOC semantics for a market order), and is
    // reported back via the return value.
    Quantity add_market_order(OrderId id, Side side, Quantity quantity,
                               Sequence sequence, const TradeCallback& on_trade);

    // Cancels a resting order by id. Returns false if the id is unknown
    // (already filled, already cancelled, or never existed).
    bool cancel_order(OrderId id);

    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;
    std::size_t order_count() const { return order_index_.size(); }

    struct Level { Price price; Quantity quantity; };
    std::vector<Level> bid_levels(std::size_t depth) const;
    std::vector<Level> ask_levels(std::size_t depth) const;

private:
    struct Location {
        Side side;
        Price price;
        Order* order;
    };

    // Matches an incoming order against `book` (the opposite side) up to
    // `limit_price` (nullopt = no limit, i.e. a market order). Reduces
    // `remaining` as fills happen and invokes on_trade per fill. Shared by
    // both add_limit_order and add_market_order.
    template <typename Book>
    void match_against(Book& book, OrderId aggressor_id, Quantity& remaining,
                        std::optional<Price> limit_price, bool aggressor_is_buy,
                        const TradeCallback& on_trade);

    std::map<Price, PriceLevel, std::greater<Price>> bids_; // best bid first
    std::map<Price, PriceLevel, std::less<Price>> asks_;    // best ask first
    std::unordered_map<OrderId, Location> order_index_;
    OrderPool pool_;
};

} // namespace lob
