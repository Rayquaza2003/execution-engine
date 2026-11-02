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
