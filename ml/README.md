# ML data plane

The bridge from the C++ validator to a Python ML layer. The C++ side writes a
flat binary replay file; this layer reads that *same* file into NumPy, builds
features, and runs an anomaly baseline. One serialization format, one source of
truth — the file the replay benchmark validates is the file the ML layer learns
from.

```
   C++ data plane (fast)                 Python ML layer (offline, slower)
 ┌───────────────────────┐            ┌──────────────────────────────────┐
 │ gen_dataset / live     │  .bin     │ replay_reader → features →         │
 │ capture writer         │ ───────►  │ baseline (→ future: autoencoder)   │
 │ 88-byte WireRecords    │  (mmap-   │ NumPy structured arrays, zero-copy │
 └───────────────────────┘   able)   └──────────────────────────────────┘
   one binary format, pinned by static_asserts in src/replay/binary_format.h
```

## Why this layer exists, and what's deliberately *not* here

The motivating paper was Sirignano, *Deep Learning for Limit Order Books* (2016).
Its "spatial neural network" is genuinely good — but it needs **Level III order-
book depth** (the first 100 nonzero price levels, every order add/cancel/execute,
nanosecond book state). We capture **trade prints + 1-minute OHLCV bars + top
quotes** (and quotes are 0 on the free historical feed). The spatial NN's entire
reason to exist is exploiting structure *across depth levels we don't have*, so
it is off the table for our data — building it would be wiring a model to inputs
we can't feed.

What we *keep* from the paper is its **methodology**: a ladder of models, each
rung justified only by beating the one below (naive → logistic → NN → spatial).
We build the same ladder, shaped to our data:

| Rung | Model                              | Status | Fits our data? |
|------|------------------------------------|--------|----------------|
| 1    | data bridge + feature layer        | **here** | — (it's the spine) |
| 2    | classical baseline (robust z/MAD)  | **here** | yes |
| 3    | autoencoder reconstruction-error   | future | yes (trade/bar features) |
| 4    | spatial NN (Sirignano)             | n/a    | **no — needs L3 depth** |

Reinforcement learning is further up the same ladder, not skipped: RL is for
*sequential decision-making with rewards* (order execution / market-making —
which is the only thing the paper cites RL for), **not** anomaly detection.
Conflating the two is a category error. RL also needs exactly the feature stream
built here, so it comes after, not instead.

## ⚠️ The honesty guardrail

On **synthetic** `gen_dataset` output, the injected defects *are* the anomalies,
and the rule validator already flags them deterministically by construction. The
baseline "catching" them proves **the pipeline round-trips** — it is **not** an
anomaly-detection result. The interesting question — *can a statistical model
catch regime/structure anomalies the rule validator misses?* — only has meaning
on **real captured market data**. Until then, treat every number here as plumbing.

## Layout

| File | What it does |
|------|--------------|
| `replay_reader.py` | reads the binary replay format into NumPy structured arrays (byte-precise, mirrors `src/model/wire.h`) |
| `features.py` | per-symbol bar features: returns, range, VWAP deviation, volume, intensity, realized vol, volume z |
| `baseline.py` | classical robust-z / MAD anomaly baseline — the rung to beat |
| `detect.py` | CLI: file → features → scores → top anomalies |
| `test_replay_reader.py` | round-trip test: C++ writes, Python reads, fields match |

## Run it

```bash
pip install -r ml/requirements.txt

# round-trip test (needs the C++ gen_dataset built: cmake --build build)
pytest ml/ -v

# score a synthetic file (plumbing demo — see the guardrail above)
./build/gen_dataset data/replay.bin 100000
python3 ml/detect.py data/replay.bin

# the real run, once a live capture exists (Monday, RTH):
# OHLCV_CAPTURE=cap.bin ./build/alpaca_ingest AAPL MSFT NVDA TSLA
# python3 ml/detect.py cap.bin
```

The features deliberately lean on **trades + bars**, not quotes/spread — free
historical quotes return 0 and live quotes are still unconfirmed. When live
quotes are verified, spread/imbalance features get their own rung.
