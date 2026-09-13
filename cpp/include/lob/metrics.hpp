// metrics.hpp - performance statistics from an equity curve and fill list.
#pragma once
#include <cstdint>
#include <vector>

#include "lob/event.hpp"
#include "lob/types.hpp"

namespace lob {

struct Metrics {
    double total_pnl{0};        // final equity (starts at 0 cash, flat)
    double realized_pnl{0};
    double fees{0};
    double max_drawdown{0};     // dollars, peak-to-trough of equity
    double sharpe{0};           // annualized, from per-sample equity changes
    double volume{0};           // shares
    std::int64_t n_fills{0};
    std::int64_t n_orders{0};
    std::int64_t n_passive_fills{0};
    std::int64_t n_aggressive_fills{0};
    double fill_rate{0};        // orders with any fill / orders submitted
    double pnl_per_share{0};    // total_pnl / volume
};

double max_drawdown(const std::vector<double>& equity);
// Annualize assuming `samples_per_day` samples over 252 trading days.
double sharpe_ratio(const std::vector<double>& equity, double samples_per_day);

}  // namespace lob
