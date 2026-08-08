# High-Frequency Trading Smart Order Router

A multi-venue smart order router (SOR) in C++20, benchmarked against a naive
best-price baseline inside a purpose-built, agent-based market simulator.

The router decides **how to split a large parent order across competing venues** —
two lit exchanges and one dark pool — by solving a dynamic program over expected
execution cost, then works the order through IOC child orders and reroutes
whatever comes back unfilled.

Against a naive router that simply sweeps the venue showing the best price, the
DP router cuts implementation shortfall by **~68%** and pays **~39% less in fees**,
while filling more of the order. Venue latency is simulated, not just priced — a
child order is held on the wire and quotes reach the router stale.

[![CI](https://github.com/Hamz714/High-Frequency-Trading-Smart-Order-Router/actions/workflows/ci.yml/badge.svg)](https://github.com/Hamz714/High-Frequency-Trading-Smart-Order-Router/actions/workflows/ci.yml)

---

## Results

100 Monte Carlo trials × 6 parent orders × 2 arms (1,200 executions), with venue
latency simulated. Both arms run against the same seeded market, so the only
variable is the routing decision.

| Metric | SOR (DP) | Naive | Delta |
|---|---:|---:|---:|
| Implementation shortfall (bps) | **8.03** | 24.99 | **−16.96** |
| VWAP slippage (bps) | **8.85** | 25.46 | **−16.61** |
| Fill rate (%) | **96.84** | 91.21 | **+5.64** |
| Total fees ($) | **1079.90** | 1774.56 | **−694.66** |

Raw per-order data: [`results/baseline/sor_vs_naive.csv`](results/baseline/sor_vs_naive.csv).
Reproduce with `./high_frequency --trials 100`.

**On run-to-run stability.** Trial seeds are deterministic (`seed_base + trial`),
so every run replays an identical market — but the arms still run on eight live
OS threads, and thread scheduling is not reproducible. Across eight 25-trial runs
(four per latency setting) the SOR's mean shortfall landed anywhere between 4.2
and 10.6 bps against the *same* seeded markets. That spread is scheduling
nondeterminism, not sampling
variation, and it is why the headline table above uses 100 trials rather than 25.
Read single-run differences smaller than a few bps as noise.

### What simulating latency changed

Turning the latency model on costs a measurable round trip and, on this
simulator, changes execution quality by nothing measurable. Paired per-order
comparison, 600 orders per arm, each order matched against itself in the
identical seeded market:

| Metric (SOR arm) | Latency on | Latency off | Paired delta | |
|---|---:|---:|---:|---|
| Implementation shortfall (bps) | 8.03 ±0.95 | 8.85 ±0.91 | −0.90 ±1.27 | within noise |
| VWAP slippage (bps) | 8.85 ±0.89 | 9.08 ±0.89 | −0.36 ±1.25 | within noise |
| Fill rate | 0.968 ±0.007 | 0.951 ±0.009 | +0.018 ±0.008 | direction unstable |
| Tick-to-trade p50 (µs) † | 573.9 | 217.4 | **+356.5** | clean separation |

Errors are standard errors over 573–600 paired orders. † is a difference of
medians rather than a paired per-order statistic: the tick-to-trade *mean* is
dominated by a long tail (1,602 µs against a 574 µs median), so the median is
the only stable read on it.

The only unambiguous effect is the round trip itself: median tick-to-trade rises
by ~356 µs for the SOR and ~381 µs for naive, with no overlap between the two
arms across five runs per setting, both landing near the 404 µs round trip to the
venue the flow concentrates on. Shortfall, slippage, and fees do not move. The fill-rate
row is marked unstable because it flips sign between experiments — positive at
100 trials, negative across the four 25-trial runs — which is what a run-level
scheduling effect looks like when a per-order paired test treats orders inside
one run as independent.

**Why latency does not cost anything here, and what that implies.** Two reasons,
and the second is the interesting one. Parent orders are priced 1,000 ticks
through the touch, so children stay marketable through a few hundred µs of drift.
More fundamentally, *this simulator has no informed flow*: market makers quote off
a random-walk fair value and noise traders arrive uninformed, so a stale quote is
exactly as likely to be stale in the router's favour as against it. Latency
therefore adds symmetric price risk, not adverse selection. Adverse selection
requires a counterparty who knows something the router does not, and modelling
that — flow whose arrival correlates with the price process's next move — is the
prerequisite for the latency term in the cost function to earn its keep. The
mechanism is now in place and measurably working; the toxic flow that would make
it bite is not yet.

### Order book microbenchmark

Single-threaded, 50k measured ops per scenario, no routing or threading involved.
Throughput is the median of 9 runs, with the observed range — a single pass on a
laptop is noisy enough that the ladder/overflow ratio can invert, so the tool
repeats and aggregates by default.

| Scenario | ops/sec (median) | range | p50 | p95 | p99 |
|---|---:|---:|---:|---:|---:|
| insert (ladder region) | **9.09 M** | 4.84 – 11.23 M | 100 ns | 100 ns | 200 ns |
| insert (overflow map region) | 6.94 M | 3.82 – 7.58 M | 100 ns | 200 ns | 200 ns |
| cancel (ladder region) | **18.71 M** | 14.07 – 20.62 M | 0 ns | 100 ns | 100 ns |
| cancel (overflow map region) | 12.62 M | 11.31 – 13.52 M | 100 ns | 100 ns | 100 ns |
| match (crossing, partial fill) | 10.10 M | 7.86 – 10.99 M | 100 ns | 100 ns | 100 ns |

Measured on an i7-1165G7 (4C/8T), Windows 11, GCC 14.2 `-O3`.
Raw data: [`results/baseline/lob_benchmark.csv`](results/baseline/lob_benchmark.csv).
Reproduce with `./bench_lob` (or `./bench_lob --repeat 25`).

The throughput column is the meaningful one. Windows `steady_clock` ticks at
~100 ns, so the per-op percentiles are quantised to that grid and should be read
as "at or below one clock tick", not as precise latencies. Throughput is measured
across all 50k ops at once and does not suffer from this.

---

## How the routing decision works

Splitting a parent order is an allocation problem: given `N` lots to place and
`V` venues, choose the allocation vector minimising total expected cost. The
router solves it exactly with a dynamic program rather than a greedy sweep,
because venue costs are **non-linear in size** — market impact grows
quadratically, so the marginal cost of the 500th share on a venue is not the
cost of the first.

`dp[k][n]` = minimum expected cost of placing `n` lots across the first `k`
venues. Lit venues are processed first so that their completed cost table can be
reused as the fallback for a dark-pool miss:

```
dp[k][n] = min over x in [0, n] of ( cost_k(x) + dp[k-1][n-x] )
```

**Lit venue cost** — half-spread, quadratic impact against visible liquidity,
per-share fees, and a latency penalty:

```
cost = half_spread·q  +  impact_coef·q²/visible_liquidity  +  fee·q  +  latency_us·λ
```

**Dark venue cost** — expected value over fill probability, where the miss branch
costs whatever it would take to fill that quantity on the lit book instead, plus
a delay penalty. Fill probability decays exponentially in size:

```
p_fill = historical_fill_ratio · exp(−decay_rate · q)
cost   = p_fill·(fee·q + latency)  +  (1 − p_fill)·(dp_lit[q] + λ·q)
```

This dark-pool term is the reason lit venues are solved first: `dp_lit` has to
already exist to price the miss branch.

Complexity is `O(V · W²)` for `W = N/lot_size + 1`. The tables are flattened to
row-major arrays and the previous row is pre-reversed, so the inner loop walks
two arrays forward instead of one forward and one backward — a materially better
access pattern than the naive `dp[k-1][n-x]` indexing.

The naive baseline it is measured against sends the entire order to whichever
lit venue currently shows the best price.

---

## Latency is simulated, not just priced

The cost function above charges each venue `latency_us · λ`. That term only means
something if the simulator actually makes slow venues slow — otherwise it is a
free parameter fitted to nothing, and the router is paying for a risk it never
takes. So each venue's `latency_us` is enforced as a one-way link time on three
legs:

| Leg | Mechanism | Consequence |
|---|---|---|
| Router → venue | [`Venue::route_order`](src/lob/Venue.cpp) stamps an arrival time; the venue worker parks the order in a delay queue until it comes due | IOC children arrive against a book that has moved since the decision |
| Venue → router, market data | `BookDelta` carries a `visible_ns`; [`market_data_loop`](src/sor/SmartOrderRouter.cpp) will not apply it to the mirror books before then | The DP sizes allocations against quotes that are one latency stale |
| Venue → router, fills | `FillEvent` carries the same stamp, held by [`fill_loop`](src/sor/SmartOrderRouter.cpp) | The reroute cascade fires a full round trip late |

Market makers and noise traders are **not** delayed. `latency_us` models the
router's link to a venue; those agents are that venue's own local liquidity, and
delaying them would slow the market uniformly rather than model anything. The
delay queue is what stops an in-flight router order from head-of-line blocking
them, and there is a test pinning that.

Arrival times are wall-clock `steady_clock` nanoseconds, **not** the simulation
clock. The sim clock advances in 1 ms steps while the three venue latencies are
202 / 46 / 71 µs — all sub-tick, so enforcing against it would delay every venue
by the same 0–1 ticks and reproduce exactly the problem this is meant to fix.
Real threads spinning against a real clock preserve the 202 vs 46 vs 71 ordering
that the DP's latency term ranks on.

That the delay is real and not merely declared is visible in the tick-to-trade
column, which is measured end to end and knows nothing about the latency model.
Over five runs per setting, switching simulation on moves SOR p50 from 217–291 µs to
574–644 µs, and naive p50 from 55–112 µs to 436–465 µs — two distributions with
no overlap, both shifting by close to the 404 µs round trip to the venue the flow
concentrates on.

`--no-latency` disables the simulation while leaving the DP's cost function
untouched. That is the A/B measured in
[What simulating latency changed](#what-simulating-latency-changed), whose short
answer is: a real round trip, and no measurable change in execution quality,
because an uninformed simulator makes staleness symmetric.

---

## Architecture

Every component runs on its own thread and communicates through bounded
lock-free ring buffers. Nothing on a hot path takes a lock; the router's mirror
books are guarded by a `shared_mutex` that readers hold only briefly.

```mermaid
flowchart LR
    subgraph sim["Simulation (1 thread)"]
        PP["PriceProcess<br/>(GBM)"] --> SE[SimulationEngine]
        SE --> MM["MarketMakers ×3"]
        SE --> NT["NoiseTraders ×2<br/>(Poisson arrivals)"]
    end

    subgraph venues["Venues (1 thread each)"]
        V1["Lit 1<br/>LimitOrderBook"]
        V2["Lit 2<br/>LimitOrderBook"]
        V3["Dark pool<br/>LimitOrderBook"]
    end

    subgraph router["SmartOrderRouter (3 threads)"]
        MD["market_data_loop<br/>→ mirror books"]
        OL["client_order_loop<br/>→ DPEngine split"]
        FL["fill_loop<br/>→ reroute unfilled"]
    end

    AE["AnalyticsEngine (1 thread)<br/>shortfall · VWAP slippage · fill rate"]

    MM -->|MPSC| venues
    NT -->|MPSC| venues
    venues -->|"SPSC BookDelta<br/>+ latency"| MD
    venues -->|"SPSC FillEvent<br/>+ latency"| FL
    OL -->|"IOC children<br/>+ latency"| venues
    FL -.->|remainder| OL
    venues -->|MPSC TradeEvent| AE
    router -->|SPSC lifecycle| AE
```

The three edges marked *+ latency* are held for that venue's configured link
time — see [Latency](#latency-is-simulated-not-just-priced).

Eight threads per trial, spawned and joined fresh each run.

| Component | Role |
|---|---|
| [`LimitOrderBook`](src/lob/LimitOrderBook.cpp) | Price-time priority book; hybrid ladder + overflow map |
| [`Venue`](src/lob/Venue.cpp) | Owns a book, matches on its own thread, publishes deltas and fills, holds inbound router orders for its link time |
| [`DPEngine`](src/sor/DPEngine.cpp) | The allocation dynamic program and the naive baseline |
| [`SmartOrderRouter`](src/sor/SmartOrderRouter.cpp) | Mirror books, parent/child lifecycle, reroute cascade |
| [`AnalyticsEngine`](src/analytics/AnalyticsEngine.cpp) | Execution quality metrics, off the hot path |
| [`SPSCQueue`](include/common/SPSCQueue.h) / [`MPSCQueue`](include/common/MPSCQueue.h) | Bounded lock-free ring buffers with drop counters |
| [`SimulationEngine`](src/sim/SimulationEngine.cpp) | Drives a virtual clock, market makers, and noise traders |
| [`MonteCarloHarness`](src/harness/MonteCarloHarness.cpp) | Runs both arms over identical seeded markets |

### Order book design

Real books cluster almost all activity within a few ticks of the touch, so the
book uses a **256-tick circular ladder of fixed-size price levels around the
touch, plus a `std::map` overflow region** for everything outside that window.
Occupancy in the ladder is tracked by a 4-word bitmask, so finding the next
non-empty level is `__builtin_ctzll` on a 64-bit word rather than a tree walk.
Orders live in a slab with intrusive prev/next indices, so cancels are O(1)
unlinks with no allocation.

The benchmark above measures both regimes separately and is what justifies the
split: ladder cancels sustain **1.48×** the throughput of overflow-map cancels
with non-overlapping ranges across 9 runs, and ladder inserts **1.31×** with
ranges that do overlap. `bench_lob` prints which of the two it is rather than
quoting a bare ratio.

---

## Build and run

Requires CMake ≥ 3.16 and a C++20 compiler. Tests fetch GoogleTest, so the first
configure needs network access.

```bash
cmake -S . -B build
cmake --build build -j
```

The build defaults to `Release` when no build type is given — latency numbers
from an unoptimised binary are meaningless, so this is deliberate.

```bash
./build/high_frequency               # SOR vs naive Monte Carlo comparison
./build/high_frequency -v            # ...with a per-order report breakdown
./build/high_frequency --no-latency  # ...with venue latency simulation disabled
./build/high_frequency --trials 100  # ...over more trials, to shrink run-to-run noise
./build/bench_lob                    # order book microbenchmark
./build/calibrate                    # randomised parameter sweep
```

Each writes a timestamped CSV into `results/`. The committed baselines in
`results/baseline/` are the runs quoted in this README.

### Tests

140 GoogleTest cases across the book, queues, router, DP engine, simulation
agents, analytics, and the venue latency model.

```bash
ctest --test-dir build --output-on-failure
# or
./build/tests/unit_tests
```

The build is warning-clean under `-Wall -Wextra`.

### Calibration

Market and router parameters were not hand-picked. [`calibrate`](src/calibrate/main.cpp)
runs a randomised sweep over venue fees, latencies, impact coefficients, market
maker behaviour, and router tuning, scoring each configuration by the resulting
execution quality. The current [`SimConfig`](src/config/SimConfig.cpp) defaults
are the output of that search, which is why they are unrounded.

Those defaults were fitted **before** latency was simulated, against a world
where every order arrived instantly. `router.latency_cost_factor = 3` in
particular was tuned against a penalty that cost the router nothing, so it is
now fit to the wrong environment. Re-running the sweep under simulated latency
is the obvious next step and would likely move it.

---

## Measurement honesty

A few things this project deliberately does not claim:

- **Tick-to-trade is not an algorithm benchmark.** The binary reports it, but it
  measures wall-clock submit→completion across eight threads that are spawned
  and joined per trial, on a loaded desktop OS. The SOR's multi-venue split and
  reroute cascade needs more cross-thread round trips than naive's single
  dispatch, so it looks *slower* on that metric by construction. Since venue
  latency became a simulated delay it is also dominated by *modelled* wire time
  — most of the reported figure is the simulator deliberately waiting, not code
  executing, which is why the numbers jumped roughly 400 µs when the latency
  model landed. The program prints this caveat itself rather than burying it.
  `bench_lob` is the clean, threading-free latency measurement.
- **The latency model is a link-time model, not a network model.** One symmetric
  one-way delay per venue covers order entry, market data, and fill acks alike.
  There is no jitter, no queueing delay at the venue gateway, no separate
  multicast path, and no asymmetry between the order and data legs — all of
  which are real effects at this timescale. Local agents are not delayed at all.
  It is enough to make stale quotes and adverse selection real; it is not a
  model of an exchange's network.
- **Book percentiles are clock-limited**, as described above.
- **The simulator is not a market.** Prices follow geometric Brownian motion,
  market makers quote off a fair value with a volatility-sensitive spread, and
  noise traders arrive as a Poisson process with lognormal sizes. That is enough
  to make routing decisions matter and to differentiate the two strategies; it is
  not a claim of realism against live venue microstructure.
- **Dropped messages are surfaced, not hidden.** Every queue counts drops, the
  counts are rolled up across the object graph, and a non-zero total prints a
  warning that the run's metrics may be biased.

## License

MIT — see [LICENSE](LICENSE).
