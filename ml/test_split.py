"""Tests for the time-ordered split + the leakage guard.

The leakage guard is the evaluation-harness analogue of the RL env's no-lookahead
proof: it must accept a correct time-ordered split and REJECT one where the future
leaked backward into fit/calibrate.

Run: pytest ml/test_split.py
"""

from __future__ import annotations

import os
import sys

import numpy as np
import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from split import Split, assert_no_leakage, time_split  # noqa: E402


def test_split_is_contiguous_in_time_and_partitions_rows():
    n = 100
    symbol = np.full(n, "AAPL", dtype="U8")
    start_ns = np.arange(n, dtype=np.uint64) * np.uint64(60_000_000_000)
    sp = time_split(symbol, start_ns, fit_frac=0.4, calib_frac=0.2)

    assert len(sp.fit) == 40 and len(sp.calibrate) == 20 and len(sp.test) == 40
    # exact partition, no overlap
    allrows = np.concatenate([sp.fit, sp.calibrate, sp.test])
    assert sorted(allrows.tolist()) == list(range(n))
    # contiguous in time: fit < calibrate < test
    assert start_ns[sp.fit].max() < start_ns[sp.calibrate].min()
    assert start_ns[sp.calibrate].max() < start_ns[sp.test].min()


def test_assert_no_leakage_accepts_a_valid_split():
    n = 60
    symbol = np.full(n, "X", dtype="U8")
    start_ns = np.arange(n, dtype=np.uint64)
    sp = time_split(symbol, start_ns)
    assert_no_leakage(sp, symbol, start_ns)  # must not raise


def test_assert_no_leakage_rejects_future_in_fit():
    n = 60
    symbol = np.full(n, "X", dtype="U8")
    start_ns = np.arange(n, dtype=np.uint64)
    sp = time_split(symbol, start_ns)
    # Inject leakage: move the LAST (latest) test row into fit.
    leaky = Split(fit=np.append(sp.fit, sp.test[-1]),
                  calibrate=sp.calibrate, test=sp.test)
    with pytest.raises(AssertionError, match="leakage"):
        assert_no_leakage(leaky, symbol, start_ns)


def test_split_is_per_symbol():
    # Two symbols interleaved in row order; the split must order WITHIN each symbol.
    rows = 40
    symbol = np.array(["AAA", "BBB"] * rows, dtype="U8")
    start_ns = np.repeat(np.arange(rows, dtype=np.uint64), 2)
    sp = time_split(symbol, start_ns, fit_frac=0.5, calib_frac=0.25)
    # No symbol's fit/calibrate may be at/after its own test.
    assert_no_leakage(sp, symbol, start_ns)
    for sym in ("AAA", "BBB"):
        fit_t = start_ns[sp.fit[symbol[sp.fit] == sym]]
        test_t = start_ns[sp.test[symbol[sp.test] == sym]]
        assert fit_t.max() < test_t.min()
