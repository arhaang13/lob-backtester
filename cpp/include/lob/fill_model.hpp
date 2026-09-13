// fill_model.hpp - decides when a simulated order would have been filled.
//
// PASSIVE (resting) order at price p on side s:
//   On arrival we join the back of the queue: queue_ahead = visible qty at p.
//   Then, as history unfolds:
//     * execution (type 4) of a resting order on side s at price p: the
//       aggressor's shares first eat the queue ahead of us, the excess fills us.
//     * execution on side s at a price WORSE than p (e.g. a trade prints on
//       the bid at 100.00 while our bid is 100.01): the aggressor would have
//       hit our better price first -> we fill up to the traded size.
//     * hidden execution (type 5) at p: visible orders have priority over
//       hidden at the same price, so if nobody is ahead of us we fill.
//     * cancel/delete at p on side s: someone ahead of us may have left.
//       We cannot know if they were ahead or behind, so the assumption is a
//       config knob: Pessimistic (behind us, no change), ProRata (reduce
//       queue_ahead in proportion to its share of the level), Optimistic
//       (all of it was ahead).
//     * new limit order on the OPPOSITE side at a price that crosses p: in
//       the real market it would have traded against us instead of resting.
// AGGRESSIVE (marketable) order: walks the opposite side's visible depth
//   level by level at arrival time and pays each level's price.
//   The walk does not remove liquidity from the historical book: market
//   impact is not modelled (documented limitation).
#pragma once
#include <vector>

#include "lob/costs.hpp"
#include "lob/event.hpp"
#include "lob/lobster.hpp"
#include "lob/order_book.hpp"

namespace lob {

enum class CancelAssumption : std::int8_t { Pessimistic = 0, ProRata = 1, Optimistic = 2 };

class FillModel {
public:
    FillModel(CancelAssumption ca, FeeSchedule fees) : ca_(ca), fees_(fees) {}

    // Order reaches the exchange. Fills the marketable part, then either rests
    // (GTC) or expires (IOC). Appends fills to `out`.
    void on_arrival(SimOrder& o, const OrderBook& book, Ts now, std::vector<Fill>& out) const;

    // A historical message was just applied. `level_qty_before` is the
    // visible qty at (o.side, o.price) before the message (for pro-rata).
    void on_market(SimOrder& o, const Message& m, Qty level_qty_before, std::vector<Fill>& out) const;

private:
    void fill(SimOrder& o, Ts ts, Price px, Qty q, bool passive, std::vector<Fill>& out) const;

    CancelAssumption ca_;
    FeeSchedule      fees_;
};

}  // namespace lob
