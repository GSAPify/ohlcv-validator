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
| 3    | autoencoder reconstruction-error   | **here** | yes (trade/bar features) |
| 4    | spatial NN (Sirignano)             | n/a    | **no — needs L3 depth** |

### Rung 3 — and the eval that makes it honest

The autoencoder (`autoencoder.py`) only earns its complexity if it catches
anomalies the lower rungs **can't**. Testing it on `gen_dataset`'s injected
defects would be circular — the rule validator and the baseline already catch
those. So the eval (`test_autoencoder.py`, demo `eval_ladder.py`) builds a
**rule-invisible** anomaly: a return↔volume *correlation break* (small price move
on median volume) where every bar passes every validator rule and no single
feature is an outlier. The whole signal lives in the *joint* structure.

```
rung 0  C++ validator     : SILENT (rule-clean — verified through replay_bench)
rung 2  robust-z baseline : flagged 0/15 anomaly bars, AUC 0.21  -> BLIND
rung 3  autoencoder       : AUC 0.99, anomaly recon-error 8.3x normal -> CATCHES IT
```

A univariate baseline scores each feature independently — it *structurally cannot*
see "volume and return decoupled." The autoencoder, trained on the joint
distribution of normal bars, reconstructs the broken combination poorly. That gap
(AUC 0.99 vs 0.21) is the entire justification for the rung. **This is a mechanism
demo on a constructed anomaly — not a field-performance claim. Real evaluation
needs live-captured data.**

`replay_writer.py` (the inverse of the reader) is what lets Python emit the
rule-clean stream so the *real C++ validator* confirms rule-invisibility, rather
than us asserting it.

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
| `replay_writer.py` | the inverse — Python emits a valid replay file the C++ validator reads (used to prove rule-invisibility) |
| `features.py` | per-symbol bar features: returns, range, VWAP deviation, volume, intensity, realized vol, volume z |
| `baseline.py` | rung 2 — classical robust-z / MAD anomaly baseline, the rung to beat |
| `synth.py` | generates a rule-clean stream carrying a return↔volume correlation break (the rung-3 eval substrate) |
| `autoencoder.py` | rung 3 — MLP autoencoder, reconstruction-error anomaly score (torch) |
| `detect.py` | CLI: file → features → baseline scores → top anomalies |
| `eval_ladder.py` | CLI: the rung-0/2/3 head-to-head on the rule-invisible anomaly |
| `test_replay_reader.py` | round-trip test: C++ writes, Python reads, fields match |
| `test_autoencoder.py` | rung-3 eval: validator silent + baseline blind + autoencoder catches it |

## Run it

```bash
pip install -r ml/requirements.txt   # numpy; torch only needed for rung 3

# all tests (needs the C++ gen_dataset + replay_bench built: cmake --build build)
pytest ml/ -v

# the ladder head-to-head: rung 3 catches what the rules + baseline miss
python3 ml/eval_ladder.py

# score a synthetic file with the baseline (plumbing demo — see the guardrail)
./build/gen_dataset data/replay.bin 100000
python3 ml/detect.py data/replay.bin

# the real run, once a live capture exists (Monday, RTH):
# OHLCV_CAPTURE=cap.bin ./build/alpaca_ingest AAPL MSFT NVDA TSLA
# python3 ml/detect.py cap.bin
```

The features deliberately lean on **trades + bars**, not quotes/spread — free
historical quotes return 0 and live quotes are still unconfirmed. When live
quotes are verified, spread/imbalance features get their own rung.
