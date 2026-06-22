#!/usr/bin/env bash
#
# Capture a bounded window of the live Alpaca feed, then validate and featurize
# the REAL captured data. This is the turnkey version of the Monday capture: one
# command from "connect to the live feed" to "baseline anomaly scores on real
# bars", so the live window isn't spent typing.
#
#   scripts/capture_session.sh [DURATION_SEC] [SYMBOL ...]
#   scripts/capture_session.sh 300 AAPL MSFT NVDA TSLA     # 5 min, 4 symbols
#   scripts/capture_session.sh                              # defaults: 300s, 4 syms
#
# Needs .env with Alpaca paper keys (set -a/source). macOS has no `timeout`, so
# the bound is a watchdog: run in the background, SIGINT at the deadline (clean
# shutdown -> the capture writer finalizes the record count), SIGKILL only if it
# refuses to die. During RTH the feed is active so SIGINT lands between frames;
# off-hours there are no data frames to capture anyway.
set -eu

DURATION="${1:-300}"
if [[ "$DURATION" =~ ^[0-9]+$ ]]; then shift || true; else DURATION=300; fi
SYMBOLS=("${@:-AAPL MSFT NVDA TSLA}")
# Re-split the default if it came through as one word.
if [[ ${#SYMBOLS[@]} -eq 1 && "${SYMBOLS[0]}" == *" "* ]]; then
  read -r -a SYMBOLS <<< "${SYMBOLS[0]}"
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INGEST="$ROOT/build/alpaca_ingest"
BENCH="$ROOT/build/replay_bench"
OUT="${CAP_OUT:-$ROOT/cap_$(date +%Y%m%d_%H%M).bin}"

[[ -x "$INGEST" ]] || { echo "ERROR: $INGEST not built (cmake --build build)"; exit 1; }
[[ -f "$ROOT/.env" ]] || { echo "ERROR: $ROOT/.env missing (Alpaca paper keys)"; exit 1; }

echo "=== capture session ==="
echo "  symbols : ${SYMBOLS[*]}"
echo "  duration: ${DURATION}s"
echo "  output  : $OUT"
echo

# Load keys without echoing them.
set -a; # shellcheck disable=SC1091
source "$ROOT/.env"; set +a

OHLCV_CAPTURE="$OUT" "$INGEST" "${SYMBOLS[@]}" &
PID=$!

# Watchdog: clean SIGINT at the deadline so the capture file is finalized.
( sleep "$DURATION"; kill -INT "$PID" 2>/dev/null || true
  sleep 5;          kill -KILL "$PID" 2>/dev/null || true ) &
WATCHDOG=$!

wait "$PID" 2>/dev/null || true
kill "$WATCHDOG" 2>/dev/null || true

echo
if [[ ! -s "$OUT" ]]; then
  echo "no data captured (empty file). Market closed, or no frames in the window."
  exit 0
fi
echo "captured $(wc -c < "$OUT" | tr -d ' ') bytes -> $OUT"

echo
echo "=== validate the real data (C++ replay path) ==="
"$BENCH" "$OUT" 1 || true

echo
echo "=== baseline anomaly scores on the real bars ==="
python3 "$ROOT/ml/detect.py" "$OUT" || true

echo
echo "done. Real capture saved to: $OUT"
echo "next: featurize / autoencoder eval on this file once a few sessions are stacked."
