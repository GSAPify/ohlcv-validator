# ohlcv-validator

A low-latency C++20 validator for live OHLCV market data feeds.

## Goal

Sustain **p99 < 5µs** from socket read to validation result on a single core, with zero allocations on the hot path. The headline number that earns this project a place on a resume is the latency histogram in `docs/`, not the feature list.

## Why this exists

Most "market data validators" are FastAPI services querying daily bars from Polygon. That solves a different problem (data quality for research) using a stack that does not transfer to low-latency engineering interviews. This project deliberately targets the *engineering* skills HFT and market-making firms hire for: lock-free data structures, cache-aware design, allocation-free hot paths, and measured nanosecond-level latency.

## Status

`v0.1.0` — live JSON ingest + a zero-allocation validator measured over a binary
replay path.

```
Alpaca IEX ──ws──► parse ──► Trade/Bar          ← live demo (JSON, convenience types)
                                                  "it works on real data"

binary file ──mmap──► WireRecord ──► Validator ──► LatencyHistogram
  (fixed-layout POD,    no parse,     zero alloc      percentiles
   ITCH/SBE-shaped)     no copy       after ctor)     ← the measured path
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

This is a **throughput** number, not a latency distribution. Per-record cost
(~2ns) sits below the M-series clock resolution (~41ns), so timing is batched
per-pass and divided — you cannot measure a per-record tail on this hardware.
A real latency histogram (p50/p99/p999) needs an x86 host with an invariant TSC
and `rdtscp`; that's the eventual measurement host (see platform note below).

The hot path is allocation-free, and that's *proven*, not asserted:
`tests/test_alloc_guard.cpp` overrides global `operator new` and requires zero
heap allocations through a 100k-record validate stream.

The validator catches non-positive prices, inverted OHLC bands, out-of-band
VWAP, volume/trade-count inconsistency, per-symbol timestamp regressions, and
dropped-message sequence gaps.

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

Developed on Apple Silicon (M-series) where the system cycle counter is virtualized at 24 MHz. Real latency benchmarks for the resume will eventually run on a Linux x86_64 host with an invariant TSC; the Mac is the development environment, not the measurement environment.
