#include "lob/fill_model.hpp"

#include <algorithm>
#include <cmath>

namespace lob {

void FillModel::fill(SimOrder& o, Ts ts, Price px, Qty q, bool passive, std::vector<Fill>& out) const {
    if (q <= 0) return;
    o.filled += q;
    out.push_back(Fill{ts, o.id, o.side, px, q, passive, fees_.fee(passive, q, px)});
    if (o.remaining() == 0) o.state = SimOrderState::Filled;
}

void FillModel::on_arrival(SimOrder& o, const OrderBook& book, Ts now, std::vector<Fill>& out) const {
    o.arrival_ts = now;
    o.state      = SimOrderState::Live;

    // 1. Marketable part: walk the opposite side while its price is within our limit.
    const Side opp = opposite(o.side);
    auto walk = [&](const auto& side_book) {
        side_book.for_each([&](const PriceLevel& lvl) {
            if (o.remaining() <= 0) return false;
            // For a buy, a level is reachable if lvl.price <= o.price, i.e. NOT better(opp=Sell, o.price, lvl.price)... 
            // spelled out: reachable when our price is at least as aggressive as the level's price.
            const bool reachable = (o.side == Side::Buy) ? lvl.price <= o.price : lvl.price >= o.price;
            if (!reachable) return false;
            const Qty take = std::min<Qty>(lvl.visible_qty(), o.remaining());
            fill(o, now, lvl.price, take, /*passive=*/false, out);
            return true;
        });
    };
    if (opp == Side::Sell) walk(book.asks()); else walk(book.bids());

    if (o.remaining() == 0) return;  // fully filled aggressively

    // 2. Remainder: IOC / market orders expire; GTC rests behind the visible queue.
    if (o.is_market || o.tif == TimeInForce::IOC) {
        o.state = SimOrderState::Cancelled;
        return;
    }
    o.queue_ahead = book.level_qty(o.side, o.price);
}

void FillModel::on_market(SimOrder& o, const Message& m, Qty level_qty_before, std::vector<Fill>& out) const {
    if (o.state != SimOrderState::Live || o.remaining() <= 0) return;

    switch (m.type) {
    case MsgType::ExecVisible:
    case MsgType::ExecHidden: {
        if (m.side != o.side) return;  // trade on the other side of the book
        if (better(o.side, o.price, m.price)) {
            // Trade printed at a worse price than ours: we were first in line.
            fill(o, m.ts, o.price, std::min<Qty>(m.size, o.remaining()), true, out);
        } else if (m.price == o.price) {
            if (m.type == MsgType::ExecHidden) {
                if (o.queue_ahead == 0) fill(o, m.ts, o.price, std::min<Qty>(m.size, o.remaining()), true, out);
            } else {
                const Qty ahead = std::min<Qty>(m.size, o.queue_ahead);
                o.queue_ahead -= ahead;
                fill(o, m.ts, o.price, std::min<Qty>(m.size - ahead, o.remaining()), true, out);
            }
        }
        return;
    }
    case MsgType::PartialCancel:
    case MsgType::Delete: {
        if (m.side != o.side || m.price != o.price || o.queue_ahead <= 0) return;
        switch (ca_) {
        case CancelAssumption::Pessimistic: break;
        case CancelAssumption::Optimistic:
            o.queue_ahead -= std::min<Qty>(m.size, o.queue_ahead); break;
        case CancelAssumption::ProRata: {
            if (level_qty_before <= 0) break;
            const double frac = static_cast<double>(o.queue_ahead) / static_cast<double>(level_qty_before);
            const Qty red = static_cast<Qty>(std::lround(static_cast<double>(m.size) * frac));
            o.queue_ahead -= std::min<Qty>(red, o.queue_ahead);
            break;
        }
        }
        return;
    }
    case MsgType::NewLimit: {
        // Opposite-side order priced through us would have hit us instead of resting.
        if (m.side == o.side) return;
        const bool crosses = (o.side == Side::Buy) ? m.price <= o.price : m.price >= o.price;
        if (crosses) fill(o, m.ts, o.price, std::min<Qty>(m.size, o.remaining()), true, out);
        return;
    }
    default: return;
    }
}

}  // namespace lob
