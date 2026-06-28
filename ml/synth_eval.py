"""Synthetic substrate for a TIME-ORDERED, leakage-free anomaly evaluation.

`synth.generate` emits one short anomaly burst -- enough to *demonstrate the
mechanism* (the autoencoder catches a joint anomaly the baseline misses). This
emits a long **clean run-up** followed by a **test region sprinkled with many**
rule-invisible anomalies (the same return<->volume correlation break). The point
is calibration: a threshold set on the clean prefix and applied to the later test
region has labels to score against, with enough positives for precision / recall /
F1 to mean something (15 anomalies makes F1 pure noise; this gives ~100+).

Anomalies live ONLY in the test tail, so the early portion is a clean calibration
set -- exactly the shape `ml/split.py` carves and `ml/evaluate.py` consumes.
"""

from __future__ import annotations

import numpy as np

from synth import SynthBars  # reuse the dataclass


def generate_eval(
    n_bars: int = 4000,
    anomaly_count: int = 120,
    clean_frac: float = 0.6,
    p0: float = 100.0,
    ret_scale: float = 0.002,
    lv_base: float = 8.0,
    lv_coef: float = 1.1,
    seed: int = 11,
) -> SynthBars:
    """Long clean prefix + a test tail carrying `anomaly_count` rule-invisible
    anomalies (small price move on median volume). Deterministic for a seed."""
    rng = np.random.default_rng(seed)

    clean_cut = int(n_bars * clean_frac)
    is_anom = np.zeros(n_bars, dtype=bool)
    region = np.arange(clean_cut, n_bars)
    k = int(min(anomaly_count, len(region)))
    is_anom[rng.choice(region, size=k, replace=False)] = True

    # Normal bars: volume tracks the size of the price move. Anomalies break the
    # coupling -- a small move on median volume (each bar individually legal).
    move = np.abs(rng.standard_normal(n_bars))
    vol = move.copy()
    na = int(is_anom.sum())
    vol[is_anom] = 0.8 + 0.2 * np.abs(rng.standard_normal(na))
    move[is_anom] = 0.08 * np.abs(rng.standard_normal(na))

    sign = rng.choice((-1.0, 1.0), size=n_bars)
    ret = sign * move * ret_scale

    open_ = np.empty(n_bars)
    high = np.empty(n_bars)
    low = np.empty(n_bars)
    close = np.empty(n_bars)
    vwap = np.empty(n_bars)
    px = p0
    for i in range(n_bars):
        o = px
        c = px * float(np.exp(ret[i]))
        w = max(move[i], 0.2) * ret_scale * px      # range scales with the move
        open_[i] = o
        high[i] = max(o, c) + 0.5 * w
        low[i] = min(o, c) - 0.5 * w
        close[i] = c
        vwap[i] = 0.5 * (o + c)                       # always within [low, high]
        px = c

    log_vol = lv_base + lv_coef * vol + 0.05 * rng.standard_normal(n_bars)
    volume = np.maximum(np.round(np.exp(log_vol)), 1.0).astype(np.uint64)
    trade_count = np.maximum(volume // np.uint64(200), np.uint64(1))
    base = 1_704_206_000_000_000_000
    start_ns = base + np.arange(n_bars, dtype=np.uint64) * np.uint64(60_000_000_000)

    return SynthBars(
        start_ns=start_ns, open=open_, high=high, low=low, close=close,
        vwap=vwap, volume=volume, trade_count=trade_count, is_anomaly=is_anom,
    )
