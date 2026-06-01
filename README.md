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

Per-record decode+validate, single core, 1M-record dataset, 200 passes
(`replay_bench`). Measured on Apple Silicon (M-series) — see platform note; the
clock floor here is ~41ns, so timing is batched per-pass and divided, not
per-call.

```
per-record decode+validate latency:  ~2 ns mean   (p50 2, p99 3, max 3)
throughput:                          ~496 M records / sec
```

The validator catches non-positive prices, inverted OHLC bands, out-of-band
VWAP, volume/trade-count inconsistency, per-symbol timestamp regressions, and
dropped-message sequence gaps — all on a heap-free hot path.

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
