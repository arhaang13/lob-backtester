// costs.hpp - transaction cost model.
//
// US equities use maker/taker pricing: an order that *adds* liquidity (rests
// and gets filled passively) earns a rebate; one that *removes* liquidity
// (crosses the spread) pays a fee. 2012 NASDAQ tiers were roughly +$0.0020 /
// -$0.0030 per share. Sign convention here: fee > 0 is a cost, fee < 0 is a
// rebate. Commission in basis points of notional is applied on every fill.
#pragma once
#include "lob/types.hpp"

namespace lob {

struct FeeSchedule {
    double maker_fee_per_share = -0.0020;  // rebate
    double taker_fee_per_share =  0.0030;
    double commission_bps      =  0.0;

    double fee(bool passive, Qty qty, Price price) const noexcept {
        const double per_share = passive ? maker_fee_per_share : taker_fee_per_share;
        const double notional  = static_cast<double>(qty) * static_cast<double>(price) / static_cast<double>(kPriceScale);
        return per_share * static_cast<double>(qty) + commission_bps * 1e-4 * notional;
    }
};

}  // namespace lob
