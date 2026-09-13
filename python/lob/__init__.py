"""lob - L3 limit order book reconstruction and event-driven backtesting.

The heavy lifting lives in the C++ extension ``lob._lob``; this package adds the
Parquet data pipeline, validation against LOBSTER's own orderbook files, signal
research helpers and a thin backtest runner.
"""
from . import _lob
from ._lob import (  # noqa: F401
    PRICE_SCALE, TICK, NS_PER_SEC, NO_ASK, NO_BID,
    Side, MsgType, TimeInForce, SimOrderState, CancelAssumption,
    OrderBook, Message, apply_message, replay,
    FeeSchedule, BacktestConfig, SimOrder, Fill, Portfolio, Metrics, BacktestResult,
    StrategyContext, Strategy, OfiStrategyParams, OfiStrategy, Backtester,
)

__all__ = [n for n in dir(_lob) if not n.startswith("_")] + ["data", "reconstruct", "validate", "signals", "research", "backtest"]
__version__ = "0.1.0"
