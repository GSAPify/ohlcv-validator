"""Fetch real historical bars from Alpaca's REST API into our binary replay format.

The live-capture path needs Claude running during US RTH (19:00-01:30 IST), which
kept missing. This sidesteps timing entirely: pull a recent session's 1-minute
bars over REST (works any hour), write them to the same binary file the C++
validator and the ML layer read. Real market data, no live window required.

    python3 ml/fetch_history.py --symbols AAPL MSFT NVDA TSLA \
        --start 2026-06-22T13:30:00Z --end 2026-06-24T20:00:00Z --out real.bin

Keys come from the environment (SECRET_ALPACA_API_KEY / SECRET_ALPACA_API_SECRET,
the same names alpaca_ingest uses); they are never printed. Free tier = IEX feed:
trades/bars are real, quotes are typically empty -- which is exactly why our
features are bar-based.

Scope: bars only (what the feature layer consumes). The Alpaca bar fields map 1:1
onto WireBar: o/h/l/c -> open/high/low/close, vw -> vwap, v -> volume,
n -> trade_count, t -> start_ns.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.parse
import urllib.request
from collections import defaultdict
from datetime import datetime

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from replay_reader import DT_BAR  # noqa: E402
from replay_writer import make_bars, write_replay  # noqa: E402

_BARS_URL = "https://data.alpaca.markets/v2/stocks/bars"


def _iso_to_ns(s: str) -> int:
    dt = datetime.fromisoformat(s.replace("Z", "+00:00"))
    return int(dt.timestamp() * 1_000_000_000)


def _auth_headers() -> dict:
    key = os.environ.get("SECRET_ALPACA_API_KEY")
    sec = os.environ.get("SECRET_ALPACA_API_SECRET")
    if not key or not sec:
        sys.exit("missing SECRET_ALPACA_API_KEY / SECRET_ALPACA_API_SECRET "
                 "(source .env first: set -a && source .env && set +a)")
    return {"APCA-API-KEY-ID": key, "APCA-API-SECRET-KEY": sec}


def fetch_bars(symbols, start, end, feed="iex", timeframe="1Min") -> dict:
    """Return {symbol: [bar, ...]} across the range, following pagination."""
    headers = _auth_headers()
    out: dict = defaultdict(list)
    page_token = None
    while True:
        params = {
            "symbols": ",".join(symbols),
            "timeframe": timeframe,
            "start": start,
            "end": end,
            "limit": 10000,
            "feed": feed,
            "adjustment": "raw",
        }
        if page_token:
            params["page_token"] = page_token
        req = urllib.request.Request(
            f"{_BARS_URL}?{urllib.parse.urlencode(params)}", headers=headers
        )
        with urllib.request.urlopen(req, timeout=30) as resp:
            data = json.loads(resp.read())
        for sym, bars in (data.get("bars") or {}).items():
            out[sym].extend(bars)
        page_token = data.get("next_page_token")
        if not page_token:
            break
    return out


def to_records(bars_by_symbol: dict) -> np.ndarray:
    """Convert fetched bars to a concatenated DT_BAR array (per symbol, time-sorted)."""
    chunks = []
    for sym in sorted(bars_by_symbol):
        bars = sorted(bars_by_symbol[sym], key=lambda b: b["t"])
        if not bars:
            continue
        start_ns = np.array([_iso_to_ns(b["t"]) for b in bars], dtype=np.uint64)
        rec = make_bars(
            symbol=sym,
            start_ns=start_ns,
            open_=np.array([b["o"] for b in bars], dtype=np.float64),
            high=np.array([b["h"] for b in bars], dtype=np.float64),
            low=np.array([b["l"] for b in bars], dtype=np.float64),
            close=np.array([b["c"] for b in bars], dtype=np.float64),
            vwap=np.array([b["vw"] for b in bars], dtype=np.float64),
            volume=np.array([b["v"] for b in bars], dtype=np.uint64),
            trade_count=np.array([b["n"] for b in bars], dtype=np.uint64),
        )
        chunks.append(rec)
    if not chunks:
        sys.exit("no bars returned for the requested symbols/range")
    # NB: np.concatenate re-packs the structured dtype (drops the alignment
    # padding -> 81-byte stride), which breaks the 88-byte on-disk contract. Fill
    # a pre-allocated DT_BAR array instead so the stride stays 88.
    total = sum(len(c) for c in chunks)
    out = np.zeros(total, dtype=DT_BAR)
    i = 0
    for c in chunks:
        out[i : i + len(c)] = c
        i += len(c)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--symbols", nargs="+", default=["AAPL", "MSFT", "NVDA", "TSLA"])
    ap.add_argument("--start", required=True, help="RFC3339 UTC, e.g. 2026-06-22T13:30:00Z")
    ap.add_argument("--end", required=True, help="RFC3339 UTC, e.g. 2026-06-24T20:00:00Z")
    ap.add_argument("--feed", default="iex")
    ap.add_argument("--timeframe", default="1Min")
    ap.add_argument("--out", default="real.bin")
    args = ap.parse_args()

    bars = fetch_bars(args.symbols, args.start, args.end, args.feed, args.timeframe)
    counts = {s: len(b) for s, b in sorted(bars.items())}
    print(f"fetched bars per symbol: {counts}")

    recs = to_records(bars)
    write_replay(args.out, recs)
    print(f"wrote {len(recs)} bar records -> {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
