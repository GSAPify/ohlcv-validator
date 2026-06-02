# Runbook

Every command needed to build, run, test, and benchmark this project — plus a
dated activity log so future-me knows what past-me actually did.

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Debug build (asserts, no -O3):

```sh
cmake -S . -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug
```

## Run

Timing baseline binary:

```sh
./build/ohlcv_validator
```

Alpaca WebSocket ingest (needs `.env` with Alpaca paper keys):

```sh
set -a && source .env && set +a
./build/alpaca_ingest
```

Output is `<arrival_ns> <raw_json_frame>` per line; Ctrl+C to stop.
Outside US market hours you'll only see the auth + subscription acks; no
trade frames will arrive until the market opens.

Replay benchmark (the headline latency artifact — no network, no market hours):

```sh
mkdir -p data
./build/gen_dataset data/replay.bin 1000000   # deterministic synthetic dataset
./build/replay_bench data/replay.bin 200       # mmap → validate → histogram
```

The generator is fixed-seed, so the dataset is byte-identical every run; it
injects ~1% each of non-positive prices, sequence gaps, and timestamp
regressions plus a couple of bad-band bars so the validator has something to
catch. `gen_dataset` stands in for capturing the live feed and re-encoding to
the binary wire format — same shape, but reproducible.

## Test

```sh
ctest --test-dir build --output-on-failure
```

Single test by name:

```sh
./build/tests/unit_tests --gtest_filter='Timing.*'
```

## Clean

```sh
rm -rf build build-debug
```

## Install / update deps (macOS)

```sh
brew install cmake ninja boost simdjson spdlog nlohmann-json googletest
brew upgrade cmake ninja boost simdjson spdlog nlohmann-json googletest
```

---

## Activity log

Date + one line of what changed and why. Newest first.

- **2026-06-03** — validate: trade→bar reconstruction cross-check. Per-symbol
  trade accumulator (Σsize, Σprice·size, running O/H/L/C) in the existing Slot
  table; bars reconcile against it (volume/count exact, vwap/OHLC within a
  relative tolerance — real feeds round, so not bit-exact). Reconstructs from
  valid trades only. `gen_dataset` reworked to emit *consistent* trades+bars
  with one isolated defect per window; `replay_bench` reports the four
  reconstruction-mismatch classes. Throughput moved ~500M→~167M rec/s (the
  accumulator runs on every trade); held steady across 10^10 validations on an
  M4 Pro. 10 new tests (51 total). Scale it up: `gen_dataset out.bin 10000000`
  then `replay_bench out.bin 1000`.
- **2026-06-02** — replay+bench: binary wire format (`model/wire.h`, fixed-layout
  64-byte POD records), `gen_dataset` (deterministic synthetic dataset with
  injected violations), fixed-bucket `LatencyHistogram`, and `replay_bench`
  (mmap → decode → validate → per-record latency histogram). Batched timing to
  dodge the ~41ns M-series clock floor. First real numbers: ~2ns/record,
  ~496M records/sec on M-series. This is the headline artifact.
- **2026-06-02** — validate: zero-alloc `Validator` over the wire types. Bar/
  trade per-record invariants (OHLC band, vwap-in-band, volume/trade-count
  consistency, positivity) plus per-symbol sequencing (timestamp regression,
  sequence-gap detection) via a fixed-capacity open-addressing symbol table —
  no heap after construction. 18 new unit tests (40 total).
- **2026-05-28** — parser: simdjson on-demand parser lifting Alpaca frames into typed `Trade` and `Bar` structs. Hand-rolled RFC3339 nano timestamp parser (libc++ chrono::parse incomplete on Apple Clang). 22 unit tests covering ack frames, trades, bars, mixed frames, errors, unknown types, malformed JSON, and timestamp edge cases. `alpaca_ingest` now prints typed bars/trades instead of raw JSON.
- **2026-05-26** — alpaca_ingest binary: Boost.Beast sync TLS WebSocket client connecting to Alpaca's IEX paper feed. Auth + subscribe + raw-frame print with arrival-ns timestamps. Sync v1; async refactor deferred.
- **2026-05-26** — scaffold: CMake + Boost/simdjson/spdlog/nlohmann/gtest wired up; `now_ns()` self-overhead measured at 41ns median on M-series (matches 24MHz CNTVCT_EL0 resolution).
