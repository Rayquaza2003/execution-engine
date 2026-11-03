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

## S5: market data from ITCH

### Real data check

Input is the first 300 MB of NASDAQ's public sample `01302019.NASDAQ_ITCH50.gz`. That
decompresses to 751 MB: 22.4M book messages from 04:00 to 09:38 on 2019-01-30.

- **Parser.** Every execute, cancel, delete and replace in the file names a live order with
  enough shares. That makes 0 bad references across 22.4M messages. Timestamps never go
  backwards.
- **Replay.** I replayed AAPL, MSFT, SPY, QQQ and TSLA and wrote 32,369 one-second bars. None
  of them shows a crossed or locked book. Of the 725,124 messages for these symbols, 75 were
  dropped. Those are stub quotes outside the book's price band, and later messages for the
  same orders.

### Throughput

`bench_md` uses a synthetic feed: 5M messages, 8 symbols, about 1000 resting orders per
symbol. Each row is the median of 5 passes, and the medians held over 5 runs:

| stage                                     | M msgs/s | ns/msg |
|-------------------------------------------|---------:|-------:|
| parse only                                | 85.4     | 12     |
| + replay into the venue's order books     | 14.3     | 70     |
| + client L2 books, NBBO, 1 s bars         | 7.3      | 137    |

The client L2 book costs as much as the matching book. It is a `std::map` per side, so it
allocates per price level and chases pointers. The upgrade path is the same flat price ladder
`OrderBook` uses. This is marked with a `ponytail:` comment in `market_data.hpp`.

End to end on the real file, `itch_replay` does the following in 0.61 s, which is
**36.9 M msgs/s** (median of 5 runs):

- reads all 751 MB
- parses every message
- books the 5 symbols
- writes the CSV

Most messages belong to other symbols and are skipped after the parse.

One bug showed up while measuring. Reserving 1M orders per symbol made each id table about
32 MB, and random lookups into it missed cache. The default capacity grows on demand instead,
and the final books came out identical. The timing difference was 0.74 vs 0.61 s, but those
were single noisy runs.

The order book checksum is still `2a79b8de12e4cd7d`.

## S6: queue position study

`fill_study` runs on the same 751 MB file with the same 5 symbols. It replays the books and
places 433k virtual passive orders: a timer join every 100 ms, plus a join at each new best
level, at six latencies each. It tracks each order's exact queue position and writes one CSV
row per order.

The run takes 0.79 s (median of 5 runs, 0.76 to 0.94 s). The plain replay in `itch_replay`
takes 0.61 s, so the study adds about 0.2 s. Results are in `research/README.md`.

The order book checksum is still `2a79b8de12e4cd7d`. The book only gained const accessors.

## S7: execution algos

`algo_compare` replays the same 751 MB file 36 times. That is 6 algos, each run as 6 groups of
parents that don't overlap. In total it works 170 parent orders per algo through the OMS and
risk, with 100 us of latency each way. The whole run takes 14.7 s, about 0.41 s per replay.
The plain replay in S5 took 0.61 s, so the algos and the OMS add little. TCA results are in
`research/README.md`.

`Venue::replay_execute` now fills our resting orders that rank ahead of a feed execution. The
code walks from the best price down to the executed order, and executions almost always hit
the front of the best level. The order book checksum is still `2a79b8de12e4cd7d`.

## S8: smart order routing

`sor_compare` builds three venues from one replay of the 751 MB file. Each feed message for
our symbols goes to every venue, and only the venue that holds the order changes its book.
It runs 395 parents in each of 4 modes. The whole run takes 1.97 s (3 runs, all 1.97 s),
about 0.49 s per mode. Two runs give byte-identical CSVs. Results are in `research/README.md`.

## S9: threads, queues, logger

macOS can't pin a thread to a core. Threads ask for the performance cores through the highest
QoS class, and the scheduler places them. The clock ticks every 41.67 ns, so single-hop
numbers near 200 ns are within a few clock ticks. Pinned, isolated cores on Linux are still to
do.

### Lock-free SPSC queue vs a mutex around a deque (`bench_spsc`)

Both queues have the same interface and a capacity of 4096. Throughput is 20M messages, median
of 5 runs. The round trip has one message in flight: thread A sends a timestamp and thread B
echoes it back on a second queue. That is 200k round trips, and the median run is shown.

| | throughput | round trip p50 | p99 | p99.9 |
|---|---|---|---|---|
| SPSC | 22.1 M msgs/s (18.1 to 30.9) | 215 ns | 255 ns | 335 ns |
| mutex | 13.3 M msgs/s (8.9 to 15.8) | 16.4 µs | 147 µs | 524 µs |

Throughput depends on where the scheduler puts the two threads. One earlier run gave
28.8 vs 16.1 M msgs/s.

### Async logger

`log()` costs 34 ns per call on the hot path. Formatting the same line with `fprintf` to
`/dev/null` costs 270 ns. That is 50 batches of 8192 calls, each batch small enough to fit
the ring so nothing is dropped.

### Pipeline (`bench_pipeline`, real ITCH, 5 symbols, 725k messages)

The run has four threads: feed, venue, market data, and strategy with OMS and risk. Five SPSC
queues connect them, and no other memory is shared. The strategy keeps one order at the bid
and one at the ask per symbol, and replaces them as the touch moves.

The table shows 3 runs of each mode:

| | throughput | tick-to-order p50 | p99 |
|---|---|---|---|
| one thread, no queues | 3.85 to 4.20 M msgs/s | 431 ns | 3.3 µs |
| pipeline, flat out | 3.0 to 4.3 M msgs/s | queue-bound | queue-bound |
| pipeline, paced at 250k msgs/s | n/a | 1.5 µs | about 1 ms |

**Throughput.** The pipeline gains no throughput: the venue stage is the bottleneck, and
splitting the other stages off doesn't speed it up.

**Hops.** Paced, each hop (enqueue to dequeue) takes 215 to 300 ns at p50 and about 0.5 to
1 µs at p99. The three hops a quote crosses add about 1 µs to tick-to-order.

**Tails come from the OS.** With four busy-spinning threads on four performance cores,
another process sometimes preempts a spinner. That gives the p99 of about 1 ms. Two waiting
strategies were measured:

- Yielding to the OS when idle made tick-to-order p99 about 60 to 100 ms in paced runs, so it
  was dropped. On macOS, a thread that yields gets parked.
- Busy-spinning with the CPU's spin hint (`yield` on ARM) doubled flat-out throughput compared
  with a bare spin, 1.2 to 2.5 M msgs/s on the synthetic feed.

**Synthetic feed.** Executions hit random queue positions instead of the front, so
`fill_ahead` walks deep queues. That makes the synthetic feed a poor yardstick here, so the
pipeline is measured on the real file.

### ThreadSanitizer

`cmake -B build-tsan -DCMAKE_BUILD_TYPE=Tsan`. All 19 test suites pass under TSan, and so does
the full `bench_pipeline` run on the real file. To check that the test catches a real bug, one
release store in the queue was weakened to relaxed. TSan then reports a data race in
`try_pop`.

The order book checksum is still `2a79b8de12e4cd7d`.

## S10: FIX gateway over TCP

`fix_gateway` and `fix_client` talk over loopback on the same M1. The client runs one FIX
session. Its requests are a mix:

- 70% new limit orders: 100 shares, a random side, within 5 cents of $100.00, so many of them
  cross. A run of 100k requests gets about 44k fill reports, both sides of each trade
  included.
- 30% cancels of an order it believes is resting.

Wire to wire means from the client's `write()` of a request to its `read()` of the first reply
to that request. The gateway also times itself, from receiving an order to writing that order's
first execution report. That covers its own read, FIX parse, the matching engine (on another
thread, through the SPSC queues), the FIX encode and the write.

These are 3 runs of each. The ranges are across runs.

| | p50 | p90 | p99 | p99.9 | throughput |
|---|---|---|---|---|---|
| bare TCP echo, 150 B (`bench_tcp`) | 29.7 µs | 36.9 µs | 66 to 78 µs | 102 to 131 µs | |
| FIX wire to wire, 1 in flight | 31.7 to 34.8 µs | 43 to 57 µs | 78 to 115 µs | 139 to 410 µs | 21 to 25k requests/s |
| inside the gateway, 1 in flight | 9.2 to 10.2 µs | 16 to 22 µs | 37 to 55 µs | 82 to 172 µs | |
| FIX wire to wire, 64 in flight | 279 to 328 µs | 0.49 to 0.59 ms | 0.85 to 1.0 ms | 3.4 to 3.8 ms | 134 to 147k requests/s |

**The kernel dominates.** A bare TCP echo over loopback already takes 29.7 µs on macOS. FIX
parsing, sessions, matching and encoding add 2 to 5 µs on top of that at the median.

**One poll pass, not two.** The first version took 49 µs at the median and handled 33k
requests/s with 64 in flight. Profiling showed its loop sat in `poll()`. A report the matcher
produced a moment after the read waited a whole extra `poll()` syscall. Now every request is
counted until the matcher answers it, and the gateway spins on the queue for those answers
before polling again. That brought the median to 33 µs and throughput to 134k+ requests/s.

**Rejects are real races.** With 1 in flight, about 600 of 100k requests are rejected. These
are cancels that arrive after the order has already filled, which is correct FIX behaviour.
A bug had turned them into endless retries: the OrderCancelReject reported OrdStatus 1, so
the client thought the order was still live. The gateway now tracks when an order is done.

With 64 in flight, the latency is mostly queueing behind the 63 requests ahead.

The order book checksum is still `2a79b8de12e4cd7d`.
