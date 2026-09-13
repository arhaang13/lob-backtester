// book_side.hpp - all price levels on one side (bids or asks), best first.
//
// WHY std::map?  We need three things from the level container:
//   1. find-or-create a level by exact price            (add)
//   2. erase a level by price when it empties           (remove/execute)
//   3. iterate from the best price outward               (best(), depth(k))
// An ordered map gives all three with O(log L), where L is the number of live
// levels (typically a few hundred for a liquid US stock), and its nodes never
// move, so PriceLevel* held by Orders stay valid. The comparator picks the
// direction: bids sort descending (highest first), asks ascending.
//
// ALTERNATIVE (interview talking point): a dense std::vector indexed by tick
// gives true O(1) find-or-create at the cost of memory proportional to the
// price range and a scan to find the next best level when the top empties.
#pragma once
#include <cstddef>
#include <functional>
#include <map>
#include <vector>

#include "lob/price_level.hpp"
#include "lob/types.hpp"

namespace lob {

struct LevelView {
    Price         price;
    Qty           qty;         // visible: identified + anonymous
    std::uint32_t num_orders;  // identified orders only
};

template <class Compare>
class BookSide {
public:
    using Map = std::map<Price, PriceLevel, Compare>;

    PriceLevel* find(Price p) noexcept {
        auto it = levels_.find(p);
        return it == levels_.end() ? nullptr : &it->second;
    }
    const PriceLevel* find(Price p) const noexcept {
        auto it = levels_.find(p);
        return it == levels_.end() ? nullptr : &it->second;
    }

    PriceLevel& get_or_create(Price p) {
        auto [it, inserted] = levels_.try_emplace(p);
        if (inserted) it->second.price = p;
        return it->second;
    }

    // Drop the level if nothing (identified or anonymous) remains at it.
    void erase_if_empty(PriceLevel* lvl) {
        if (lvl->empty()) levels_.erase(lvl->price);
    }

    const PriceLevel* best() const noexcept {
        return levels_.empty() ? nullptr : &levels_.begin()->second;
    }

    std::size_t num_levels() const noexcept { return levels_.size(); }
    bool        empty() const noexcept { return levels_.empty(); }

    // Top-k snapshot, best first.
    std::vector<LevelView> depth(std::size_t k) const {
        std::vector<LevelView> out;
        out.reserve(k);
        for (auto it = levels_.begin(); it != levels_.end() && out.size() < k; ++it)
            out.push_back({it->first, it->second.visible_qty(), it->second.num_orders});
        return out;
    }

    // Visit levels best-first; `f(const PriceLevel&)` returns false to stop.
    template <class F>
    void for_each(F&& f) const {
        for (const auto& kv : levels_)
            if (!f(kv.second)) break;
    }

    const Map& levels() const noexcept { return levels_; }

private:
    Map levels_;
};

using BidSide = BookSide<std::greater<Price>>;  // highest bid first
using AskSide = BookSide<std::less<Price>>;     // lowest ask first

}  // namespace lob
