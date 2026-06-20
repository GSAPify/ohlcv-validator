"""Reader for the ohlcv-validator binary replay format.

This is the bridge between the C++ data plane and the Python ML layer. The C++
side (gen_dataset, the live capture writer) writes a flat, fixed-stride binary
file; this module reads it straight into NumPy structured arrays with zero
parsing beyond a header unpack. The same file the replay benchmark validates is
the file the ML layer learns from -- one format, no second serialization path.

On-disk layout (must match src/replay/binary_format.h and src/model/wire.h,
which carry static_asserts pinning these exact sizes):

    FileHeader   16 bytes : u32 magic, u32 version, u64 record_count
    WireRecord   88 bytes : u8 type, 7 pad, then an 80-byte union body

The 88-byte stride is set by WireBar (the largest variant). The dtypes below
give each record type itemsize=88 with explicit field offsets, so a single
np.frombuffer over the body region yields one structured array per type; we then
mask by the leading type byte. Field offsets are transcribed from the C++ structs
and guarded by test_replay_reader.py's round-trip against a real gen_dataset file.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

import numpy as np

# Header: '<' little-endian, u32 magic, u32 version, u64 record_count.
_HEADER_FMT = "<IIQ"
HEADER_SIZE = struct.calcsize(_HEADER_FMT)  # 16
RECORD_STRIDE = 88

MAGIC = 0x564C484F  # 'OHLV' little-endian
VERSION = 2

# RecordType discriminator (src/replay/binary_format.h).
TYPE_TRADE = 0
TYPE_BAR = 1
TYPE_QUOTE = 2

# All three dtypes have itemsize == RECORD_STRIDE: each is a *view* over the same
# 88-byte record, interpreting the union body according to the type byte.
DT_TRADE = np.dtype(
    {
        "names": ["type", "symbol", "ts_ns", "seq", "trade_id", "price", "size",
                  "exchange", "tape"],
        "formats": ["u1", "S8", "u8", "u8", "u8", "f8", "u8", "S1", "S1"],
        "offsets": [0, 8, 16, 24, 32, 40, 48, 56, 57],
        "itemsize": RECORD_STRIDE,
    }
)

DT_BAR = np.dtype(
    {
        "names": ["type", "symbol", "start_ns", "seq", "open", "high", "low",
                  "close", "vwap", "volume", "trade_count"],
        "formats": ["u1", "S8", "u8", "u8", "f8", "f8", "f8", "f8", "f8", "u8",
                    "u8"],
        "offsets": [0, 8, 16, 24, 32, 40, 48, 56, 64, 72, 80],
        "itemsize": RECORD_STRIDE,
    }
)

DT_QUOTE = np.dtype(
    {
        "names": ["type", "symbol", "ts_ns", "seq", "bid_price", "ask_price",
                  "bid_size", "ask_size", "bid_exchange", "ask_exchange", "tape"],
        "formats": ["u1", "S8", "u8", "u8", "f8", "f8", "u8", "u8", "S1", "S1",
                    "S1"],
        "offsets": [0, 8, 16, 24, 32, 40, 48, 56, 64, 65, 66],
        "itemsize": RECORD_STRIDE,
    }
)


@dataclass
class ReplayData:
    """The decoded replay file, split by record type.

    Each array is a NumPy structured array; access fields by name, e.g.
    ``data.trades["price"]``. ``symbol`` is fixed 8-byte NUL-padded -- use
    :func:`symbols` to get clean str tickers.
    """

    trades: np.ndarray
    bars: np.ndarray
    quotes: np.ndarray
    record_count: int

    def __repr__(self) -> str:  # pragma: no cover - convenience only
        return (
            f"ReplayData(records={self.record_count}, "
            f"trades={len(self.trades)}, bars={len(self.bars)}, "
            f"quotes={len(self.quotes)})"
        )


def symbols(arr: np.ndarray) -> np.ndarray:
    """Decode the fixed 8-byte ``symbol`` field of a record array to str tickers."""
    # 'S8' holds NUL-padded bytes; np.char.decode + the implicit NUL trim gives
    # back the ticker. astype('U') keeps it a plain unicode array.
    return np.char.decode(arr["symbol"], "ascii").astype("U8")


def read_replay(path: str) -> ReplayData:
    """Read a binary replay file into per-type NumPy structured arrays.

    Raises ValueError if the magic, version, or file size don't match the format
    -- a wrong stride is the classic silent reader bug, so the size check is not
    optional.
    """
    with open(path, "rb") as f:
        header = f.read(HEADER_SIZE)
        if len(header) < HEADER_SIZE:
            raise ValueError(f"{path}: truncated header ({len(header)} bytes)")
        magic, version, count = struct.unpack(_HEADER_FMT, header)
        body = f.read()

    if magic != MAGIC:
        raise ValueError(f"{path}: bad magic 0x{magic:08X} (want 0x{MAGIC:08X})")
    if version != VERSION:
        raise ValueError(f"{path}: version {version} (reader supports {VERSION})")

    expected = count * RECORD_STRIDE
    if len(body) != expected:
        raise ValueError(
            f"{path}: body is {len(body)} bytes, expected "
            f"{expected} ({count} records x {RECORD_STRIDE}); "
            "stride mismatch -- C++ wire layout and Python dtypes disagree"
        )

    # One typed view over the whole body per record type, then mask by the
    # leading type byte (offset 0 is identical across all three dtypes).
    types = np.frombuffer(body, dtype=np.uint8).reshape(count, RECORD_STRIDE)[:, 0]
    trades = np.frombuffer(body, dtype=DT_TRADE)[types == TYPE_TRADE]
    bars = np.frombuffer(body, dtype=DT_BAR)[types == TYPE_BAR]
    quotes = np.frombuffer(body, dtype=DT_QUOTE)[types == TYPE_QUOTE]

    return ReplayData(trades=trades, bars=bars, quotes=quotes, record_count=count)
