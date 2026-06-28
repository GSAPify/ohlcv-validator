"""Leakage-free, calibrated evaluation of the anomaly ladder.

This is the "robustness" centerpiece: not a bigger model, but an evaluation that
can't fool itself. The spine is a time-ordered fit -> calibrate -> test split
(ml/split.py) with the leakage guard asserted on every run. Off it hang both
rungs, each calibrated on CLEAN past data, never the future:

  - robust-z baseline: per-symbol/feature median+MAD fit on the clean fit+calibrate
    rows; the flag threshold set to a target flag-rate on the calibrate scores
    (data-driven, replacing the textbook 5-sigma that over-flags fat tails ~7x);
    scored on test.
  - autoencoder: trained on the fit rows; reconstruction-error threshold from the
    clean calibrate quantile; scored on test.

On SYNTHETIC data (we own the labels) it reports precision / recall / F1 + AUC. On
REAL data (no labels) it reports flag-rate + a sample of flagged bars, explicitly
"candidates, not validated" -- it NEVER manufactures labels to fabricate a
precision number.

    python3 ml/evaluate.py             # synthetic, labelled
    python3 ml/evaluate.py real.bin    # real bars, unlabelled
"""

from __future__ import annotations

import datetime as dt
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import replay_reader  # noqa: E402
import replay_writer  # noqa: E402
import synth_eval  # noqa: E402
from autoencoder import Autoencoder  # noqa: E402
from features import FeatureFrame, extract_features  # noqa: E402
from metrics import auc, precision_recall_f1  # noqa: E402
from split import Split, assert_no_leakage, time_split  # noqa: E402

_MAD_TO_STD = 1.4826


# --- robust-z with a calibrate-derived scale (the leakage-free variant of
#     baseline.py: median/MAD come from clean past data, applied forward) --------

def _fit_robustz(feat: np.ndarray, symbol: np.ndarray) -> dict:
    stats = {}
    for sym in np.unique(symbol):
        x = feat[symbol == sym]
        med = np.median(x, axis=0)
        scale = _MAD_TO_STD * np.median(np.abs(x - med), axis=0)
        stats[sym] = (med, scale)
    return stats


def _score_robustz(feat: np.ndarray, symbol: np.ndarray, stats: dict) -> np.ndarray:
    score = np.zeros(len(feat), dtype=np.float64)
    for sym, (med, scale) in stats.items():
        m = symbol == sym
        x = feat[m]
        z = np.zeros_like(x)
        nz = scale > 0
        z[:, nz] = np.abs((x[:, nz] - med[nz]) / scale[nz])
        score[m] = z.max(axis=1)
    return score


def calibrate_threshold(scores: np.ndarray, target_rate: float = 0.01) -> float:
    """Threshold whose tail mass on the (clean) calibrate scores is `target_rate`
    -- a flag rate set by the data's own distribution, not a textbook sigma."""
    if len(scores) == 0:
        return float("inf")
    return float(np.quantile(scores, 1.0 - target_rate))


# --- the eval -----------------------------------------------------------------

class LadderEval:
    def __init__(self, frame: FeatureFrame, target_rate: float, ae_epochs: int,
                 seed: int):
        sym, ts = frame.symbol, frame.start_ns
        feat = frame.features.astype(np.float64)
        self.split: Split = time_split(sym, ts)
        assert_no_leakage(self.split, sym, ts)          # the guard, every run
        clean = np.concatenate([self.split.fit, self.split.calibrate])

        # rung 2 -- robust-z baseline
        stats = _fit_robustz(feat[clean], sym[clean])
        self.base_scores = _score_robustz(feat, sym, stats)
        self.base_thr = calibrate_threshold(self.base_scores[self.split.calibrate],
                                            target_rate)

        # rung 3 -- autoencoder
        ae = Autoencoder(epochs=ae_epochs, seed=seed).fit(feat[self.split.fit])
        self.ae_scores = ae.score(feat)
        self.ae_thr = calibrate_threshold(self.ae_scores[self.split.calibrate],
                                          target_rate)

    def test_flags(self):
        t = self.split.test
        return (self.base_scores[t] > self.base_thr,
                self.ae_scores[t] > self.ae_thr)


def _frame_from_bars(sb) -> FeatureFrame:
    recs = replay_writer.make_bars(
        "SYNTH", sb.start_ns, sb.open, sb.high, sb.low, sb.close, sb.vwap,
        sb.volume, sb.trade_count,
    )
    data = replay_reader.ReplayData(
        trades=np.empty(0, replay_reader.DT_TRADE), bars=recs,
        quotes=np.empty(0, replay_reader.DT_QUOTE), record_count=len(recs),
    )
    return extract_features(data)


def _fmt_ts(ns: int) -> str:
    return dt.datetime.utcfromtimestamp(int(ns) / 1e9).strftime("%Y-%m-%d %H:%M")


def evaluate_synthetic(target_rate: float = 0.01, ae_epochs: int = 300,
                       seed: int = 0) -> int:
    sb = synth_eval.generate_eval()
    frame = _frame_from_bars(sb)
    ev = LadderEval(frame, target_rate, ae_epochs, seed)
    s = ev.split
    label = sb.is_anomaly
    base_flag, ae_flag = ev.test_flags()
    test_label = label[s.test]

    print("=== leakage-free calibrated eval (SYNTHETIC, labelled) ===")
    print(f"  rows: fit={len(s.fit)} calibrate={len(s.calibrate)} test={len(s.test)} "
          f"(no leakage: asserted)")
    print(f"  test anomalies: {int(test_label.sum())} / {len(test_label)}  "
          f"| calibration target flag-rate: {target_rate:.1%}\n")
    base = precision_recall_f1(base_flag, test_label)
    ae = precision_recall_f1(ae_flag, test_label)
    print(f"  rung 2  robust-z (calibrated) : {base}  AUC={auc(ev.base_scores[s.test], test_label):.3f}")
    print(f"  rung 3  autoencoder (calib.)  : {ae}  AUC={auc(ev.ae_scores[s.test], test_label):.3f}")
    print("\n  Both calibrated to the SAME target rate on clean past data, then "
          "scored on the\n  unseen test tail -- a fair, leakage-free comparison.")
    return 0


def evaluate_real(path: str, target_rate: float = 0.01, ae_epochs: int = 300,
                  seed: int = 0, top: int = 12) -> int:
    data = replay_reader.read_replay(path)
    frame = extract_features(data)
    if len(frame) == 0:
        print("no bars in the file -> nothing to evaluate.")
        return 0
    ev = LadderEval(frame, target_rate, ae_epochs, seed)
    s = ev.split
    base_flag, ae_flag = ev.test_flags()

    print("=== leakage-free calibrated eval (REAL data, NO labels) ===")
    print(f"  rows: fit={len(s.fit)} calibrate={len(s.calibrate)} test={len(s.test)} "
          f"(no leakage: asserted)")
    print(f"  calibration target flag-rate: {target_rate:.1%}\n")
    for name, flag, score in (("robust-z baseline", base_flag, ev.base_scores[s.test]),
                              ("autoencoder", ae_flag, ev.ae_scores[s.test])):
        print(f"  {name}: flagged {int(flag.sum())}/{len(flag)} test bars "
              f"({np.mean(flag):.2%})")
        order = np.argsort(score)[::-1][:top]
        shown = [i for i in order if flag[i]][:5]
        for i in shown:
            row = s.test[i]
            print(f"      {frame.symbol[row]:<6} {_fmt_ts(frame.start_ns[row])}  "
                  f"score={score[i]:.2f}")
        print()
    print("  NOTE: real data has NO ground-truth labels. These are CANDIDATES, not\n"
          "  validated detections -- flag-rate + inspection only, never a precision\n"
          "  number. On real bars the tails are typically session-boundary / regime\n"
          "  effects, not necessarily faults.")
    return 0


def main() -> int:
    args = sys.argv[1:]
    if args and not args[0].startswith("-"):
        return evaluate_real(args[0])
    return evaluate_synthetic()


if __name__ == "__main__":
    raise SystemExit(main())
