# ohlcv-validator

A low-latency C++20 validator for live OHLCV market data feeds.

## Goal

Validate live OHLCV market data **on the hot path** — zero heap allocations,
~6 ns per record on a single core — not a research-grade data-quality script.
The bar I hold it to: every correctness claim is backed by a test, and every
performance claim by a reproducible measurement, framed honestly.

## Why this exists

Most "market data validators" are FastAPI services querying daily bars from Polygon. That solves a different problem (data quality for research) using a stack that does not transfer to low-latency engineering interviews. This project deliberately targets the *engineering* skills HFT and market-making firms hire for: cache-aware design, allocation-free hot paths, and measured throughput.

## Engineering decisions

The decisions matter more than the feature list:

- **Binary over JSON for the measured path.** A live JSON websocket (Alpaca IEX) is included as a "runs on real data" demo — but timing socket-to-validation on JSON measures the parser and the network, not market-data handling. Real venues ship fixed-layout binary messages (ITCH/SBE-style), so the benchmark reads a fixed-stride record straight out of an `mmap`'d file by cast: no parse, no copy, no allocation.
- **Throughput, framed honestly.** ~167 M records/sec on one core, steady from 1M to 10¹⁰ validations. It is deliberately *not* reported as a latency distribution: per-record cost is below the ~41 ns Apple-Silicon clock floor, so p50/p99 there would be timer noise. The latency distribution itself is measured on x86 with `rdtscp` (see Benchmark) — p50 20 ns / p99 30 ns on a Ryzen 9 7900X3D.
- **Claims are proven, not asserted.** "Zero allocations on the hot path" is a test (`tests/test_alloc_guard.cpp` overrides global `operator new` and fails on any allocation), not a sentence in a README.
- **Reconstruction with tolerance, not `==`.** The cross-record check — do a bar's constituent trades rebuild its volume, VWAP, and OHLC? — uses a relative tolerance, because real feeds round prices to a tick and exact equality would false-positive on every live bar.

## Status

`v0.1.0` — live JSON ingest + a zero-allocation validator measured over a binary
replay path.

```
Alpaca IEX ──ws──► parse ──► Trade/Bar          ← live demo (JSON, convenience types)
                                                  "it works on real data"

binary file ──mmap──► WireRecord ──► Validator ──► throughput
  (fixed-layout POD,    no parse,     zero alloc      ~167M rec/s
   ITCH/SBE-shaped)     no copy       after ctor)     ← the measured path

binary file ──► [decode thread] ──push──► SPSC ring ──pop──► [validate thread]
                                          (lock-free,                  ← pipeline
                                           cache-line aware)             (x86)
```

## Benchmark

Decode+validate, single core, 1M-record dataset, 200 passes (`replay_bench`),
on Apple Silicon (M-series).

```
throughput:  ~167 M records / sec
mean:        ~6 ns / record   (allocation-free hot path)
```

(The per-trade reconstruction accumulator — running on every trade — is what
moved this from the ~2 ns/record of the bounds-only validator; still allocation-
free, the work is just real now.)

That M-series figure is **throughput** (batched per-pass) — per-record cost is
below the ~41 ns Apple-Silicon clock floor, so a per-record *tail* can't be
measured there. For that, the bench runs on x86.

### Latency distribution — x86 (`rdtscp`)

Ryzen 9 7900X3D (4.4 GHz invariant TSC), single core pinned with `taskset`, each
decode+validate timed with one `rdtscp` pair (timer overhead measured and
subtracted), 5M samples (`replay_bench_rdtsc`):

```
p50  20 ns   ·   p99  30 ns   ·   p99.9  50 ns   ·   p99.99 ~200 ns   ·   mean ~17 ns
```

Stable to p99.9 across runs. This is per-event, `lfence`-serialized latency (the
point-in-time metric) — higher than the amortized throughput above because
serialization defeats pipelining. The far tail (max tens of µs) is OS preemption:
WSL2 isn't an isolated `isolcpus`/`nohz_full` core, so rare scheduler stalls show
up; bare metal would erase them.

### Multicore scaling — x86, 24 threads

Shard-by-symbol across the 7900X3D's 24 threads (`replay_bench_mt`):

```
 1c 115 M/s   2c 1.8×   4c 3.0×   8c 5.1×   10c 7.4×   24c ~9× → ~1.0 B records/sec
```

Near-linear to ~10 cores, then memory bandwidth and the chip's dual-CCD / 3D
V-cache asymmetry taper it (a reproducible dip at 14 threads, as work spills
across both chiplets). Peak ≈ **1.0 billion records/sec**, single machine.

### Pipeline — lock-free SPSC ring (x86)

A single-producer/single-consumer ring (`src/concurrent/spsc_ring.h`) decouples a
decode thread from a validate thread — the structure every real feed handler has.
At IEX volume one thread copes fine; this isn't load-bearing. It's here to make
the thread-to-thread handoff measurable, and to settle a cache-line question with
data instead of lore.

Correctness first: the threaded 1M-item strict-FIFO test
(`tests/test_spsc_ring.cpp`) passes under ThreadSanitizer (`-DSANITIZER=thread`),
so the acquire/release pairing is *proven* race-free — no drops, dups, or
reordering — not just argued.

Ring-bound throughput (the consumer does no work, so the cursors are the
bottleneck), median of 5, producer and consumer pinned to two cores of one CCD:

```
payload      cursors on separate lines     cursors packed on one line
64B record        34 M ops/s                    44 M ops/s   (packed +22%)
8B word          180 M ops/s                   243 M ops/s   (packed +26%)
```

The surprise: **packing the two cursors onto one cache line is faster** — and the
textbook rule is to pad them *apart* to avoid false sharing. My read of why this
ring inverts that (a hypothesis, to be confirmed by the experiment below): it
re-reads *both* cursors every iteration — the producer checks `head` to see if the
ring is full, the consumer checks `tail` to see if it's empty — so the cursors are
*truly* shared, not falsely. On one line a single cross-core transfer carries both
updates; split across two lines, both lines bounce every item. The "pad your
cursors" advice assumes the production optimization — each side *caches* the far
cursor and re-reads it only when its cache says full/empty — which this naive ring
doesn't have yet. Adding that, and re-measuring whether the padding result flips,
is the next step.

Realistic pipeline (the consumer validates each record): the producer is a 64-byte
`mmap` copy, so it outruns the validator and the ring sits ~95% full. End-to-end
throughput is validate-bound (~19 M rec/s), and the enqueue→dequeue "latency" is
therefore *queue residency* (ring depth × consume time ≈ 50 µs), not the ring's
sync cost — the bench prints mean ring occupancy so which regime you're in is
explicit, never implied.

The hot path is allocation-free, and that's *proven*, not asserted:
`tests/test_alloc_guard.cpp` overrides global `operator new` and requires zero
heap allocations through a 100k-record validate stream.

The validator catches non-positive prices, inverted OHLC bands, out-of-band
VWAP, volume/trade-count inconsistency, per-symbol timestamp regressions,
dropped-message sequence gaps, bars whose constituent trades fail to reconstruct
their volume/count/VWAP/OHLC (the cross-record check), quote anomalies
(crossed/locked book, non-positive or zero-size sides), and per-symbol
price-band outliers: trades or quote mids that deviate more than 5% from a
per-symbol EWMA reference are flagged; outliers are excluded from the EWMA so
one bad tick cannot shift the band for subsequent records.

## Build

```
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/ohlcv_validator
```

## Test

```
ctest --test-dir build --output-on-failure
```

## Runbook

See [`docs/runbook.md`](docs/runbook.md) for all build/run/test commands and an activity log.

## Platform notes

Developed on Apple Silicon (M-series), where the user-space cycle counter is virtualized at 24 MHz (~41 ns) — fine for throughput, too coarse for a per-record latency tail. The latency distribution is therefore measured on a Linux x86_64 host (Ryzen 9 7900X3D, WSL2) with an invariant TSC read via `rdtscp`. The Mac is the development environment; the x86 box is the measurement environment.
