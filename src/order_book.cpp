#include "lob/order_book.hpp"

#include <algorithm>

namespace lob {

template <typename Book>
void OrderBook::match_against(Book& book, OrderId aggressor_id, Quantity& remaining,
                               std::optional<Price> limit_price, bool aggressor_is_buy,
                               const TradeCallback& on_trade) {
    while (remaining > 0 && !book.empty()) {
        auto it = book.begin();
        PriceLevel& level = it->second;

        if (limit_price.has_value()) {
            // A buy can only cross asks at or below its limit; a sell can
            // only cross bids at or above its limit. book is sorted so
            // that begin() is always the best (most aggressive-friendly)
            // price, so the first level that fails this check means every
            // remaining level does too -- stop rather than scan further.
            if (aggressor_is_buy && level.price > *limit_price) break;
            if (!aggressor_is_buy && level.price < *limit_price) break;
        }

        while (remaining > 0 && level.head) {
            Order* resting = level.head; // oldest order at this price: time priority
            Quantity traded = std::min(remaining, resting->quantity);

            on_trade(Trade{resting->id, aggressor_id, level.price, traded});

            remaining -= traded;
            resting->quantity -= traded;
            level.total_quantity -= traded;

            if (resting->quantity == 0) {
                level.remove(resting);
                order_index_.erase(resting->id);
                pool_.release(resting);
            }
        }

        if (level.empty()) {
            book.erase(it);
        }
    }
}

void OrderBook::add_limit_order(OrderId id, Side side, Price price, Quantity quantity,
                                 Sequence sequence, const TradeCallback& on_trade) {
    Quantity remaining = quantity;

    if (side == Side::Buy) {
        match_against(asks_, id, remaining, price, /*aggressor_is_buy=*/true, on_trade);
    } else {
        match_against(bids_, id, remaining, price, /*aggressor_is_buy=*/false, on_trade);
    }

    if (remaining == 0) return;

    Order* o = pool_.acquire();
    o->id = id;
    o->side = side;
    o->type = OrderType::Limit;
    o->price = price;
    o->quantity = remaining;
    o->sequence = sequence;

    if (side == Side::Buy) {
        PriceLevel& level = bids_[price];
        level.price = price;
        level.push_back(o);
    } else {
        PriceLevel& level = asks_[price];
        level.price = price;
        level.push_back(o);
    }
    order_index_[id] = Location{side, price, o};
}

Quantity OrderBook::add_market_order(OrderId id, Side side, Quantity quantity,
                                      Sequence /*sequence*/, const TradeCallback& on_trade) {
    Quantity remaining = quantity;
    if (side == Side::Buy) {
        match_against(asks_, id, remaining, std::nullopt, /*aggressor_is_buy=*/true, on_trade);
    } else {
        match_against(bids_, id, remaining, std::nullopt, /*aggressor_is_buy=*/false, on_trade);
    }
    return remaining; // any unfilled quantity is dropped (IOC semantics), not rested
}

bool OrderBook::cancel_order(OrderId id) {
    auto idx_it = order_index_.find(id);
    if (idx_it == order_index_.end()) return false;

    const Location loc = idx_it->second;
    Order* o = loc.order;

    if (loc.side == Side::Buy) {
        auto level_it = bids_.find(loc.price);
        PriceLevel& level = level_it->second;
        level.total_quantity -= o->quantity;
        level.remove(o);
        if (level.empty()) bids_.erase(level_it);
    } else {
        auto level_it = asks_.find(loc.price);
        PriceLevel& level = level_it->second;
        level.total_quantity -= o->quantity;
        level.remove(o);
        if (level.empty()) asks_.erase(level_it);
    }

    order_index_.erase(idx_it);
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

std::vector<OrderBook::Level> OrderBook::bid_levels(std::size_t depth) const {
    std::vector<Level> result;
    result.reserve(std::min(depth, bids_.size()));
    for (auto it = bids_.begin(); it != bids_.end() && result.size() < depth; ++it) {
        result.push_back({it->first, it->second.total_quantity});
    }
    return result;
}

std::vector<OrderBook::Level> OrderBook::ask_levels(std::size_t depth) const {
    std::vector<Level> result;
    result.reserve(std::min(depth, asks_.size()));
    for (auto it = asks_.begin(); it != asks_.end() && result.size() < depth; ++it) {
        result.push_back({it->first, it->second.total_quantity});
    }
    return result;
}

} // namespace lob
