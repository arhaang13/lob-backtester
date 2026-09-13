#include "lob/lobster.hpp"

#include <vector>

namespace lob {

bool apply(OrderBook& book, const Message& m, ApplyStats& st, const ApplyOptions& opt) {
    ++st.messages;
    switch (m.type) {
    case MsgType::NewLimit:
        if (m.size <= 0 || m.price <= 0) { ++st.ignored; return false; }
        if (opt.price_priority_purge) {
            // A visible order resting at m.price means nothing on the other side
            // is priced through it (it would have executed instead of resting).
            const Side opp = opposite(m.side);
            // purge opposite levels at or better (for opp) than m.price: for a
            // new bid at p, asks with price <= p; for a new ask at p, bids >= p.
            const Price limit = m.price + (opp == Side::Sell ? 1 : -1);
            if (book.purge_better_than(opp, limit)) ++st.priority_purges;
        }
        book.add(m.id, m.side, m.price, m.size, m.ts);
        return true;

    case MsgType::PartialCancel:
        if (!book.cancel(m.id, m.size)) { ++st.unknown_id; book.reduce_anon(m.side, m.price, m.size); }
        return true;

    case MsgType::Delete:
        if (!book.remove(m.id)) { ++st.unknown_id; book.reduce_anon(m.side, m.price, m.size); }
        return true;

    case MsgType::ExecVisible:
        if (opt.price_priority_purge && book.purge_better_than(m.side, m.price)) ++st.priority_purges;
        if (!book.execute(m.id, m.size)) { ++st.unknown_id; book.reduce_anon(m.side, m.price, m.size); }
        return true;

    case MsgType::ExecHidden:
        ++st.hidden_execs;
        // Visible orders at a better price would have traded before this hidden one.
        if (opt.price_priority_purge && m.price > 0 && book.purge_better_than(m.side, m.price)) ++st.priority_purges;
        return false;
    case MsgType::Cross:      ++st.crosses;      return false;
    case MsgType::Halt:       ++st.halts;        return false;
    }
    ++st.ignored;
    return false;
}

namespace {
struct SnapLevel { Price px; Qty sz; };

template <class SideT>
void resync_side(OrderBook& book, Side side, const SideT& ours, const std::vector<SnapLevel>& snap, ResyncStats& st) {
    // Both lists are best-first. Levels of ours worse than the snapshot's last
    // level are outside the window and left alone.
    struct Fix { int kind; Price px; Qty sz; };  // 0 = add anon, 1 = purge, 2 = set visible
    std::vector<Fix> fixes;
    const bool bounded = !snap.empty();
    const Price boundary = bounded ? snap.back().px : 0;
    std::size_t j = 0;
    ours.for_each([&](const PriceLevel& l) {
        if (bounded && better(side, boundary, l.price)) return false;  // beyond the window
        while (j < snap.size() && better(side, snap[j].px, l.price)) { fixes.push_back({0, snap[j].px, snap[j].sz}); ++j; }
        if (j < snap.size() && snap[j].px == l.price) {
            if (snap[j].sz != l.visible_qty()) fixes.push_back({2, l.price, snap[j].sz});
            ++j;
        } else {
            fixes.push_back({1, l.price, 0});
        }
        return true;
    });
    for (; j < snap.size(); ++j) fixes.push_back({0, snap[j].px, snap[j].sz});
    for (const Fix& f : fixes) {
        switch (f.kind) {
        case 0: book.add_anon(side, f.px, f.sz); ++st.levels_added; break;
        case 1: st.orders_trimmed += book.purge_level(side, f.px); ++st.levels_purged; break;
        case 2: st.orders_trimmed += book.set_level_visible(side, f.px, f.sz); ++st.qty_adjusted; break;
        }
    }
}
}  // namespace

void resync(OrderBook& book, const SnapshotColumns& snap, std::size_t row, ResyncStats& st) {
    if (!snap.valid()) return;
    ++st.rows;
    std::vector<SnapLevel> asks, bids;
    asks.reserve(snap.k); bids.reserve(snap.k);
    const std::size_t base = row * snap.k;
    for (std::size_t j = 0; j < snap.k; ++j) {
        const Price ap = snap.ask_px[base + j]; const Qty as = snap.ask_sz[base + j];
        const Price bp = snap.bid_px[base + j]; const Qty bs = snap.bid_sz[base + j];
        if (as > 0 && ap < kNoAsk) asks.push_back({ap, as});
        if (bs > 0 && bp > kNoBid) bids.push_back({bp, bs});
    }
    resync_side(book, Side::Sell, book.asks(), asks, st);
    resync_side(book, Side::Buy,  book.bids(), bids, st);
}

}  // namespace lob
