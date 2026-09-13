#include "lob/backtester.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace lob {

Backtester::Backtester(MessageColumns cols, std::vector<SeedLevel> seed, BacktestConfig cfg, SnapshotColumns snapshot)
    : cols_(cols), snap_(snapshot), seed_(std::move(seed)), cfg_(cfg), fills_(cfg.cancel_assumption, cfg.fees) {
    for (const auto& s : seed_) book_.add_anon(s.side, s.price, s.qty);
    now_ = cfg_.start_ts;
    next_sample_ = cfg_.start_ts;
}

double Backtester::mid_dollars() const noexcept {
    if (book_.has_both_sides()) return static_cast<double>(book_.mid2()) / (2.0 * static_cast<double>(kPriceScale));
    if (book_.best_bid()) return static_cast<double>(book_.best_bid_price()) / static_cast<double>(kPriceScale);
    if (book_.best_ask()) return static_cast<double>(book_.best_ask_price()) / static_cast<double>(kPriceScale);
    return res_.sample_mid.empty() ? 0.0 : res_.sample_mid.back();
}

void Backtester::sample_until(Ts ts) {
    // Record equity at every sample boundary strictly before `ts`: a sample at
    // time T reflects every event with timestamp <= T. Called before an event
    // at `ts` is processed, and once more at the end with `ts` = end_ts + 1.
    while (next_sample_ < ts && next_sample_ <= cfg_.end_ts) {
        const double mid = mid_dollars();
        res_.sample_ts.push_back(next_sample_);
        res_.equity.push_back(pf_.equity(mid));
        res_.sample_position.push_back(pf_.position);
        res_.sample_mid.push_back(mid);
        next_sample_ += cfg_.pnl_sample_ns;
    }
}

// ---- StrategyContext ----------------------------------------------------------

std::uint64_t Backtester::submit(Side side, Price price, Qty qty, TimeInForce tif, bool is_market) {
    ++res_.orders_submitted;
    if (qty <= 0 || (closing_ && !strat_closing_guard_)) { ++res_.orders_rejected; return 0; }
    // Position cap: position + same-side open quantity must stay within max_position.
    const Qty projected = std::abs(pf_.position + sign(side) * (qty + open_qty(side)));
    if (projected > cfg_.max_position) { ++res_.orders_rejected; return 0; }

    SimOrder o;
    o.id = next_order_id_++; o.side = side; o.price = price; o.qty = qty;
    o.submit_ts = now_; o.arrival_ts = now_ + latency(); o.tif = tif; o.is_market = is_market;
    orders_.emplace(o.id, o);
    live_.push_back(o.id);
    actions_.push(Action{o.arrival_ts, next_seq_++, ActionKind::Submit, o.id});
    return o.id;
}

std::uint64_t Backtester::submit_limit(Side side, Price price, Qty qty, TimeInForce tif) {
    return submit(side, price, qty, tif, false);
}
std::uint64_t Backtester::submit_market(Side side, Qty qty) {
    return submit(side, side == Side::Buy ? kNoAsk : kNoBid, qty, TimeInForce::IOC, true);
}
void Backtester::cancel(std::uint64_t order_id) {
    auto it = orders_.find(order_id);
    if (it == orders_.end() || !it->second.open()) return;
    actions_.push(Action{now_ + latency(), next_seq_++, ActionKind::Cancel, order_id});
}
void Backtester::schedule_timer(Ts at) {
    actions_.push(Action{std::max(at, now_), next_seq_++, ActionKind::Timer, 0});
}
const SimOrder* Backtester::order(std::uint64_t id) const {
    auto it = orders_.find(id);
    return it == orders_.end() ? nullptr : &it->second;
}
std::vector<const SimOrder*> Backtester::open_orders() const {
    std::vector<const SimOrder*> out;
    for (auto id : live_) { auto it = orders_.find(id); if (it != orders_.end() && it->second.open()) out.push_back(&it->second); }
    return out;
}
Qty Backtester::open_qty(Side side) const {
    Qty q = 0;
    for (auto id : live_) { auto it = orders_.find(id); if (it != orders_.end() && it->second.open() && it->second.side == side) q += it->second.remaining(); }
    return q;
}

void Backtester::prune_closed() {
    live_.erase(std::remove_if(live_.begin(), live_.end(), [&](std::uint64_t id) {
        auto it = orders_.find(id); return it == orders_.end() || !it->second.open(); }), live_.end());
}

// ---- event processing -----------------------------------------------------------

void Backtester::dispatch_fills(Strategy& strat) {
    for (const Fill& f : pending_fills_) {
        pf_.apply(f);
        if (cfg_.record_fills) res_.fills.push_back(f);
        strat.on_fill(*this, f);
    }
    pending_fills_.clear();
}

void Backtester::process_action(const Action& a, Strategy& strat) {
    now_ = std::max(now_, a.at);
    switch (a.kind) {
    case ActionKind::Submit: {
        auto it = orders_.find(a.order_id);
        if (it == orders_.end() || it->second.state != SimOrderState::Pending) return;
        SimOrder& o = it->second;
        const Qty before = o.filled;
        fills_.on_arrival(o, book_, now_, pending_fills_);
        if (o.filled > before) ++res_.orders_with_fill;
        if (o.state == SimOrderState::Cancelled) ++res_.orders_cancelled;
        dispatch_fills(strat);
        break;
    }
    case ActionKind::Cancel: {
        auto it = orders_.find(a.order_id);
        if (it == orders_.end() || !it->second.open()) return;
        it->second.state = SimOrderState::Cancelled;
        ++res_.orders_cancelled;
        break;
    }
    case ActionKind::Timer:
        if (!ended_) strat.on_timer(*this, now_);
        break;
    }
    prune_closed();
}

void Backtester::process_message(std::size_t i, Strategy& strat) {
    const Message m = cols_.at(i);
    now_ = std::max(now_, m.ts);

    // Level qty at the touched price before the message (pro-rata cancel model).
    Qty level_before = 0;
    if (m.type == MsgType::PartialCancel || m.type == MsgType::Delete)
        level_before = book_.level_qty(m.side, m.price);

    const Price pb0 = book_.best_bid_price(), pa0 = book_.best_ask_price();
    const Qty   qb0 = book_.best_bid_qty(),   qa0 = book_.best_ask_qty();

    apply(book_, m, res_.apply_stats, ApplyOptions{cfg_.price_priority_purge});
    if (snap_.valid()) resync(book_, snap_, i, res_.resync_stats);

    const Price pb1 = book_.best_bid_price(), pa1 = book_.best_ask_price();
    const Qty   qb1 = book_.best_bid_qty(),   qa1 = book_.best_ask_qty();
    const bool ok = pb0 != kNoBid && pa0 != kNoAsk && pb1 != kNoBid && pa1 != kNoAsk;
    current_ofi_ = ok ? ofi_term(pb0, qb0, pa0, qa0, pb1, qb1, pa1, qa1) : 0;

    // Would history have filled any of our resting orders?
    bool any_fill_progress = false;
    for (auto id : live_) {
        auto it = orders_.find(id);
        if (it == orders_.end() || it->second.state != SimOrderState::Live) continue;
        const Qty before = it->second.filled;
        fills_.on_market(it->second, m, level_before, pending_fills_);
        if (it->second.filled > before) { any_fill_progress = true; if (before == 0) ++res_.orders_with_fill; }
    }
    if (any_fill_progress) { dispatch_fills(strat); prune_closed(); }

    if (!closing_ && m.ts >= cfg_.end_ts - cfg_.flatten_before_close_ns) begin_close();

    if (!ended_) strat.on_market(*this, m, current_ofi_);
}

void Backtester::begin_close() {
    closing_ = true;
    for (auto id : live_) cancel(id);
    if (pf_.position != 0) {
        strat_closing_guard_ = true;  // allow our own flatten order through the closing gate
        submit_market(pf_.position > 0 ? Side::Sell : Side::Buy, std::abs(pf_.position));
        strat_closing_guard_ = false;
    }
}

BacktestResult Backtester::run(Strategy& strat) {
    const std::size_t n = cols_.n;
    std::size_t i = std::min(cfg_.start_index, n);

    strat.on_start(*this);

    while (true) {
        // Data exhausted before the scheduled flatten time (short files, tests):
        // flatten now so the close still goes through the fill model.
        if (i >= n && !closing_) begin_close();
        const Ts next_msg_ts = (i < n) ? cols_.ts[i] : std::numeric_limits<Ts>::max();
        if (!actions_.empty() && actions_.top().at < next_msg_ts) {
            Action a = actions_.top(); actions_.pop();
            sample_until(a.at);
            process_action(a, strat);
        } else if (i < n) {
            sample_until(next_msg_ts);
            process_message(i, strat);
            ++i;
        } else {
            break;
        }
    }
    ended_ = true;
    strat.on_end(*this);

    // Fallback: still holding? close at the last mid, paying the taker fee.
    if (pf_.position != 0) {
        const double mid = mid_dollars();
        const Price px = static_cast<Price>(std::llround(mid * static_cast<double>(kPriceScale)));
        const Side s = pf_.position > 0 ? Side::Sell : Side::Buy;
        const Qty q = std::abs(pf_.position);
        Fill f{now_, 0, s, px, q, false, cfg_.fees.fee(false, q, px)};
        pf_.apply(f);
        if (cfg_.record_fills) res_.fills.push_back(f);
        res_.forced_close = true;
    }
    sample_until(cfg_.end_ts + 1);

    res_.portfolio  = pf_;
    res_.book_stats = book_.stats();

    Metrics& mt = res_.metrics;
    mt.total_pnl    = res_.equity.empty() ? pf_.cash : res_.equity.back();
    mt.realized_pnl = pf_.realized;
    mt.fees         = pf_.fees;
    mt.volume       = static_cast<double>(pf_.volume);
    mt.n_fills      = pf_.n_fills;
    mt.n_orders     = res_.orders_submitted - res_.orders_rejected;
    for (const Fill& f : res_.fills) (f.passive ? mt.n_passive_fills : mt.n_aggressive_fills)++;
    mt.fill_rate    = mt.n_orders ? static_cast<double>(res_.orders_with_fill) / static_cast<double>(mt.n_orders) : 0.0;
    mt.pnl_per_share = pf_.volume ? mt.total_pnl / static_cast<double>(pf_.volume) : 0.0;
    mt.max_drawdown = max_drawdown(res_.equity);
    const double samples_per_day = static_cast<double>(cfg_.end_ts - cfg_.start_ts) / static_cast<double>(cfg_.pnl_sample_ns);
    mt.sharpe       = sharpe_ratio(res_.equity, samples_per_day);
    return res_;
}

}  // namespace lob
