#include "lob/order_book.hpp"

#include <cassert>
#include <vector>

namespace lob {

Order* OrderBook::add(OrderId id, Side side, Price price, Qty qty, Ts ts) {
    // try_emplace first: if the id already exists we must not touch the book.
    auto [it, inserted] = index_.try_emplace(id, nullptr);
    if (!inserted) { ++stats_.duplicate_ids; return nullptr; }

    Order* o = pool_.alloc();
    o->id = id; o->side = side; o->price = price; o->qty = qty; o->ts = ts;

    PriceLevel& lvl = (side == Side::Buy) ? bids_.get_or_create(price)
                                          : asks_.get_or_create(price);
    lvl.push_back(o);
    it->second = o;
    ++stats_.adds;
    return o;
}

bool OrderBook::reduce_identified(OrderId id, Qty qty, bool is_exec) {
    auto it = index_.find(id);
    if (it == index_.end()) return false;
    Order* o = it->second;

    if (qty >= o->qty) {
        if (qty > o->qty) ++stats_.over_qty;
        // Reducing to zero == removing. Keep the accounting consistent.
        if (is_exec) ++stats_.executes; else ++stats_.cancels;
        index_.erase(it);
        erase_order(o);
        return true;
    }
    o->level->reduce(o, qty);
    if (is_exec) ++stats_.executes; else ++stats_.cancels;
    return true;
}

bool OrderBook::cancel(OrderId id, Qty qty)  { return reduce_identified(id, qty, /*is_exec=*/false); }
bool OrderBook::execute(OrderId id, Qty qty) { return reduce_identified(id, qty, /*is_exec=*/true); }

bool OrderBook::remove(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end()) return false;
    Order* o = it->second;
    index_.erase(it);
    erase_order(o);
    ++stats_.removes;
    return true;
}

void OrderBook::erase_order(Order* o) {
    PriceLevel* lvl = o->level;
    const Side  side = o->side;
    lvl->erase(o);
    pool_.release(o);
    if (side == Side::Buy) bids_.erase_if_empty(lvl); else asks_.erase_if_empty(lvl);
}

void OrderBook::add_anon(Side side, Price price, Qty qty) {
    if (qty <= 0) return;
    PriceLevel& lvl = (side == Side::Buy) ? bids_.get_or_create(price)
                                          : asks_.get_or_create(price);
    lvl.anon_qty += qty;
}

Qty OrderBook::reduce_anon(Side side, Price price, Qty qty) {
    PriceLevel* lvl = (side == Side::Buy) ? bids_.find(price) : asks_.find(price);
    ++stats_.anon_reduce;
    if (!lvl) { ++stats_.anon_underflow; return 0; }
    Qty removed = qty <= lvl->anon_qty ? qty : lvl->anon_qty;
    if (removed < qty) ++stats_.anon_underflow;
    lvl->anon_qty -= removed;
    if (side == Side::Buy) bids_.erase_if_empty(lvl); else asks_.erase_if_empty(lvl);
    return removed;
}

namespace {
template <class SideT>
std::size_t purge_level_impl(SideT& side, PriceLevel* lvl, std::unordered_map<OrderId, Order*>& index, OrderPool& pool) {
    std::size_t n = 0;
    for (Order* o = lvl->head; o;) {
        Order* next = o->next;
        index.erase(o->id);
        pool.release(o);
        o = next; ++n;
    }
    lvl->head = lvl->tail = nullptr; lvl->num_orders = 0; lvl->total_qty = 0; lvl->anon_qty = 0;
    side.erase_if_empty(lvl);
    return n;
}
}  // namespace

std::size_t OrderBook::purge_level(Side side, Price price) {
    std::size_t n = 0;
    if (side == Side::Buy) { if (auto* l = bids_.find(price)) n = purge_level_impl(bids_, l, index_, pool_); }
    else                   { if (auto* l = asks_.find(price)) n = purge_level_impl(asks_, l, index_, pool_); }
    stats_.purged_orders += n;
    return n;
}

std::size_t OrderBook::purge_better_than(Side side, Price limit) {
    std::vector<Price> victims;
    auto collect = [&](const auto& sd) {
        sd.for_each([&](const PriceLevel& l) { if (!better(side, l.price, limit)) return false; victims.push_back(l.price); return true; });
    };
    if (side == Side::Buy) collect(bids_); else collect(asks_);
    for (Price p : victims) purge_level(side, p);
    return victims.size();
}

std::size_t OrderBook::set_level_visible(Side side, Price price, Qty target) {
    if (target <= 0) return purge_level(side, price);
    PriceLevel& lvl = (side == Side::Buy) ? bids_.get_or_create(price) : asks_.get_or_create(price);
    std::size_t trimmed = 0;
    if (lvl.total_qty <= target) {
        lvl.anon_qty = target - lvl.total_qty;
        return 0;
    }
    // Identified qty exceeds what the exchange shows: some of these orders were
    // cancelled while the level was outside the data window. Drop youngest first.
    lvl.anon_qty = 0;
    while (lvl.total_qty > target && lvl.tail) {
        Order* o = lvl.tail;
        const Qty excess = lvl.total_qty - target;
        if (o->qty <= excess) { index_.erase(o->id); lvl.erase(o); pool_.release(o); }
        else                  { lvl.reduce(o, excess); }
        ++trimmed;
    }
    stats_.purged_orders += trimmed;
    return trimmed;
}

Order* OrderBook::find(OrderId id) noexcept {
    auto it = index_.find(id);
    return it == index_.end() ? nullptr : it->second;
}
const Order* OrderBook::find(OrderId id) const noexcept {
    auto it = index_.find(id);
    return it == index_.end() ? nullptr : it->second;
}

bool OrderBook::check_invariants() const {
    std::size_t counted = 0;
    auto check_side = [&](const auto& side, Side s) {
        Price prev = 0; bool first = true;
        for (const auto& [price, lvl] : side.levels()) {
            if (lvl.price != price) return false;
            if (lvl.empty()) return false;                       // empty levels must be erased
            if (!first && !better(s, prev, price)) return false; // strictly ordered best-first
            first = false; prev = price;
            Qty sum = 0; std::uint32_t n = 0;
            for (const Order* o = lvl.head; o; o = o->next) {
                if (o->level != &lvl || o->side != s || o->price != price) return false;
                if (o->next && o->next->prev != o) return false;
                sum += o->qty; ++n;
            }
            if (sum != lvl.total_qty || n != lvl.num_orders) return false;
            if (lvl.tail && lvl.tail->next) return false;
            counted += n;
        }
        return true;
    };
    if (!check_side(bids_, Side::Buy) || !check_side(asks_, Side::Sell)) return false;
    return counted == index_.size() && counted == pool_.live();
}

}  // namespace lob
