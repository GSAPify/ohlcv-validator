"""Writer for the binary replay format -- the symmetric inverse of replay_reader.

Lets Python emit a valid replay file that the C++ validator/benchmark reads back
natively. The point isn't a second data source (gen_dataset owns synthetic data);
it's that Python can now construct a *specific* stream -- e.g. a rule-clean stream
carrying a statistical anomaly -- and feed it through the real C++ validator to
prove a property (here: "this anomaly is invisible to the rules"). Round-tripping
through the actual validator beats asserting rule-cleanliness in a comment.

Scope: bars only, for now. A bars-only stream skips the trade->bar reconstruction
check (the validator only reconciles a bar when its symbol has accumulated
trades), so a bar that satisfies the per-record invariants -- OHLC ordering,
vwap in band, positivity, volume/trade_count both nonzero -- passes every rule.
Trades/quotes can be added the day a caller needs them.
"""

from __future__ import annotations

import struct

import numpy as np

from replay_reader import DT_BAR, MAGIC, RECORD_STRIDE, TYPE_BAR, VERSION


def make_bars(
    symbol: str,
    start_ns: np.ndarray,
    open_: np.ndarray,
    high: np.ndarray,
    low: np.ndarray,
    close: np.ndarray,
    vwap: np.ndarray,
    volume: np.ndarray,
    trade_count: np.ndarray,
    seq_start: int = 1,
) -> np.ndarray:
    """Build a DT_BAR structured array for one symbol.

    seq is assigned monotonically from seq_start; start_ns must be supplied
    already non-decreasing (the validator flags timestamp regressions).
    """
    n = len(start_ns)
    rec = np.zeros(n, dtype=DT_BAR)
    rec["type"] = TYPE_BAR
    rec["symbol"] = np.asarray(symbol.encode("ascii")[:8], dtype="S8")
    rec["start_ns"] = start_ns
    rec["seq"] = np.arange(seq_start, seq_start + n, dtype=np.uint64)
    rec["open"] = open_
    rec["high"] = high
    rec["low"] = low
    rec["close"] = close
    rec["vwap"] = vwap
    rec["volume"] = volume
    rec["trade_count"] = trade_count
    return rec


def write_replay(path: str, records: np.ndarray) -> None:
    """Write a structured record array (e.g. from make_bars) to a replay file.

    The array's itemsize must equal the on-disk stride; this is the writer-side
    mirror of the reader's size check.
    """
    if records.dtype.itemsize != RECORD_STRIDE:
        raise ValueError(
            f"record dtype itemsize {records.dtype.itemsize} != on-disk stride "
            f"{RECORD_STRIDE}"
        )
    with open(path, "wb") as f:
        f.write(struct.pack("<IIQ", MAGIC, VERSION, len(records)))
        f.write(records.tobytes())
