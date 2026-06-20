"""Baseline policies for the bar-trading sandbox.

These are NOT learned agents -- they're the reference behaviours a trained policy
would have to beat, and the instruments the mechanics tests use. A policy is just
a callable: observation -> action in {-1, 0, +1}.

On i.i.d. synthetic returns none of these has an edge; that's the point. If a
momentum baseline ever shows positive PnL on the synthetic data, that's a lookahead
leak in the env, not a discovery. (See the clairvoyant policy in test_rl_env.py
for the other half of the leakage proof.)
"""

from __future__ import annotations

import numpy as np

from features import FEATURE_NAMES


def always_flat(obs: np.ndarray) -> int:
    """Never take a position -> zero PnL, zero cost. The trivial floor."""
    del obs
    return 0


class RandomPolicy:
    """Uniform random action. Seeded for reproducibility."""

    def __init__(self, seed: int = 0):
        self._rng = np.random.default_rng(seed)

    def __call__(self, obs: np.ndarray) -> int:
        del obs
        return int(self._rng.choice((-1, 0, 1)))


class MomentumPolicy:
    """Go with the last bar's return: long if it was up, short if down.

    Reads the log_return feature by index. On i.i.d. returns this has no edge --
    it's here precisely as the leakage canary.
    """

    def __init__(self, return_idx: int | None = None):
        self._idx = (
            FEATURE_NAMES.index("log_return") if return_idx is None else return_idx
        )

    def __call__(self, obs: np.ndarray) -> int:
        r = obs[self._idx]
        return int(np.sign(r))


def run_episode(env, policy) -> dict:
    """Run one full episode of a policy against an env; return PnL + trade count."""
    obs, _ = env.reset()
    cum = 0.0
    trades = 0
    prev = 0
    done = False
    while not done:
        action = policy(obs)
        obs, reward, terminated, truncated, _ = env.step(action)
        cum += reward
        if action != prev:
            trades += 1
        prev = action
        done = terminated or truncated
    return {"cum_reward": cum, "trades": trades}
