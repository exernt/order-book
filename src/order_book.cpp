#include "orderbook/order_book.hpp"

#include <algorithm>
#include <cassert>
#include <functional>
#include <limits>
#include <list>
#include <map>
#include <new>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace orderbook {
namespace {

struct RestingOrder {
    OrderId id;
    std::uint64_t remaining_quantity;
};

struct PriceLevel {
    std::list<RestingOrder> orders;
    std::uint64_t total_quantity{0};
};

using OrderIterator = std::list<RestingOrder>::iterator;
using AskLevels = std::map<std::uint32_t, PriceLevel>;
using BidLevels = std::map<std::uint32_t, PriceLevel, std::greater<>>;

struct OrderLocation {
    Side side;
    std::uint32_t price_ticks;
    OrderIterator order;
};

[[nodiscard]] constexpr bool is_valid(Side side) noexcept
{
    return side == Side::buy || side == Side::sell;
}

[[nodiscard]] constexpr bool is_valid(TimeInForce time_in_force) noexcept
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

template <typename Levels>
[[nodiscard]] bool has_available_quantity(
    const Levels& opposite_levels,
    Side taker_side,
    std::uint32_t limit_price,
    std::uint64_t quantity_needed) noexcept
{
    for (const auto& [price, level] : opposite_levels) {
        if (!crosses(taker_side, limit_price, price)) {
            break;
        }

        if (level.total_quantity >= quantity_needed) {
            return true;
        }
        quantity_needed -= level.total_quantity;
    }
    return false;
}

} // namespace

struct OrderBook::Impl {
    explicit Impl(OrderBookConfig book_config)
        : config(book_config)
    {
        if (config.max_orders == 0 ||
            config.max_order_quantity == 0 ||
            config.min_price_ticks == 0 ||
            config.min_price_ticks > config.max_price_ticks ||
            config.max_order_quantity >
                std::numeric_limits<std::uint64_t>::max() / config.max_orders) {
            throw std::invalid_argument("invalid order book configuration");
        }

        active_orders.reserve(config.max_orders);
    }

    [[nodiscard]] bool valid_order(const NewOrder& order) const noexcept
    {
        return order.id != 0 &&
               order.quantity != 0 &&
               order.quantity <= config.max_order_quantity &&
               order.price_ticks >= config.min_price_ticks &&
               order.price_ticks <= config.max_price_ticks &&
               is_valid(order.side) &&
               is_valid(order.time_in_force);
    }

    [[nodiscard]] bool can_fill(const NewOrder& order) const noexcept
    {
        if (order.side == Side::buy) {
            return has_available_quantity(
                asks, order.side, order.price_ticks, order.quantity);
        }
        return has_available_quantity(
            bids, order.side, order.price_ticks, order.quantity);
    }

    template <typename Levels>
    [[nodiscard]] std::uint64_t match(
        Levels& opposite_levels,
        const NewOrder& taker,
        void* sink,
        TradeCallback on_trade) noexcept
    {
        std::uint64_t remaining = taker.quantity;

        while (remaining != 0 && !opposite_levels.empty()) {
            auto level_it = opposite_levels.begin();
            const std::uint32_t maker_price = level_it->first;
            if (!crosses(taker.side, taker.price_ticks, maker_price)) {
                break;
            }

            PriceLevel& level = level_it->second;
            assert(!level.orders.empty());
            RestingOrder& maker = level.orders.front();
            const std::uint64_t traded =
                std::min(remaining, maker.remaining_quantity);
            const Trade trade{
                .maker_id = maker.id,
                .taker_id = taker.id,
                .quantity = traded,
                .price_ticks = maker_price
            };

            remaining -= traded;
            maker.remaining_quantity -= traded;
            level.total_quantity -= traded;

            if (maker.remaining_quantity == 0) {
                active_orders.erase(maker.id);
                level.orders.pop_front();
                if (level.orders.empty()) {
                    assert(level.total_quantity == 0);
                    opposite_levels.erase(level_it);
                }
            }

            // The trade is reported only after every associated book mutation.
            on_trade(sink, trade);
        }

        return remaining;
    }

    OrderBookConfig config;
    BidLevels bids;
    AskLevels asks;
    std::unordered_map<OrderId, OrderLocation> active_orders;
};

OrderBook::OrderBook(OrderBookConfig config)
    : impl_(std::make_unique<Impl>(config))
{
}

OrderBook::~OrderBook() = default;

SubmitResult OrderBook::submit_impl(
    NewOrder order,
    void* sink,
    TradeCallback on_trade) noexcept
{
    if (!impl_->valid_order(order)) {
        return {
            .executed_quantity = 0,
            .resting_quantity = 0,
            .canceled_quantity = 0,
            .status = SubmitStatus::invalid
        };
    }
    if (impl_->active_orders.contains(order.id)) {
        return {
            .executed_quantity = 0,
            .resting_quantity = 0,
            .canceled_quantity = 0,
            .status = SubmitStatus::duplicate_id
        };
    }

    if (order.time_in_force == TimeInForce::fok && !impl_->can_fill(order)) {
        return {
            .executed_quantity = 0,
            .resting_quantity = 0,
            .canceled_quantity = order.quantity,
            .status = SubmitStatus::accepted
        };
    }

    // A reference-book capacity reservation is deliberately conservative: a
    // GTC is rejected at full capacity even if matching might free a slot.
    if (order.time_in_force == TimeInForce::gtc &&
        impl_->active_orders.size() >= impl_->config.max_orders) {
        return {
            .executed_quantity = 0,
            .resting_quantity = 0,
            .canceled_quantity = 0,
            .status = SubmitStatus::capacity_exhausted
        };
    }

    // A GTC may need to rest after matching. Allocate its list node, price
    // level, and ID-index entry first so allocation failure cannot occur after
    // trades have already changed the book.
    std::list<RestingOrder> staged_order;
    bool created_level = false;
    bool indexed_order = false;

    try {
        if (order.time_in_force == TimeInForce::gtc) {
            if (order.side == Side::buy) {
                const auto [level, inserted] = impl_->bids.try_emplace(order.price_ticks);
                (void)level;
                created_level = inserted;
            } else {
                const auto [level, inserted] = impl_->asks.try_emplace(order.price_ticks);
                (void)level;
                created_level = inserted;
            }

            staged_order.push_back({order.id, order.quantity});
            const auto [location, inserted] = impl_->active_orders.try_emplace(
                order.id,
                OrderLocation{order.side, order.price_ticks, staged_order.begin()});
            (void)location;
            assert(inserted);
            indexed_order = inserted;
        }
    } catch (const std::bad_alloc&) {
        if (indexed_order) {
            impl_->active_orders.erase(order.id);
        }
        if (created_level) {
            if (order.side == Side::buy) {
                impl_->bids.erase(order.price_ticks);
            } else {
                impl_->asks.erase(order.price_ticks);
            }
        }
        return {
            .executed_quantity = 0,
            .resting_quantity = 0,
            .canceled_quantity = 0,
            .status = SubmitStatus::capacity_exhausted
        };
    }

    const std::uint64_t remaining = order.side == Side::buy
        ? impl_->match(impl_->asks, order, sink, on_trade)
        : impl_->match(impl_->bids, order, sink, on_trade);
    const std::uint64_t executed = order.quantity - remaining;

    if (order.time_in_force == TimeInForce::gtc) {
        if (remaining != 0) {
            staged_order.front().remaining_quantity = remaining;
            if (order.side == Side::buy) {
                PriceLevel& level = impl_->bids.find(order.price_ticks)->second;
                level.orders.splice(level.orders.end(), staged_order);
                level.total_quantity += remaining;
            } else {
                PriceLevel& level = impl_->asks.find(order.price_ticks)->second;
                level.orders.splice(level.orders.end(), staged_order);
                level.total_quantity += remaining;
            }

            return {
                .executed_quantity = executed,
                .resting_quantity = remaining,
                .canceled_quantity = 0,
                .status = SubmitStatus::accepted
            };
        }

        impl_->active_orders.erase(order.id);
        if (created_level) {
            if (order.side == Side::buy) {
                impl_->bids.erase(order.price_ticks);
            } else {
                impl_->asks.erase(order.price_ticks);
            }
        }
    }

    if (order.time_in_force == TimeInForce::ioc) {
        return {
            .executed_quantity = executed,
            .resting_quantity = 0,
            .canceled_quantity = remaining,
            .status = SubmitStatus::accepted
        };
    }

    assert(remaining == 0);
    return {
        .executed_quantity = executed,
        .resting_quantity = 0,
        .canceled_quantity = 0,
        .status = SubmitStatus::accepted
    };
}

CancelResult OrderBook::cancel(OrderId id) noexcept
{
    if (id == 0) {
        return {.canceled_quantity = 0, .status = CancelStatus::invalid};
    }

    const auto location_it = impl_->active_orders.find(id);
    if (location_it == impl_->active_orders.end()) {
        return {.canceled_quantity = 0, .status = CancelStatus::not_found};
    }

    const OrderLocation location = location_it->second;
    const std::uint64_t canceled = location.order->remaining_quantity;

    if (location.side == Side::buy) {
        const auto level_it = impl_->bids.find(location.price_ticks);
        assert(level_it != impl_->bids.end());
        PriceLevel& level = level_it->second;
        level.total_quantity -= canceled;
        level.orders.erase(location.order);
        if (level.orders.empty()) {
            impl_->bids.erase(level_it);
        }
    } else {
        const auto level_it = impl_->asks.find(location.price_ticks);
        assert(level_it != impl_->asks.end());
        PriceLevel& level = level_it->second;
        level.total_quantity -= canceled;
        level.orders.erase(location.order);
        if (level.orders.empty()) {
            impl_->asks.erase(level_it);
        }
    }

    impl_->active_orders.erase(location_it);
    return {.canceled_quantity = canceled, .status = CancelStatus::accepted};
}

Quote OrderBook::best_quote() const noexcept
{
    Quote quote;
    if (!impl_->bids.empty()) {
        const auto& [price, level] = *impl_->bids.begin();
        quote.bid = LevelQuote{level.total_quantity, price};
    }
    if (!impl_->asks.empty()) {
        const auto& [price, level] = *impl_->asks.begin();
        quote.ask = LevelQuote{level.total_quantity, price};
    }
    return quote;
}

BookSnapshot OrderBook::snapshot() const
{
    BookSnapshot result;
    result.bids.reserve(impl_->bids.size());
    result.asks.reserve(impl_->asks.size());

    const auto append_levels = [](const auto& source, auto& destination) {
        for (const auto& [price, level] : source) {
            PriceLevelView view{
                .total_quantity = level.total_quantity,
                .price_ticks = price,
                .orders = {}
            };
            view.orders.reserve(level.orders.size());
            for (const RestingOrder& order : level.orders) {
                view.orders.push_back({
                    .id = order.id,
                    .remaining_quantity = order.remaining_quantity
                });
            }
            destination.push_back(std::move(view));
        }
    };

    append_levels(impl_->bids, result.bids);
    append_levels(impl_->asks, result.asks);
    return result;
}

BookStats OrderBook::stats() const noexcept
{
    BookStats result{};
    for (const auto& [price, level] : impl_->bids) {
        (void)price;
        result.bid_quantity += level.total_quantity;
    }
    for (const auto& [price, level] : impl_->asks) {
        (void)price;
        result.ask_quantity += level.total_quantity;
    }

    result.id_index_capacity = impl_->config.max_orders;
    result.live_orders = static_cast<std::uint32_t>(impl_->active_orders.size());
    result.free_order_slots = impl_->config.max_orders - result.live_orders;
    result.bid_levels = static_cast<std::uint32_t>(impl_->bids.size());
    result.ask_levels = static_cast<std::uint32_t>(impl_->asks.size());
    result.active_id_entries = result.live_orders;
    return result;
}

bool OrderBook::check_invariants() const
{
    if (impl_->active_orders.size() > impl_->config.max_orders) {
        return false;
    }

    std::unordered_set<OrderId> seen;
    seen.reserve(impl_->active_orders.size());

    const auto check_side = [&](const auto& levels, Side expected_side) {
        for (const auto& [price, level] : levels) {
            if (price < impl_->config.min_price_ticks ||
                price > impl_->config.max_price_ticks ||
                level.orders.empty()) {
                return false;
            }

            std::uint64_t total = 0;
            for (auto order_it = level.orders.begin(); order_it != level.orders.end(); ++order_it) {
                if (order_it->id == 0 || order_it->remaining_quantity == 0 ||
                    total > std::numeric_limits<std::uint64_t>::max() -
                        order_it->remaining_quantity ||
                    !seen.insert(order_it->id).second) {
                    return false;
                }
                total += order_it->remaining_quantity;

                const auto location = impl_->active_orders.find(order_it->id);
                if (location == impl_->active_orders.end() ||
                    location->second.side != expected_side ||
                    location->second.price_ticks != price ||
                    &*location->second.order != &*order_it) {
                    return false;
                }
            }
            if (total != level.total_quantity) {
                return false;
            }
        }
        return true;
    };

    if (!check_side(impl_->bids, Side::buy) ||
        !check_side(impl_->asks, Side::sell) ||
        seen.size() != impl_->active_orders.size()) {
        return false;
    }

    return impl_->bids.empty() || impl_->asks.empty() ||
           impl_->bids.begin()->first < impl_->asks.begin()->first;
}

} // namespace orderbook
