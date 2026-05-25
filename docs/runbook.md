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

- **2026-05-26** — alpaca_ingest binary: Boost.Beast sync TLS WebSocket client connecting to Alpaca's IEX paper feed. Auth + subscribe + raw-frame print with arrival-ns timestamps. Sync v1; async refactor deferred.
- **2026-05-26** — scaffold: CMake + Boost/simdjson/spdlog/nlohmann/gtest wired up; `now_ns()` self-overhead measured at 41ns median on M-series (matches 24MHz CNTVCT_EL0 resolution).
