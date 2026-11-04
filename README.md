# Execution Engine

[![ci](https://github.com/Rayquaza2003/execution-engine/actions/workflows/ci.yml/badge.svg)](https://github.com/Rayquaza2003/execution-engine/actions/workflows/ci.yml)

A trading stack in C++20, from the matching engine up to execution algorithms. The system:

- matches orders;
- tracks every order's state and checks risk before anything is sent;
- replays real NASDAQ market data;
- works large orders with execution algorithms;
- routes them across venues;
- speaks FIX over TCP;
- survives a crash.

It is built and measured stage by stage. Every number in this README was measured on this
code, and [BENCHMARKS.md](BENCHMARKS.md) shows how.

The stack is deliberately small: C++20, CMake and POSIX sockets, with no third-party
libraries. The tests use `assert` and research runs in Python (pandas and matplotlib) over CSV
output.

## Architecture

```mermaid
flowchart LR
    subgraph client["Trading side"]
        ALGO["Algos<br/>TWAP VWAP POV Iceberg IS Adaptive"] --> OMS["OMS<br/>order state machine"]
        OMS --> RISK["Pre-trade risk<br/>O(1) checks, kill switch"]
        RISK --> SOR["Smart order router<br/>fees, latency, depth"]
        MD["Market data<br/>L2 books, NBBO, OFI"] --> ALGO
        TCA["TCA<br/>shortfall, spread, timing"]
        OMS -->|fills| TCA
    end
    subgraph venue["Exchange side"]
        GW["FIX 4.4 gateway<br/>poll(), sessions, journal"] -->|SPSC queue| ME["Matching engine<br/>price-time books"]
        ME -->|SPSC queue| GW
        ITCH["NASDAQ ITCH 5.0<br/>replay"] --> ME
    end
    SOR -->|"orders (FIX or in-process)"| GW
    GW -->|execution reports| OMS
    ME -->|"level updates, trades"| MD
    JRN[("Write-ahead<br/>journal")] --- GW
```

The two sides share no memory. They exchange messages only, as a firm and an exchange do.

- **In simulation**, everything runs on one deterministic event loop. Latency is a scheduled
  delay, so the same inputs always give byte-identical outputs.
- **Live**, each component gets its own thread, joined by lock-free single-producer
  single-consumer (SPSC) queues.

## What's here

| module | files | what it does |
|---|---|---|
| order book | `src/exchange/orderbook.hpp` | Price-time priority. Prices sit on a flat ladder; orders are pooled in intrusive lists. Order ids use open addressing. No allocation in steady state. |
| venue | `src/exchange/venue.hpp` | One book per symbol behind a message interface. Supports IOC, FOK, Day and GTC orders, stop-limits, self-trade prevention and a circuit breaker, and publishes L2 updates and trades. A replay mode lets our own orders sit among real ones. |
| event loop | `src/core/event_loop.hpp` | A deterministic discrete-event loop on a simulated clock. |
| OMS | `src/oms/oms.hpp` | A `constexpr` order state machine with race handling and idempotent ClOrdIDs. Reconciles against the venue after lost reports. |
| risk | `src/risk/risk.hpp` | Checks order size, notional, worst-case position, open orders, a fat-finger band and a loss limit. Tracks mark-to-market PnL and has an atomic kill switch. |
| market data | `src/md/` | An ITCH 5.0 parser and replay, client L2 books and the NBBO. |
| microstructure | `src/analytics/microstructure.hpp` | Per-second bars: spread, imbalance, microprice, order-flow imbalance, depth and VWAP. |
| queue study | `src/analytics/fill_study.hpp` | Virtual passive orders with exact queue position (from ITCH order ids), across a latency sweep. |
| algos | `src/algo/` | TWAP, VWAP, POV, Iceberg, implementation shortfall (Almgren-Chriss) and Adaptive. A harness runs them over the ITCH replay with latency. |
| TCA | `src/analytics/tca.hpp` | Implementation shortfall split into spread, timing and opportunity cost, plus cost against VWAP and post-trade reversion. |
| router | `src/sor/` | Water-fills an order across venues by all-in cost per share. Picks a venue to rest on by expected value. Three venues are carved out of one real feed. |
| concurrency | `src/core/spsc.hpp`, `histogram.hpp`, `logger.hpp` | A lock-free SPSC queue, a log-linear latency histogram and an async logger. |
| FIX | `src/fix/` | A tag=value codec with BodyLength and CheckSum. Sessions handle sequence numbers, gaps with exact resends, PossDup, GapFill, heartbeats and resets. |
| gateway | `src/net/gateway.hpp` | A `poll()` TCP server. Converts FIX order messages (D, F, G, AF) into matching-engine requests on another thread, and sends back execution reports and cancel rejects (8, 9). Logons need a username and password, and every order is checked against hard quantity and price limits. |
| journal | `src/journal/journal.hpp` | An append-only binary journal with a CRC32 per record and torn-tail recovery. The gateway journals write-ahead and recovers from it. |

## Results

Most numbers come from an Apple M1 (8 GB, macOS, Apple clang `-O3 -march=native`). The
ones that don't need market data also run on Linux x86 in CI on every push: a shared Azure
VM, Xeon Platinum 8370C, 4 vCPUs (2 cores), g++ 14, threads pinned but not on isolated cores.

| what | Apple M1 | Linux x86 (CI VM) | details |
|---|---|---|---|
| order book throughput | about 30 M ops/s (29.6 M median), up from 7.1 M for the `std::map` baseline (4.1x) | 30.8 to 31.4 M ops/s; per op p50 61 ns, p99 152 ns, p99.99 591 ns | [BENCHMARKS](BENCHMARKS.md#phase-3-one-change-at-a-time), [Linux](BENCHMARKS.md#linux-x86-ci-runner) |
| order book allocations | 0 in steady state; every optimization kept the trade checksum identical | 0 | |
| pre-trade risk check | 11 ns per order | 9.7 ns | [S4](BENCHMARKS.md#s4-pre-trade-risk-check) |
| ITCH into order books | 70 ns per booked message (14.3 M msgs/s, synthetic feed). A full pass over the real 751 MB day runs at 36.9 M msgs/s, but only 3% of its 22.4M messages belong to the 5 booked symbols, so that figure is mostly parsing. 0 bad references. | replay into books 215 ns per message (synthetic) | [S5](BENCHMARKS.md#s5-market-data-from-itch) |
| queue position | shortest vs longest fifth of queues fill within 1 s at 0.74 vs 0.46 (SPY); new-level joins lose 0.21 ticks more markout at 100 ms latency than at 0 | (needs the ITCH data) | [research](research/README.md) |
| execution algos, 95 paired parents, out of sample | Adaptive beats TWAP by 0.29 bps (95% interval 0.05 to 0.53, errors clustered by start time) | (needs the ITCH data) | [research](research/README.md#execution-algos-and-tca-s7) |
| smart order routing, 245 paired parents, out of sample | beats best-price routing by 0.046 bps (0.036 to 0.056), all of it fees on venues I configured, in 1 round instead of 2.2 | (needs the ITCH data) | [research](research/README.md#smart-order-routing-s8) |
| SPSC queue vs mutex | round trip p50 215 ns vs 16.4 µs; p99.9 335 ns vs 524 µs | pinned: 369 M msgs/s; round trip p50 83 ns vs 703 ns | [S9](BENCHMARKS.md#s9-threads-queues-logger) |
| async logger | 34 ns per call vs 270 ns for `fprintf` | 23 ns vs 134 ns | |
| FIX order round trip over TCP | p50 32 to 35 µs, against a 29.7 µs floor for bare TCP; 134 to 147k requests/s with 64 in flight | not run (bare TCP floor 17.4 µs) | [S10](BENCHMARKS.md#s10-fix-gateway-over-tcp) |
| durability | journal append 59 ns; write-ahead flush adds about 6 µs per reply; recovered 125k orders in about 0.55 s after `kill -9` | `write` per record 0.6 µs; `fsync` per record 207 µs | [S11](BENCHMARKS.md#s11-journal-recovery-reconciliation) |

Some results came out negative, and the notes say so:

- Smart passive routing lost to rebate chasing in this simulator.
- Threading the pipeline added no throughput.
- Each one has its own explanation in the linked notes.

## Build and test

```sh
cmake -B build && cmake --build build -j          # Release (-O3 -march=native; asserts stay on in tests)
ctest --test-dir build                            # 27: 24 test suites + 3 fuzz smoke runs

cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug     # AddressSanitizer + UndefinedBehaviorSanitizer
cmake -B build-tsan -DCMAKE_BUILD_TYPE=Tsan       # ThreadSanitizer
cmake -B build-fuzz -DFUZZ=ON -DCMAKE_CXX_COMPILER=clang++   # libFuzzer targets
```

**CI** ([`.github/workflows/ci.yml`](.github/workflows/ci.yml)) runs on every push:

- **Builds and tests:** Linux with g++ 14 and clang 18, each as Release, ASan+UBSan and
  TSan, plus macOS. Warnings are errors.
- **Fuzzing:** each libFuzzer target for 2 minutes under ASan+UBSan.
- **Benchmarks:** the Linux benchmark script, whose output is attached to every run.

**How correctness is checked, besides unit tests:**

- **Differential test (`tests/test_book_diff.cpp`).** The order book runs against a naive
  reference book, a vector with linear scans written straight from the rules. That covers
  400k random operations: limits, markets, IOC, FOK, self-trade prevention, cancels,
  modifies, reduces and end of day. Results, trades and book state must match after every
  call. Breaking time priority or FOK's self-trade check in a scratch copy made it fail at
  once.
- **Fuzzing (`fuzz/`).** Targets for the FIX codec and session, the ITCH parser plus replay,
  and journal recovery. They check invariants, not just crashes: framing always makes
  progress, a book never ends up crossed, and a recovered journal accepts appends.
- **Platforms.** The same tests run on Linux/libstdc++ and macOS/libc++. That caught a
  wrong bound in a risk test which only libstdc++'s random stream reached.

## Run

Benchmarks that need no data:

```sh
./build/bench_book          # order book: throughput, per-op latency, allocation count, checksum
./build/bench_risk          # risk check latency
./build/bench_md            # ITCH parse / replay / L2 throughput on a synthetic feed
./build/bench_spsc          # lock-free queue vs mutex
./build/bench_pipeline      # 4-thread pipeline vs one thread (synthetic feed; pass an ITCH file for real data)
./build/bench_tcp           # bare loopback TCP round trip, the floor under FIX
./build/bench_journal       # what durability costs per record
```

### FIX gateway and load generator

Passwords are kept out of argv, because `ps` shows argv. The gateway reads
`COMPID PASSWORD` lines from a file, and the client reads `FIX_PASSWORD`.

```sh
echo "LOADGEN $(openssl rand -hex 16)" > users.txt && chmod 600 users.txt
export FIX_PASSWORD=$(cut -d' ' -f2 users.txt)

./build/fix_gateway --users users.txt --journal /tmp/gw.journal 9878 AAPL MSFT &
./build/fix_client 9878 AAPL 100000 1     # one request in flight: wire-to-wire latency
./build/fix_client 9878 AAPL 200000 64    # 64 in flight: throughput
kill -9 %1; ./build/fix_gateway --users users.txt --journal /tmp/gw.journal 9878 AAPL MSFT   # recovers
```

### Real market data

The studies use NASDAQ's public TotalView-ITCH 5.0 sample day. The first 300 MB of the
compressed file is enough: it decompresses to 22.4M messages, up to 09:38.

```sh
curl -s https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/01302019.NASDAQ_ITCH50.gz | head -c 300000000 | gunzip > day.itch
# gunzip complains about the cut-off end; the 751 MB it writes is complete up to there

./build/itch_replay  day.itch bars.csv  AAPL MSFT SPY QQQ TSLA                 # per-second microstructure
./build/fill_study   day.itch fills.csv AAPL MSFT SPY QQQ TSLA                 # queue position study
python3 research/fill_study.py fills.csv                                       # charts + research/fill_prob.csv
./build/algo_compare day.itch research/fill_prob.csv algos.csv AAPL MSFT SPY QQQ TSLA
python3 research/algo_compare.py algos.csv
./build/sor_compare  day.itch research/fill_prob.csv sor.csv AAPL MSFT SPY QQQ TSLA
python3 research/sor_compare.py sor.csv
```

## Layout

```
src/core/       types, event loop, id index, SPSC queue, histogram, async logger
src/exchange/   order book, venue
src/oms/        order management
src/risk/       pre-trade risk
src/md/         ITCH parser and replay, client market data
src/analytics/  microstructure bars, queue-position study, TCA
src/algo/       execution algos, fill-probability table, simulation harness
src/sor/        smart order router, multi-venue simulation
src/fix/        FIX codec and session
src/net/        FIX gateway
src/journal/    write-ahead journal
tests/          one assert-based test per module (ctest)
bench/          benchmarks
tools/          command-line tools over real data and the network
research/       Python analysis, figures, results write-ups
```

## Design choices worth knowing

- **Determinism first.** The simulator is a discrete-event loop keyed by (time, sequence
  number). Replays are exact, so every optimization was checked against a checksum of every
  trade.
- **Measure, change one thing, measure again.** The order book went from 7.1 to 29.6 M ops/s
  in four steps. Each step kept the same trade checksum, and each was kept only if it won.
- **The OMS is a table.** Every (state, event) pair is listed in a `constexpr` function and
  tested exhaustively. Races are handled explicitly: fill vs cancel, replace vs fill, and a
  cancel sent before the ack arrives.
- **Two sides, messages only.** The firm and the venue share nothing but messages. Latency is
  a delay on those messages, and threads are just a different way to deliver them.
- **Event sourcing for recovery.** The gateway journals only the FIX messages it processed and
  sent. Replaying them through the same code rebuilds the books, orders and sessions exactly,
  and the matching engine's output checksum proves it.
- **Write-ahead.** No client sees an acknowledgement the journal doesn't have.

## Known limits

Deliberate shortcuts are marked in the code with `ponytail:` comments. The main ones:

- **Simulator:**
  - The replayed market never reacts to our orders.
  - Our fills ahead of a real order don't reduce that order.
- **No market-data latency in the simulations:** algos and the router read books
  instantly; only orders pay latency.
- **Gateway:**
  - No cancel-on-disconnect.
  - No session-level Reject (35=3).
  - The resend store lives in memory as well as in the journal.
  - It binds to loopback only, and FIX travels in plaintext with no TLS, so the logon
    password is only as private as the link.
- **Benchmark platform:**
  - The M1 numbers come from macOS: no core pinning, and a 41.67 ns clock tick.
  - The Linux numbers come from a shared 4-vCPU CI VM. Threads are pinned, but the cores
    aren't isolated and there are only 2 physical cores, so the 4-thread pipeline starves
    there.
  - Numbers from a dedicated box with isolated cores are still to do;
    `scripts/bench_linux.sh` says how to set one up.
- **Checksum per standard library:** the order-book benchmark draws its workload from
  `std::uniform_int_distribution`, which libc++ and libstdc++ implement differently. The
  trade checksum is `2a79b8de12e4cd7d` on macOS (libc++) and `72661ba474b30268` on Linux
  (libstdc++). Each is stable from run to run.
- **Data:** one morning of one day; studies need more days before any parameter is fitted.

## How it was built

The stack was built in stages, each one shipped working with tests:

- **Phases 0–3.** The order book, then its optimization.
- **S1.** Repository layout and the event loop.
- **S2.** The venue.
- **S3.** The OMS.
- **S4.** Risk.
- **S5.** ITCH market data.
- **S6.** The queue-position study.
- **S7.** Algos and TCA.
- **S8.** The smart order router.
- **S9.** Concurrency.
- **S10.** FIX over TCP.
- **S11.** The journal and recovery.
- **S12.** These docs.

The git history follows the stages in small commits.
