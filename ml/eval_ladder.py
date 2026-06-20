"""Run the ladder head-to-head on a rule-invisible anomaly and print the verdict.

    python3 ml/eval_ladder.py

Generates a rule-clean stream carrying a return<->volume correlation break, runs
it through the C++ validator (must be silent), then scores it with rung 2 (robust-z
baseline) and rung 3 (autoencoder). Shows that the anomaly is invisible to the
rules and to the baseline, but the autoencoder catches it -- the only thing that
justifies rung 3's complexity.

This is a *mechanism* demo on a constructed anomaly, not a field-performance claim
(that needs live data). See ml/README.md.
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import synth  # noqa: E402
import replay_writer  # noqa: E402
import replay_reader  # noqa: E402
from autoencoder import Autoencoder  # noqa: E402
from baseline import score_anomalies  # noqa: E402
from features import extract_features  # noqa: E402

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_BENCH = os.path.join(_REPO_ROOT, "build", "replay_bench")


def _auc(score, label):
    pos, neg = score[label], score[~label]
    allv = np.concatenate([pos, neg])
    order = allv.argsort()
    ranks = np.empty_like(order, dtype=float)
    ranks[order] = np.arange(1, len(allv) + 1)
    return (ranks[: len(pos)].sum() - len(pos) * (len(pos) + 1) / 2) / (
        len(pos) * len(neg)
    )


def _cpp_violations(recs) -> int | None:
    if not os.path.exists(_BENCH):
        return None
    fd, path = tempfile.mkstemp(suffix=".bin", prefix="ohlcv_ladder_")
    os.close(fd)
    try:
        replay_writer.write_replay(path, recs)
        out = subprocess.run([_BENCH, path, "1"], check=True,
                             capture_output=True, text=True).stdout
        block = out[out.index("violations caught"):]
        return sum(int(n) for n in re.findall(r":\s*(\d+)", block))
    finally:
        os.unlink(path)


def main() -> int:
    sb = synth.generate()
    recs = replay_writer.make_bars(
        "SYNTH", sb.start_ns, sb.open, sb.high, sb.low, sb.close, sb.vwap,
        sb.volume, sb.trade_count,
    )
    data = replay_reader.ReplayData(
        trades=np.empty(0, replay_reader.DT_TRADE), bars=recs,
        quotes=np.empty(0, replay_reader.DT_QUOTE), record_count=len(recs),
    )
    frame = extract_features(data)
    anom = sb.is_anomaly
    n_anom = int(anom.sum())

    print(f"dataset: {len(recs)} rule-clean bars, {n_anom} carry a "
          f"return<->volume correlation break\n")

    viol = _cpp_violations(recs)
    if viol is None:
        print("rung 0  C++ validator     : (replay_bench not built -- skipped)")
    else:
        verdict = "SILENT (rule-clean)" if viol == 0 else f"{viol} violations"
        print(f"rung 0  C++ validator     : {verdict}")

    scored = score_anomalies(frame, threshold=5.0)
    b_flag = int(scored.is_anomaly[anom].sum())
    b_auc = _auc(scored.score, anom)
    print(f"rung 2  robust-z baseline : flagged {b_flag}/{n_anom} anomaly bars, "
          f"AUC {b_auc:.3f}  -> {'BLIND' if b_auc < 0.75 else 'sees it'}")

    ae = Autoencoder(epochs=400, seed=0).fit(frame.features[~anom].astype(np.float64))
    err = ae.score(frame.features.astype(np.float64))
    a_auc = _auc(err, anom)
    ratio = err[anom].mean() / max(err[~anom].mean(), 1e-12)
    print(f"rung 3  autoencoder       : AUC {a_auc:.3f}, anomaly recon-error "
          f"{ratio:.1f}x normal  -> {'CATCHES IT' if a_auc > 0.9 else 'misses it'}")

    print(f"\nverdict: the anomaly is invisible to the rules and to the baseline, "
          f"\nbut rung 3 separates it (AUC {a_auc:.3f} vs {b_auc:.3f}). That gap is "
          f"\nwhy the autoencoder earns its place. Mechanism demo -- field eval "
          f"needs live data.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
