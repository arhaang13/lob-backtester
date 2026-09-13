// module.cpp - pybind11 bindings for the lob engine.
//
// Data crossing the boundary:
//   Python -> C++ : numpy columns as py::array_t<T, c_style | forcecast>. If the
//                   array already has the right dtype and is contiguous this is
//                   zero-copy (we read through .data()); otherwise pybind makes
//                   one converted copy. We hold the py::array objects for the
//                   duration of the call, so the buffers stay alive.
//   C++ -> Python : std::vector<T> results are moved to the heap and wrapped in
//                   a numpy array whose base is a capsule that deletes the
//                   vector. No copy back into Python.
// GIL: released while the C++ loop runs. Python strategies are called through
// a trampoline whose PYBIND11_OVERRIDE re-acquires the GIL per callback.
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <memory>
#include <stdexcept>
#include <vector>

#include "lob/backtester.hpp"
#include "lob/ofi_strategy.hpp"
#include "lob/order_book.hpp"
#include "lob/replay.hpp"

namespace py = pybind11;
using namespace lob;

namespace {

template <class T>
py::array_t<T> to_numpy(std::vector<T>&& v) {
    auto* heap = new std::vector<T>(std::move(v));
    py::capsule owner(heap, [](void* p) { delete reinterpret_cast<std::vector<T>*>(p); });
    return py::array_t<T>({static_cast<py::ssize_t>(heap->size())}, {static_cast<py::ssize_t>(sizeof(T))},
                          heap->data(), owner);
}
template <class T>
py::array_t<T> to_numpy_2d(std::vector<T>&& v, std::size_t rows, std::size_t cols) {
    auto* heap = new std::vector<T>(std::move(v));
    py::capsule owner(heap, [](void* p) { delete reinterpret_cast<std::vector<T>*>(p); });
    return py::array_t<T>({static_cast<py::ssize_t>(rows), static_cast<py::ssize_t>(cols)},
                          {static_cast<py::ssize_t>(cols * sizeof(T)), static_cast<py::ssize_t>(sizeof(T))},
                          heap->data(), owner);
}

template <class T> using arr = py::array_t<T, py::array::c_style | py::array::forcecast>;

// Bundle of the six message columns; keeps the arrays alive and exposes a view.
struct Columns {
    arr<std::int64_t> ts, id, price;
    arr<std::int8_t>  type, dir;
    arr<std::int32_t> size;
    Columns(arr<std::int64_t> ts_, arr<std::int8_t> type_, arr<std::int64_t> id_, arr<std::int32_t> size_,
            arr<std::int64_t> price_, arr<std::int8_t> dir_)
        : ts(std::move(ts_)), id(std::move(id_)), price(std::move(price_)), type(std::move(type_)),
          dir(std::move(dir_)), size(std::move(size_)) {
        const auto n = ts.size();
        if (type.size() != n || id.size() != n || size.size() != n || price.size() != n || dir.size() != n)
            throw py::value_error("all message columns must have the same length");
        if (ts.ndim() != 1) throw py::value_error("message columns must be 1-D");
    }
    MessageColumns view() const {
        return MessageColumns{ts.data(), type.data(), id.data(), size.data(), price.data(), dir.data(),
                              static_cast<std::size_t>(ts.size())};
    }
};

// Optional (n x k) snapshot arrays from the LOBSTER orderbook file.
struct Snapshot {
    arr<std::int64_t> ask_px, bid_px;
    arr<std::int32_t> ask_sz, bid_sz;
    std::size_t k{0};
    bool valid{false};
    Snapshot() = default;
    explicit Snapshot(py::object obj) {
        if (obj.is_none()) return;
        py::dict d = obj.cast<py::dict>();
        ask_px = d["ask_px"].cast<arr<std::int64_t>>(); ask_sz = d["ask_sz"].cast<arr<std::int32_t>>();
        bid_px = d["bid_px"].cast<arr<std::int64_t>>(); bid_sz = d["bid_sz"].cast<arr<std::int32_t>>();
        if (ask_px.ndim() != 2) throw py::value_error("snapshot arrays must be 2-D (n x k)");
        k = static_cast<std::size_t>(ask_px.shape(1));
        for (auto sh : {ask_sz.shape(1), bid_px.shape(1), bid_sz.shape(1)})
            if (static_cast<std::size_t>(sh) != k) throw py::value_error("snapshot arrays must share shape");
        valid = true;
    }
    SnapshotColumns view() const {
        if (!valid) return SnapshotColumns{};
        return SnapshotColumns{ask_px.data(), ask_sz.data(), bid_px.data(), bid_sz.data(), k};
    }
    std::size_t rows() const { return valid ? static_cast<std::size_t>(ask_px.shape(0)) : 0; }
};

std::vector<SeedLevel> seed_from(const std::vector<std::tuple<int, std::int64_t, std::int32_t>>& seed) {
    std::vector<SeedLevel> out;
    out.reserve(seed.size());
    for (const auto& [side, price, qty] : seed)
        out.push_back(SeedLevel{side >= 0 ? Side::Buy : Side::Sell, price, qty});
    return out;
}

py::dict stats_dict(const ApplyStats& a, const BookStats& b, const ResyncStats* r = nullptr) {
    py::dict d;
    d["priority_purges"] = a.priority_purges; d["purged_orders"] = b.purged_orders;
    if (r) {
        d["resync_rows"] = r->rows; d["resync_levels_added"] = r->levels_added; d["resync_levels_purged"] = r->levels_purged;
        d["resync_qty_adjusted"] = r->qty_adjusted; d["resync_orders_trimmed"] = r->orders_trimmed;
    }
    d["messages"] = a.messages; d["unknown_id"] = a.unknown_id; d["hidden_execs"] = a.hidden_execs;
    d["crosses"] = a.crosses; d["halts"] = a.halts; d["ignored"] = a.ignored;
    d["adds"] = b.adds; d["cancels"] = b.cancels; d["removes"] = b.removes; d["executes"] = b.executes;
    d["anon_reduce"] = b.anon_reduce; d["duplicate_ids"] = b.duplicate_ids; d["over_qty"] = b.over_qty;
    d["anon_underflow"] = b.anon_underflow;
    return d;
}

// Trampoline so Python subclasses of Strategy can override the virtuals.
// Written by hand rather than with PYBIND11_OVERRIDE because the context must
// cross into Python as a POINTER (pybind11 casts lvalue references with policy
// `copy`, and the Backtester is not copyable; a raw pointer is wrapped as a
// non-owning reference). get_override() acquires the GIL for the callback.
class PyStrategy : public Strategy {
public:
    using Strategy::Strategy;
    void on_start(StrategyContext& c) override {
        py::gil_scoped_acquire gil;
        if (py::function f = py::get_override(static_cast<const Strategy*>(this), "on_start")) f(&c);
        else Strategy::on_start(c);
    }
    void on_market(StrategyContext& c, const Message& m, std::int64_t e) override {
        py::gil_scoped_acquire gil;
        py::function f = py::get_override(static_cast<const Strategy*>(this), "on_market");
        if (!f) throw std::runtime_error("Strategy.on_market must be overridden in Python");
        f(&c, Message(m), e);
    }
    void on_fill(StrategyContext& c, const Fill& f_) override {
        py::gil_scoped_acquire gil;
        if (py::function f = py::get_override(static_cast<const Strategy*>(this), "on_fill")) f(&c, Fill(f_));
        else Strategy::on_fill(c, f_);
    }
    void on_timer(StrategyContext& c, Ts t) override {
        py::gil_scoped_acquire gil;
        if (py::function f = py::get_override(static_cast<const Strategy*>(this), "on_timer")) f(&c, t);
        else Strategy::on_timer(c, t);
    }
    void on_end(StrategyContext& c) override {
        py::gil_scoped_acquire gil;
        if (py::function f = py::get_override(static_cast<const Strategy*>(this), "on_end")) f(&c);
        else Strategy::on_end(c);
    }
};

// Owns the numpy columns for the lifetime of the C++ Backtester.
struct PyBacktester {
    Columns cols;
    Snapshot snap;
    std::vector<SeedLevel> seed;
    BacktestConfig cfg;
    std::unique_ptr<Backtester> bt;
    PyBacktester(Columns c, Snapshot sn, std::vector<SeedLevel> s, BacktestConfig cf)
        : cols(std::move(c)), snap(std::move(sn)), seed(std::move(s)), cfg(cf) {
        if (snap.valid && snap.rows() != cols.view().n) throw py::value_error("snapshot rows must equal message count");
        bt = std::make_unique<Backtester>(cols.view(), seed, cfg, snap.view());
    }
    BacktestResult run(Strategy& strat) {
        py::gil_scoped_release release;
        return bt->run(strat);
    }
};

}  // namespace

PYBIND11_MODULE(_lob, m) {
    m.doc() = "L3 limit order book reconstruction and event-driven backtester";

    m.attr("PRICE_SCALE") = kPriceScale;
    m.attr("TICK") = kTick;
    m.attr("NS_PER_SEC") = kNsPerSec;
    m.attr("NO_ASK") = kNoAsk;
    m.attr("NO_BID") = kNoBid;

    py::enum_<Side>(m, "Side").value("Buy", Side::Buy).value("Sell", Side::Sell);
    py::enum_<MsgType>(m, "MsgType")
        .value("NewLimit", MsgType::NewLimit).value("PartialCancel", MsgType::PartialCancel)
        .value("Delete", MsgType::Delete).value("ExecVisible", MsgType::ExecVisible)
        .value("ExecHidden", MsgType::ExecHidden).value("Cross", MsgType::Cross).value("Halt", MsgType::Halt);
    py::enum_<TimeInForce>(m, "TimeInForce").value("GTC", TimeInForce::GTC).value("IOC", TimeInForce::IOC);
    py::enum_<SimOrderState>(m, "SimOrderState")
        .value("Pending", SimOrderState::Pending).value("Live", SimOrderState::Live)
        .value("Filled", SimOrderState::Filled).value("Cancelled", SimOrderState::Cancelled);
    py::enum_<CancelAssumption>(m, "CancelAssumption")
        .value("Pessimistic", CancelAssumption::Pessimistic).value("ProRata", CancelAssumption::ProRata)
        .value("Optimistic", CancelAssumption::Optimistic);

    // ---- order book -----------------------------------------------------------
    py::class_<LevelView>(m, "LevelView")
        .def_readonly("price", &LevelView::price).def_readonly("qty", &LevelView::qty)
        .def_readonly("num_orders", &LevelView::num_orders)
        .def("__repr__", [](const LevelView& l) { return "LevelView(price=" + std::to_string(l.price) + ", qty=" + std::to_string(l.qty) + ", n=" + std::to_string(l.num_orders) + ")"; });

    py::class_<OrderBook>(m, "OrderBook")
        .def(py::init<>())
        .def("add", [](OrderBook& b, OrderId id, Side s, Price p, Qty q, Ts ts) { return b.add(id, s, p, q, ts) != nullptr; },
             py::arg("id"), py::arg("side"), py::arg("price"), py::arg("qty"), py::arg("ts") = 0)
        .def("cancel", &OrderBook::cancel, py::arg("id"), py::arg("qty"))
        .def("remove", &OrderBook::remove, py::arg("id"))
        .def("execute", &OrderBook::execute, py::arg("id"), py::arg("qty"))
        .def("add_anon", &OrderBook::add_anon).def("reduce_anon", &OrderBook::reduce_anon)
        .def("best_bid_price", &OrderBook::best_bid_price).def("best_ask_price", &OrderBook::best_ask_price)
        .def("best_bid_qty", &OrderBook::best_bid_qty).def("best_ask_qty", &OrderBook::best_ask_qty)
        .def("spread", &OrderBook::spread)
        .def("mid", [](const OrderBook& b) { return static_cast<double>(b.mid2()) / 2.0; })
        .def("level_qty", &OrderBook::level_qty)
        .def("depth", &OrderBook::depth, py::arg("side"), py::arg("k") = 10)
        .def("num_orders", &OrderBook::num_orders)
        .def("num_levels", [](const OrderBook& b, Side s) { return s == Side::Buy ? b.bids().num_levels() : b.asks().num_levels(); })
        .def("order", [](const OrderBook& b, OrderId id) -> py::object {
            const Order* o = b.find(id);
            if (!o) return py::none();
            py::dict d; d["id"] = o->id; d["price"] = o->price; d["qty"] = o->qty; d["side"] = o->side; d["ts"] = o->ts;
            d["queue_ahead"] = o->level->qty_ahead(o);
            return d; })
        .def("stats", [](const OrderBook& b) { return stats_dict(ApplyStats{}, b.stats()); })
        .def("check_invariants", &OrderBook::check_invariants);

    py::class_<Message>(m, "Message")
        .def_readonly("ts", &Message::ts).def_readonly("type", &Message::type).def_readonly("id", &Message::id)
        .def_readonly("size", &Message::size).def_readonly("price", &Message::price).def_readonly("side", &Message::side);

    m.def("apply_message", [](OrderBook& b, Ts ts, int type, OrderId id, Qty size, Price price, int dir) {
            ApplyStats st;
            return apply(b, Message{ts, static_cast<MsgType>(type), id, size, price, dir >= 0 ? Side::Buy : Side::Sell}, st); },
          "Apply one LOBSTER message to a book (unknown ids go to anonymous liquidity)");

    // ---- replay -----------------------------------------------------------------
    m.def("replay",
        [](arr<std::int64_t> ts, arr<std::int8_t> type, arr<std::int64_t> id, arr<std::int32_t> size,
           arr<std::int64_t> price, arr<std::int8_t> dir,
           const std::vector<std::tuple<int, std::int64_t, std::int32_t>>& seed, std::size_t start, std::size_t depth_k,
           bool price_priority_purge, py::object snapshot) {
            Columns cols(std::move(ts), std::move(type), std::move(id), std::move(size), std::move(price), std::move(dir));
            auto seed_levels = seed_from(seed);
            Snapshot snap(snapshot);
            if (snap.valid && snap.rows() != cols.view().n) throw py::value_error("snapshot rows must equal message count");
            ReplayOptions opt; opt.start = start; opt.depth_k = depth_k; opt.price_priority_purge = price_priority_purge;
            opt.snapshot = snap.view();
            ReplayResult r;
            {
                py::gil_scoped_release release;
                r = replay(cols.view(), seed_levels, opt);
            }
            const std::size_t n = cols.view().n;
            py::dict d;
            d["best_bid"] = to_numpy(std::move(r.best_bid)); d["best_ask"] = to_numpy(std::move(r.best_ask));
            d["bid_qty"]  = to_numpy(std::move(r.bid_qty));  d["ask_qty"]  = to_numpy(std::move(r.ask_qty));
            d["ofi_e"]    = to_numpy(std::move(r.ofi_e));
            d["trade_qty"] = to_numpy(std::move(r.trade_qty)); d["trade_side"] = to_numpy(std::move(r.trade_side));
            if (depth_k) {
                d["ask_px"] = to_numpy_2d(std::move(r.ask_px), n, depth_k); d["ask_sz"] = to_numpy_2d(std::move(r.ask_sz), n, depth_k);
                d["bid_px"] = to_numpy_2d(std::move(r.bid_px), n, depth_k); d["bid_sz"] = to_numpy_2d(std::move(r.bid_sz), n, depth_k);
            }
            d["stats"] = stats_dict(r.apply_stats, r.book_stats, &r.resync_stats);
            return d;
        },
        py::arg("ts"), py::arg("type"), py::arg("id"), py::arg("size"), py::arg("price"), py::arg("dir"),
        py::arg("seed") = std::vector<std::tuple<int, std::int64_t, std::int32_t>>{}, py::arg("start") = 0,
        py::arg("depth_k") = 0, py::arg("price_priority_purge") = true, py::arg("snapshot") = py::none(),
        "Replay LOBSTER message columns through the book; returns per-event feature arrays.");

    // ---- backtester ----------------------------------------------------------------
    py::class_<FeeSchedule>(m, "FeeSchedule")
        .def(py::init<>())
        .def_readwrite("maker_fee_per_share", &FeeSchedule::maker_fee_per_share)
        .def_readwrite("taker_fee_per_share", &FeeSchedule::taker_fee_per_share)
        .def_readwrite("commission_bps", &FeeSchedule::commission_bps)
        .def("fee", &FeeSchedule::fee);

    py::class_<BacktestConfig>(m, "BacktestConfig")
        .def(py::init<>())
        .def_readwrite("md_latency_ns", &BacktestConfig::md_latency_ns)
        .def_readwrite("order_latency_ns", &BacktestConfig::order_latency_ns)
        .def_readwrite("fees", &BacktestConfig::fees)
        .def_readwrite("cancel_assumption", &BacktestConfig::cancel_assumption)
        .def_readwrite("pnl_sample_ns", &BacktestConfig::pnl_sample_ns)
        .def_readwrite("start_ts", &BacktestConfig::start_ts)
        .def_readwrite("end_ts", &BacktestConfig::end_ts)
        .def_readwrite("flatten_before_close_ns", &BacktestConfig::flatten_before_close_ns)
        .def_readwrite("max_position", &BacktestConfig::max_position)
        .def_readwrite("start_index", &BacktestConfig::start_index)
        .def_readwrite("record_fills", &BacktestConfig::record_fills)
        .def_readwrite("price_priority_purge", &BacktestConfig::price_priority_purge);

    py::class_<SimOrder>(m, "SimOrder")
        .def_readonly("id", &SimOrder::id).def_readonly("side", &SimOrder::side).def_readonly("price", &SimOrder::price)
        .def_readonly("qty", &SimOrder::qty).def_readonly("filled", &SimOrder::filled)
        .def_readonly("queue_ahead", &SimOrder::queue_ahead).def_readonly("submit_ts", &SimOrder::submit_ts)
        .def_readonly("arrival_ts", &SimOrder::arrival_ts).def_readonly("tif", &SimOrder::tif)
        .def_readonly("state", &SimOrder::state).def_readonly("is_market", &SimOrder::is_market)
        .def("remaining", &SimOrder::remaining).def("open", &SimOrder::open);

    py::class_<Fill>(m, "Fill")
        .def_readonly("ts", &Fill::ts).def_readonly("order_id", &Fill::order_id).def_readonly("side", &Fill::side)
        .def_readonly("price", &Fill::price).def_readonly("qty", &Fill::qty).def_readonly("passive", &Fill::passive)
        .def_readonly("fee", &Fill::fee);

    py::class_<Portfolio>(m, "Portfolio")
        .def_readonly("position", &Portfolio::position).def_readonly("cash", &Portfolio::cash)
        .def_readonly("fees", &Portfolio::fees).def_readonly("realized", &Portfolio::realized)
        .def_readonly("avg_cost", &Portfolio::avg_cost).def_readonly("volume", &Portfolio::volume)
        .def_readonly("n_fills", &Portfolio::n_fills);

    py::class_<Metrics>(m, "Metrics")
        .def_readonly("total_pnl", &Metrics::total_pnl).def_readonly("realized_pnl", &Metrics::realized_pnl)
        .def_readonly("fees", &Metrics::fees).def_readonly("max_drawdown", &Metrics::max_drawdown)
        .def_readonly("sharpe", &Metrics::sharpe).def_readonly("volume", &Metrics::volume)
        .def_readonly("n_fills", &Metrics::n_fills).def_readonly("n_orders", &Metrics::n_orders)
        .def_readonly("n_passive_fills", &Metrics::n_passive_fills).def_readonly("n_aggressive_fills", &Metrics::n_aggressive_fills)
        .def_readonly("fill_rate", &Metrics::fill_rate).def_readonly("pnl_per_share", &Metrics::pnl_per_share)
        .def("to_dict", [](const Metrics& x) {
            py::dict d;
            d["total_pnl"] = x.total_pnl; d["realized_pnl"] = x.realized_pnl; d["fees"] = x.fees;
            d["max_drawdown"] = x.max_drawdown; d["sharpe"] = x.sharpe; d["volume"] = x.volume;
            d["n_fills"] = x.n_fills; d["n_orders"] = x.n_orders; d["n_passive_fills"] = x.n_passive_fills;
            d["n_aggressive_fills"] = x.n_aggressive_fills; d["fill_rate"] = x.fill_rate; d["pnl_per_share"] = x.pnl_per_share;
            return d; });

    py::class_<BacktestResult>(m, "BacktestResult")
        .def_readonly("portfolio", &BacktestResult::portfolio)
        .def_readonly("metrics", &BacktestResult::metrics)
        .def_readonly("orders_submitted", &BacktestResult::orders_submitted)
        .def_readonly("orders_rejected", &BacktestResult::orders_rejected)
        .def_readonly("orders_cancelled", &BacktestResult::orders_cancelled)
        .def_readonly("orders_with_fill", &BacktestResult::orders_with_fill)
        .def_readonly("forced_close", &BacktestResult::forced_close)
        .def("stats", [](const BacktestResult& r) { return stats_dict(r.apply_stats, r.book_stats, &r.resync_stats); })
        .def("fills", [](const BacktestResult& r) {
            const std::size_t n = r.fills.size();
            std::vector<std::int64_t> ts(n), oid(n), px(n); std::vector<std::int8_t> side(n), passive(n);
            std::vector<std::int32_t> qty(n); std::vector<double> fee(n);
            for (std::size_t i = 0; i < n; ++i) {
                const Fill& f = r.fills[i];
                ts[i] = f.ts; oid[i] = static_cast<std::int64_t>(f.order_id); px[i] = f.price; side[i] = static_cast<std::int8_t>(f.side);
                passive[i] = f.passive; qty[i] = f.qty; fee[i] = f.fee;
            }
            py::dict d;
            d["ts"] = to_numpy(std::move(ts)); d["order_id"] = to_numpy(std::move(oid)); d["side"] = to_numpy(std::move(side));
            d["price"] = to_numpy(std::move(px)); d["qty"] = to_numpy(std::move(qty)); d["passive"] = to_numpy(std::move(passive));
            d["fee"] = to_numpy(std::move(fee));
            return d; }, "Fills as a dict of numpy arrays")
        .def("samples", [](const BacktestResult& r) {
            py::dict d;
            d["ts"] = to_numpy(std::vector<std::int64_t>(r.sample_ts));
            d["equity"] = to_numpy(std::vector<double>(r.equity));
            d["position"] = to_numpy(std::vector<std::int32_t>(r.sample_position));
            d["mid"] = to_numpy(std::vector<double>(r.sample_mid));
            return d; }, "Equity curve samples as a dict of numpy arrays");

    py::class_<StrategyContext>(m, "StrategyContext")
        .def("now", &StrategyContext::now)
        .def("book", &StrategyContext::book, py::return_value_policy::reference)
        .def("position", &StrategyContext::position).def("cash", &StrategyContext::cash)
        .def("closing", &StrategyContext::closing)
        .def("submit_limit", &StrategyContext::submit_limit, py::arg("side"), py::arg("price"), py::arg("qty"),
             py::arg("tif") = TimeInForce::GTC)
        .def("submit_market", &StrategyContext::submit_market, py::arg("side"), py::arg("qty"))
        .def("cancel", &StrategyContext::cancel).def("schedule_timer", &StrategyContext::schedule_timer)
        .def("order", &StrategyContext::order, py::return_value_policy::reference)
        .def("open_orders", &StrategyContext::open_orders, py::return_value_policy::reference)
        .def("open_qty", &StrategyContext::open_qty);

    py::class_<Strategy, PyStrategy>(m, "Strategy")
        .def(py::init<>())
        .def("on_start", &Strategy::on_start).def("on_market", &Strategy::on_market)
        .def("on_fill", &Strategy::on_fill).def("on_timer", &Strategy::on_timer).def("on_end", &Strategy::on_end);

    py::class_<OfiStrategyParams>(m, "OfiStrategyParams")
        .def(py::init<>())
        .def_readwrite("window_events", &OfiStrategyParams::window_events)
        .def_readwrite("zscore_window", &OfiStrategyParams::zscore_window)
        .def_readwrite("entry_z", &OfiStrategyParams::entry_z).def_readwrite("exit_z", &OfiStrategyParams::exit_z)
        .def_readwrite("order_qty", &OfiStrategyParams::order_qty)
        .def_readwrite("passive_entry", &OfiStrategyParams::passive_entry)
        .def_readwrite("order_ttl_ns", &OfiStrategyParams::order_ttl_ns)
        .def_readwrite("max_hold_ns", &OfiStrategyParams::max_hold_ns)
        .def_readwrite("cooldown_ns", &OfiStrategyParams::cooldown_ns);

    py::class_<OfiStrategy, Strategy>(m, "OfiStrategy")
        .def(py::init<OfiStrategyParams>())
        .def("zscores", [](const OfiStrategy& s) { return to_numpy(std::vector<double>(s.zscores())); })
        .def("last_z", &OfiStrategy::last_z);

    py::class_<PyBacktester>(m, "Backtester")
        .def(py::init([](arr<std::int64_t> ts, arr<std::int8_t> type, arr<std::int64_t> id, arr<std::int32_t> size,
                         arr<std::int64_t> price, arr<std::int8_t> dir,
                         const std::vector<std::tuple<int, std::int64_t, std::int32_t>>& seed, BacktestConfig cfg,
                         py::object snapshot) {
                 return new PyBacktester(Columns(std::move(ts), std::move(type), std::move(id), std::move(size),
                                                 std::move(price), std::move(dir)), Snapshot(snapshot), seed_from(seed), cfg); }),
             py::arg("ts"), py::arg("type"), py::arg("id"), py::arg("size"), py::arg("price"), py::arg("dir"),
             py::arg("seed"), py::arg("config"), py::arg("snapshot") = py::none())
        .def("run", &PyBacktester::run, py::arg("strategy"), py::keep_alive<1, 2>(),
             "Run the strategy over the whole message stream; returns BacktestResult");
}
