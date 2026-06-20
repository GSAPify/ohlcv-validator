"""Round-trip tests: C++ writes the replay file, Python reads it back.

This is the test that actually matters for the data plane -- it proves the Python
dtypes' byte offsets agree with the C++ wire structs. A wrong stride or offset is
a silent corruption bug, so we generate a real file with the built gen_dataset
binary and assert both structure (counts add up) and content (fields are sane).

Run: pytest ml/  (or python3 ml/test_replay_reader.py)
Requires the C++ gen_dataset to be built (cmake --build build).
"""

from __future__ import annotations

import os
import struct
import subprocess
import sys
import tempfile

import numpy as np
import pytest

# Make the ml/ modules importable whether invoked via pytest from the repo root
# or run directly.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from baseline import score_anomalies  # noqa: E402
from features import extract_features  # noqa: E402
from replay_reader import (  # noqa: E402
    HEADER_SIZE,
    MAGIC,
    RECORD_STRIDE,
    VERSION,
    read_replay,
    symbols,
)

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_GEN = os.path.join(_REPO_ROOT, "build", "gen_dataset")


@pytest.fixture(scope="module")
def replay_file():
    """Generate a real replay file with the built C++ binary."""
    if not os.path.exists(_GEN):
        pytest.skip(f"gen_dataset not built at {_GEN} -- run cmake --build build")
    fd, path = tempfile.mkstemp(suffix=".bin", prefix="ohlcv_ml_test_")
    os.close(fd)
    subprocess.run([_GEN, path, "5000"], check=True, capture_output=True)
    yield path
    os.unlink(path)


def test_constants_match_format():
    assert HEADER_SIZE == 16
    assert RECORD_STRIDE == 88
    assert MAGIC == 0x564C484F
    assert VERSION == 2


def test_header_and_counts_consistent(replay_file):
    data = read_replay(replay_file)
    # Every record is exactly one of the three types -> the split must be a
    # partition of record_count. This is the core stride/offset proof: if the
    # type byte were read at the wrong place, this wouldn't add up.
    total = len(data.trades) + len(data.bars) + len(data.quotes)
    assert total == data.record_count
    # gen_dataset emits all three kinds.
    assert len(data.trades) > 0
    assert len(data.bars) > 0


def test_file_size_matches_stride(replay_file):
    size = os.path.getsize(replay_file)
    with open(replay_file, "rb") as f:
        _, _, count = struct.unpack("<IIQ", f.read(HEADER_SIZE))
    assert size == HEADER_SIZE + count * RECORD_STRIDE


def test_trade_fields_sane(replay_file):
    data = read_replay(replay_file)
    t = data.trades
    # Symbols decode to non-empty ASCII tickers.
    syms = symbols(t)
    assert all(len(s) > 0 for s in np.unique(syms))
    # The vast majority of trades carry a positive price/size (gen_dataset injects
    # a small number of negative-price/zero-size defects on purpose).
    assert np.mean(t["price"] > 0) > 0.9
    assert np.mean(t["size"] > 0) > 0.9
    # Timestamps are plausible nanoseconds-since-epoch (post-2000, pre-2100).
    assert t["ts_ns"].min() > 946_684_800_000_000_000


def test_bar_fields_sane(replay_file):
    data = read_replay(replay_file)
    b = data.bars
    # OHLC ordering holds for the overwhelming majority (defects aside).
    ok = (b["high"] >= b["low"]) & (b["high"] >= b["open"]) & (b["low"] <= b["close"])
    assert np.mean(ok) > 0.8
    assert np.all(b["trade_count"] >= 0)


def test_bad_magic_rejected():
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        f.write(struct.pack("<IIQ", 0xDEADBEEF, VERSION, 0))
        path = f.name
    try:
        with pytest.raises(ValueError, match="bad magic"):
            read_replay(path)
    finally:
        os.unlink(path)


def test_stride_mismatch_rejected():
    # A header claiming 10 records but a body that isn't 10*88 bytes must raise,
    # not silently read garbage.
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        f.write(struct.pack("<IIQ", MAGIC, VERSION, 10))
        f.write(b"\x00" * 50)  # nowhere near 10 * 88
        path = f.name
    try:
        with pytest.raises(ValueError, match="stride mismatch"):
            read_replay(path)
    finally:
        os.unlink(path)


def test_features_and_baseline_smoke(replay_file):
    """End-to-end: file -> features -> baseline scores, shapes line up.

    This asserts the pipeline runs and is internally consistent. It deliberately
    does NOT assert anything about anomaly-detection quality -- on synthetic data
    the injected defects are the anomalies and the rule validator already flags
    them, so any "catch" here is plumbing, not a result.
    """
    data = read_replay(replay_file)
    frame = extract_features(data)
    assert len(frame) == len(data.bars)
    assert frame.features.shape == (len(data.bars), len(frame.names))
    assert np.all(np.isfinite(frame.features))

    scored = score_anomalies(frame)
    assert len(scored.score) == len(frame)
    assert scored.is_anomaly.dtype == bool
    assert np.all(np.isfinite(scored.score))


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
