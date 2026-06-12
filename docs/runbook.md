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

Each parsed trade/quote/bar is validated inline: a `TRADE`/`QUOTE`/`BAR` line, a
`!!` line if a (live-meaningful) check fires, and a violation summary on Ctrl+C.
Quotes carry the order-book checks (crossed/locked, mid-outlier). Outside US
market hours you'll only see the auth + subscription acks; no data frames arrive
until the market opens. The catch logic is proven offline regardless via
`./build/tests/unit_tests --gtest_filter='Live*'` (no network/keys).

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

## Sanitizers & fuzzing

The default build is untouched; sanitizers go in their own build dir.

ASan + UBSan over the full test suite:

```sh
cmake -B build-san -G Ninja -DCMAKE_BUILD_TYPE=Release -DSANITIZER="address;undefined"
cmake --build build-san
UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-san --output-on-failure
```

ThreadSanitizer on the multicore path (proves the shard-by-symbol design is
race-free; `address` and `thread` are mutually exclusive):

```sh
cmake -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Release -DSANITIZER=thread
cmake --build build-tsan --target gen_dataset replay_bench_mt
./build-tsan/gen_dataset /tmp/h.bin 500000 64 && ./build-tsan/replay_bench_mt /tmp/h.bin 5
```

ThreadSanitizer on the SPSC ring (proves the lock-free ring's acquire/release
ordering is correct — the threaded strict-FIFO test must run *under* TSan, so
build and run `unit_tests` in the sanitizer build dir, not just the benches):

```sh
cmake --build build-tsan --target unit_tests
./build-tsan/tests/unit_tests --gtest_filter='SpscRing.*'
```

libFuzzer on the JSON parser. Apple clang lacks the fuzzer runtime, so use
Homebrew LLVM (`brew install llvm`):

```sh
LLVM=$(brew --prefix llvm); SJ=$(brew --prefix simdjson)
"$LLVM/bin/clang++" -std=c++20 -g -O1 -fsanitize=fuzzer,address,undefined \
  -I src -I "$SJ/include" tests/fuzz_parser.cpp src/ingest/parser.cpp \
  "$SJ/lib/libsimdjson.dylib" -o /tmp/fuzz_parser
/tmp/fuzz_parser -max_total_time=30
```

## x86 latency + scaling (on a Linux x86 host)

The latency distribution needs an invariant TSC + user-readable cycle counter
(rdtscp) — not available on Apple Silicon. On an x86 Linux box (here: a Ryzen 9
7900X3D under WSL2), only a compiler is needed (the validator/bench/generator are
pure C++ — no simdjson/Boost/spdlog):

```sh
g++ -O3 -march=native -std=c++20 -Isrc src/bin/gen_dataset.cpp -o gen_dataset
g++ -O3 -march=native -std=c++20 -Isrc src/bin/replay_bench_rdtsc.cpp src/validate/validator.cpp -o rdtsc_bench
g++ -O3 -march=native -std=c++20 -pthread -Isrc src/bin/replay_bench_mt.cpp src/validate/validator.cpp -o bench_mt

./gen_dataset data/replay.bin 1000000 64
taskset -c 2 ./rdtsc_bench data/replay.bin 5   # per-record p50/p99/p999, pinned core
./bench_mt data/replay.bin 20                  # shard-by-symbol scaling sweep

# Producer/consumer pipeline over the SPSC ring. Args: passes, producer core,
# consumer core — pick two cores on the same CCD for a clean number.
g++ -O3 -march=native -std=c++20 -pthread -Isrc src/bin/replay_bench_spsc.cpp src/validate/validator.cpp -o spsc_bench
./spsc_bench data/replay.bin 5 2 4             # ring throughput + handoff residency
```

## Install / update deps (macOS)

```sh
brew install cmake ninja boost simdjson spdlog nlohmann-json googletest
brew upgrade cmake ninja boost simdjson spdlog nlohmann-json googletest
```

---

## Activity log

Date + one line of what changed and why. Newest first.

- **2026-06-12** — ingest: live **quotes**. Parser handles `"T":"q"` →
  `model::Quote`; `to_wire(Quote)`; `alpaca_ingest` subscribes + validates quotes
  inline. Quotes add the order-book checks (crossed/locked, quote-mid outliers)
  and ~10x message volume — coverage, not noise: on clean single-venue IEX data a
  matching engine won't cross/lock against itself, so they mostly pass too (silence
  stays the expected result). Found + handled a timestamp trap: trades,
  quotes, and bars share one per-symbol `last_ts`, so a quote advancing the clock
  false-flags a following trade — `live_report.h` now suppresses ts-regression
  across the whole live stream (and `RecordKind` is gone, since that was its only
  use). Proper fix (per-stream `last_ts` in the validator) is the next PR. 78
  tests (quote parse, crossed-quote catch, quote-pollutes-clock evidence).
- **2026-06-10** — ingest: wire the live Alpaca path through the validator
  (`ingest/to_wire.h` adapter; `alpaca_ingest` validates inline + prints a
  summary). "Runs on real data" now means it *validates*, not just parses.
  Honest scoping: reconstruction + sequence-gap are N/A on the IEX sample
  (partial volume, no feed seq → per-symbol synthetic seq), so the summary
  suppresses them; also suppresses bar-level timestamp regression (a bar's
  window-start precedes its trades, which share one per-symbol ts tracker, so
  every bar would else false-flag — `ingest/live_report.h`). On clean vendor data
  the live checks are mostly quiet by design. Catch logic + the suppression
  proven offline through the real Parser→adapt→Validator chain
  (`tests/test_live_validation.cpp`, no network/keys). Quotes are the next step
  (more frequent; crossed/locked + mid-outliers live there). 74 tests.
- **2026-06-07** — concurrent: lock-free SPSC ring (`concurrent/spsc_ring.h`) +
  a producer/consumer pipeline bench (`replay_bench_spsc`). Threaded strict-FIFO
  test passes under TSan → acquire/release ordering proven race-free. Ring-bound
  throughput on the 7900X3D (two cores, one CCD): ~33 M ops/s for 64B records,
  ~185 M ops/s for 8B words. Surprise finding: **packing the two cursors onto one
  cache line is *faster* than padding them apart** (steady ~+22% for 64B; noisier
  +8–26% for 8B on this WSL host, but the sign is robust) — this naive ring reads
  both cursors every iteration (true sharing), so one line beats two. Caching the
  far cursor (Vyukov/Folly style) and re-measuring whether that flips is the next
  PR. Realistic (validate-bound) pipeline: ring sits ~95% full, so the handoff
  number is queue residency (~50 µs), not sync cost — occupancy is printed to make
  that explicit.
- **2026-06-06** — bench: per-record latency histogram via `rdtscp`
  (`util/tsc_timer.h`, `replay_bench_rdtsc`). Finally measured the README's
  day-one promise on x86 (Ryzen 9 7900X3D, WSL2, pinned core): **p50 20 ns /
  p99 30 ns / p99.9 50 ns**, mean ~17 ns, stable across runs. Clean 24-thread
  scaling too (~1.0 B rec/s peak; reproducible dip at 14 threads = dual-CCD
  spill). README/platform-notes updated to real numbers; fixed a GCC
  `-Wformat-truncation` nit in `gen_dataset`.

- **2026-06-05** — harden: `-DSANITIZER="address;undefined"` / `=thread` build
  option + a libFuzzer parser harness (`tests/fuzz_parser.cpp`). Verified clean:
  ASan+UBSan over the full suite, TSan over the multicore bench (shard-by-symbol
  is race-free), and 1.59M fuzzer executions on the parser with zero crashes.
  CI gains an ASan+UBSan job. Default Release build unchanged.
- **2026-06-05** — validate: quote validation. New `WireQuote` wire type (one
  cache line) + `RecordType::Quote` (binary format v2). Validator gains
  crossed (bid > ask), locked (bid == ask), non-positive, and zero-size checks;
  quotes share the per-symbol sequencing but don't feed bar reconstruction.
  Generator emits quotes (with injected crossed/locked defects); both benches
  dispatch the third record type. 6 new tests (57 total).

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
