#include <gtest/gtest.h>

#include "lob/fill_model.hpp"

using namespace lob;

namespace {
SimOrder make(Side s, Price p, Qty q) { SimOrder o; o.id = 1; o.side = s; o.price = p; o.qty = q; return o; }
Message msg(MsgType t, Side s, Price p, Qty q, Ts ts = 10) { return Message{ts, t, 99, q, p, s}; }
}  // namespace

TEST(FillModel, PassiveArrivalJoinsBackOfQueue) {
    OrderBook b; b.add(1, Side::Buy, 1000, 300, 0); b.add(2, Side::Sell, 1100, 100, 0);
    FillModel fm(CancelAssumption::ProRata, FeeSchedule{});
    std::vector<Fill> out;
    SimOrder o = make(Side::Buy, 1000, 100);
    fm.on_arrival(o, b, 5, out);
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(o.state, SimOrderState::Live);
    EXPECT_EQ(o.queue_ahead, 300);
}

TEST(FillModel, QueueDepletesThenFills) {
    FillModel fm(CancelAssumption::ProRata, FeeSchedule{});
    std::vector<Fill> out;
    SimOrder o = make(Side::Buy, 1000, 100); o.state = SimOrderState::Live; o.queue_ahead = 300;
    fm.on_market(o, msg(MsgType::ExecVisible, Side::Buy, 1000, 200), 300, out);
    EXPECT_TRUE(out.empty()); EXPECT_EQ(o.queue_ahead, 100);
    fm.on_market(o, msg(MsgType::ExecVisible, Side::Buy, 1000, 150), 100, out);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].qty, 50); EXPECT_TRUE(out[0].passive); EXPECT_EQ(out[0].price, 1000);
    EXPECT_DOUBLE_EQ(out[0].fee, -0.0020 * 50);
    EXPECT_EQ(o.queue_ahead, 0); EXPECT_EQ(o.remaining(), 50);
    fm.on_market(o, msg(MsgType::ExecVisible, Side::Buy, 1000, 500), 0, out);
    EXPECT_EQ(o.state, SimOrderState::Filled); EXPECT_EQ(out.back().qty, 50);
}

TEST(FillModel, TradeAtWorsePriceFillsUs) {
    FillModel fm(CancelAssumption::ProRata, FeeSchedule{});
    std::vector<Fill> out;
    SimOrder o = make(Side::Buy, 1000, 100); o.state = SimOrderState::Live; o.queue_ahead = 999;
    fm.on_market(o, msg(MsgType::ExecVisible, Side::Buy, 900, 60), 0, out);  // bid at 9.00 hit; ours is 10.00
    ASSERT_EQ(out.size(), 1u); EXPECT_EQ(out[0].qty, 60); EXPECT_EQ(out[0].price, 1000);
    // Sell-side trades are irrelevant to a bid.
    fm.on_market(o, msg(MsgType::ExecVisible, Side::Sell, 1100, 60), 0, out);
    EXPECT_EQ(out.size(), 1u);
}

TEST(FillModel, CancelAssumptions) {
    std::vector<Fill> out;
    const Message c = msg(MsgType::Delete, Side::Buy, 1000, 100);
    SimOrder o = make(Side::Buy, 1000, 10); o.state = SimOrderState::Live; o.queue_ahead = 200;
    FillModel(CancelAssumption::Pessimistic, FeeSchedule{}).on_market(o, c, 400, out);
    EXPECT_EQ(o.queue_ahead, 200);
    FillModel(CancelAssumption::Optimistic, FeeSchedule{}).on_market(o, c, 400, out);
    EXPECT_EQ(o.queue_ahead, 100);
    o.queue_ahead = 200;
    FillModel(CancelAssumption::ProRata, FeeSchedule{}).on_market(o, c, 400, out);  // 100 * 200/400 = 50
    EXPECT_EQ(o.queue_ahead, 150);
}

TEST(FillModel, OppositeCrossingLimitFillsUs) {
    FillModel fm(CancelAssumption::ProRata, FeeSchedule{});
    std::vector<Fill> out;
    SimOrder o = make(Side::Sell, 1100, 100); o.state = SimOrderState::Live; o.queue_ahead = 50;
    fm.on_market(o, msg(MsgType::NewLimit, Side::Buy, 1150, 30), 0, out);  // buyer bids through our ask
    ASSERT_EQ(out.size(), 1u); EXPECT_EQ(out[0].qty, 30); EXPECT_EQ(out[0].price, 1100);
    fm.on_market(o, msg(MsgType::NewLimit, Side::Buy, 1050, 30), 0, out);  // does not cross
    EXPECT_EQ(out.size(), 1u);
}

TEST(FillModel, AggressiveWalksLevels) {
    OrderBook b;
    b.add(1, Side::Sell, 1100, 40, 0); b.add(2, Side::Sell, 1150, 40, 0); b.add(3, Side::Sell, 1200, 40, 0);
    FillModel fm(CancelAssumption::ProRata, FeeSchedule{});
    std::vector<Fill> out;
    SimOrder o = make(Side::Buy, 1150, 100); o.tif = TimeInForce::IOC;
    fm.on_arrival(o, b, 7, out);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].price, 1100); EXPECT_EQ(out[0].qty, 40); EXPECT_FALSE(out[0].passive);
    EXPECT_EQ(out[1].price, 1150); EXPECT_EQ(out[1].qty, 40);
    EXPECT_EQ(o.state, SimOrderState::Cancelled);  // IOC remainder (20) expires
    EXPECT_EQ(o.filled, 80);
    // GTC remainder rests at the limit with nobody ahead.
    std::vector<Fill> out2;
    SimOrder g = make(Side::Buy, 1150, 100);
    fm.on_arrival(g, b, 7, out2);
    EXPECT_EQ(g.state, SimOrderState::Live); EXPECT_EQ(g.remaining(), 20); EXPECT_EQ(g.queue_ahead, 0);
}

TEST(FillModel, MarketOrderTakesEverythingAvailable) {
    OrderBook b; b.add(1, Side::Buy, 1000, 30, 0);
    FillModel fm(CancelAssumption::ProRata, FeeSchedule{});
    std::vector<Fill> out;
    SimOrder o = make(Side::Sell, kNoBid, 100); o.is_market = true; o.tif = TimeInForce::IOC;
    fm.on_arrival(o, b, 7, out);
    ASSERT_EQ(out.size(), 1u); EXPECT_EQ(out[0].qty, 30);
    EXPECT_EQ(o.state, SimOrderState::Cancelled);
}
