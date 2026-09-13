// backtester.hpp - the event loop.
//
// Two time-ordered streams are merged:
//   1. historical market messages (exchange timestamps), and
//   2. our own actions, each delayed by latency (a min-heap on arrival time).
// At every step we pop whichever is earlier; on ties history goes first
// (conservative: our order arrives *after* the message with the same stamp).
//
// LATENCY MODEL. The strategy is invoked when a message is applied, i.e. it
// observes the book as of exchange time t. Anything it sends arrives at
// t + md_latency + order_latency. The market-data leg therefore does not
// stale the strategy's *view* (it always sees the true book at t); it lengthens
// the round trip, which is what determines whether the liquidity you aimed at
// is still there. With zero latency the backtest has look-ahead: you can
// react to an event and trade on the book state that existed at the same
// instant, which nobody can do in production.
#pragma once
#include <cstdint>
#include <queue>
#include <unordered_map>
#include <vector>

#include "lob/costs.hpp"
#include "lob/event.hpp"
#include "lob/fill_model.hpp"
#include "lob/lobster.hpp"
#include "lob/metrics.hpp"
#include "lob/order_book.hpp"
#include "lob/portfolio.hpp"
#include "lob/replay.hpp"
#include "lob/strategy.hpp"

namespace lob {

struct BacktestConfig {
    Ts md_latency_ns    = 0;
    Ts order_latency_ns = 0;
    FeeSchedule      fees{};
    CancelAssumption cancel_assumption = CancelAssumption::ProRata;
    Ts  pnl_sample_ns           = kNsPerSec;             // equity sampling interval
    Ts  start_ts                = 34200 * kNsPerSec;     // 09:30
    Ts  end_ts                  = 57600 * kNsPerSec;     // 16:00
    Ts  flatten_before_close_ns = 60 * kNsPerSec;        // start flattening at 15:59
    Qty max_position            = 1000;                  // |position + open same-side qty| cap
    std::size_t start_index     = 0;                     // first message to apply (seed covers earlier rows)
    bool record_fills           = true;
    bool price_priority_purge   = true;                  // see lobster.hpp
};

struct BacktestResult {
    std::vector<Fill>          fills;
    std::vector<Ts>            sample_ts;
    std::vector<double>        equity;         // dollars, marked to mid
    std::vector<std::int32_t>  sample_position;
    std::vector<double>        sample_mid;     // dollars
    Portfolio   portfolio;
    Metrics     metrics;
    std::int64_t orders_submitted{0}, orders_rejected{0}, orders_cancelled{0}, orders_with_fill{0};
    bool        forced_close{false};   // position was closed at the last mid because the book was empty
    ApplyStats  apply_stats;
    BookStats   book_stats;
    ResyncStats resync_stats;
};

class Backtester final : public StrategyContext {
public:
    Backtester(MessageColumns cols, std::vector<SeedLevel> seed, BacktestConfig cfg,
               SnapshotColumns snapshot = SnapshotColumns{});

    BacktestResult run(Strategy& strat);

    // --- StrategyContext -----------------------------------------------------
    Ts               now() const override { return now_; }
    const OrderBook& book() const override { return book_; }
    Qty              position() const override { return pf_.position; }
    double           cash() const override { return pf_.cash; }
    bool             closing() const override { return closing_; }
    std::uint64_t submit_limit(Side side, Price price, Qty qty, TimeInForce tif) override;
    std::uint64_t submit_market(Side side, Qty qty) override;
    void          cancel(std::uint64_t order_id) override;
    void          schedule_timer(Ts at) override;
    const SimOrder*              order(std::uint64_t id) const override;
    std::vector<const SimOrder*> open_orders() const override;
    Qty                          open_qty(Side side) const override;

private:
    Ts   latency() const noexcept { return cfg_.md_latency_ns + cfg_.order_latency_ns; }
    std::uint64_t submit(Side side, Price price, Qty qty, TimeInForce tif, bool is_market);
    void process_action(const Action& a, Strategy& strat);
    void process_message(std::size_t i, Strategy& strat);
    void dispatch_fills(Strategy& strat);
    void sample_until(Ts ts);
    void begin_close();
    double mid_dollars() const noexcept;
    void prune_closed();

    MessageColumns         cols_;
    SnapshotColumns        snap_;
    std::vector<SeedLevel> seed_;
    BacktestConfig         cfg_;
    OrderBook              book_;
    FillModel              fills_;
    Portfolio              pf_;
    BacktestResult         res_;

    std::priority_queue<Action, std::vector<Action>, ActionLater> actions_;
    std::unordered_map<std::uint64_t, SimOrder> orders_;   // all sim orders by id
    std::vector<std::uint64_t> live_;                       // ids of Pending/Live orders (small)
    std::vector<Fill>          pending_fills_;
    std::uint64_t next_order_id_{1}, next_seq_{1};
    Ts   now_{0};
    Ts   next_sample_{0};
    bool closing_{false};
    bool ended_{false};
    bool strat_closing_guard_{false};  // lets the end-of-day flatten bypass the closing gate
    std::int64_t current_ofi_{0};
};

}  // namespace lob
