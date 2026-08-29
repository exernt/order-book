#include "orderbook/order_book.hpp"
#include "orderbook/fast_order_book.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using orderbook::BookSnapshot;
using orderbook::CancelResult;
using orderbook::CancelStatus;
using orderbook::FastOrderBook;
using orderbook::NewOrder;
using orderbook::OrderBook;
using orderbook::OrderBookConfig;
using orderbook::OrderId;
using orderbook::PriceLevelView;
using orderbook::Quote;
using orderbook::RestingOrderView;
using orderbook::Side;
using orderbook::SubmitResult;
using orderbook::SubmitStatus;
using orderbook::TimeInForce;
using orderbook::Trade;

constexpr std::size_t history_limit = 48;

struct RecordingSink {
    std::vector<Trade> trades;

    void on_trade(const Trade& trade) noexcept
    {
        trades.push_back(trade);
    }
};

struct TestContext {
    std::uint64_t seed{};
    std::uint64_t step{};
    std::vector<std::string> history;
};

[[nodiscard]] std::string side_name(Side side)
{
    switch (side) {
    case Side::buy:
        return "buy";
    case Side::sell:
        return "sell";
    }
    return "side(" + std::to_string(static_cast<unsigned>(side)) + ")";
}

[[nodiscard]] std::string tif_name(TimeInForce time_in_force)
{
    switch (time_in_force) {
    case TimeInForce::gtc:
        return "gtc";
    case TimeInForce::ioc:
        return "ioc";
    case TimeInForce::fok:
        return "fok";
    }
    return "tif(" + std::to_string(static_cast<unsigned>(time_in_force)) + ")";
}

[[nodiscard]] std::string describe(const NewOrder& order)
{
    std::ostringstream output;
    output << "submit{id=" << order.id
           << ", side=" << side_name(order.side)
           << ", price=" << order.price_ticks
           << ", quantity=" << order.quantity
           << ", tif=" << tif_name(order.time_in_force) << '}';
    return output.str();
}

[[noreturn]] void fail(const TestContext& context, std::string_view reason)
{
    std::cerr << "randomized test failure: " << reason
              << "\nseed=" << context.seed
              << " step=" << context.step << "\nrecent command history:\n";
    for (const std::string& command : context.history) {
        std::cerr << "  " << command << '\n';
    }
    std::exit(1);
}

void require(bool condition, const TestContext& context, std::string_view reason)
{
    if (!condition) {
        fail(context, reason);
    }
}

void record(TestContext& context, std::string command)
{
    ++context.step;
    if (context.history.size() == history_limit) {
        context.history.erase(context.history.begin());
    }
    context.history.push_back(std::move(command));
}

[[nodiscard]] constexpr bool valid_side(Side side) noexcept
{
    return side == Side::buy || side == Side::sell;
}

[[nodiscard]] constexpr bool valid_tif(TimeInForce time_in_force) noexcept
{
    return time_in_force == TimeInForce::gtc ||
           time_in_force == TimeInForce::ioc ||
           time_in_force == TimeInForce::fok;
}

[[nodiscard]] constexpr bool crosses(
    Side taker_side,
    std::uint32_t taker_limit,
    std::uint32_t maker_price) noexcept
{
    return taker_side == Side::buy
        ? maker_price <= taker_limit
        : maker_price >= taker_limit;
}

[[nodiscard]] bool same_trade(const Trade& lhs, const Trade& rhs) noexcept
{
    return lhs.maker_id == rhs.maker_id &&
           lhs.taker_id == rhs.taker_id &&
           lhs.quantity == rhs.quantity &&
           lhs.price_ticks == rhs.price_ticks;
}

[[nodiscard]] bool same_trades(
    const std::vector<Trade>& lhs,
    const std::vector<Trade>& rhs) noexcept
{
    return lhs.size() == rhs.size() &&
           std::equal(lhs.begin(), lhs.end(), rhs.begin(), same_trade);
}

[[nodiscard]] bool same_snapshot(
    const BookSnapshot& lhs,
    const BookSnapshot& rhs) noexcept
{
    const auto same_levels = [](const auto& left, const auto& right) {
        if (left.size() != right.size()) {
            return false;
        }
        for (std::size_t level_index = 0; level_index < left.size(); ++level_index) {
            const PriceLevelView& left_level = left[level_index];
            const PriceLevelView& right_level = right[level_index];
            if (left_level.price_ticks != right_level.price_ticks ||
                left_level.total_quantity != right_level.total_quantity ||
                left_level.orders.size() != right_level.orders.size()) {
                return false;
            }
            for (std::size_t order_index = 0;
                 order_index < left_level.orders.size();
                 ++order_index) {
                const RestingOrderView& left_order = left_level.orders[order_index];
                const RestingOrderView& right_order = right_level.orders[order_index];
                if (left_order.id != right_order.id ||
                    left_order.remaining_quantity != right_order.remaining_quantity) {
                    return false;
                }
            }
        }
        return true;
    };

    return same_levels(lhs.bids, rhs.bids) && same_levels(lhs.asks, rhs.asks);
}

[[nodiscard]] bool same_quote(const Quote& lhs, const Quote& rhs) noexcept
{
    const auto same_level = [](const auto& left, const auto& right) {
        return left.has_value() == right.has_value() &&
               (!left.has_value() ||
                (left->price_ticks == right->price_ticks &&
                 left->quantity == right->quantity));
    };
    return same_level(lhs.bid, rhs.bid) && same_level(lhs.ask, rhs.ask);
}

[[nodiscard]] bool same_submit_result(
    const SubmitResult& lhs,
    const SubmitResult& rhs) noexcept
{
    return lhs.status == rhs.status &&
           lhs.executed_quantity == rhs.executed_quantity &&
           lhs.resting_quantity == rhs.resting_quantity &&
           lhs.canceled_quantity == rhs.canceled_quantity;
}

[[nodiscard]] bool same_cancel_result(
    const CancelResult& lhs,
    const CancelResult& rhs) noexcept
{
    return lhs.status == rhs.status &&
           lhs.canceled_quantity == rhs.canceled_quantity;
}

// This intentionally uses a flat, arrival-ordered vector and price scans rather
// than the map/list/index representation used by the reference OrderBook.
class ModelBook {
public:
    explicit ModelBook(OrderBookConfig config)
        : config_(config)
    {
    }

    [[nodiscard]] SubmitResult submit(NewOrder order, RecordingSink& sink)
    {
        if (!valid_order(order)) {
            return {
                .executed_quantity = 0,
                .resting_quantity = 0,
                .canceled_quantity = 0,
                .status = SubmitStatus::invalid
            };
        }
        if (find_order(order.id) != orders_.end()) {
            return {
                .executed_quantity = 0,
                .resting_quantity = 0,
                .canceled_quantity = 0,
                .status = SubmitStatus::duplicate_id
            };
        }
        if (order.time_in_force == TimeInForce::fok && !can_fill(order)) {
            return {
                .executed_quantity = 0,
                .resting_quantity = 0,
                .canceled_quantity = order.quantity,
                .status = SubmitStatus::accepted
            };
        }
        if (order.time_in_force == TimeInForce::gtc &&
            orders_.size() >= config_.max_orders) {
            return {
                .executed_quantity = 0,
                .resting_quantity = 0,
                .canceled_quantity = 0,
                .status = SubmitStatus::capacity_exhausted
            };
        }

        std::uint64_t remaining = order.quantity;
        while (remaining != 0) {
            const std::size_t maker_index = best_maker(order);
            if (maker_index == orders_.size()) {
                break;
            }

            ModelOrder& maker = orders_[maker_index];
            const std::uint64_t traded = std::min(remaining, maker.remaining_quantity);
            const Trade trade{
                .maker_id = maker.id,
                .taker_id = order.id,
                .quantity = traded,
                .price_ticks = maker.price_ticks
            };
            remaining -= traded;
            maker.remaining_quantity -= traded;
            if (maker.remaining_quantity == 0) {
                orders_.erase(orders_.begin() + static_cast<std::ptrdiff_t>(maker_index));
            }
            sink.on_trade(trade);
        }

        const std::uint64_t executed = order.quantity - remaining;
        if (order.time_in_force == TimeInForce::gtc) {
            if (remaining != 0) {
                orders_.push_back({order.id, remaining, order.price_ticks, order.side});
            }
            return {
                .executed_quantity = executed,
                .resting_quantity = remaining,
                .canceled_quantity = 0,
                .status = SubmitStatus::accepted
            };
        }
        if (order.time_in_force == TimeInForce::ioc) {
            return {
                .executed_quantity = executed,
                .resting_quantity = 0,
                .canceled_quantity = remaining,
                .status = SubmitStatus::accepted
            };
        }
        return {
            .executed_quantity = executed,
            .resting_quantity = 0,
            .canceled_quantity = 0,
            .status = SubmitStatus::accepted
        };
    }

    [[nodiscard]] CancelResult cancel(OrderId id)
    {
        if (id == 0) {
            return {.canceled_quantity = 0, .status = CancelStatus::invalid};
        }
        const auto order = find_order(id);
        if (order == orders_.end()) {
            return {.canceled_quantity = 0, .status = CancelStatus::not_found};
        }
        const std::uint64_t canceled = order->remaining_quantity;
        orders_.erase(order);
        return {.canceled_quantity = canceled, .status = CancelStatus::accepted};
    }

    [[nodiscard]] BookSnapshot snapshot() const
    {
        std::map<std::uint32_t, PriceLevelView, std::greater<>> bids;
        std::map<std::uint32_t, PriceLevelView> asks;

        for (const ModelOrder& order : orders_) {
            if (order.side == Side::buy) {
                append_order(bids[order.price_ticks], order);
            } else {
                append_order(asks[order.price_ticks], order);
            }
        }

        BookSnapshot result;
        result.bids.reserve(bids.size());
        result.asks.reserve(asks.size());
        for (auto& [price, level] : bids) {
            (void)price;
            result.bids.push_back(std::move(level));
        }
        for (auto& [price, level] : asks) {
            (void)price;
            result.asks.push_back(std::move(level));
        }
        return result;
    }

    [[nodiscard]] Quote best_quote() const
    {
        const BookSnapshot view = snapshot();
        Quote result;
        if (!view.bids.empty()) {
            result.bid = orderbook::LevelQuote{
                view.bids.front().total_quantity,
                view.bids.front().price_ticks
            };
        }
        if (!view.asks.empty()) {
            result.ask = orderbook::LevelQuote{
                view.asks.front().total_quantity,
                view.asks.front().price_ticks
            };
        }
        return result;
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return orders_.size();
    }

    [[nodiscard]] OrderId id_at(std::size_t index) const noexcept
    {
        return orders_[index].id;
    }

    [[nodiscard]] std::uint64_t bid_quantity() const noexcept
    {
        return total_for(Side::buy);
    }

    [[nodiscard]] std::uint64_t ask_quantity() const noexcept
    {
        return total_for(Side::sell);
    }

private:
    struct ModelOrder {
        OrderId id;
        std::uint64_t remaining_quantity;
        std::uint32_t price_ticks;
        Side side;
    };

    using OrderIterator = std::vector<ModelOrder>::iterator;
    using ConstOrderIterator = std::vector<ModelOrder>::const_iterator;

    [[nodiscard]] bool valid_order(const NewOrder& order) const noexcept
    {
        return order.id != 0 &&
               order.quantity != 0 &&
               order.quantity <= config_.max_order_quantity &&
               order.price_ticks >= config_.min_price_ticks &&
               order.price_ticks <= config_.max_price_ticks &&
               valid_side(order.side) &&
               valid_tif(order.time_in_force);
    }

    [[nodiscard]] OrderIterator find_order(OrderId id)
    {
        return std::find_if(orders_.begin(), orders_.end(), [id](const ModelOrder& order) {
            return order.id == id;
        });
    }

    [[nodiscard]] ConstOrderIterator find_order(OrderId id) const
    {
        return std::find_if(orders_.begin(), orders_.end(), [id](const ModelOrder& order) {
            return order.id == id;
        });
    }

    [[nodiscard]] bool can_fill(const NewOrder& taker) const noexcept
    {
        std::uint64_t needed = taker.quantity;
        for (const ModelOrder& maker : orders_) {
            if (maker.side != taker.side &&
                crosses(taker.side, taker.price_ticks, maker.price_ticks)) {
                if (maker.remaining_quantity >= needed) {
                    return true;
                }
                needed -= maker.remaining_quantity;
            }
        }
        return false;
    }

    [[nodiscard]] std::size_t best_maker(const NewOrder& taker) const noexcept
    {
        std::size_t best = orders_.size();
        for (std::size_t index = 0; index < orders_.size(); ++index) {
            const ModelOrder& candidate = orders_[index];
            if (candidate.side == taker.side ||
                !crosses(taker.side, taker.price_ticks, candidate.price_ticks)) {
                continue;
            }
            if (best == orders_.size() ||
                (taker.side == Side::buy
                     ? candidate.price_ticks < orders_[best].price_ticks
                     : candidate.price_ticks > orders_[best].price_ticks)) {
                best = index;
            }
        }
        return best;
    }

    static void append_order(PriceLevelView& level, const ModelOrder& order)
    {
        level.price_ticks = order.price_ticks;
        level.total_quantity += order.remaining_quantity;
        level.orders.push_back({order.id, order.remaining_quantity});
    }

    [[nodiscard]] std::uint64_t total_for(Side side) const noexcept
    {
        std::uint64_t total = 0;
        for (const ModelOrder& order : orders_) {
            if (order.side == side) {
                total += order.remaining_quantity;
            }
        }
        return total;
    }

    OrderBookConfig config_;
    std::vector<ModelOrder> orders_;
};

class Harness {
public:
    Harness(OrderBookConfig config, std::uint64_t seed)
        : config_(config),
          actual_(config),
          fast_(config),
          model_(config),
          context_{.seed = seed, .step = 0, .history = {}}
    {
    }

    void submit(NewOrder order)
    {
        record(context_, describe(order));
        const BookSnapshot before = actual_.snapshot();
        const BookSnapshot fast_before = fast_.snapshot();

        RecordingSink actual_sink;
        RecordingSink fast_sink;
        RecordingSink expected_sink;
        actual_sink.trades.reserve(config_.max_orders);
        fast_sink.trades.reserve(config_.max_orders);
        expected_sink.trades.reserve(config_.max_orders);
        const SubmitResult actual_result = actual_.submit(order, actual_sink);
        const SubmitResult fast_result = fast_.submit(order, fast_sink);
        const SubmitResult expected_result = model_.submit(order, expected_sink);

        require(
            same_submit_result(actual_result, expected_result),
            context_,
            "submit result differs from model");
        require(
            same_submit_result(fast_result, actual_result),
            context_,
            "fast submit result differs from reference");
        require(
            same_trades(actual_sink.trades, expected_sink.trades),
            context_,
            "ordered trades differ from model");
        require(
            same_trades(fast_sink.trades, actual_sink.trades),
            context_,
            "fast ordered trades differ from reference");

        std::uint64_t traded_quantity = 0;
        for (const Trade& trade : actual_sink.trades) {
            require(trade.quantity != 0, context_, "zero-quantity trade");
            require(trade.taker_id == order.id, context_, "wrong taker ID in trade");
            require(
                crosses(order.side, order.price_ticks, trade.price_ticks),
                context_,
                "trade price does not cross taker limit");
            require(
                traded_quantity <=
                    std::numeric_limits<std::uint64_t>::max() - trade.quantity,
                context_,
                "trade quantity sum overflow");
            traded_quantity += trade.quantity;
        }
        require(
            traded_quantity == actual_result.executed_quantity,
            context_,
            "trade sum differs from executed quantity");

        if (actual_result.status == SubmitStatus::accepted) {
            require(
                actual_result.executed_quantity + actual_result.resting_quantity +
                        actual_result.canceled_quantity ==
                    order.quantity,
                context_,
                "accepted submission does not conserve quantity");
        } else {
            require(
                actual_result.executed_quantity == 0 &&
                    actual_result.resting_quantity == 0 &&
                    actual_result.canceled_quantity == 0 &&
                    actual_sink.trades.empty(),
                context_,
                "rejected submission changed result quantities or emitted trades");
            require(
                same_snapshot(before, actual_.snapshot()),
                context_,
                "rejected submission changed the book");
            require(
                same_snapshot(fast_before, fast_.snapshot()),
                context_,
                "rejected submission changed the fast book");
        }

        if (order.time_in_force == TimeInForce::fok &&
            actual_result.status == SubmitStatus::accepted &&
            actual_result.canceled_quantity == order.quantity) {
            require(actual_sink.trades.empty(), context_, "failed FOK emitted a trade");
            require(
                same_snapshot(before, actual_.snapshot()),
                context_,
                "failed FOK changed the book");
            require(
                fast_sink.trades.empty(),
                context_,
                "failed FOK emitted a trade in the fast book");
            require(
                same_snapshot(fast_before, fast_.snapshot()),
                context_,
                "failed FOK changed the fast book");
        }

        verify_state();
    }

    void cancel(OrderId id)
    {
        record(context_, "cancel{id=" + std::to_string(id) + '}');
        const BookSnapshot before = actual_.snapshot();
        const BookSnapshot fast_before = fast_.snapshot();
        const CancelResult actual_result = actual_.cancel(id);
        const CancelResult fast_result = fast_.cancel(id);
        const CancelResult expected_result = model_.cancel(id);

        require(
            same_cancel_result(actual_result, expected_result),
            context_,
            "cancel result differs from model");
        require(
            same_cancel_result(fast_result, actual_result),
            context_,
            "fast cancel result differs from reference");
        if (actual_result.status != CancelStatus::accepted) {
            require(
                same_snapshot(before, actual_.snapshot()),
                context_,
                "rejected cancel changed the book");
            require(
                same_snapshot(fast_before, fast_.snapshot()),
                context_,
                "rejected cancel changed the fast book");
        }
        verify_state();
    }

    [[nodiscard]] const ModelBook& model() const noexcept
    {
        return model_;
    }

private:
    void verify_state()
    {
        require(actual_.check_invariants(), context_, "OrderBook invariant failure");
        require(fast_.check_invariants(), context_, "FastOrderBook invariant failure");
        require(
            same_snapshot(actual_.snapshot(), model_.snapshot()),
            context_,
            "logical snapshot differs from model");
        require(
            same_snapshot(fast_.snapshot(), actual_.snapshot()),
            context_,
            "fast logical snapshot differs from reference");
        require(
            same_quote(actual_.best_quote(), model_.best_quote()),
            context_,
            "best quote differs from model");
        require(
            same_quote(fast_.best_quote(), actual_.best_quote()),
            context_,
            "fast best quote differs from reference");

        const orderbook::BookStats stats = actual_.stats();
        const orderbook::BookStats fast_stats = fast_.stats();
        require(stats.live_orders == model_.size(), context_, "wrong live-order count");
        require(
            stats.active_id_entries == model_.size(),
            context_,
            "wrong active-ID count");
        require(
            stats.live_orders + stats.free_order_slots == config_.max_orders,
            context_,
            "live plus free count differs from capacity");
        require(
            stats.bid_quantity == model_.bid_quantity() &&
                stats.ask_quantity == model_.ask_quantity(),
            context_,
            "side aggregate quantity differs from model");
        require(
            fast_stats.live_orders == stats.live_orders &&
                fast_stats.free_order_slots == stats.free_order_slots &&
                fast_stats.active_id_entries == stats.active_id_entries &&
                fast_stats.bid_quantity == stats.bid_quantity &&
                fast_stats.ask_quantity == stats.ask_quantity &&
                fast_stats.bid_levels == stats.bid_levels &&
                fast_stats.ask_levels == stats.ask_levels,
            context_,
            "fast statistics differ from reference");
        require(
            fast_stats.id_index_capacity >= config_.max_orders &&
                fast_stats.id_index_tombstones == 0 &&
#if ORDERBOOK_ENABLE_ID_INDEX_DIAGNOSTICS
            fast_stats.total_probes >= fast_stats.id_index_operations &&
            (fast_stats.id_index_operations == 0 ||
                 fast_stats.max_probe_length != 0),
#else
            fast_stats.total_probes == 0 &&
            fast_stats.id_index_operations == 0 &&
            fast_stats.max_probe_length == 0,
#endif
            context_,
            "fast active-ID statistics are inconsistent");
    }

    OrderBookConfig config_;
    OrderBook actual_;
    FastOrderBook fast_;
    ModelBook model_;
    TestContext context_;
};

[[nodiscard]] NewOrder make_order(
    OrderId id,
    Side side,
    std::uint32_t price,
    std::uint64_t quantity,
    TimeInForce time_in_force = TimeInForce::gtc)
{
    return {
        .id = id,
        .quantity = quantity,
        .price_ticks = price,
        .side = side,
        .time_in_force = time_in_force
    };
}

void run_adversarial_scenarios()
{
    constexpr OrderBookConfig queue_config{
        .max_order_quantity = 100'000,
        .max_orders = 256,
        .min_price_ticks = 100,
        .max_price_ticks = 200
    };
    Harness queues(queue_config, 0xA11CE001ULL);

    // Same low bits, long same-price queues, extreme prices/quantities, and
    // alternating sides all stress index and FIFO assumptions.
    std::vector<OrderId> bid_ids;
    std::vector<OrderId> ask_ids;
    for (std::uint64_t index = 0; index < 64; ++index) {
        const OrderId bid_id = 1 + index * 65'536;
        const OrderId ask_id = 2 + index * 65'536;
        bid_ids.push_back(bid_id);
        ask_ids.push_back(ask_id);
        queues.submit(make_order(
            bid_id,
            Side::buy,
            queue_config.min_price_ticks,
            queue_config.max_order_quantity - index));
        queues.submit(make_order(
            ask_id,
            Side::sell,
            queue_config.max_price_ticks,
            queue_config.max_order_quantity - index));
    }

    for (std::size_t index = 0; index < bid_ids.size(); index += 3) {
        queues.cancel(bid_ids[index]);
        queues.cancel(bid_ids[index]);
        queues.cancel(0);
    }
    for (std::size_t index = 1; index < ask_ids.size(); index += 4) {
        queues.cancel(ask_ids[index]);
        queues.cancel(ask_ids[index]);
    }

    constexpr OrderBookConfig sweep_config{
        .max_order_quantity = 1'000'000,
        .max_orders = 256,
        .min_price_ticks = 1,
        .max_price_ticks = 1'000
    };
    Harness sweeps(sweep_config, 0xA11CE002ULL);
    OrderId id = 1;
    std::uint64_t ask_quantity = 0;
    for (std::uint32_t price = 500; price <= 900; price += 20) {
        for (std::uint32_t queue_position = 0; queue_position < 3; ++queue_position) {
            const std::uint64_t quantity = 900 + queue_position;
            sweeps.submit(make_order(id++, Side::sell, price, quantity));
            ask_quantity += quantity;
        }
    }
    sweeps.submit(make_order(
        id++, Side::buy, sweep_config.max_price_ticks, ask_quantity - 1, TimeInForce::ioc));
    sweeps.submit(make_order(
        id++, Side::buy, sweep_config.max_price_ticks, 1, TimeInForce::fok));

    std::uint64_t bid_quantity = 0;
    for (std::uint32_t price = 100; price <= 480; price += 20) {
        for (std::uint32_t queue_position = 0; queue_position < 3; ++queue_position) {
            const std::uint64_t quantity = 800 + queue_position;
            sweeps.submit(make_order(id++, Side::buy, price, quantity));
            bid_quantity += quantity;
        }
    }
    sweeps.submit(make_order(
        id++, Side::sell, sweep_config.min_price_ticks, bid_quantity + 1, TimeInForce::fok));
    sweeps.submit(make_order(
        id++, Side::sell, sweep_config.min_price_ticks, bid_quantity, TimeInForce::fok));
}

void run_near_capacity_scenario()
{
    constexpr OrderBookConfig config{
        .max_order_quantity = 100,
        .max_orders = 128,
        .min_price_ticks = 100,
        .max_price_ticks = 200
    };
    Harness harness(config, 0xA11CE003ULL);
    std::vector<OrderId> ids;
    ids.reserve(config.max_orders);

    for (std::uint64_t value = 0; value < config.max_orders; ++value) {
        const OrderId id = 7U + (value << 32U);
        ids.push_back(id);
        harness.submit(make_order(id, Side::buy, config.min_price_ticks, 1));
    }

    // The logical order capacity is reached before the hash table's spare
    // buckets. Rejection must leave both implementations unchanged.
    harness.submit(make_order(
        std::numeric_limits<OrderId>::max(),
        Side::buy,
        config.min_price_ticks,
        1));

    for (std::size_t index = 0; index < ids.size(); index += 3) {
        harness.cancel(ids[index]);
    }
    for (std::size_t index = 0; index < ids.size(); index += 3) {
        harness.submit(make_order(
            11U + (static_cast<std::uint64_t>(index) << 32U),
            Side::buy,
            config.min_price_ticks,
            1));
    }
}

[[nodiscard]] std::uint32_t random_price(
    std::mt19937_64& random,
    const OrderBookConfig& config)
{
    switch (random() % 12) {
    case 0:
        return config.min_price_ticks;
    case 1:
        return config.max_price_ticks;
    case 2:
        return config.min_price_ticks - 1;
    case 3:
        return config.max_price_ticks + 1;
    case 4:
        return config.min_price_ticks +
               (config.max_price_ticks - config.min_price_ticks) / 2;
    case 5: {
        constexpr std::array<std::uint32_t, 5> sparse_offsets{0, 1, 16, 32, 64};
        return config.min_price_ticks + sparse_offsets[random() % sparse_offsets.size()];
    }
    default:
        return config.min_price_ticks + static_cast<std::uint32_t>(
            random() % (config.max_price_ticks - config.min_price_ticks + 1));
    }
}

[[nodiscard]] std::uint64_t random_quantity(
    std::mt19937_64& random,
    const OrderBookConfig& config)
{
    switch (random() % 10) {
    case 0:
        return 0;
    case 1:
        return config.max_order_quantity + 1;
    case 2:
        return config.max_order_quantity;
    case 3:
        return config.max_order_quantity - 1;
    case 4:
        return 1;
    default:
        return 1 + random() % config.max_order_quantity;
    }
}

void run_random_seed(std::uint64_t seed)
{
    constexpr OrderBookConfig config{
        .max_order_quantity = 1'000,
        .max_orders = 128,
        .min_price_ticks = 100,
        .max_price_ticks = 164
    };
    constexpr std::uint64_t command_count = 20'000;

    Harness harness(config, seed);
    std::mt19937_64 random(seed);
    OrderId sequential_id = 1;
    OrderId colliding_id = 1;
    OrderId last_cancel = std::numeric_limits<OrderId>::max();

    for (std::uint64_t step = 0; step < command_count; ++step) {
        if (random() % 100 < 30) {
            OrderId id = 0;
            switch (random() % 6) {
            case 0:
                id = 0;
                break;
            case 1:
            case 2:
                if (harness.model().size() != 0) {
                    id = harness.model().id_at(random() % harness.model().size());
                }
                break;
            case 3:
                id = last_cancel;
                break;
            case 4:
                id = std::numeric_limits<OrderId>::max() - random() % 1'024;
                break;
            default:
                id = 1 + random() % 512;
                break;
            }
            harness.cancel(id);
            last_cancel = id;
            continue;
        }

        OrderId id = 0;
        switch (random() % 8) {
        case 0:
            id = 0;
            break;
        case 1:
            if (harness.model().size() != 0) {
                id = harness.model().id_at(random() % harness.model().size());
                break;
            }
            [[fallthrough]];
        case 2:
        case 3:
            id = sequential_id++;
            break;
        case 4:
        case 5:
            id = 0x1'0000'0000ULL + (colliding_id++ << 16U);
            break;
        default:
            id = 1 + random() % 512;
            break;
        }

        Side side = (step % 2 == 0) ? Side::buy : Side::sell;
        if (random() % 3 == 0) {
            side = random() % 2 == 0 ? Side::buy : Side::sell;
        }
        if (random() % 100 == 0) {
            side = static_cast<Side>(7);
        }

        TimeInForce time_in_force = static_cast<TimeInForce>(random() % 3);
        if (random() % 100 == 0) {
            time_in_force = static_cast<TimeInForce>(9);
        }

        harness.submit(make_order(
            id,
            side,
            random_price(random, config),
            random_quantity(random, config),
            time_in_force));
    }
}

} // namespace

int main()
{
    run_adversarial_scenarios();
    run_near_capacity_scenario();
    constexpr std::array<std::uint64_t, 8> seeds{
        0x0000000000000001ULL,
        0x00000000C0FFEE00ULL,
        0x0123456789ABCDEFULL,
        0x13579BDF2468ACE0ULL,
        0x5EED5EED5EED5EEDULL,
        0x9E3779B97F4A7C15ULL,
        0xDEADBEEFCAFEBABEULL,
        0xFFFFFFFFFFFFFFFFULL
    };
    for (const std::uint64_t seed : seeds) {
        run_random_seed(seed);
    }
    std::cout << "all randomized order-book tests passed ("
              << seeds.size() << " seeds, 160000 random commands)\n";
    return 0;
}
