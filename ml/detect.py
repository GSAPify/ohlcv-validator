"""CLI: replay file -> features -> baseline anomaly scores.

    python3 ml/detect.py data/replay.bin
    python3 ml/detect.py cap.bin --threshold 4 --top 20

Reads a binary replay file (synthetic from gen_dataset, or a real live capture),
builds per-symbol bar features, runs the classical baseline, and prints the
highest-scoring bars. See ml/README.md for what this does and does not prove.
"""

from __future__ import annotations

import argparse
import datetime as dt
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from baseline import score_anomalies  # noqa: E402
from features import extract_features  # noqa: E402
from replay_reader import read_replay  # noqa: E402


def _fmt_ts(ns: int) -> str:
    return dt.datetime.utcfromtimestamp(ns / 1e9).strftime("%Y-%m-%d %H:%M:%S")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path", help="binary replay file (.bin)")
    ap.add_argument("--threshold", type=float, default=5.0,
                    help="robust-z flag threshold (default 5.0)")
    ap.add_argument("--top", type=int, default=15,
                    help="how many top-scoring bars to print (default 15)")
    args = ap.parse_args()

    data = read_replay(args.path)
    print(f"loaded {data.record_count} records: "
          f"{len(data.trades)} trades, {len(data.bars)} bars, "
          f"{len(data.quotes)} quotes")

    frame = extract_features(data)
    if len(frame) == 0:
        print("no bars -> no features. (Need bar records to build the bar grid.)")
        return 0

    scored = score_anomalies(frame, threshold=args.threshold)
    print(scored.summary())
    print()

    order = np.argsort(scored.score)[::-1][: args.top]
    print(f"top {len(order)} bars by anomaly score:")
    print(f"  {'symbol':<8} {'bar start (UTC)':<20} {'score':>8}  worst feature")
    print(f"  {'-'*8} {'-'*20} {'-'*8}  {'-'*13}")
    for i in order:
        flag = "*" if scored.is_anomaly[i] else " "
        print(f"{flag} {scored.symbol[i]:<8} {_fmt_ts(int(scored.start_ns[i])):<20} "
              f"{scored.score[i]:>8.2f}  {scored.worst_feature[i]}")

    print()
    print("NOTE: on synthetic gen_dataset data the injected defects ARE the "
          "anomalies and\nthe rule validator already flags them -- this shows the "
          "pipeline runs, not that\nanomaly detection works. Real evaluation needs "
          "live-captured data.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
