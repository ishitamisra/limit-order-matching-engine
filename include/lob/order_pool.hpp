#pragma once

#include <cstddef>
#include <deque>
#include <vector>

#include "lob/order.hpp"

namespace lob {

// Fixed-block allocator for Order objects: a growable slab plus a freelist
// of reclaimed slots. Real matching engines avoid malloc/free on the hot
// path (both because it's slow -- a syscall-backed allocator under
// contention is a common source of tail latency -- and because it's
// non-deterministic). This pool hands out a raw Order* in O(1), and
// `release` returns it to the freelist in O(1) instead of calling delete.
//
// The slab is a std::deque<Order> rather than std::vector<Order>: deque
// only ever appends via emplace_back here and never erases individual
// elements, and per the standard, push/emplace_back on a deque never
// invalidates references or pointers to existing elements (only
// iterators). That makes it safe to hand out Order* that the order book
// stores long-term (in intrusive list links and the id index) while the
// pool keeps growing -- a std::vector would reallocate and dangle them.
class OrderPool {
public:
    explicit OrderPool(std::size_t /*initial_capacity_hint*/ = 1024) {}

    Order* acquire() {
        if (!freelist_.empty()) {
            Order* o = freelist_.back();
            freelist_.pop_back();
            *o = Order{};
            return o;
        }
        slab_.emplace_back();
        return &slab_.back();
    }

    void release(Order* o) { freelist_.push_back(o); }

    std::size_t allocated() const { return slab_.size(); }
    std::size_t free_count() const { return freelist_.size(); }

private:
    std::deque<Order> slab_;
    std::vector<Order*> freelist_;
};

} // namespace lob
