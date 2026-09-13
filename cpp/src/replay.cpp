#include "lob/replay.hpp"

namespace lob {

namespace {
void write_snapshot(const OrderBook& book, std::size_t i, ReplayResult& r) {
    const std::size_t k = r.depth_k;
    auto asks = book.depth(Side::Sell, k);
    auto bids = book.depth(Side::Buy, k);
    for (std::size_t j = 0; j < k; ++j) {
        const std::size_t o = i * k + j;
        if (j < asks.size()) { r.ask_px[o] = asks[j].price; r.ask_sz[o] = asks[j].qty; }
        else                 { r.ask_px[o] = kNoAsk;        r.ask_sz[o] = 0; }
        if (j < bids.size()) { r.bid_px[o] = bids[j].price; r.bid_sz[o] = bids[j].qty; }
        else                 { r.bid_px[o] = kNoBid;        r.bid_sz[o] = 0; }
    }
}
}  // namespace

ReplayResult replay(const MessageColumns& cols, const std::vector<SeedLevel>& seed,
                    const ReplayOptions& opt) {
    const std::size_t n = cols.n;
    ReplayResult r;
    r.depth_k = opt.depth_k;
    r.best_bid.resize(n); r.best_ask.resize(n);
    r.bid_qty.resize(n);  r.ask_qty.resize(n);
    r.ofi_e.resize(n);    r.trade_qty.resize(n); r.trade_side.resize(n);
    if (opt.depth_k) {
        r.ask_px.resize(n * opt.depth_k); r.ask_sz.resize(n * opt.depth_k);
        r.bid_px.resize(n * opt.depth_k); r.bid_sz.resize(n * opt.depth_k);
    }

    OrderBook book;
    for (const auto& s : seed) book.add_anon(s.side, s.price, s.qty);

    // L1 state before the current message, for the OFI term.
    Price pb0 = book.best_bid_price(), pa0 = book.best_ask_price();
    Qty   qb0 = book.best_bid_qty(),   qa0 = book.best_ask_qty();

    auto record = [&](std::size_t i) {
        r.best_bid[i] = book.best_bid_price(); r.best_ask[i] = book.best_ask_price();
        r.bid_qty[i]  = book.best_bid_qty();   r.ask_qty[i]  = book.best_ask_qty();
        if (opt.depth_k) write_snapshot(book, i, r);
    };

    // Rows before `start` are not applied (they are already reflected in the seed).
    for (std::size_t i = 0; i < opt.start && i < n; ++i) {
        record(i);
        r.ofi_e[i] = 0; r.trade_qty[i] = 0; r.trade_side[i] = 0;
    }

    for (std::size_t i = opt.start; i < n; ++i) {
        const Message m = cols.at(i);
        apply(book, m, r.apply_stats, ApplyOptions{opt.price_priority_purge});
        if (opt.snapshot.valid()) resync(book, opt.snapshot, i, r.resync_stats);

        const Price pb1 = book.best_bid_price(), pa1 = book.best_ask_price();
        const Qty   qb1 = book.best_bid_qty(),   qa1 = book.best_ask_qty();
        // Only compute OFI when both sides exist before and after (avoids sentinels).
        const bool ok = pb0 != kNoBid && pa0 != kNoAsk && pb1 != kNoBid && pa1 != kNoAsk;
        r.ofi_e[i] = ok ? ofi_term(pb0, qb0, pa0, qa0, pb1, qb1, pa1, qa1) : 0;
        pb0 = pb1; pa0 = pa1; qb0 = qb1; qa0 = qa1;

        const bool is_trade = m.type == MsgType::ExecVisible || m.type == MsgType::ExecHidden;
        r.trade_qty[i]  = is_trade ? m.size : 0;
        r.trade_side[i] = is_trade ? static_cast<std::int8_t>(m.side) : 0;
        record(i);
    }
    r.book_stats = book.stats();
    return r;
}

}  // namespace lob
