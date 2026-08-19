#pragma once

#include <cstdint>

namespace lob {

using OrderId = uint64_t;
using Price = int64_t;   // integer ticks, never floating point
using Quantity = uint64_t;
using Sequence = uint64_t;

enum class Side : uint8_t { Buy, Sell };
enum class OrderType : uint8_t { Limit, Market };

// A resting or in-flight order. Deliberately a plain struct with intrusive
// prev/next pointers instead of living inside a std::list<Order>: a price
// level's FIFO queue links Order objects directly, so removing a filled or
// cancelled order is an O(1) pointer splice with no queue lookup and no
// second heap allocation for a list node.
struct Order {
    OrderId id = 0;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    Price price = 0;          // ignored for Market orders
    Quantity quantity = 0;    // remaining, unfilled quantity
    Sequence sequence = 0;    // assigned on arrival; breaks price ties by time

    Order* prev = nullptr;
    Order* next = nullptr;
};

struct Trade {
    OrderId resting_order_id;
    OrderId aggressor_order_id;
    Price price;
    Quantity quantity;
};

} // namespace lob
