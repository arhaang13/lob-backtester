#include <gtest/gtest.h>

#include <map>
#include <random>

#include "lob/order_book.hpp"

using namespace lob;

TEST(OrderBook, AddAndBest) {
    OrderBook b;
    b.add(1, Side::Buy,  1000, 100, 0);
    b.add(2, Side::Buy,  1100, 50,  1);
    b.add(3, Side::Sell, 1200, 70,  2);
    b.add(4, Side::Sell, 1300, 10,  3);
    EXPECT_EQ(b.best_bid_price(), 1100); EXPECT_EQ(b.best_bid_qty(), 50);
    EXPECT_EQ(b.best_ask_price(), 1200); EXPECT_EQ(b.best_ask_qty(), 70);
    EXPECT_EQ(b.spread(), 100); EXPECT_EQ(b.num_orders(), 4u);
    EXPECT_TRUE(b.check_invariants());
}

TEST(OrderBook, DuplicateIdRejected) {
    OrderBook b;
    EXPECT_NE(b.add(1, Side::Buy, 1000, 100, 0), nullptr);
    EXPECT_EQ(b.add(1, Side::Buy, 1000, 100, 0), nullptr);
    EXPECT_EQ(b.stats().duplicate_ids, 1u);
    EXPECT_EQ(b.best_bid_qty(), 100);
}

TEST(OrderBook, PartialThenFullCancel) {
    OrderBook b;
    b.add(1, Side::Buy, 1000, 100, 0);
    b.add(2, Side::Buy, 1000, 40, 1);
    EXPECT_TRUE(b.cancel(1, 30));
    EXPECT_EQ(b.find(1)->qty, 70); EXPECT_EQ(b.best_bid_qty(), 110);
    EXPECT_TRUE(b.remove(1));
    EXPECT_EQ(b.find(1), nullptr); EXPECT_EQ(b.best_bid_qty(), 40);
    EXPECT_EQ(b.best_bid()->head->id, 2);
    EXPECT_FALSE(b.remove(1));
    EXPECT_TRUE(b.check_invariants());
}

TEST(OrderBook, RemovingBestLevelRestoresNextBest) {
    OrderBook b;
    b.add(1, Side::Sell, 1200, 10, 0);
    b.add(2, Side::Sell, 1250, 10, 0);
    EXPECT_TRUE(b.execute(1, 10));
    EXPECT_EQ(b.best_ask_price(), 1250);
    EXPECT_EQ(b.asks().num_levels(), 1u);
    EXPECT_TRUE(b.execute(2, 10));
    EXPECT_EQ(b.best_ask(), nullptr);
    EXPECT_EQ(b.best_ask_price(), kNoAsk);
    EXPECT_TRUE(b.check_invariants());
}

TEST(OrderBook, OverExecuteClampsAndCounts) {
    OrderBook b;
    b.add(1, Side::Sell, 1200, 10, 0);
    EXPECT_TRUE(b.execute(1, 25));
    EXPECT_EQ(b.stats().over_qty, 1u);
    EXPECT_EQ(b.num_orders(), 0u);
    EXPECT_TRUE(b.check_invariants());
}

TEST(OrderBook, AnonLiquidity) {
    OrderBook b;
    b.add_anon(Side::Buy, 1000, 300);
    b.add(7, Side::Buy, 1000, 50, 0);
    EXPECT_EQ(b.best_bid_qty(), 350);
    EXPECT_EQ(b.best_bid()->qty_ahead(b.find(7)), 300);
    EXPECT_EQ(b.reduce_anon(Side::Buy, 1000, 100), 100);
    EXPECT_EQ(b.reduce_anon(Side::Buy, 1000, 500), 200);  // clamped
    EXPECT_EQ(b.stats().anon_underflow, 1u);
    EXPECT_EQ(b.best_bid_qty(), 50);
    b.remove(7);
    EXPECT_EQ(b.best_bid(), nullptr);  // level gone once both parts are zero
    EXPECT_TRUE(b.check_invariants());
}

TEST(OrderBook, DepthSnapshot) {
    OrderBook b;
    for (int i = 0; i < 5; ++i) b.add(i + 1, Side::Buy, 1000 - 100 * i, 10 * (i + 1), 0);
    auto d = b.depth(Side::Buy, 3);
    ASSERT_EQ(d.size(), 3u);
    EXPECT_EQ(d[0].price, 1000); EXPECT_EQ(d[0].qty, 10);
    EXPECT_EQ(d[2].price, 800);  EXPECT_EQ(d[2].qty, 30);
}

// Fuzz: random ops against a naive reference (multimap of price -> orders).
TEST(OrderBook, FuzzAgainstReference) {
    struct Ref { Side side; Price price; Qty qty; };
    std::map<OrderId, Ref> ref;
    OrderBook b;
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<int> op(0, 99), px(1, 60), qd(1, 500);
    OrderId next = 1;
    for (int it = 0; it < 300000; ++it) {
        int o = op(rng);
        if (o < 45 || ref.empty()) {
            Side s = (rng() & 1) ? Side::Buy : Side::Sell;
            Price p = 100000 + px(rng) * kTick;
            Qty q = qd(rng);
            ref[next] = {s, p, q};
            ASSERT_NE(b.add(next, s, p, q, it), nullptr);
            ++next;
        } else {
            auto rit = ref.begin(); std::advance(rit, static_cast<long>(rng() % ref.size()));
            OrderId id = rit->first;
            if (o < 65) {                           // partial cancel
                Qty q = std::max<Qty>(1, rit->second.qty / 2);
                if (q >= rit->second.qty) ref.erase(rit); else rit->second.qty -= q;
                ASSERT_TRUE(b.cancel(id, q));
            } else if (o < 85) {                    // execute
                Qty q = std::min<Qty>(rit->second.qty, qd(rng));
                if (q >= rit->second.qty) ref.erase(rit); else rit->second.qty -= q;
                ASSERT_TRUE(b.execute(id, q));
            } else {                                // delete
                ref.erase(rit);
                ASSERT_TRUE(b.remove(id));
            }
        }
        if (it % 20000 == 0) ASSERT_TRUE(b.check_invariants());
    }
    ASSERT_TRUE(b.check_invariants());
    // Compare aggregated levels.
    std::map<Price, Qty> rb, ra;
    for (auto& [id, r] : ref) (r.side == Side::Buy ? rb : ra)[r.price] += r.qty;
    EXPECT_EQ(b.num_orders(), ref.size());
    EXPECT_EQ(b.bids().num_levels(), rb.size());
    EXPECT_EQ(b.asks().num_levels(), ra.size());
    for (auto& [p, q] : rb) EXPECT_EQ(b.level_qty(Side::Buy, p), q);
    for (auto& [p, q] : ra) EXPECT_EQ(b.level_qty(Side::Sell, p), q);
    if (!rb.empty()) EXPECT_EQ(b.best_bid_price(), rb.rbegin()->first);
    if (!ra.empty()) EXPECT_EQ(b.best_ask_price(), ra.begin()->first);
}
