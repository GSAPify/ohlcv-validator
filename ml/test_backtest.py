"""Tests for RL reward robustness + the walk-forward policy backtest.

Verifies the robustness knobs and the leakage-free backtest mechanics -- NOT that
any policy profits (it shouldn't, and the backtest is built to show that).

Run: pytest ml/test_backtest.py
"""

from __future__ import annotations

import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import backtest  # noqa: E402
import replay_reader  # noqa: E402
import replay_writer  # noqa: E402
import synth_eval  # noqa: E402
from rl_env import BarTradingEnv, from_replay  # noqa: E402
from rl_policies import MomentumPolicy, always_flat, run_episode  # noqa: E402


def _small_data(n_bars: int = 600):
    sb = synth_eval.generate_eval(n_bars=n_bars, anomaly_count=0, seed=5)
    recs = replay_writer.make_bars(
        "SYNTH", sb.start_ns, sb.open, sb.high, sb.low, sb.close, sb.vwap,
        sb.volume, sb.trade_count,
    )
    return replay_reader.ReplayData(
        trades=np.empty(0, replay_reader.DT_TRADE), bars=recs,
        quotes=np.empty(0, replay_reader.DT_QUOTE), record_count=len(recs),
    )


def test_risk_aversion_penalizes_reward_but_not_pnl():
    obs = np.zeros((2, 1))
    fwd = np.array([0.1, 0.0])  # a +10% move on the first step
    base = BarTradingEnv(obs, fwd, txn_cost=0.0, risk_aversion=0.0)
    base.reset()
    _, r0, _, _, i0 = base.step(1)
    rv = BarTradingEnv(obs, fwd, txn_cost=0.0, risk_aversion=1.0)
    rv.reset()
    _, r1, _, _, i1 = rv.step(1)

    assert i0["pnl"] == i1["pnl"] == 0.1        # PnL is identical...
    assert r1 < r0                               # ...but the reward is penalized
    assert abs(r1 - (0.1 - 1.0 * 0.1 * 0.1)) < 1e-12


def test_run_episode_reports_risk_stats_and_flat_is_zero():
    env = BarTradingEnv(np.zeros((5, 1)), np.array([0.3, -0.1, 0.2, -0.4, 0.05]))
    r = run_episode(env, always_flat)
    assert r["cum_pnl"] == 0.0 and r["trades"] == 0
    assert r["max_drawdown"] == 0.0 and r["sharpe"] == 0.0
    # a trading policy yields finite, well-formed stats
    env2 = BarTradingEnv(np.zeros((5, 1)), np.array([0.3, -0.1, 0.2, -0.4, 0.05]))
    r2 = run_episode(env2, lambda obs: 1)  # always long
    assert np.isfinite(r2["sharpe"]) and r2["max_drawdown"] >= 0.0


def test_higher_txn_cost_lowers_pnl_for_a_trading_policy():
    data = _small_data()
    lo = run_episode(backtest._test_env(data, "SYNTH", txn_cost=0.0), MomentumPolicy())
    hi = run_episode(backtest._test_env(data, "SYNTH", txn_cost=0.001), MomentumPolicy())
    # Same trades, more cost -> strictly less PnL. (Robustness: an "edge" that
    # can't survive realistic cost isn't one.)
    assert hi["cum_pnl"] < lo["cum_pnl"]


def test_backtest_env_is_the_leakage_free_test_tail():
    data = _small_data()
    full = from_replay(data, "SYNTH")
    test_env = backtest._test_env(data, "SYNTH", txn_cost=0.0,
                                  fit_frac=0.4, calib_frac=0.2)
    # The env is only the last ~40% (1 - fit - calibrate) of the series in time.
    assert test_env.n_steps < full.n_steps
    assert abs(test_env.n_steps - 0.4 * full.n_steps) < 0.05 * full.n_steps
