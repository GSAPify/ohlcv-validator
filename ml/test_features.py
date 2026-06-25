"""Feature-layer tests — focused on the session-aware return fix.

Real multi-day data exposed that `log_return` spanned overnight gaps (the first
bar of a session booked a ~17h move as a 1-minute return). The single-series
synthetic data never produced a gap, so this guards the fix on real-shaped input.

Run: pytest ml/test_features.py
"""

from __future__ import annotations

import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import replay_reader  # noqa: E402
import replay_writer  # noqa: E402
from features import SESSION_GAP_NS, extract_features  # noqa: E402

_MIN_NS = 60 * 1_000_000_000


def _one_symbol_replay(start_ns, close):
    n = len(close)
    px = np.asarray(close, dtype=np.float64)
    recs = replay_writer.make_bars(
        symbol="TEST",
        start_ns=np.asarray(start_ns, dtype=np.uint64),
        open_=px, high=px * 1.001, low=px * 0.999, close=px, vwap=px,
        volume=np.full(n, 1000, np.uint64), trade_count=np.full(n, 10, np.uint64),
    )
    return replay_reader.ReplayData(
        trades=np.empty(0, replay_reader.DT_TRADE), bars=recs,
        quotes=np.empty(0, replay_reader.DT_QUOTE), record_count=n,
    )


def test_overnight_gap_return_is_zeroed():
    # Session A: 3 consecutive 1-min bars at 100. Then a ~17h gap, then session B
    # opening at 105 (a 5% overnight jump) and continuing.
    base = 1_704_206_000_000_000_000
    overnight = 17 * 60 * _MIN_NS
    start_ns = [
        base, base + _MIN_NS, base + 2 * _MIN_NS,           # session A
        base + 2 * _MIN_NS + overnight,                      # session B open (post-gap)
        base + 3 * _MIN_NS + overnight,                      # session B + 1
    ]
    close = [100.0, 100.0, 100.0, 105.0, 105.5]
    frame = extract_features(_one_symbol_replay(start_ns, close))
    ret = frame.column("log_return")

    # The post-gap bar (index 3) jumped 100 -> 105 but across a 17h gap: its return
    # MUST be zeroed, not log(105/100) ~ 0.049.
    assert ret[3] == 0.0, f"overnight gap booked as a 1-min return: {ret[3]}"
    # The within-session move right after (105 -> 105.5) is a real 1-min return.
    assert ret[4] != 0.0 and abs(ret[4] - np.log(105.5 / 105.0)) < 1e-12


def test_consecutive_bars_keep_their_return():
    # No gaps: every return is a genuine 1-min return (the synthetic-data case).
    base = 1_704_206_000_000_000_000
    start_ns = [base + i * _MIN_NS for i in range(5)]
    close = [100.0, 101.0, 100.5, 102.0, 101.0]
    frame = extract_features(_one_symbol_replay(start_ns, close))
    ret = frame.column("log_return")
    assert ret[0] == 0.0  # first bar, no predecessor
    for i in range(1, 5):
        assert abs(ret[i] - np.log(close[i] / close[i - 1])) < 1e-12


def test_gap_threshold_is_minutes_not_seconds():
    # A 2-minute gap (a single missing bar) is tolerated as a real return; only a
    # gap beyond SESSION_GAP_NS is treated as a session break.
    assert SESSION_GAP_NS > 60 * 1_000_000_000  # more than one bar
    base = 1_704_206_000_000_000_000
    start_ns = [base, base + 2 * _MIN_NS]  # 2-min gap (< threshold)
    close = [100.0, 101.0]
    ret = extract_features(_one_symbol_replay(start_ns, close)).column("log_return")
    assert abs(ret[1] - np.log(101.0 / 100.0)) < 1e-12


if __name__ == "__main__":
    import pytest
    sys.exit(pytest.main([__file__, "-v"]))
