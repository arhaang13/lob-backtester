// portfolio.hpp - position, cash and PnL accounting.
//
// PnL decomposition: realized (closed round-trips, via average-cost basis),
// unrealized (open position marked to mid), fees. equity = cash + pos * mid.
#pragma once
#include <algorithm>
#include <cmath>

#include "lob/event.hpp"
#include "lob/types.hpp"

namespace lob {

struct Portfolio {
    Qty    position{0};
    double cash{0.0};       // dollars
    double fees{0.0};       // cumulative fees paid (negative if net rebates)
    double realized{0.0};   // gross of fees
    double avg_cost{0.0};   // dollars per share of the open position
    long   volume{0};       // shares traded
    long   n_fills{0};

    void apply(const Fill& f) {
        const double px  = static_cast<double>(f.price) / static_cast<double>(kPriceScale);
        const int    dir = sign(f.side);
        const Qty    q   = f.qty;

        // Closing part first (position and fill have opposite signs).
        if (position != 0 && (position > 0) != (dir > 0)) {
            const Qty closed = std::min<Qty>(q, std::abs(position));
            realized += static_cast<double>(closed) * (px - avg_cost) * (position > 0 ? 1.0 : -1.0);
            position += dir * closed;
            const Qty opened = q - closed;
            if (opened > 0) { avg_cost = px; position += dir * opened; }
            if (position == 0) avg_cost = 0.0;
        } else {
            // Opening / adding: weighted average cost.
            const double old_abs = static_cast<double>(std::abs(position));
            avg_cost  = (avg_cost * old_abs + px * static_cast<double>(q)) / (old_abs + static_cast<double>(q));
            position += dir * q;
        }
        cash   -= dir * static_cast<double>(q) * px;
        cash   -= f.fee;
        fees   += f.fee;
        volume += q;
        ++n_fills;
    }

    double equity(double mid_dollars) const noexcept { return cash + static_cast<double>(position) * mid_dollars; }
    double unrealized(double mid_dollars) const noexcept { return static_cast<double>(position) * (mid_dollars - avg_cost); }
};

}  // namespace lob
