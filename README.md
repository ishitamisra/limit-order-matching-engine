# Limit Order Book / Matching Engine

A price-time priority limit order matching engine in C++17, the same
matching model real exchanges (NASDAQ, NYSE, most crypto venues) use:
at a given price, the order that arrived first fills first.

Supports limit orders, market orders (IOC — fill what you can, drop the
rest), and cancels. Multi-symbol and thread-safe, sharded by symbol so
trading two different symbols concurrently never contends on the same
lock. Comes with unit tests and a benchmark that measures throughput and
latency percentiles, including a deliberate contended-vs-sharded
comparison to make lock contention visible as a number, not just a claim.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
cd build && ctest --output-on-failure && cd ..   # unit tests
./build/lob_demo                                 # interactive terminal REPL
./build/lob_server 8080                          # web UI: http://localhost:8080
./build/lob_bench [orders] [threads] [orders_per_thread]  # benchmark
```

Requires a C++17 compiler, CMake 3.16+, and threading support (pthreads
on Linux/macOS). No third-party dependencies — the test framework is a
~30-line header (`tests/test_framework.hpp`) and the web UI is a hand-rolled
HTTP server (`src/http_server.cpp`) serving a single embedded HTML page;
pulling in Catch2/GTest or a real web framework felt like overkill for
what this project needs.

(If your `ctest` predates CMake 3.20, it doesn't understand `--test-dir`
and will silently report every test "Not Run" — `cd build` first, as
above, works on any version.)

## Interactive demo

```sh
$ ./build/lob_demo
> limit AAPL sell 101 5
  order #1 accepted
> limit AAPL sell 100 5
  order #2 accepted
> book AAPL
ASK PRICE   QTY
101         5
100         5
----
BID PRICE   QTY
> limit AAPL buy 101 7
  TRADE AAPL 5 @ 100  (resting #2 x aggressor #3)
  TRADE AAPL 2 @ 101  (resting #1 x aggressor #3)
  order #3 accepted
```

The incoming buy at 101 crosses the book: it fills against the *cheaper*
resting order first (100, price priority) even though that order arrived
second, then spills into the next price level for its remaining 2 shares.

## Web UI

```sh
./build/lob_server 8080   # then open http://localhost:8080
```

![Web UI showing a live depth ladder, order entry form, and trade tape](docs/web-ui.png)

A live view of the same engine the REPL and benchmark drive: a
depth-proportional bid/ask ladder, an order entry form (limit/market,
buy/sell), a running trade tape, and a "seed liquidity" button that
populates the book with random two-sided resting orders so it doesn't
start empty. The page polls a small JSON API every 400ms — no
frameworks, no build step, single embedded HTML file
(`include/lob/web_ui.hpp`) served by a ~150-line hand-rolled HTTP server
(`src/http_server.cpp`). Endpoints: `GET /api/book`, `GET /api/trades`,
`POST /api/order`, `POST /api/cancel`, `POST /api/seed` — see
`src/server_main.cpp`, which is a thin translation layer only; all
matching logic still lives in `OrderBook`/`MatchingEngine`.

## Architecture

```
include/lob/order.hpp            Order, Trade, Side, OrderType — plain value types
include/lob/order_pool.hpp       fixed-block allocator for Order (no malloc per order)
include/lob/order_book.hpp       single-symbol OrderBook + PriceLevel (the matching core)
src/order_book.cpp
include/lob/matching_engine.hpp  thread-safe, multi-symbol wrapper (sharded by symbol)
include/lob/http_server.hpp      minimal HTTP/1.1 server (used only by lob_server)
src/http_server.cpp
include/lob/web_ui.hpp           embedded HTML/CSS/JS for the web UI
src/server_main.cpp              REST API + web UI, thin wrapper over MatchingEngine
src/main.cpp                     interactive terminal REPL demo
bench/benchmark.cpp              throughput + latency percentile benchmark
tests/                           unit tests (ctest)
```

## Data structures, and why

This is the part of the project that's actually interesting, so here's
the reasoning, not just the result.

**Price levels: `std::map<Price, PriceLevel>`.** An order book needs
"give me the best price" in O(1)-ish time and "insert/remove a price
level" in better than O(n). A `std::map` (red-black tree) gives O(log P)
insert/erase/lookup where P is the number of *distinct price levels* —
not the number of orders, which matters because P is usually small (tens
to low hundreds) even when order count is in the millions. `begin()`
always yields the best price with no scan, which is why bids are keyed
with `std::greater<Price>` (best bid = highest price = `begin()`) and
asks with `std::less<Price>` (best ask = lowest price = `begin()`) —
both sides read as "the best price is always at the front."

Alternatives and why they lose here: a hash map is O(1) per level but
can't answer "what's the best price" without scanning every key; a flat
sorted `std::vector<PriceLevel>` gives cache-friendly iteration but
O(P) `insert`/`erase` because opening or closing a price level shifts
every element after it — expensive exactly when the book is moving
fast. A production engine (see "microsecond latency" below) usually
goes further, e.g. a fixed-size array indexed directly by
`price - min_price` for O(1) level access when the tradable price range
is bounded, but that's a real complexity jump for a bounded, known price
range this project doesn't assume.

**Time priority within a level: an intrusive doubly linked list.**
`PriceLevel` doesn't hold a `std::list<Order>`; it holds a raw
`head`/`tail` `Order*` pair, and `Order` itself carries `prev`/`next`
pointers (`include/lob/order.hpp`). That's what "intrusive" means: the
list is threaded directly through the object instead of wrapping it in
a separate list node. Two things fall out of that:

- **O(1) cancel-by-pointer.** Given an `Order*`, unlinking it from its
  price level's FIFO is three pointer writes — no scanning the list to
  find it. A non-intrusive `std::list` would need the same, in
  principle, given an iterator, but you'd have to *keep* that iterator
  valid somewhere else, which is exactly what the intrusive list gives
  you for free by being the object itself.
- **No per-node allocation.** A `std::list<Order>` allocates a
  heap-owned list node *in addition to* the `Order`. The intrusive list
  has no separate node — there's nothing to allocate beyond the `Order`
  itself.

**Order allocation: a fixed-block pool with a freelist**
(`include/lob/order_pool.hpp`), not `new`/`delete` per order. Matching
engines are exactly the kind of hot loop where `malloc`/`free` cost
shows up in tail latency — general-purpose allocators take internal
locks and have unpredictable worst-case behavior (arena contention,
`mmap` calls when a large enough block needs a fresh page). The pool
here preallocates `Order` slots in a `std::deque<Order>` (chosen over
`std::vector<Order>` specifically because growing a `deque` never
invalidates references to existing elements, so the pool can grow
without dangling the `Order*` pointers already stored in the book —
`vector` would reallocate and break all of them) and hands slots back to
a freelist on cancel/fill instead of freeing them. `acquire()`/
`release()` are both O(1), no syscalls, no locks (a shard's pool is only
ever touched while already holding that shard's mutex).

**Cancel-by-id: `std::unordered_map<OrderId, Location>`.** Cancelling by
order id needs to find the order without scanning every price level.
The index hash map stores `{side, price, Order*}`, so a cancel is:
hash lookup → jump straight to the price level → O(1) intrusive unlink →
erase the index entry → return the `Order*` to the pool. All O(1)
average case.

## Matching algorithm (price-time priority)

`OrderBook::match_against` (`src/order_book.cpp`) is the shared core for
both limit and market orders: walk the opposite side's price levels from
best to worst; within each level, walk the FIFO from `head` (oldest); at
each resting order, trade `min(incoming remaining, resting remaining)`;
if a resting order is fully consumed, unlink it and return it to the
pool; if a price level empties out, erase it from the map; stop when the
incoming order is fully filled, or (for a limit order) the next price
level would violate the limit price, or the opposite side runs out of
liquidity.

- **Limit order**: matches while price crosses; any unfilled remainder
  rests in the book at its limit price (joining the back of that price
  level's FIFO — respecting time priority for future incoming orders).
- **Market order**: matches with no price limit until filled or the book
  is empty; an unfilled remainder is *dropped*, never rested (standard
  IOC — "immediate or cancel" — semantics for a market order; a market
  order that could rest indefinitely at no price isn't really a market
  order).
- **Cancel**: O(1) removal via the id index, described above.

## Concurrency: sharded by symbol

`MatchingEngine` (`include/lob/matching_engine.hpp`) owns one
`OrderBook` per symbol, **each behind its own `std::mutex`**. Price-time
priority is only meaningful *within* one symbol's book — an AAPL order
and an MSFT order never need to serialize against each other — so
sharding by symbol is the natural unit of concurrency: submitting orders
for different symbols from different threads should scale close to
linearly, while submitting orders for the *same* symbol from multiple
threads necessarily serializes on that symbol's mutex, because matching
is inherently a serial operation (order N+1's outcome can depend on
order N having already been applied).

New-symbol creation (rare) takes a brief exclusive lock on a
`shared_mutex` guarding the symbol→shard map; looking up an existing
symbol (the hot path) takes only a shared lock, so concurrent order flow
for different existing symbols never blocks on that map either.

### Contention, measured

`bench/benchmark.cpp` runs the same total order count three ways —
single-threaded, N threads all hammering **one** shared symbol
(contended), and N threads each on their **own** symbol (sharded) — and
reports throughput and latency percentiles for each. Representative run
on this machine (4 cores, `Release` build, `./build/lob_bench 200000 4
50000`):

| scenario | throughput (orders/sec) | p50 | p90 | p99 | p99.9 |
|---|---|---|---|---|---|
| single-threaded baseline | ~6.0M | 0.11 us | 0.18 us | 0.31 us | 2.70 us |
| **contended** (4 threads, 1 symbol) | ~1.1M | 0.40 us | 3.45 us | 52.40 us | 113.16 us |
| **sharded** (4 threads, 4 symbols) | ~3.3M | 0.96 us | 1.66 us | 2.57 us | 15.72 us |

Sharding recovers **~2.9x** throughput over the contended case on 4
cores (ideal would be 4x; the gap is mutex handoff and scheduler
overhead, not algorithmic) — and the effect is far more visible in the
tail than in the median: contended p50 is only 4x worse than baseline,
but contended p99 is over **150x** worse. That's the concrete shape of
lock contention: it doesn't slow every request down a little, it makes a
growing fraction of requests wait behind whichever request currently
holds the lock, which is exactly what latency percentiles are for
measuring and averages hide. Run it yourself — numbers depend heavily on
core count and whether other load is sharing the machine:

```sh
./build/lob_bench 200000 4 50000   # order_count threads orders_per_thread
```

## Interview talking points

**Why `std::map` and not a hash map for price levels?** Covered above —
you need sorted iteration to find the best price cheaply; a hash map
would need an auxiliary sorted structure (or a full scan) to answer
that.

**Why an intrusive linked list instead of `std::list` or `std::deque`?**
No per-node allocation, and O(1) removal given a raw pointer instead of
an iterator you have to keep valid. This is also literally how
production order books are built — e.g. LMAX Disruptor-style designs
and most C++ matching engines use intrusive containers for exactly this
reason.

**How do real exchanges get microsecond (or sub-microsecond) latency?**
Several things this project deliberately does *not* do, because they're
a different tier of complexity than a portfolio project needs, but are
worth knowing about:
- **One thread per instrument, no locks at all.** Instead of a mutex per
  symbol, pin a single writer thread to each symbol/shard and feed it
  orders through a lock-free single-producer/multi-consumer queue (or
  route by symbol upstream so each writer is genuinely
  single-consumer). No mutex means no futex syscalls and no scheduler
  involvement on the hot path at all — see the "next step" below.
- **Kernel bypass networking** (e.g. `DPDK`, `Solarflare`/`OpenOnload`)
  to skip the OS network stack, which otherwise dominates a
  microsecond budget on its own.
- **Custom memory allocators and pre-touched, huge-page-backed memory**
  to eliminate page faults and TLB misses on the hot path — this
  project's `OrderPool` is a first step in that direction (no
  malloc/free per order) but doesn't go as far as huge pages or NUMA
  pinning.
- **Cache-line-aware layout**: padding hot structs to avoid false
  sharing between cores, keeping the matching loop's working set inside
  L1/L2.
- **Busy-polling instead of blocking waits** — a thread parked on a
  futex has to be woken by the scheduler, which costs microseconds by
  itself; a busy-spinning thread avoids that at the cost of burning a
  full core.

**What happens under a burst of orders?** Throughput at the shard level
is bounded by however fast that shard's single mutex can be
acquired/released and the matching loop can run — the benchmark's
contended scenario *is* a burst simulation. Queueing theory says the
useful number isn't "can it clear a burst eventually" but "what does the
queue-wait tail look like while it's clearing" — which is exactly what
the p99/p99.9 columns above are measuring, not just the mean.

**How would you shard this further / scale it out?** Sharding by symbol
is the first cut and is what this project does. Beyond that: (1) within
one very high-volume symbol, you generally *can't* shard further without
breaking price-time priority — it's inherently a single serial stream of
truth — so the next lever is making that one stream faster (remove the
lock, see above), not splitting it; (2) across symbols, shard-per-thread
already lets you scale roughly with core count and, since shards share
no state, you could just as easily run each shard's writer thread on a
separate *process* or *machine*, exchanging only trade/market-data
output instead of shared memory.

**Why does a market order drop its unfilled remainder instead of
resting?** Because a market order has no price — resting it would mean
resting an order with no limit on what it's willing to pay, which isn't
what "market order" means on any real venue. This mirrors standard IOC
(immediate-or-cancel) semantics.

## Profiling / flame graphs

`lob_bench` is built with `-g` even in the `Release` config
(`CMakeLists.txt`) specifically so a profiler can resolve symbols. On
Linux, with `perf` installed:

```sh
perf record -F 999 --call-graph dwarf -- ./build/lob_bench 500000 4 100000
perf script > out.perf
# then feed out.perf to Brendan Gregg's FlameGraph scripts:
#   https://github.com/brendangregg/FlameGraph
#   stackcollapse-perf.pl out.perf | flamegraph.pl > flame.svg
```

On macOS, `xctrace record --template 'Time Profiler' --launch --
./build/lob_bench 500000 4 100000` (or just Instruments' Time Profiler
template via the Xcode GUI) does the same job.

What you should expect to see dominating the profile: the `std::map`
insert/erase for price levels (tree rebalancing) and the mutex
acquire/release in the contended benchmark scenario specifically —
confirming, visually, the same story the p99 numbers already tell.

## Testing

`ctest --test-dir build` runs two suites:
- `tests/test_order_book.cpp` — single-symbol correctness: resting,
  full/partial fills, price priority, time priority, cancel, market
  order IOC behavior.
- `tests/test_matching_engine.cpp` — multi-symbol isolation, unique
  order ids across symbols, and a concurrency correctness check (8
  threads submitting 500 orders each against one seeded resting order,
  asserting the total filled quantity is exactly right with no lost or
  double-counted fills — a correctness check, distinct from the
  benchmark's throughput numbers).

Both suites, and the benchmark, have been run clean under AddressSanitizer
+ UndefinedBehaviorSanitizer (`-fsanitize=address,undefined`) with no
reports, including the multi-threaded scenarios.

## Known limitations / explicitly out of scope

- **Prices are `int64_t` ticks, not decimals.** Real venues quote in
  fixed tick sizes for exactly this reason — floating point isn't safe
  for money. Converting a display price like `100.25` to ticks is left
  to a caller/adapter layer this project doesn't include.
- **No persistence or replay/recovery log.** A real exchange logs every
  order/cancel/fill to durable storage so state can be rebuilt after a
  crash; this project is in-memory only.
- **No order modification ("cancel-replace").** Only add and cancel are
  supported; a modify is a cancel + a new add with a new id at the API
  layer, which loses the original order's time priority — the same
  tradeoff real venues make (a modify that changes price or increases
  quantity loses queue position; this project doesn't special-case a
  quantity-decrease-only modify that could keep it).
- **No self-trade prevention, no risk checks, no fees/rebates.** Pure
  matching logic only.
