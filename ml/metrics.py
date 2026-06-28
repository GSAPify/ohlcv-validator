"""Anomaly-detection metrics.

AUC measures *ranking* and needs no threshold -- how well scores separate anomaly
from normal in principle. Precision / recall / F1 measure a *calibrated* detector
at its actual operating point -- what you'd really catch and falsely flag once a
threshold is chosen. Reporting both is the honest pair: a great AUC with a badly
calibrated threshold still detects nothing useful.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np


def auc(score: np.ndarray, label: np.ndarray) -> float:
    """ROC AUC via the Mann-Whitney statistic = P(score[anomaly] > score[normal])."""
    pos, neg = score[label], score[~label]
    if len(pos) == 0 or len(neg) == 0:
        return float("nan")
    allv = np.concatenate([pos, neg])
    order = allv.argsort()
    ranks = np.empty(len(allv), dtype=np.float64)
    ranks[order] = np.arange(1, len(allv) + 1)
    return (ranks[: len(pos)].sum() - len(pos) * (len(pos) + 1) / 2) / (
        len(pos) * len(neg)
    )


@dataclass
class Scored:
    precision: float
    recall: float
    f1: float
    tp: int
    fp: int
    fn: int
    tn: int
    flag_rate: float

    def __str__(self) -> str:
        return (f"precision={self.precision:.3f} recall={self.recall:.3f} "
                f"f1={self.f1:.3f} (tp={self.tp} fp={self.fp} fn={self.fn}) "
                f"flag_rate={self.flag_rate:.3%}")


def precision_recall_f1(flagged: np.ndarray, label: np.ndarray) -> Scored:
    """Score a boolean `flagged` mask against ground-truth `label`."""
    tp = int(np.sum(flagged & label))
    fp = int(np.sum(flagged & ~label))
    fn = int(np.sum(~flagged & label))
    tn = int(np.sum(~flagged & ~label))
    precision = tp / (tp + fp) if (tp + fp) else 0.0
    recall = tp / (tp + fn) if (tp + fn) else 0.0
    f1 = 2 * precision * recall / (precision + recall) if (precision + recall) else 0.0
    flag_rate = float(np.mean(flagged)) if len(flagged) else 0.0
    return Scored(precision, recall, f1, tp, fp, fn, tn, flag_rate)
