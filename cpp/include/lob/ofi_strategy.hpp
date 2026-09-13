// ofi_strategy.hpp - a simple order-flow-imbalance threshold strategy.
//
// Signal: OFI_w = sum of the last `window_events` per-event OFI terms.
// Normalization: z = (OFI_w - mean) / std over the last `zscore_window`
// windowed values (rolling, exact via ring buffer sums).
// Rules:
//   flat  & z >  entry_z -> buy  `order_qty`  (passive at best bid, or cross)
//   flat  & z < -entry_z -> sell `order_qty`
//   long  & (z < exit_z  or held > max_hold) -> exit with a marketable order
//   short & (z > -exit_z or held > max_hold) -> exit
//   resting entry order older than `order_ttl` -> cancel (signal is stale)
// One working order at a time; cooldown between submissions.
#pragma once
#include <cstdint>
#include <vector>

#include "lob/strategy.hpp"

namespace lob {

struct OfiStrategyParams {
    int    window_events = 50;
    int    zscore_window = 1000;
    double entry_z       = 2.0;
    double exit_z        = 0.5;
    Qty    order_qty     = 100;
    bool   passive_entry = true;
    Ts     order_ttl_ns  = 2 * kNsPerSec;
    Ts     max_hold_ns   = 60 * kNsPerSec;
    Ts     cooldown_ns   = 100'000'000;  // 100 ms
};

class OfiStrategy final : public Strategy {
public:
    explicit OfiStrategy(OfiStrategyParams p);

    void on_start(StrategyContext& ctx) override;
    void on_market(StrategyContext& ctx, const Message& m, std::int64_t ofi_e) override;
    void on_fill(StrategyContext& ctx, const Fill& f) override;

    // Diagnostics for research: the signal as the strategy saw it.
    const std::vector<double>& zscores() const noexcept { return z_hist_; }
    double last_z() const noexcept { return last_z_; }

private:
    void update_signal(std::int64_t ofi_e);
    void enter(StrategyContext& ctx, Side side);
    void exit(StrategyContext& ctx);

    OfiStrategyParams p_;
    // rolling sum of per-event terms
    std::vector<std::int64_t> e_ring_;  std::size_t e_pos_{0}; std::int64_t e_sum_{0}; std::size_t e_count_{0};
    // rolling mean/var of windowed OFI
    std::vector<double> w_ring_; std::size_t w_pos_{0}; double w_sum_{0}, w_sumsq_{0}; std::size_t w_count_{0};
    double last_z_{0};
    std::vector<double> z_hist_;

    std::uint64_t working_order_{0};
    Ts entry_ts_{0};
    Ts last_action_ts_{0};
    bool exiting_{false};
};

}  // namespace lob
