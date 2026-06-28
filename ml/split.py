"""Time-ordered, leakage-free train / calibrate / test split.

The cardinal sin of finance ML is lookahead -- a model or threshold that has seen
data from after the point it's evaluated at. A random shuffle of a time series
does exactly that. This carves a per-symbol, time-sorted frame into three
*contiguous* time blocks -- fit -> calibrate -> test -- so within each symbol every
fit and calibrate row strictly precedes every test row. `assert_no_leakage` proves
it (the evaluation-harness analogue of the RL env's no-lookahead test).

Calibrate is the held-out *clean* segment a threshold is set on; test is what the
detector is actually scored on. Same code path for synthetic and real data.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np


@dataclass
class Split:
    fit: np.ndarray        # row indices into the frame, time-ordered per symbol
    calibrate: np.ndarray
    test: np.ndarray


def time_split(symbol: np.ndarray, start_ns: np.ndarray,
               fit_frac: float = 0.4, calib_frac: float = 0.2) -> Split:
    """Contiguous per-symbol time blocks: first `fit_frac` -> fit, next
    `calib_frac` -> calibrate, remainder -> test. No shuffling."""
    fit_i, cal_i, test_i = [], [], []
    for sym in np.unique(symbol):
        idx = np.where(symbol == sym)[0]
        idx = idx[np.argsort(start_ns[idx], kind="stable")]   # strict time order
        n = len(idx)
        a = int(n * fit_frac)
        b = a + int(n * calib_frac)
        fit_i.append(idx[:a])
        cal_i.append(idx[a:b])
        test_i.append(idx[b:])
    return Split(
        fit=np.concatenate(fit_i) if fit_i else np.array([], dtype=int),
        calibrate=np.concatenate(cal_i) if cal_i else np.array([], dtype=int),
        test=np.concatenate(test_i) if test_i else np.array([], dtype=int),
    )


def assert_no_leakage(split: Split, symbol: np.ndarray, start_ns: np.ndarray) -> None:
    """Raise if, for any symbol, a fit/calibrate row is not strictly earlier than
    that symbol's test rows -- i.e., if the future leaked backward into training."""
    for sym in np.unique(symbol):
        def ts(rows: np.ndarray) -> np.ndarray:
            s = rows[symbol[rows] == sym]
            return start_ns[s]
        f, c, t = ts(split.fit), ts(split.calibrate), ts(split.test)
        if len(t) and len(f) and f.max() >= t.min():
            raise AssertionError(f"leakage: {sym!r} fit not before test")
        if len(t) and len(c) and c.max() >= t.min():
            raise AssertionError(f"leakage: {sym!r} calibrate not before test")
        if len(c) and len(f) and f.max() >= c.min():
            raise AssertionError(f"leakage: {sym!r} fit not before calibrate")
