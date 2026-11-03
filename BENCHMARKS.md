# Benchmarks

Run: `cmake --build build && ./build/bench_book` (10M ops, fixed seed 42, first 10k ops are untimed warmup).

Workload after warmup: 45% limit, 45% cancel, 5% modify, 5% market. Prices 0-40 ticks
behind a slowly walking mid, ~7% of limits cross. The book stays around 5-7k resting orders.
Every phase replays the exact same order stream, so numbers are directly comparable.

## Phase 1 baseline: `std::map` + `std::list` + `std::unordered_map`

Machine: Apple M1, 8 GB, macOS 27.0.1, Apple clang 21, `-O3 -march=native`.
5 runs; table shows the median run, ranges in brackets.

Throughput: **7.14 M ops/s** (140 ns/op) [6.81 - 7.46]

| op     | p50 ns | p90 ns | p99 ns         | p99.9 ns            |
|--------|-------:|-------:|---------------:|--------------------:|
| limit  | 167    | 291    | 583 [334-625]  | 4875 [3291-7083]    |
| market | 125    | 334    | 833 [459-958]  | 4834 [3417-7500]    |
| cancel | 125    | 250    | 667 [292-791]  | 4708 [3291-7042]    |
| modify | 250    | 459    | 1041 [500-1167]| 5417 [3667-13250]   |
| all    | 166    | 292    | 667 [375-750]  | 4833 [3292-7292]    |

### Reading these numbers

- The macOS clock ticks at 24 MHz, so every latency is a multiple of ~41.67 ns, and a
  `Clock::now()` pair costs one tick. Medians below ~200 ns are coarse. On Linux x86, `rdtsc`
  gives ns resolution.
- p99.9 and above vary 2x run to run. That noise comes from the OS (preemption, other apps,
  no core pinning on macOS), not from the book. Take tail numbers on an isolated Linux core.

### Profile (macOS `sample`, book code only)

| bucket                                  | share |
|-----------------------------------------|------:|
| `malloc`/`free` (list, map, hash nodes) | ~34%  |
| `unordered_map` id index                | ~7%   |
| map level lookups, list ops, matching   | ~60%  |

Phase 3 targets, in order: remove per-order heap allocation (object pool, intrusive list),
then replace the `std::map` level lookup with a flat price-indexed array.

## Phase 3: one change at a time

Same machine, workload and seed. Each row is the median of 5 runs, with the range in brackets.
Every row produces the identical trade checksum `2a79b8de12e4cd7d`, so the output is
byte-for-byte the same as Phase 1.

| step | change                                              | throughput M ops/s     | ns/op | p50 ns | p99 ns |
|------|-----------------------------------------------------|-----------------------:|------:|-------:|-------:|
| 0    | baseline: `std::map` + `std::list` + `unordered_map` | 7.14 [6.81 - 7.46]     | 140   | 166    | 667    |
| 1    | pooled orders, index-linked intrusive list          | 9.25 [8.36 - 10.02]    | 108   | 125    | 292    |
| 2    | flat price-indexed ladder, cached best price        | 12.92 [4.54* - 14.46]  | 77    | 83     | 250    |
| 3    | open-addressing `IdIndex` replaces `unordered_map`  | 22.93 [17.30 - 26.65]  | 44    | 42     | 208    |
| 4    | free list reserved up front                         | 29.61 [26.34 - 30.60]  | 34    | 42     | 125    |

\* OS-noise outlier. The other four runs were 12.2 - 14.5.

Overall: **4.1x throughput** (7.14 to 29.6 M ops/s) and **zero heap allocations** in the
timed pass. The bench counts them with a global `operator new` hook.

Step 4 mostly removes the last `free_` vector growths. Part of the step 3 to 4 gap is run
to run noise; compare the ranges, not just the medians.

### Final per-op latency (step 4, one run)

| op     | p50 ns | p90 ns | p99 ns | p99.9 ns | p99.99 ns |
|--------|-------:|-------:|-------:|---------:|----------:|
| limit  | 42     | 83     | 125    | 167      | 292       |
| market | 42     | 125    | 167    | 209      | 292       |
| cancel | 42     | 83     | 84     | 125      | 291       |
| modify | 42     | 84     | 125    | 208      | 334       |
| all    | 42     | 83     | 125    | 167      | 292       |

A p50 of 42 ns is one clock tick, the macOS timer floor. The real median cost is lower;
throughput implies about 34 ns/op on average. Sub-tick precision needs `rdtsc` on Linux x86.

### What changed in the design

- **Orders** sit in a `std::vector` pool with a free list. Each price level is a doubly linked
  list of 32-bit pool indices, so there are no pointers, no per-order `new`, and 40-byte
  orders.
- **Price levels** are a flat array indexed by `price - base`. The best price is cached. When
  a best level empties, a linear scan finds the next one, which is O(gap) and about one step in
  a dense book. The ladder grows and recenters on demand. A price band (`kMaxSpan`) bounds its
  memory.
- **Order id lookup** uses open addressing with linear probing, Fibonacci hashing, a load factor
  of at most 1/2, and backward-shift deletion with no tombstones. `test.cpp` checks it against
  `std::unordered_map` over 200k random operations.

## S2: exchange features on the hot path

S2 added the following to `OrderBook`:

- A running quantity per level, which makes `Level` 16 bytes.
- A level-change hook.
- Time in force, with a `tif` byte per order.
- Self-trade prevention, with an `account` field per order and one compare per fill.
- Status and reason codes on every call.

`Order` is still 40 bytes, enforced by a `static_assert`. The bench uses none of the new
features, so this measures the cost a plain order pays for them.

The S1 and S2 builds ran alternately, 7 runs each, on the same machine and workload:

| build | median M ops/s | range        |
|-------|---------------:|--------------|
| S1    | 26.72          | 16.09 - 31.23 |
| S2    | 28.39          | 21.90 - 29.98 |

The cost is not measurable; the difference is within run-to-run noise. The trade checksum is
still `2a79b8de12e4cd7d`, and the timed pass still makes zero heap allocations.

## S4: pre-trade risk check

`bench_risk` times `Risk::check()` alone. The setup is 64 symbols, each with open
exposure and a position, and every limit is enabled. One check takes far less than the
41.67 ns clock tick, so the bench times 10M checks per pass and divides. It runs 7 passes
and takes the median. The table shows the median of 5 such runs:

| workload                                                  | ns/check |
|-----------------------------------------------------------|---------:|
| random mix: 24% rejected, across every reason, 5% market orders | 11.16    |
| all pass: same code, predictable branches                 | 8.49     |

The two rows differ by about 2.7 ns. That gap is branch misprediction: in the random mix,
which check rejects is unpredictable.

Every check is constant-time arithmetic on running totals:

- kill flag
- quantity
- reference price
- collar
- notional
- worst-case position
- open orders
- loss

There are no locks, allocations, or scans over orders. The kill flag is a relaxed
`std::atomic<bool>`. Replacing it with a plain `false` made no measurable difference
(8.43 vs 8.49 ns).

The order book bench is unchanged: checksum `2a79b8de12e4cd7d`, 0 hot-path allocations.
