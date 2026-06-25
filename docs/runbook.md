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
./build/alpaca_ingest AAPL                       # one symbol: prints every record
./build/alpaca_ingest AAPL MSFT NVDA            # many: quiet, violations + summary only
OHLCV_VIOLATIONS_LOG=violations.jsonl ./build/alpaca_ingest AAPL MSFT  # + structured log
OHLCV_CAPTURE=cap.bin ./build/alpaca_ingest AAPL MSFT   # record the stream for replay
./build/replay_bench cap.bin 1                          # ...then replay it offline
```

Symbols are positional args (default `AAPL`). `OHLCV_VIOLATIONS_LOG=path` appends
each flagged record as a JSONL line (kind/symbol/ts-as-string/seq/checks).
`OHLCV_CAPTURE=path` records the full validated stream in the binary replay
format (OVERWRITES the file) so it can be replayed/benchmarked offline — the real
counterpart to `gen_dataset`'s synthetic data.
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

Reconnect integration test (separate target — links the real client + Boost/
OpenSSL + a local TLS server; no network, no market). Drops a live connection
and asserts the client re-establishes:

```sh
./build/tests/reconnect_test
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

- **2026-06-25** — feed: **A/B line arbitration + gap detection core** (`src/feed/`).
  Real exchange feeds publish one sequenced binary stream redundantly on two lines;
  `FeedArbitrator` (`feed/arbitrator.h`, header-only, zero-alloc) reconstructs the
  true stream — delivers each seq exactly once in order (whichever line is first),
  detects gaps lost on both lines. Gets the two pillars right: doesn't cry gap on
  mere reordering (buffers a power-of-two reorder window, releases the contiguous
  prefix; gap only on window overflow or explicit flush), and stores+validates the
  seq per ring slot so a far-ahead jump can't alias a live message. Payload reuses
  the 88-byte `WireRecord` (`FeedPacket = seq + line + WireRecord`). DETECTION not
  recovery (a gap is the retransmit/snapshot hook); UDP multicast transport +
  publisher/handler demo = follow-up (#1b), order-book builder = #2 (the gap signal
  feeds it: a book is invalid until snapshot recovery). 9 new tests incl. the two
  that matter (dup-below-frontier dropped; far-ahead jump → gap, no aliasing) +
  arbitrator zero-alloc in the alloc guard. 101 C++ tests. Highest-leverage of the
  three "push it closer to HFT" moves; built core-first (multicast on CI is flaky).
- **2026-06-22** — ops: **turnkey capture session script** (`scripts/capture_session.sh`).
  One command from live feed to baseline scores on real data: sources `.env`,
  runs `alpaca_ingest` with `OHLCV_CAPTURE` for a bounded window (macOS has no
  `timeout`, so a watchdog SIGINTs at the deadline for a clean finalize, SIGKILL
  only as a fallback), then runs `replay_bench` + `ml/detect.py` on the captured
  file. `scripts/capture_session.sh 300 AAPL MSFT NVDA TSLA`. Off-hours pre-flight
  confirmed connect/auth/subscribe and graceful empty-file handling; CAVEAT: the
  clean finalize on a *populated* capture can only be verified during RTH (off
  hours the idle feed blocks `read()` so SIGINT can't land — the known idle-feed
  issue — and the watchdog falls through to SIGKILL → empty file). During RTH the
  active feed makes SIGINT land between frames. First real run: market open tonight.
- **2026-06-21** — ci: **full pipeline DAG** (`.github/workflows/ci.yml`). Replaced
  the 2-job workflow with a 7-node graph: a fast `lint` gate (ruff + py-compile)
  fans out to five parallel jobs — `cpp-test` (build + ctest), `cpp-asan-ubsan`,
  `cpp-tsan` (the lock-free SPSC ring proven race-free in CI, not just locally),
  `cpp-linux` (pure-C++ x86 build + gen→validate→bench smoke on ubuntu), and
  `ml-tests` (builds the C++ binaries the round-trip tests shell out to, then runs
  all 19 `pytest ml/` tests) — and fans back into a single `ci-success` required
  gate. Adds concurrency cancellation, manual dispatch, pip caching. The ML layer
  (ladder + RL) was previously tested nowhere automated; now it can't rot silently.
  README gains CI + status badges. Every job verified green locally before wiring
  (ruff clean, TSan ring passes, Linux smoke runs).
- **2026-06-20** — ml: **RL sandbox — position-taking environment** (`ml/rl_env.py`,
  `rl_policies.py`, `rl_demo.py`). Gymnasium-style `reset`/`step` (no gym dep) over
  the bar/feature stream; action {-1,0,+1}, reward `position·fwd_return −
  txn_cost·|Δposition|`. A *separate track* from the anomaly ladder, not a higher
  rung (RL = sequential decisions, not anomaly detection). Sandbox mechanics only —
  no training loop, no "learned to trade" claim (synthetic data has no alpha). The
  headline is the **no-lookahead proof**: a cheating clairvoyant policy profits
  (+1.09, proving the reward pays for correct bets) while obs-only baselines net ~0
  (random/momentum, no edge on i.i.d. returns) — if an obs-only policy turned
  strongly positive that'd be a leak. `from_replay` computes forward returns
  strictly within a symbol. 8 tests. Run: `python3 ml/rl_demo.py`.
- **2026-06-20** — ml: **autoencoder (rung 3) + the eval that justifies it**
  (`ml/autoencoder.py`, `synth.py`, `replay_writer.py`, `eval_ladder.py`). The
  honest test of rung 3 is whether it catches what the rules AND the baseline miss
  — not gen_dataset's injected defects (circular). So `synth.py` builds a
  **rule-invisible** anomaly: a return↔volume correlation break (small move on
  median volume) where every bar passes every validator rule and no single feature
  is an outlier. `replay_writer.py` (inverse of the reader) emits it as a real
  `.bin` so the **C++ validator confirms 0 violations** — proven, not asserted.
  Result (out-of-sample): validator SILENT, robust-z baseline BLIND (0/15, AUC
  0.21), autoencoder CATCHES IT (AUC 0.997, 12.7× recon-error). torch added (rung
  3 only). Run: `python3 ml/eval_ladder.py`. (Restored: this entry and the one
  below were dropped in the #15/#16 merge conflict resolution; the code landed.)
- **2026-06-20** — ml: **data-plane bridge + classical anomaly baseline** (`ml/`).
  Python layer that reads the *same* binary replay file the C++ benches validate —
  byte-precise NumPy reader (`replay_reader.py`, mirrors `model/wire.h`; pinned by
  `static_assert`s on `sizeof(WireRecord)==88` / `WireBar==80` / header==16),
  per-symbol bar features, and a robust-z/MAD baseline (the rung a model must
  beat). Motivated by Sirignano (2016), but its spatial NN is off the table — it
  needs L3 order-book depth we don't capture. NumPy-only, kept out of CMake/ctest.
- **2026-06-20** — test: **reconnect integration test** (`tests/test_reconnect.cpp`,
  separate `reconnect_test` target). Stands up a local TLS websocket server,
  drops a live connection mid-stream, and proves the *unmodified* client
  re-establishes (reconnect → re-auth → re-subscribe) and resumes — converting
  #13's socket path from asserted to tested. No Alpaca/network/market: the test
  trusts the server's self-signed cert via `SSL_CERT_FILE`, so no production code
  changed. (Prompted by REST recon that day confirming the IEX trade feed is real
  during RTH — ~57 AAPL / 413 NVDA trades/min — but historical quotes return 0 on
  the free plan; live quotes TBD Monday.) 92 tests.
- **2026-06-19** — ingest: **WS read-timeout + transparent reconnect**. The first
  real live run hung forever on an idle feed (couldn't Ctrl+C) — root cause: Beast's
  client `timeout::suggested` leaves `idle_timeout = none`, `keep_alive_pings =
  false`, so a quiet/half-open peer parks `read()` indefinitely (verified in the
  Beast source). Fix: finite `idle_timeout` + keep-alive pings → dead peers surface
  as a read timeout, which `read_frame()` turns into a transparent reconnect
  (connect → auth → replay stored subscription) with exponential backoff
  (`util/backoff.h`, ceiling unit-tested; equal-jitter on top). Fully inside the
  client → `alpaca_ingest` unchanged (branched off main). Caveats: socket-reconnect
  path is integration-only (local-server test = follow-up); instant Ctrl+C on a
  live-but-idle feed still needs the deferred async refactor. 89 tests.
- **2026-06-18** — replay: **live capture → binary replay** (`replay/capture_writer.h`,
  `OHLCV_CAPTURE=path` in `alpaca_ingest`). Records the full validated live stream
  in the same fixed-stride format `gen_dataset` synthesizes, so the benchmark and
  validator now run on *real captured market data* — the loop `gen_dataset`'s
  comment said it was standing in for. RAII writer patches the header's
  record_count on clean shutdown (idempotent finalize; hard-kill caveat
  documented). Stores real `ts_ns`, synthetic `seq` (replay artifact, not a raw
  archive). Verified the *real* `replay_bench` ingests a captured file (records/pass
  matched). 89 tests (round-trip + finalize-idempotent). Off the hot path.
- **2026-06-17** — ingest: make the live validator a tool. Symbols are now CLI
  args (multi-symbol; many symbols → quiet mode, stdout = violations + summary,
  since per-record prints across N names are a firehose). `OHLCV_VIOLATIONS_LOG`
  writes flagged records as structured JSONL (`ingest/violation_log.h`): logs only
  the surfaced checks (mirrors the live `!!` set) and emits 64-bit ns timestamps
  as JSON *strings* (a ~1.7e18 ns ts exceeds 2^53 and truncates in jq/JS doubles).
  Also fixed a stale summary note that still blamed the "shared clock" for
  ts-regression suppression (per-stream `last_ts` fixed that in #10; the real
  reason now is pending live verification). 87 tests. Off the replay/hot path.
- **2026-06-16** — validate: **per-stream `last_ts`**. The Slot tracked one
  `last_ts` per symbol shared across trades/quotes/bars, so a quote (or a bar's
  window-start) advancing the clock false-flagged a following record of another
  stream as a timestamp regression. Now each stream tracks its own `last_ts`
  (`Stream` enum, unseen-sentinel 0); `seq`/gap stays a shared per-symbol cursor
  (the feed/generator advances it across all types). Timestamp regression is now
  a correct *within-stream* check — robust for any multi-stream dataset, though
  no observed binary-path change (the generator emits monotonic-across-type ts;
  replay still flags 756 injected regressions/run). Tests inverted (cross-stream
  no longer flags) + within-stream positives for trades, quotes, and bars. Stays
  suppressed on the live report for the narrower, honest reason that
  it's unverified on real (possibly bursty/reordered) delivery; un-suppressing is
  gated on a live measurement. 82 tests; alloc-guard still clean (Slot +16B,
  still a fixed std::array, zero-heap).
- **2026-06-13** — concurrent: cached-cursor SPSC variant (`CacheFarCursor`) +
  the measurement that was blocked on the box for days. Caching the far cursor
  (re-read only on full/empty) **raised ring-bound throughput ~30%** (64B
  separate-lines ~33→43 M ops/s) — as predicted, since most cross-core cursor
  reads vanish. But it did **not** flip the cache-line result: packed cursors
  still win (~9% for 64B, down from ~23% naive — the gap shrank, consistent with
  the true-sharing story, but didn't cross zero). So PR #6's "padding wins once
  cached" hypothesis is half-confirmed; honest null on the flip. `SpscRingCached`
  tests TSan-clean. README updated to the measured outcome. (7900X3D, 2 cores/1
  CCD, WSL2 — 8B-word deltas too noisy to quote.)
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
