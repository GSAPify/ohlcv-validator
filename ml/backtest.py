"""Walk-forward, leakage-free policy backtest over the bar/feature stream.

Reuses the anomaly eval's time-ordered split (ml/split.py): each policy is scored
ONLY on the test tail -- strictly later in time than the fit/calibrate portion, so
no lookahead and no fitting on the future. It reports PnL, a per-step Sharpe,
trade count, and max drawdown, and **sweeps the transaction cost**: a policy that
only "wins" at zero cost isn't robust.

This is a backtest, not a learning agent -- deliberately. On a minute-bar series
obs-only heuristics net ~0 after realistic costs (no free lunch), and this is
built to reveal that, not hide it. There is no profit claim here.

    python3 ml/backtest.py            # synthetic price series
    python3 ml/backtest.py real.bin   # real bars
"""

from __future__ import annotations

import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import replay_reader  # noqa: E402
import replay_writer  # noqa: E402
import synth_eval  # noqa: E402
from replay_reader import symbols as decode_symbols  # noqa: E402
from rl_env import BarTradingEnv, from_replay  # noqa: E402
from rl_policies import MomentumPolicy, RandomPolicy, always_flat, run_episode  # noqa: E402
from split import assert_no_leakage, time_split  # noqa: E402

# Realistic per-side cost levels to sweep (fraction of price). 0 -> the frictionless
# fiction; 1-10 bps -> where real strategies live and most "edges" die.
COST_SWEEP = (0.0, 0.0001, 0.0005, 0.0010)


def _test_env(data, symbol: str, txn_cost: float, risk_aversion: float = 0.0,
              fit_frac: float = 0.4, calib_frac: float = 0.2) -> BarTradingEnv:
    """An env over the leakage-free TEST tail of one symbol (the time-ordered
    remainder after the fit+calibrate blocks)."""
    full = from_replay(data, symbol, txn_cost=txn_cost, risk_aversion=risk_aversion)
    n = full.n_steps
    sym = np.full(n, symbol)
    order = np.arange(n)  # from_replay already time-sorted this symbol
    sp = time_split(sym, order, fit_frac, calib_frac)
    assert_no_leakage(sp, sym, order)  # the test tail is strictly later in time
    return BarTradingEnv(full.observations[sp.test], full.forward_returns[sp.test],
                         txn_cost=txn_cost, risk_aversion=risk_aversion)


def _policies():
    return [("always_flat", always_flat),
            ("random", RandomPolicy(seed=0)),
            ("momentum", MomentumPolicy())]


def backtest(data, syms: list[str], top_symbol_note: str) -> int:
    print(f"=== walk-forward policy backtest ({top_symbol_note}) ===")
    print(f"  symbols: {', '.join(syms)}   (scored on the out-of-sample test tail "
          f"only -- leakage-free)\n")
    print(f"  {'txn':>5}  {'policy':<12} {'cum_pnl':>10} {'sharpe':>8} {'trades':>8} {'max_dd':>9}")
    print(f"  {'-'*5}  {'-'*12} {'-'*10} {'-'*8} {'-'*8} {'-'*9}")
    for cost in COST_SWEEP:
        for name, make in _policies():
            tot_pnl = 0.0
            tot_trades = 0
            sharpes = []
            max_dd = 0.0
            for sym in syms:
                env = _test_env(data, sym, cost)
                # fresh policy instance per symbol so RandomPolicy's RNG is comparable
                policy = make if name != "random" else RandomPolicy(seed=0)
                r = run_episode(env, policy)
                tot_pnl += r["cum_pnl"]
                tot_trades += r["trades"]
                sharpes.append(r["sharpe"])
                max_dd = max(max_dd, r["max_drawdown"])
            bps = f"{cost * 1e4:.0f}bp"
            print(f"  {bps:>5}  {name:<12} {tot_pnl:>10.4f} "
                  f"{np.mean(sharpes):>8.2f} {tot_trades:>8} {max_dd:>9.4f}")
        print()
    print("  verdict: obs-only heuristics net ~0 and erode with cost -- no edge on\n"
          "  this data. That's the honest result; the point is the leakage-free\n"
          "  methodology + cost sweep, not a profit number.")
    return 0


def _synthetic_data():
    sb = synth_eval.generate_eval(n_bars=4000, anomaly_count=0, seed=5)
    recs = replay_writer.make_bars(
        "SYNTH", sb.start_ns, sb.open, sb.high, sb.low, sb.close, sb.vwap,
        sb.volume, sb.trade_count,
    )
    return replay_reader.ReplayData(
        trades=np.empty(0, replay_reader.DT_TRADE), bars=recs,
        quotes=np.empty(0, replay_reader.DT_QUOTE), record_count=len(recs),
    )


def main() -> int:
    args = sys.argv[1:]
    if args and not args[0].startswith("-"):
        data = replay_reader.read_replay(args[0])
        syms = sorted(set(decode_symbols(data.bars).tolist()))
        return backtest(data, syms, f"REAL: {os.path.basename(args[0])}")
    return backtest(_synthetic_data(), ["SYNTH"], "SYNTHETIC random-walk series")


if __name__ == "__main__":
    raise SystemExit(main())
