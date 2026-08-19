// Throughput and latency benchmark for MatchingEngine.
//
// Three scenarios:
//   1. single-threaded  -- baseline: one thread, one symbol, no locking cost.
//   2. contended        -- N threads, ALL trading the same symbol, so every
//                          submit serializes on that one symbol's mutex.
//   3. sharded          -- N threads, each trading its OWN symbol, so no
//                          thread ever waits on another's lock.
//
// (2) vs (3) is the concrete, measured version of "what happens under lock
// contention, and how would you shard it" -- see README.md for numbers
// from a representative run and how to read them.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "lob/matching_engine.hpp"

using namespace lob;
using Clock = std::chrono::steady_clock;

namespace {

struct Report {
    std::string name;
    std::uint64_t order_count = 0;
    double wall_seconds = 0.0;
    std::vector<double> latencies_us; // one entry per submitted order

    double throughput_ops_per_sec() const {
        return wall_seconds > 0.0 ? static_cast<double>(order_count) / wall_seconds : 0.0;
    }
};

double percentile(std::vector<double>& sorted_us, double p) {
    if (sorted_us.empty()) return 0.0;
    std::size_t idx = static_cast<std::size_t>(p / 100.0 * static_cast<double>(sorted_us.size() - 1));
    return sorted_us[idx];
}

void print_report(Report& r) {
    std::sort(r.latencies_us.begin(), r.latencies_us.end());
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "\n== " << r.name << " ==\n"
              << "  orders:       " << r.order_count << "\n"
              << "  wall time:    " << r.wall_seconds << " s\n"
              << "  throughput:   " << r.throughput_ops_per_sec() << " orders/sec\n"
              << "  latency p50:  " << percentile(r.latencies_us, 50) << " us\n"
              << "  latency p90:  " << percentile(r.latencies_us, 90) << " us\n"
              << "  latency p99:  " << percentile(r.latencies_us, 99) << " us\n"
              << "  latency p99.9:" << percentile(r.latencies_us, 99.9) << " us\n"
              << "  latency max:  " << (r.latencies_us.empty() ? 0.0 : r.latencies_us.back()) << " us\n";
}

// One thread's worth of synthetic order flow against `symbol`: a mix of
// limit orders (most of them, spread around a walking mid-price so some
// cross and most rest), market orders, and cancels of its own recently
// resting orders. Records one latency sample per engine call into
// `out_latencies_us`. `seed` should differ per thread so RNG streams
// don't correlate (and so each thread's mt19937_64 lives on its own
// cache line, not shared -- a shared RNG would itself be a contention
// point unrelated to the thing we're trying to measure).
void generate_load(MatchingEngine& engine, const std::string& symbol, std::uint64_t seed,
                    int order_count, std::vector<double>& out_latencies_us) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> action_dist(0, 99);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<Price> price_offset_dist(-50, 50);
    std::uniform_int_distribution<Quantity> qty_dist(1, 100);

    Price mid = 10000;
    std::vector<OrderId> recent_ids;
    recent_ids.reserve(256);

    out_latencies_us.reserve(order_count);

    for (int i = 0; i < order_count; ++i) {
        int action = action_dist(rng);
        Side side = side_dist(rng) == 0 ? Side::Buy : Side::Sell;

        auto t0 = Clock::now();
        if (action < 80) { // 80%: limit order
            Price price = mid + price_offset_dist(rng);
            Quantity qty = qty_dist(rng);
            OrderId id = engine.submit_limit_order(symbol, side, price, qty);
            if (recent_ids.size() < 256) recent_ids.push_back(id);
            // Slow, gentle random walk so the book doesn't drift unboundedly.
            mid += (price_offset_dist(rng) % 3);
        } else if (action < 90) { // 10%: market order
            Quantity qty = qty_dist(rng);
            engine.submit_market_order(symbol, side, qty);
        } else { // 10%: cancel a recently placed order (may already be filled -- that's fine)
            if (!recent_ids.empty()) {
                std::uniform_int_distribution<std::size_t> pick(0, recent_ids.size() - 1);
                std::size_t idx = pick(rng);
                engine.cancel_order(symbol, recent_ids[idx]);
                recent_ids[idx] = recent_ids.back();
                recent_ids.pop_back();
            }
        }
        auto t1 = Clock::now();
        out_latencies_us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
}

Report run_single_threaded(int order_count) {
    MatchingEngine engine([](const std::string&, const Trade&) {});
    Report r;
    r.name = "single-threaded (baseline)";
    r.order_count = static_cast<std::uint64_t>(order_count);

    auto start = Clock::now();
    generate_load(engine, "SYM", /*seed=*/1, order_count, r.latencies_us);
    auto end = Clock::now();

    r.wall_seconds = std::chrono::duration<double>(end - start).count();
    return r;
}

// shared_symbol = true  -> every thread trades "SYM" (contended shard)
// shared_symbol = false -> thread i trades "SYM<i>" (own shard, no contention)
Report run_multi_threaded(int thread_count, int orders_per_thread, bool shared_symbol,
                           const std::string& label) {
    MatchingEngine engine([](const std::string&, const Trade&) {});
    Report r;
    r.name = label;
    r.order_count = static_cast<std::uint64_t>(thread_count) * static_cast<std::uint64_t>(orders_per_thread);

    std::vector<std::vector<double>> per_thread_latencies(thread_count);
    std::vector<std::thread> threads;
    threads.reserve(thread_count);

    auto start = Clock::now();
    for (int t = 0; t < thread_count; ++t) {
        std::string symbol = shared_symbol ? "SYM" : ("SYM" + std::to_string(t));
        threads.emplace_back([&engine, symbol, t, orders_per_thread, &per_thread_latencies]() {
            generate_load(engine, symbol, /*seed=*/1000 + static_cast<std::uint64_t>(t),
                          orders_per_thread, per_thread_latencies[t]);
        });
    }
    for (auto& th : threads) th.join();
    auto end = Clock::now();

    r.wall_seconds = std::chrono::duration<double>(end - start).count();
    for (auto& v : per_thread_latencies) {
        r.latencies_us.insert(r.latencies_us.end(), v.begin(), v.end());
    }
    return r;
}

} // namespace

int main(int argc, char** argv) {
    int single_count = 200000;
    int thread_count = static_cast<int>(std::max(2u, std::thread::hardware_concurrency()));
    int orders_per_thread = 50000;

    if (argc > 1) single_count = std::atoi(argv[1]);
    if (argc > 2) thread_count = std::atoi(argv[2]);
    if (argc > 3) orders_per_thread = std::atoi(argv[3]);

    std::cout << "Limit order book benchmark\n"
              << "  hardware_concurrency: " << std::thread::hardware_concurrency() << "\n"
              << "  threads used for multi-threaded scenarios: " << thread_count << "\n";

    Report baseline = run_single_threaded(single_count);
    print_report(baseline);

    Report contended = run_multi_threaded(thread_count, orders_per_thread, /*shared_symbol=*/true,
                                           "contended (" + std::to_string(thread_count) + " threads, 1 shared symbol)");
    print_report(contended);

    Report sharded = run_multi_threaded(thread_count, orders_per_thread, /*shared_symbol=*/false,
                                         "sharded (" + std::to_string(thread_count) + " threads, " +
                                             std::to_string(thread_count) + " symbols)");
    print_report(sharded);

    std::cout << "\n== summary ==\n"
              << "  sharded / contended throughput ratio: "
              << (contended.throughput_ops_per_sec() > 0
                      ? sharded.throughput_ops_per_sec() / contended.throughput_ops_per_sec()
                      : 0.0)
              << "x\n"
              << "  (ratio near " << thread_count << "x means the contended run's single mutex is the "
              << "bottleneck; near 1x means something else -- e.g. memory bandwidth -- dominates first)\n";

    return 0;
}
