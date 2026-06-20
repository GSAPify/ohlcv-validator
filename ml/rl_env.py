"""A position-taking RL environment over the bar/feature stream -- the sandbox.

Gymnasium-style API (reset / step), but NO gymnasium dependency: a market env's
contract is small enough that mirroring the conventions is clearer than pulling a
framework. An agent could later be wrapped to gymnasium without changing this.

    observation[t] : the feature vector for bar t (the same features rungs 1-3 use)
    action         : -1 short, 0 flat, +1 long  (position to hold over bar t -> t+1)
    reward[t]      : position * forward_return[t]  -  txn_cost * |position - prev|
    episode        : one symbol's bars, in time order; ends after the last bar that
                     has a forward return.

THE ONE INVARIANT THAT MATTERS -- NO LOOKAHEAD: observation[t] is strictly the
information available at decision time (bar t and trailing), and forward_return[t]
is the return realized *after* that decision, close[t+1]/close[t] - 1. If those
ever line up (forward_return[t] leaking into observation[t]), a trivial policy
shows fake alpha. The guarantee comes from the alignment test (forward_return
checked directly against source closes) plus features being trailing by
construction; test_rl_env.py's clairvoyant-wins / obs-only-can't pair corroborates
it (the momentum baseline specifically rules out log_return being a forward
return).

Scope: this is a *sandbox* -- mechanics only. On synthetic data there is no alpha
to learn, so no training loop and no "agent learned to trade" claim lives here.

Honesty notes:
- The reward uses SIMPLE returns and sums them: cumulative reward is additive PnL
  in return units, NOT a compounded portfolio return.
- No terminal liquidation cost: the final position is left marked, not charged a
  closing trade. A deliberate sandbox simplification.
"""

from __future__ import annotations

import numpy as np

ACTIONS = (-1, 0, 1)  # short, flat, long


class BarTradingEnv:
    def __init__(
        self,
        observations: np.ndarray,
        forward_returns: np.ndarray,
        txn_cost: float = 1e-4,
    ):
        if len(observations) != len(forward_returns):
            raise ValueError(
                f"observations ({len(observations)}) and forward_returns "
                f"({len(forward_returns)}) must align row-for-row"
            )
        self.observations = np.asarray(observations, dtype=np.float64)
        self.forward_returns = np.asarray(forward_returns, dtype=np.float64)
        self.txn_cost = float(txn_cost)
        self.n_steps = len(self.forward_returns)
        self.n_features = self.observations.shape[1] if self.observations.ndim == 2 else 0
        self._t = 0
        self._position = 0
        self._cum_reward = 0.0

    def reset(self, *, seed: int | None = None):
        del seed  # stateless reset; arg kept for gymnasium-style call sites
        self._t = 0
        self._position = 0
        self._cum_reward = 0.0
        return self.observations[0], self._info()

    def step(self, action: int):
        if action not in ACTIONS:
            raise ValueError(f"action {action!r} not in {ACTIONS}")
        if self._t >= self.n_steps:
            raise RuntimeError("step() called on a finished episode; call reset()")

        reward = action * self.forward_returns[self._t] - self.txn_cost * abs(
            action - self._position
        )
        self._position = action
        self._cum_reward += reward
        self._t += 1

        terminated = self._t >= self.n_steps
        # On termination there's no next decision; hand back the last observation.
        obs = self.observations[min(self._t, self.n_steps - 1)]
        return obs, reward, terminated, False, self._info()

    def _info(self) -> dict:
        return {
            "t": self._t,
            "position": self._position,
            "cum_reward": self._cum_reward,
        }


def from_replay(data, symbol: str | None = None, txn_cost: float = 1e-4) -> BarTradingEnv:
    """Build an env for ONE symbol from a ReplayData.

    Forward returns are computed strictly within the chosen symbol -- never across
    a symbol boundary (close of one symbol's last bar -> another's first). If no
    symbol is given, the one with the most bars is used.
    """
    # Imported here to keep the import graph flat and avoid a cycle at module load.
    from features import extract_features
    from replay_reader import DT_QUOTE, DT_TRADE, ReplayData, symbols

    bars = data.bars
    if len(bars) == 0:
        raise ValueError("no bars in replay data")

    syms = symbols(bars)
    if symbol is None:
        uniq, counts = np.unique(syms, return_counts=True)
        symbol = str(uniq[counts.argmax()])

    sub = bars[syms == symbol]
    if len(sub) < 2:
        raise ValueError(f"symbol {symbol!r} has < 2 bars; can't form a forward return")
    # Sort to time order before differencing closes. NB: the tests feed
    # already-sorted bars, so this reorder path itself is not exercised -- it's
    # belt-and-suspenders for real captures (which arrive in order anyway).
    sub = sub[np.argsort(sub["start_ns"], kind="stable")]

    # Features for this symbol only (extract_features groups per symbol; passing a
    # single-symbol slice keeps the rows in lockstep with the close series below).
    one = ReplayData(
        trades=np.empty(0, DT_TRADE), bars=sub, quotes=np.empty(0, DT_QUOTE),
        record_count=len(sub),
    )
    frame = extract_features(one)

    close = sub["close"].astype(np.float64)
    # Simple forward return for the step taken at bar t: close[t+1]/close[t] - 1.
    # Defined for t = 0 .. n-2, so the last bar (no successor) is dropped.
    fwd = close[1:] / close[:-1] - 1.0
    obs = frame.features[:-1]
    return BarTradingEnv(obs, fwd, txn_cost=txn_cost)
