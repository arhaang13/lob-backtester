#include <gtest/gtest.h>

#include <functional>
#include <vector>

#include "lob/backtester.hpp"
#include "lob/ofi_strategy.hpp"

using namespace lob;

namespace {
struct Cols {
    std::vector<std::int64_t> ts, id, price; std::vector<std::int8_t> type, dir; std::vector<std::int32_t> size;
    void push(Ts t, int ty, OrderId i, Qty s, Price p, int d) {
        ts.push_back(t); type.push_back(static_cast<std::int8_t>(ty)); id.push_back(i);
        size.push_back(s); price.push_back(p); dir.push_back(static_cast<std::int8_t>(d));
    }
    MessageColumns view() const { return {ts.data(), type.data(), id.data(), size.data(), price.data(), dir.data(), ts.size()}; }
};

// Strategy driven by a script: at message index k, call fn.
struct Scripted : Strategy {
    std::function<void(StrategyContext&, std::size_t, const Message&)> fn;
    std::size_t k{0};
    std::vector<Fill> fills;
    void on_market(StrategyContext& c, const Message& m, std::int64_t) override { fn(c, k++, m); }
    void on_fill(StrategyContext&, const Fill& f) override { fills.push_back(f); }
};

constexpr Ts S = kNsPerSec;
BacktestConfig cfg_short() {
    BacktestConfig c; c.start_ts = 0; c.end_ts = 100 * S; c.flatten_before_close_ns = 5 * S; c.pnl_sample_ns = S;
    return c;
}
}  // namespace

TEST(Backtester, PassiveFillAfterQueueAndCosts) {
    Cols c;
    c.push(1 * S, 1, 1, 100, 1000, +1);   // bid 100 @ 10.00
    c.push(2 * S, 1, 2, 100, 1100, -1);   // ask 100 @ 11.00
    c.push(3 * S, 4, 1, 100, 1000, +1);   // bid hit for 100 -> our order (behind 100) not yet filled
    c.push(4 * S, 1, 3, 500, 1000, +1);   // someone else joins behind us
    c.push(5 * S, 4, 3, 50, 1000, +1);    // another 50 trades at 10.00 -> that fills US (we were ahead of id 3)
    c.push(6 * S, 4, 2, 10, 1100, -1);    // irrelevant ask trade
    Scripted st;
    st.fn = [](StrategyContext& ctx, std::size_t k, const Message&) {
        if (k == 1) ctx.submit_limit(Side::Buy, 1000, 50);
    };
    auto cfg = cfg_short();
    Backtester bt(c.view(), {}, cfg);
    auto r = bt.run(st);
    ASSERT_GE(r.fills.size(), 1u);
    EXPECT_EQ(r.fills[0].qty, 50); EXPECT_EQ(r.fills[0].price, 1000); EXPECT_TRUE(r.fills[0].passive);
    EXPECT_EQ(r.fills[0].ts, 5 * S);
    EXPECT_DOUBLE_EQ(r.fills[0].fee, -0.0020 * 50);
    // Flatten: sells 50 at the bid 10.00 (only bid is id 3 at 10.00) with taker fee.
    ASSERT_EQ(r.fills.size(), 2u);
    EXPECT_FALSE(r.fills[1].passive); EXPECT_EQ(r.fills[1].price, 1000);
    EXPECT_NEAR(r.metrics.total_pnl, 50 * 0.0020 - 50 * 0.0030, 1e-9);
    EXPECT_EQ(r.portfolio.position, 0);
}

TEST(Backtester, LatencyDelaysArrivalPastLiquidity) {
    // Ask 100 @ 11.00 exists at t=2s and is deleted at t=3s. A marketable buy
    // decided at t=2s fills with zero latency but misses with 2s latency.
    auto make_cols = [] { Cols c;
        c.push(1 * S, 1, 1, 100, 1000, +1);
        c.push(2 * S, 1, 2, 100, 1100, -1);
        c.push(3 * S, 3, 2, 100, 1100, -1);
        c.push(4 * S, 1, 4, 100, 1200, -1);
        return c; };
    for (Ts lat : {Ts{0}, 2 * S}) {
        Cols c = make_cols();
        Scripted st;
        st.fn = [](StrategyContext& ctx, std::size_t k, const Message&) {
            if (k == 1) ctx.submit_limit(Side::Buy, 1100, 100, TimeInForce::IOC);
        };
        auto cfg = cfg_short(); cfg.order_latency_ns = lat;
        Backtester bt(c.view(), {}, cfg);
        auto r = bt.run(st);
        if (lat == 0) { ASSERT_EQ(r.fills.size(), 2u); EXPECT_EQ(r.fills[0].price, 1100); }
        else          { EXPECT_TRUE(r.fills.empty()); EXPECT_EQ(r.orders_cancelled, 1); }
    }
}

TEST(Backtester, PositionCapRejects) {
    Cols c; c.push(1 * S, 1, 1, 100, 1000, +1); c.push(2 * S, 1, 2, 100, 1100, -1);
    Scripted st;
    st.fn = [](StrategyContext& ctx, std::size_t k, const Message&) {
        if (k == 1) { EXPECT_NE(ctx.submit_limit(Side::Buy, 1000, 100), 0u); EXPECT_EQ(ctx.submit_limit(Side::Buy, 1000, 100), 0u); }
    };
    auto cfg = cfg_short(); cfg.max_position = 150;
    Backtester bt(c.view(), {}, cfg);
    auto r = bt.run(st);
    EXPECT_EQ(r.orders_rejected, 1);
}

TEST(Backtester, EquitySamplingAndForcedClose) {
    Cols c;
    c.push(1 * S, 1, 1, 100, 1000, +1);
    c.push(2 * S, 1, 2, 100, 1100, -1);
    c.push(3 * S, 3, 1, 100, 1000, +1);   // bid disappears; nothing to sell into later
    Scripted st;
    st.fn = [](StrategyContext& ctx, std::size_t k, const Message&) {
        if (k == 1) ctx.submit_limit(Side::Buy, 1100, 10, TimeInForce::IOC);  // pay 0.11
    };
    auto cfg = cfg_short();
    Backtester bt(c.view(), {}, cfg);
    auto r = bt.run(st);
    EXPECT_TRUE(r.forced_close);
    EXPECT_EQ(r.portfolio.position, 0);
    EXPECT_EQ(r.sample_ts.size(), 101u);  // 0..100 inclusive at 1s
    // Prices are LOBSTER units (1100 == $0.11). Equity at t=2s after buying 10 @ 0.11 with mid 0.105: 10*(0.105-0.11) - fee
    EXPECT_NEAR(r.equity[2], 10 * (0.105 - 0.11) - 10 * 0.0030, 1e-9);
}

TEST(Backtester, OfiStrategyRunsOnSynthetic) {
    // Alternating heavy bid adds should push OFI z-score up and trigger a buy.
    Cols c;
    Ts t = 0;
    c.push(t += 1000, 1, 1, 100, 1000, +1); c.push(t += 1000, 1, 2, 100, 1100, -1);
    OrderId id = 10;
    for (int i = 0; i < 3000; ++i) {   // noise: add/delete small orders
        c.push(t += 1000, 1, id, 10, (i % 2) ? 1000 : 1100, (i % 2) ? +1 : -1);
        c.push(t += 1000, 3, id, 10, (i % 2) ? 1000 : 1100, (i % 2) ? +1 : -1);
        ++id;
    }
    for (int i = 0; i < 100; ++i) c.push(t += 1000, 1, id++, 500, 1000, +1);  // burst of bid demand
    for (int i = 0; i < 50; ++i)  c.push(t += 1000, 4, 2, 1, 1100, -1);        // ask gets eaten
    OfiStrategyParams p; p.window_events = 20; p.zscore_window = 200; p.entry_z = 2.0; p.passive_entry = false; p.cooldown_ns = 0;
    OfiStrategy strat(p);
    BacktestConfig cfg; cfg.start_ts = 0; cfg.end_ts = t + 10 * S; cfg.flatten_before_close_ns = S;
    Backtester bt(c.view(), {}, cfg);
    auto r = bt.run(strat);
    EXPECT_GT(r.orders_submitted, 0);
    EXPECT_GE(r.fills.size(), 1u);
    EXPECT_EQ(r.portfolio.position, 0);
}
