#include "lob/metrics.hpp"

#include <cmath>

namespace lob {

double max_drawdown(const std::vector<double>& equity) {
    double peak = 0.0, mdd = 0.0;
    for (double e : equity) {
        if (e > peak) peak = e;
        if (peak - e > mdd) mdd = peak - e;
    }
    return mdd;
}

double sharpe_ratio(const std::vector<double>& equity, double samples_per_day) {
    if (equity.size() < 3) return 0.0;
    const std::size_t n = equity.size() - 1;
    double mean = 0.0;
    for (std::size_t i = 1; i <= n; ++i) mean += equity[i] - equity[i - 1];
    mean /= static_cast<double>(n);
    double var = 0.0;
    for (std::size_t i = 1; i <= n; ++i) { const double d = equity[i] - equity[i - 1] - mean; var += d * d; }
    var /= static_cast<double>(n - 1);
    if (var <= 0.0) return 0.0;
    return mean / std::sqrt(var) * std::sqrt(samples_per_day * 252.0);
}

}  // namespace lob
