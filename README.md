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
- **Throughput, framed honestly.** ~167 M records/sec on one core, steady from 1M to 10¹⁰ validations. It is deliberately *not* reported as a latency distribution: per-record cost is below the ~41 ns Apple-Silicon clock floor, so p50/p99 there would be timer noise. A real per-record histogram needs an x86 host with `rdtscp`.
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
(~6ns) sits below the M-series clock resolution (~41ns), so timing is batched
per-pass and divided — you cannot measure a per-record tail on this hardware.
A real latency histogram (p50/p99/p999) needs an x86 host with an invariant TSC
and `rdtscp`; that's the eventual measurement host (see platform note below).

The hot path is allocation-free, and that's *proven*, not asserted:
`tests/test_alloc_guard.cpp` overrides global `operator new` and requires zero
heap allocations through a 100k-record validate stream.

The validator catches non-positive prices, inverted OHLC bands, out-of-band
VWAP, volume/trade-count inconsistency, per-symbol timestamp regressions,
dropped-message sequence gaps, bars whose constituent trades fail to reconstruct
their volume/count/VWAP/OHLC (the cross-record check), and — on quotes — crossed
(bid > ask) and locked (bid == ask) markets plus non-positive or zero-size sides.

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
