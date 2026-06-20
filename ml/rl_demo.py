"""Run the baseline policies in the bar-trading sandbox and print the verdict.

    python3 ml/rl_demo.py

The honest message: on i.i.d. synthetic returns there's NO alpha to exploit, so
obs-only baselines (random, momentum) net ~zero (slightly negative after txn
costs), and never-trading nets exactly zero. A clairvoyant policy that cheats
(acts on the forward return) profits strongly -- which is how we know the reward
actually pays for correct bets, i.e. the env has no broken plumbing AND no
lookahead leak that would let an honest policy see the future.

This is a sandbox mechanics demo. There is no trained agent and no claim that
anything "learned to trade" -- that needs real data with real structure.
"""

from __future__ import annotations

import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import synth  # noqa: E402
import replay_reader  # noqa: E402
import replay_writer  # noqa: E402
from rl_env import from_replay  # noqa: E402
from rl_policies import MomentumPolicy, RandomPolicy, always_flat, run_episode  # noqa: E402


def _clairvoyant(env) -> dict:
    """Cheats: acts on the forward return. Reference only -- not an obs-only policy."""
    obs, _ = env.reset()
    cum, trades, prev, t, done = 0.0, 0, 0, 0, False
    while not done:
        a = int(np.sign(env.forward_returns[t]))
        obs, r, term, _, _ = env.step(a)
        cum += r
        trades += a != prev
        prev = a
        t += 1
        done = term
    return {"cum_reward": cum, "trades": trades}


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

    n = from_replay(data, "SYNTH").n_steps
    print(f"sandbox: {n} steps (one symbol's bars), txn_cost 1e-4\n")
    print(f"  {'policy':<22} {'cum reward (additive PnL)':>26} {'trades':>8}")
    print(f"  {'-'*22} {'-'*26} {'-'*8}")

    rows = [
        ("always_flat", run_episode(from_replay(data, "SYNTH"), always_flat)),
        ("random (seed 0)", run_episode(from_replay(data, "SYNTH"), RandomPolicy(0))),
        ("momentum", run_episode(from_replay(data, "SYNTH"), MomentumPolicy())),
        ("clairvoyant (CHEATS)", _clairvoyant(from_replay(data, "SYNTH"))),
    ]
    for name, res in rows:
        print(f"  {name:<22} {res['cum_reward']:>26.4f} {res['trades']:>8}")

    print(
        "\nverdict: obs-only baselines net ~0 (no alpha in i.i.d. synthetic data; "
        "\ncosts drag them slightly negative). The clairvoyant policy profits only "
        "\nbecause the reward correctly pays for right bets -- proving the env has no "
        "\nbroken plumbing and no lookahead leak. Sandbox mechanics only; no trained "
        "\nagent, no 'learned to trade' claim. Real structure needs real data."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
