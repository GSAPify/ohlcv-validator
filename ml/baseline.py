"""Classical anomaly baseline -- the bottom rung of the ML ladder.

This is deliberately the simplest thing that could work: per symbol, robust-
standardize each feature (median / MAD) and score each bar by its largest
absolute deviation across features. No training, no torch, no sklearn -- pure
NumPy. Cheap-model-first.

It exists to be *beaten*, mirroring the methodology of Sirignano (2016): naive ->
logistic -> NN -> spatial NN, each rung justified only by outperforming the one
below. A deep autoencoder-reconstruction detector (the next rung, which *does*
fit our trade/bar data, unlike the paper's depth-hungry spatial NN) has to clear
this bar before it earns its complexity.

HONESTY GUARDRAIL: on gen_dataset (synthetic) output the injected defects ARE the
anomalies, and the rule validator already flags them deterministically. This
baseline "catching" them proves the data plane round-trips -- it is NOT an
anomaly-detection result. The real question (does a statistical model catch
regime/structure anomalies the rule validator misses?) only has meaning on real
captured market data. See ml/README.md.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from features import FeatureFrame

# Default flag threshold: a bar whose worst feature sits > 5 robust-sigma from
# that symbol's median is "anomalous" for this baseline. Robust-z, so 5 is genuinely
# extreme, not a 5-sigma-of-a-fat-tail.
DEFAULT_THRESHOLD = 5.0

# Constant scaling MAD to a std estimator under normality.
_MAD_TO_STD = 1.4826


@dataclass
class ScoredAnomalies:
    symbol: np.ndarray        # U8 ticker per row, aligned to the FeatureFrame
    start_ns: np.ndarray      # bar start, ns since epoch
    score: np.ndarray         # max abs robust-z across features, per row
    is_anomaly: np.ndarray    # bool, score > threshold
    worst_feature: np.ndarray # name of the feature driving each row's score

    def summary(self) -> str:
        n = len(self.score)
        flagged = int(self.is_anomaly.sum())
        if n == 0:
            return "no rows scored (empty feature frame)"
        pct = 100.0 * flagged / n
        return f"{flagged}/{n} bars flagged ({pct:.2f}%) at the configured threshold"


def _robust_z_columns(feat: np.ndarray) -> np.ndarray:
    """Column-wise robust z-score (median / MAD) over the whole column."""
    med = np.median(feat, axis=0)
    mad = np.median(np.abs(feat - med), axis=0)
    scale = _MAD_TO_STD * mad
    # Where a feature is constant (scale == 0) it carries no signal -> z = 0.
    z = np.zeros_like(feat, dtype=np.float64)
    nz = scale > 0
    z[:, nz] = (feat[:, nz] - med[nz]) / scale[nz]
    return z


def score_anomalies(
    frame: FeatureFrame, threshold: float = DEFAULT_THRESHOLD
) -> ScoredAnomalies:
    """Score each bar; standardization is per-symbol so a quiet name and a busy
    name are each judged against their own normal."""
    n = len(frame)
    names = np.array(frame.names)
    if n == 0:
        return ScoredAnomalies(
            symbol=frame.symbol,
            start_ns=frame.start_ns,
            score=np.array([], dtype=np.float64),
            is_anomaly=np.array([], dtype=bool),
            worst_feature=np.array([], dtype="U16"),
        )

    score = np.zeros(n, dtype=np.float64)
    worst_idx = np.zeros(n, dtype=np.intp)
    for sym in np.unique(frame.symbol):
        mask = frame.symbol == sym
        z = np.abs(_robust_z_columns(frame.features[mask]))
        score[mask] = z.max(axis=1)
        worst_idx[mask] = z.argmax(axis=1)

    return ScoredAnomalies(
        symbol=frame.symbol,
        start_ns=frame.start_ns,
        score=score,
        is_anomaly=score > threshold,
        worst_feature=names[worst_idx],
    )
