"""Mechanics + no-lookahead proof for the bar-trading sandbox.

The load-bearing no-lookahead guarantee is test_forward_return_alignment_no_offset
(forward_return[t] checked directly against the source closes) plus features being
trailing by construction. The clairvoyant / obs-only pair below CORROBORATES it:

  * a clairvoyant policy that cheats (acts on the forward return) MUST profit
    strongly  -> the reward plumbing actually pays for correct directional bets
    (if it were silently broken, everyone would score ~0 and "baselines hover near
    zero" would be false comfort);
  * the momentum baseline staying flat rules out the likeliest leak specifically --
    log_return accidentally being a forward return. (random ignores the obs, so
    it's only a sanity floor, not leak evidence.)

The rest are mechanics: exact PnL accounting, txn cost only on position change,
episode boundary, within-symbol forward returns.

Run: pytest ml/test_rl_env.py
"""

from __future__ import annotations

import os
import sys

import numpy as np
import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import synth  # noqa: E402
import replay_reader  # noqa: E402
import replay_writer  # noqa: E402
from rl_env import BarTradingEnv, from_replay  # noqa: E402
from rl_policies import MomentumPolicy, RandomPolicy, always_flat, run_episode  # noqa: E402


def _replay_from_bars(*bar_arrays) -> replay_reader.ReplayData:
    bars = np.concatenate(bar_arrays)
    return replay_reader.ReplayData(
        trades=np.empty(0, replay_reader.DT_TRADE),
        bars=bars,
        quotes=np.empty(0, replay_reader.DT_QUOTE),
        record_count=len(bars),
    )


# ---- mechanics -----------------------------------------------------------------

def test_reset_is_deterministic_and_flat():
    env = BarTradingEnv(np.zeros((3, 1)), np.array([0.1, -0.2, 0.05]), txn_cost=0.01)
    obs, info = env.reset()
    assert np.array_equal(obs, env.observations[0])
    assert info["position"] == 0 and info["cum_reward"] == 0.0 and info["t"] == 0


def test_exact_pnl_accounting():
    env = BarTradingEnv(np.zeros((3, 1)), np.array([0.1, -0.2, 0.05]), txn_cost=0.01)
    env.reset()
    _, r0, _, _, _ = env.step(1)   # 1*0.1  - 0.01*|1-0|
    _, r1, _, _, _ = env.step(-1)  # -1*-0.2 - 0.01*|-1-1|
    _, r2, term, _, info = env.step(0)  # 0*0.05 - 0.01*|0-(-1)|
    assert r0 == pytest.approx(0.09)
    assert r1 == pytest.approx(0.18)
    assert r2 == pytest.approx(-0.01)
    assert term
    assert info["cum_reward"] == pytest.approx(0.26)


def test_txn_cost_only_on_position_change():
    env = BarTradingEnv(np.zeros((2, 1)), np.array([0.1, 0.1]), txn_cost=0.05)
    env.reset()
    _, r_change, _, _, _ = env.step(1)  # 0 -> 1, pays one unit of cost
    _, r_hold, _, _, _ = env.step(1)    # 1 -> 1, no cost
    assert r_change == pytest.approx(0.1 - 0.05)
    assert r_hold == pytest.approx(0.1)


def test_episode_boundary():
    env = BarTradingEnv(np.zeros((2, 1)), np.array([0.0, 0.0]))
    env.reset()
    _, _, t0, _, _ = env.step(0)
    _, _, t1, _, _ = env.step(0)
    assert not t0 and t1
    with pytest.raises(RuntimeError):
        env.step(0)  # stepping a finished episode


def test_always_flat_is_exactly_zero():
    # Whatever the returns, never holding a position yields exactly zero PnL/cost.
    env = BarTradingEnv(np.zeros((5, 1)), np.array([0.3, -0.1, 0.2, -0.4, 0.05]))
    assert run_episode(env, always_flat)["cum_reward"] == 0.0


# ---- alignment / no cross-symbol leak -----------------------------------------

def test_forward_return_alignment_no_offset():
    """from_replay's forward_return[t] must equal close[t+1]/close[t]-1 from the
    source bars -- the off-by-one that would leak the future."""
    sb = synth.generate()
    recs = replay_writer.make_bars(
        "SYNTH", sb.start_ns, sb.open, sb.high, sb.low, sb.close, sb.vwap,
        sb.volume, sb.trade_count,
    )
    env = from_replay(_replay_from_bars(recs), "SYNTH")
    expected = sb.close[1:] / sb.close[:-1] - 1.0
    assert np.allclose(env.forward_returns, expected)
    # obs has one fewer row than bars (last bar has no successor).
    assert len(env.observations) == len(sb.close) - 1


def test_forward_returns_are_within_symbol():
    """A symbol's forward returns must never span into another symbol's bars."""
    n = 50
    ts = 1_704_206_000_000_000_000 + np.arange(n, dtype=np.uint64) * np.uint64(60_000_000_000)
    flat = np.full(n, 100.0)            # AAPL: constant close -> every fwd ret == 0
    rising = 100.0 * np.cumprod(np.full(n, 1.001))  # MSFT: strictly rising
    def mk(sym, close):
        return replay_writer.make_bars(
            sym, ts, close, close * 1.001, close * 0.999, close,
            close, np.full(n, 1000, np.uint64), np.full(n, 10, np.uint64),
        )
    data = _replay_from_bars(mk("AAPL", flat), mk("MSFT", rising))
    aapl = from_replay(data, "AAPL")
    # If AAPL's returns leaked into MSFT's first bar, this would be nonzero.
    assert np.allclose(aapl.forward_returns, 0.0)
    msft = from_replay(data, "MSFT")
    assert np.all(msft.forward_returns > 0.0)


# ---- the no-lookahead proof pair ----------------------------------------------

def test_no_lookahead_clairvoyant_wins_obs_only_cannot():
    sb = synth.generate()
    recs = replay_writer.make_bars(
        "SYNTH", sb.start_ns, sb.open, sb.high, sb.low, sb.close, sb.vwap,
        sb.volume, sb.trade_count,
    )
    data = _replay_from_bars(recs)

    # Clairvoyant: acts on the forward return (cheating by construction). It can
    # only profit if the reward actually pays for correct directional bets.
    env = from_replay(data, "SYNTH")
    obs, _ = env.reset()
    clair = 0.0
    t = 0
    done = False
    while not done:
        a = int(np.sign(env.forward_returns[t]))
        obs, r, term, _, _ = env.step(a)
        clair += r
        t += 1
        done = term

    # Obs-only policies: on i.i.d. synthetic returns they have no edge.
    momentum = run_episode(from_replay(data, "SYNTH"), MomentumPolicy())["cum_reward"]
    random = run_episode(from_replay(data, "SYNTH"), RandomPolicy(0))["cum_reward"]

    # A leak would manifest as an obs-only policy turning STRONGLY POSITIVE (it
    # would be seeing the future). A near-zero or negative result is the correct
    # "no edge" outcome -- so the canary is an upper bound, not |.|.
    assert clair > 0.5, f"clairvoyant didn't profit ({clair:.3f}) -> reward plumbing broken"
    assert momentum < 0.3, f"momentum has a fake positive edge ({momentum:.3f}) -> lookahead leak"
    assert random < 0.3, f"random has a fake positive edge ({random:.3f}) -> lookahead leak"
    # Cheating must dominate every obs-only policy by a wide margin.
    assert clair > momentum + 0.4 and clair > random + 0.4, \
        "cheating should dominate obs-only policies"


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
