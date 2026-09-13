#include "lob/ofi_strategy.hpp"

#include <cmath>

namespace lob {

OfiStrategy::OfiStrategy(OfiStrategyParams p) : p_(p) {
    e_ring_.assign(static_cast<std::size_t>(std::max(1, p_.window_events)), 0);
    w_ring_.assign(static_cast<std::size_t>(std::max(2, p_.zscore_window)), 0.0);
}

void OfiStrategy::on_start(StrategyContext& ctx) { last_action_ts_ = ctx.now(); }

void OfiStrategy::update_signal(std::int64_t ofi_e) {
    // Rolling sum over the last window_events terms.
    e_sum_ += ofi_e - e_ring_[e_pos_];
    e_ring_[e_pos_] = ofi_e;
    e_pos_ = (e_pos_ + 1) % e_ring_.size();
    if (e_count_ < e_ring_.size()) ++e_count_;

    // Rolling mean / std of the windowed value.
    const double w = static_cast<double>(e_sum_);
    const double old = w_ring_[w_pos_];
    w_sum_   += w - old;
    w_sumsq_ += w * w - old * old;
    w_ring_[w_pos_] = w;
    w_pos_ = (w_pos_ + 1) % w_ring_.size();
    if (w_count_ < w_ring_.size()) ++w_count_;

    if (w_count_ < w_ring_.size() || e_count_ < e_ring_.size()) { last_z_ = 0.0; return; }  // warm-up
    const double n = static_cast<double>(w_count_);
    const double mean = w_sum_ / n;
    double var = w_sumsq_ / n - mean * mean;
    if (var < 1e-12) { last_z_ = 0.0; return; }
    last_z_ = (w - mean) / std::sqrt(var);
}

void OfiStrategy::enter(StrategyContext& ctx, Side side) {
    const OrderBook& b = ctx.book();
    if (!b.has_both_sides()) return;
    if (p_.passive_entry) {
        const Price px = side == Side::Buy ? b.best_bid_price() : b.best_ask_price();
        working_order_ = ctx.submit_limit(side, px, p_.order_qty, TimeInForce::GTC);
    } else {
        const Price px = side == Side::Buy ? b.best_ask_price() : b.best_bid_price();
        working_order_ = ctx.submit_limit(side, px, p_.order_qty, TimeInForce::IOC);
    }
    exiting_ = false;
    last_action_ts_ = ctx.now();
}

void OfiStrategy::exit(StrategyContext& ctx) {
    const Qty pos = ctx.position();
    if (pos == 0) return;
    if (working_order_) { ctx.cancel(working_order_); working_order_ = 0; }
    working_order_ = ctx.submit_market(pos > 0 ? Side::Sell : Side::Buy, std::abs(pos));
    exiting_ = true;
    last_action_ts_ = ctx.now();
}

void OfiStrategy::on_market(StrategyContext& ctx, const Message&, std::int64_t ofi_e) {
    update_signal(ofi_e);
    z_hist_.push_back(last_z_);
    if (ctx.closing()) return;
    const Ts now = ctx.now();

    // Housekeeping on the working order.
    if (working_order_) {
        const SimOrder* o = ctx.order(working_order_);
        if (!o || !o->open()) working_order_ = 0;
        else if (!exiting_ && now - o->submit_ts > p_.order_ttl_ns) { ctx.cancel(working_order_); working_order_ = 0; }
    }

    const Qty pos = ctx.position();
    if (pos != 0) {
        if (exiting_ && working_order_) return;  // exit in flight
        const bool faded = pos > 0 ? last_z_ < p_.exit_z : last_z_ > -p_.exit_z;
        if (faded || now - entry_ts_ > p_.max_hold_ns) exit(ctx);
        return;
    }
    if (working_order_) return;                       // entry resting; wait
    if (now - last_action_ts_ < p_.cooldown_ns) return;
    if (last_z_ > p_.entry_z)       enter(ctx, Side::Buy);
    else if (last_z_ < -p_.entry_z) enter(ctx, Side::Sell);
}

void OfiStrategy::on_fill(StrategyContext& ctx, const Fill& f) {
    if (ctx.position() != 0 && entry_ts_ == 0) entry_ts_ = f.ts;
    if (ctx.position() != 0 && !exiting_) entry_ts_ = entry_ts_ ? entry_ts_ : f.ts;
    if (ctx.position() == 0) { entry_ts_ = 0; exiting_ = false; }
    if (ctx.position() != 0 && entry_ts_ == 0) entry_ts_ = f.ts;
}

}  // namespace lob
