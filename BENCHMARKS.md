# Benchmarks

Run: `cmake --build build && ./build/bench` (10M ops, fixed seed 42, first 10k ops are untimed warmup).

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
