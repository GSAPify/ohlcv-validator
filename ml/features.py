"""Feature extraction from the replay stream, per symbol, on the bar grid.

The 1-minute bar is the natural time grid for our feed: trades are irregular and
quotes are absent on the free IEX feed (0 on historical, live TBD), so features
are built from bars, with trade aggregates folded in where they add signal. Every
feature here is computable from what we actually capture -- nothing assumes order-
book depth we don't have.

Features (per symbol, ordered by bar start time):

    log_return   log(close / prev_close)            -- the headline signal
    hl_range     (high - low) / close               -- intrabar volatility proxy
    vwap_dev     (close - vwap) / vwap              -- where close sits vs VWAP
    log_volume   log1p(volume)                       -- size, compressed
    trade_count  raw trades in the bar               -- activity / intensity
    realized_vol rolling std of log_return (window)  -- recent volatility regime
    volume_z     rolling robust z of log_volume      -- volume surprise vs recent

These feed the classical baseline today and whatever model sits on the stream
later. The deliberate omission is anything quote/spread-derived -- see ml/README.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from replay_reader import ReplayData, symbols

# Rolling window (in bars) for the regime features. One trading hour at 1-min
# bars; small enough to react, long enough for a stable estimate.
DEFAULT_WINDOW = 60

# A return is only meaningful between *consecutive* bars. Anything past this gap
# (overnight, a halt, or a missing-data hole) is a session break, not a 1-minute
# move. 1-min bars are normally 60s apart; 5 min tolerates minor irregularity
# while catching the ~17h overnight gap that real multi-day data exposed (a gap
# the single-series synthetic data never produced).
SESSION_GAP_NS = 5 * 60 * 1_000_000_000

FEATURE_NAMES = (
    "log_return",
    "hl_range",
    "vwap_dev",
    "log_volume",
    "trade_count",
    "realized_vol",
    "volume_z",
)


@dataclass
class FeatureFrame:
    """Per-bar features, aligned row-for-row across these parallel arrays."""

    symbol: np.ndarray       # U8 ticker per row
    start_ns: np.ndarray     # bar start, ns since epoch
    features: np.ndarray     # shape (n_rows, len(FEATURE_NAMES)), float64
    names: tuple = FEATURE_NAMES

    def __len__(self) -> int:
        return len(self.start_ns)

    def column(self, name: str) -> np.ndarray:
        return self.features[:, self.names.index(name)]


def _rolling_std(x: np.ndarray, window: int) -> np.ndarray:
    """Trailing rolling std, same length as x; the first row is 0 (no history)."""
    out = np.zeros_like(x, dtype=np.float64)
    for i in range(1, len(x)):
        lo = max(0, i - window + 1)
        seg = x[lo : i + 1]
        out[i] = seg.std() if len(seg) > 1 else 0.0
    return out


def _rolling_robust_z(x: np.ndarray, window: int) -> np.ndarray:
    """Trailing robust z-score (median / MAD), same length as x.

    Robust because volume is heavy-tailed: a single huge bar would inflate a
    mean/std z and swallow everything else. MAD is scaled to be a consistent
    estimator of std for normal data (* 1.4826).
    """
    out = np.zeros_like(x, dtype=np.float64)
    for i in range(len(x)):
        lo = max(0, i - window + 1)
        seg = x[lo : i + 1]
        med = np.median(seg)
        mad = np.median(np.abs(seg - med))
        scale = 1.4826 * mad
        out[i] = (x[i] - med) / scale if scale > 0 else 0.0
    return out


def _features_for_symbol(bars: np.ndarray, window: int) -> np.ndarray:
    """Compute the feature matrix for one symbol's bars, already time-sorted."""
    close = bars["close"].astype(np.float64)
    high = bars["high"].astype(np.float64)
    low = bars["low"].astype(np.float64)
    vwap = bars["vwap"].astype(np.float64)
    volume = bars["volume"].astype(np.float64)
    trade_count = bars["trade_count"].astype(np.float64)

    n = len(bars)
    log_return = np.zeros(n, dtype=np.float64)
    # log(close / prev_close); row 0 has no predecessor -> 0. Guard non-positive
    # prices (a corrupt tick the validator already flags) to avoid log domain err.
    prev = close[:-1]
    cur = close[1:]
    safe = (prev > 0) & (cur > 0)
    log_return[1:][safe] = np.log(cur[safe] / prev[safe])

    # Session awareness: zero the return across any gap > SESSION_GAP_NS, so the
    # first bar of a session doesn't book a multi-hour overnight move as a 1-min
    # return. This propagates into realized_vol (rolling std of the return) too.
    ts = bars["start_ns"].astype(np.int64)
    session_break = np.zeros(n, dtype=bool)
    session_break[1:] = np.diff(ts) > SESSION_GAP_NS
    log_return[session_break] = 0.0

    hl_range = np.divide(high - low, close, out=np.zeros(n), where=close > 0)
    vwap_dev = np.divide(close - vwap, vwap, out=np.zeros(n), where=vwap > 0)
    log_volume = np.log1p(np.maximum(volume, 0.0))

    realized_vol = _rolling_std(log_return, window)
    volume_z = _rolling_robust_z(log_volume, window)

    return np.column_stack(
        [log_return, hl_range, vwap_dev, log_volume, trade_count, realized_vol,
         volume_z]
    )


def extract_features(data: ReplayData, window: int = DEFAULT_WINDOW) -> FeatureFrame:
    """Build the per-bar feature frame for every symbol in the replay data.

    Symbols are processed independently (no cross-symbol contamination) and the
    output is concatenated, grouped by symbol then sorted by bar start time.
    """
    bars = data.bars
    if len(bars) == 0:
        empty_f = np.zeros((0, len(FEATURE_NAMES)), dtype=np.float64)
        return FeatureFrame(
            symbol=np.array([], dtype="U8"),
            start_ns=np.array([], dtype=np.uint64),
            features=empty_f,
        )

    syms = symbols(bars)
    out_sym, out_ts, out_feat = [], [], []
    for sym in np.unique(syms):
        mask = syms == sym
        sub = bars[mask]
        order = np.argsort(sub["start_ns"], kind="stable")
        sub = sub[order]
        out_sym.append(np.full(len(sub), sym, dtype="U8"))
        out_ts.append(sub["start_ns"].copy())
        out_feat.append(_features_for_symbol(sub, window))

    return FeatureFrame(
        symbol=np.concatenate(out_sym),
        start_ns=np.concatenate(out_ts),
        features=np.vstack(out_feat),
    )
