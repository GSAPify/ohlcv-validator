"""Rung 3 eval: does the autoencoder catch what the rules AND the baseline miss?

This is the test that makes building rung 3 honest rather than circular. It does
NOT test the autoencoder on gen_dataset's injected defects (the rule validator and
the baseline already catch those -- "beating" them there would be a tautology).
Instead it builds a RULE-INVISIBLE anomaly -- a return<->volume correlation break
where every bar is individually valid and no single feature is an outlier -- and
asserts three things:

  1. the C++ validator stays silent on it   (it's genuinely rule-clean),
  2. the univariate robust-z baseline is blind (no single-feature signal),
  3. the autoencoder separates it cleanly    (the joint model sees the break).

Together: rung 3 catches an anomaly invisible to both lower rungs -- which is the
only thing that justifies its complexity.

HONESTY NOTE: this demonstrates the *mechanism* on a constructed anomaly. It is
not a claim about field performance -- that needs live-captured data (Monday).

Run: pytest ml/test_autoencoder.py  (needs torch + the built C++ replay_bench)
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile

import numpy as np
import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import synth  # noqa: E402
import replay_writer  # noqa: E402
import replay_reader  # noqa: E402
from autoencoder import Autoencoder  # noqa: E402
from baseline import score_anomalies  # noqa: E402
from features import extract_features  # noqa: E402

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_BENCH = os.path.join(_REPO_ROOT, "build", "replay_bench")


def _auc(score: np.ndarray, label: np.ndarray) -> float:
    """ROC AUC via the Mann-Whitney statistic = P(score[pos] > score[neg])."""
    pos, neg = score[label], score[~label]
    allv = np.concatenate([pos, neg])
    order = allv.argsort()
    ranks = np.empty_like(order, dtype=float)
    ranks[order] = np.arange(1, len(allv) + 1)
    return (ranks[: len(pos)].sum() - len(pos) * (len(pos) + 1) / 2) / (
        len(pos) * len(neg)
    )


@pytest.fixture(scope="module")
def synthetic():
    """Generate the rule-clean / correlation-break dataset once for all tests."""
    sb = synth.generate()
    recs = replay_writer.make_bars(
        "SYNTH", sb.start_ns, sb.open, sb.high, sb.low, sb.close, sb.vwap,
        sb.volume, sb.trade_count,
    )
    data = replay_reader.ReplayData(
        trades=np.empty(0, replay_reader.DT_TRADE),
        bars=recs,
        quotes=np.empty(0, replay_reader.DT_QUOTE),
        record_count=len(recs),
    )
    frame = extract_features(data)
    return sb, recs, frame


def test_anomaly_is_rule_invisible(synthetic):
    """The C++ validator must report zero violations -- proving rule-cleanliness
    through the real validator, not a Python assertion about it."""
    if not os.path.exists(_BENCH):
        pytest.skip(f"replay_bench not built at {_BENCH}")
    _, recs, _ = synthetic
    fd, path = tempfile.mkstemp(suffix=".bin", prefix="ohlcv_ae_")
    os.close(fd)
    try:
        replay_writer.write_replay(path, recs)
        out = subprocess.run([_BENCH, path, "1"], check=True,
                             capture_output=True, text=True).stdout
        block = out[out.index("violations caught"):]
        counts = [int(n) for n in re.findall(r":\s*(\d+)", block)]
        assert counts, "could not parse the violations block"
        assert sum(counts) == 0, f"validator flagged the 'clean' stream: {block}"
    finally:
        os.unlink(path)


def test_baseline_is_blind(synthetic):
    """The univariate robust-z baseline must NOT see the correlation break: no
    single feature is an outlier, so it flags ~no anomaly bars and ranks them no
    better than chance."""
    sb, _, frame = synthetic
    anom = sb.is_anomaly
    scored = score_anomalies(frame, threshold=5.0)
    assert int(scored.is_anomaly[anom].sum()) == 0, \
        "baseline flagged anomaly bars -- the anomaly isn't single-feature-invisible"
    # Around chance (well below a competent detector). Not asserting ~0.5 exactly
    # since a few features dip slightly, but it must be a non-detector.
    assert _auc(scored.score, anom) < 0.75


def test_autoencoder_beats_both_lower_rungs(synthetic):
    """The autoencoder, trained on normal bars only, must separate the anomaly
    cleanly -- and decisively beat the baseline that's blind to it."""
    sb, _, frame = synthetic
    anom = sb.is_anomaly
    X = frame.features.astype(np.float64)

    ae = Autoencoder(epochs=400, seed=0).fit(X[~anom])  # train on normal only
    err = ae.score(X)

    ae_auc = _auc(err, anom)
    base_auc = _auc(score_anomalies(frame, threshold=5.0).score, anom)

    assert ae_auc > 0.90, f"autoencoder failed to separate the anomaly (AUC {ae_auc:.3f})"
    assert err[anom].mean() > 3.0 * err[~anom].mean(), \
        "anomaly reconstruction error not clearly elevated"
    # The headline: rung 3 beats rung 2 by a wide margin on this anomaly.
    assert ae_auc - base_auc > 0.20, \
        f"autoencoder ({ae_auc:.3f}) did not clearly beat baseline ({base_auc:.3f})"


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
