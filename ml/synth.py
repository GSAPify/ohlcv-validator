"""Synthetic bars with a RULE-INVISIBLE anomaly -- the eval substrate for rung 3.

The whole point of an autoencoder over a univariate baseline is catching anomalies
in the *joint* structure of the features, not a level shift in any one of them. So
the honest test of rung 3 needs an anomaly that:

  1. passes every validator rule    (so the C++ validator stays silent),
  2. shows no single-feature outlier (so the robust-z baseline stays silent),
  3. breaks a relationship          (so only a model of the joint dist. can see it).

The construction: in the normal regime, bar volume tracks the size of the bar's
price move -- big moves trade big size, small moves trade small size (the usual
return/volume coupling). Returns and volume both swing widely bar-to-bar, so no
single bar's volume or return is unusual on its own.

The anomaly segment breaks the coupling: small price moves on high volume. Each
such bar is individually unremarkable -- a small return is normal (quiet bars
exist), high volume is normal (busy bars exist) -- but small-return-AND-high-
volume together is not. The segment is kept short relative to the rolling feature
windows (realized_vol, volume_z) so it doesn't shift those either. The only tell
is the broken correlation.

Returns are i.i.d. bar-to-bar (no persistence) on purpose: it makes volume swing
widely every bar, so a high-volume anomaly bar isn't a rolling-window outlier.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np


@dataclass
class SynthBars:
    start_ns: np.ndarray
    open: np.ndarray
    high: np.ndarray
    low: np.ndarray
    close: np.ndarray
    vwap: np.ndarray
    volume: np.ndarray
    trade_count: np.ndarray
    is_anomaly: np.ndarray  # bool per bar -- ground truth for the eval


def generate(
    n_bars: int = 800,
    anomaly_start: int = 380,
    anomaly_len: int = 15,
    p0: float = 100.0,
    ret_scale: float = 0.002,
    lv_base: float = 8.0,
    lv_coef: float = 1.1,
    seed: int = 7,
) -> SynthBars:
    """Generate one symbol's rule-clean bars with a correlation-break anomaly."""
    rng = np.random.default_rng(seed)

    is_anom = np.zeros(n_bars, dtype=bool)
    is_anom[anomaly_start : anomaly_start + anomaly_len] = True

    # Per-bar move magnitude and the volume magnitude it drives. In normal bars
    # these are the SAME draw (coupled). In anomaly bars the price-move magnitude
    # is small while the volume magnitude stays large (decoupled).
    move_mag = np.abs(rng.standard_normal(n_bars))      # drives the price move
    vol_mag = move_mag.copy()                           # drives volume -> coupled
    # Anomaly: shrink the price move to ~zero, but set volume to the *median* of
    # the distribution (vol_mag ~ E|N(0,1)| ~ 0.8). Median volume is deliberate:
    # a HIGH-volume bar would be a univariate outlier the robust-z baseline could
    # catch, defeating the point. A median-volume bar trips no single-feature
    # threshold -- yet for a near-zero-return bar it's still anomalous, because in
    # the normal regime near-zero-return bars carry LOW volume. The tell is purely
    # the broken return<->volume coupling, which only a joint model sees.
    vol_mag[is_anom] = 0.8 + 0.2 * np.abs(rng.standard_normal(is_anom.sum()))
    move_mag[is_anom] = 0.08 * np.abs(rng.standard_normal(is_anom.sum()))

    sign = rng.choice([-1.0, 1.0], size=n_bars)
    ret = sign * move_mag * ret_scale

    open_ = np.empty(n_bars)
    high = np.empty(n_bars)
    low = np.empty(n_bars)
    close = np.empty(n_bars)
    vwap = np.empty(n_bars)

    prev_close = p0
    for i in range(n_bars):
        o = prev_close
        c = prev_close * float(np.exp(ret[i]))
        # Intrabar range scales with the move (+ a floor so it's never zero).
        rng_width = max(move_mag[i], 0.2) * ret_scale * prev_close
        hi = max(o, c) + 0.5 * rng_width
        lo = min(o, c) - 0.5 * rng_width
        open_[i], high[i], low[i], close[i] = o, hi, lo, c
        vwap[i] = 0.5 * (o + c)  # always within [lo, hi]
        prev_close = c

    log_volume = lv_base + lv_coef * vol_mag + 0.05 * rng.standard_normal(n_bars)
    volume = np.maximum(np.round(np.exp(log_volume)), 1.0).astype(np.uint64)
    # Both nonzero (the validator flags volume/trade_count disagreeing on zero).
    trade_count = np.maximum(volume // np.uint64(200), np.uint64(1))

    # 1-minute bars starting at a fixed, plausible epoch (2024-01-02 14:30 UTC).
    base = 1_704_206_000_000_000_000
    start_ns = base + np.arange(n_bars, dtype=np.uint64) * np.uint64(60_000_000_000)

    return SynthBars(
        start_ns=start_ns,
        open=open_,
        high=high,
        low=low,
        close=close,
        vwap=vwap,
        volume=volume,
        trade_count=trade_count,
        is_anomaly=is_anom,
    )
