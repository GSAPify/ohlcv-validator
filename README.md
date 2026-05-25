# ohlcv-validator

A low-latency C++20 validator for live OHLCV market data feeds.

## Goal

Sustain **p99 < 5µs** from socket read to validation result on a single core, with zero allocations on the hot path. The headline number that earns this project a place on a resume is the latency histogram in `docs/`, not the feature list.

## Why this exists

Most "market data validators" are FastAPI services querying daily bars from Polygon. That solves a different problem (data quality for research) using a stack that does not transfer to low-latency engineering interviews. This project deliberately targets the *engineering* skills HFT and market-making firms hire for: lock-free data structures, cache-aware design, allocation-free hot paths, and measured nanosecond-level latency.

## Status

`v0.0.1` — toolchain bootstrapped, baseline timing primitive in place.

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
