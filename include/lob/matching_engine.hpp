#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include "lob/order.hpp"
#include "lob/order_book.hpp"

namespace lob {

// Thread-safe, multi-symbol matching engine. Owns one OrderBook per
// symbol, each behind its own mutex ("sharding by symbol"): price-time
// priority only has meaning within a single symbol's book, so two threads
// trading different symbols never need to touch the same lock. Threads
// trading the *same* symbol still serialize on that symbol's mutex --
// that contention is real and is exactly what the benchmark measures (see
// bench/benchmark.cpp), and the README discusses how a production engine
// removes it entirely (one writer thread per symbol, no lock at all).
class MatchingEngine {
public:
    using TradeCallback = std::function<void(const std::string& symbol, const Trade&)>;

    explicit MatchingEngine(TradeCallback on_trade) : on_trade_(std::move(on_trade)) {}

    OrderId submit_limit_order(const std::string& symbol, Side side, Price price, Quantity quantity) {
        Shard& shard = shard_for(symbol);
        OrderId id = next_order_id_.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(shard.mutex);
        Sequence seq = shard.next_sequence++;
        shard.book.add_limit_order(id, side, price, quantity, seq,
                                    [&](const Trade& t) { on_trade_(symbol, t); });
        return id;
    }

    // Returns the unfilled quantity (0 if fully filled).
    Quantity submit_market_order(const std::string& symbol, Side side, Quantity quantity) {
        Shard& shard = shard_for(symbol);
        OrderId id = next_order_id_.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(shard.mutex);
        Sequence seq = shard.next_sequence++;
        return shard.book.add_market_order(id, side, quantity, seq,
                                            [&](const Trade& t) { on_trade_(symbol, t); });
    }

    bool cancel_order(const std::string& symbol, OrderId id) {
        Shard& shard = shard_for(symbol);
        std::lock_guard<std::mutex> lock(shard.mutex);
        return shard.book.cancel_order(id);
    }

    // Runs `fn(const OrderBook&)` while holding the symbol's shard lock,
    // for read-only inspection (best bid/ask, depth snapshots, tests).
    template <typename Fn>
    auto with_book(const std::string& symbol, Fn&& fn) -> decltype(fn(std::declval<const OrderBook&>())) {
        Shard& shard = shard_for(symbol);
        std::lock_guard<std::mutex> lock(shard.mutex);
        return fn(static_cast<const OrderBook&>(shard.book));
    }

    std::size_t symbol_count() const {
        std::shared_lock<std::shared_mutex> lock(shards_mutex_);
        return shards_.size();
    }

private:
    struct Shard {
        std::mutex mutex;
        OrderBook book;
        Sequence next_sequence = 1;
    };

    // Looks up (or lazily creates) the shard for a symbol. New-symbol
    // creation is rare relative to order flow, so it takes an exclusive
    // lock only on that slow path; the common case (symbol already
    // exists) takes a shared lock, letting lookups for different symbols
    // proceed concurrently.
    Shard& shard_for(const std::string& symbol) {
        {
            std::shared_lock<std::shared_mutex> lock(shards_mutex_);
            auto it = shards_.find(symbol);
            if (it != shards_.end()) return *it->second;
        }
        std::unique_lock<std::shared_mutex> lock(shards_mutex_);
        auto it = shards_.find(symbol);
        if (it != shards_.end()) return *it->second;
        auto [inserted, _] = shards_.emplace(symbol, std::make_unique<Shard>());
        return *inserted->second;
    }

    std::unordered_map<std::string, std::unique_ptr<Shard>> shards_;
    mutable std::shared_mutex shards_mutex_;
    std::atomic<OrderId> next_order_id_{1};
    TradeCallback on_trade_;
};

} // namespace lob
