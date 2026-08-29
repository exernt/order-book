#include "orderbook/fast_order_book.hpp"

#include "orderbook/detail/active_id_index.hpp"
#include "orderbook/detail/occupied_price_set.hpp"
#include "orderbook/detail/order_pool.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace orderbook {
namespace {

using detail::NodeState;
using detail::ActiveIdIndex;
using detail::IdInsertResult;
#if ORDERBOOK_USE_FLAT_PRICE_BITMAP
using OccupiedPriceSet = detail::FlatOccupiedPriceSet;
#else
using detail::OccupiedPriceSet;
#endif
using detail::OrderNode;
using detail::OrderPool;
using detail::PoolIndex;
using detail::PriceLevel;
using detail::invalid_index;


// Side and TimeInForce checks (to avoid invalid orders)
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

[[nodiscard]] constexpr Side opposite(Side side) noexcept
{
    return side == Side::buy ? Side::sell : Side::buy;
}

} // namespace

struct FastOrderBook::Impl {
    [[nodiscard]] static OrderBookConfig validate(OrderBookConfig config)
    {
        const std::uint64_t price_levels = config.min_price_ticks <=
                config.max_price_ticks
            ? static_cast<std::uint64_t>(config.max_price_ticks) -
                  config.min_price_ticks + 1U
            : 0U;
        if (config.max_orders == 0 ||
            config.max_orders == std::numeric_limits<std::uint32_t>::max() ||
            config.max_order_quantity == 0 || config.min_price_ticks == 0 ||
            config.min_price_ticks > config.max_price_ticks ||
            price_levels > FastOrderBook::max_dense_price_levels ||
            config.max_order_quantity >
                std::numeric_limits<std::uint64_t>::max() / config.max_orders) {
            throw std::invalid_argument("invalid order book configuration");
        }
        (void)ActiveIdIndex::required_capacity(config.max_orders);
        return config;
    }

    [[nodiscard]] static std::size_t price_level_count(
        const OrderBookConfig& config) noexcept
    {
        return static_cast<std::size_t>(
            static_cast<std::uint64_t>(config.max_price_ticks) -
            config.min_price_ticks + 1U);
    }

    explicit Impl(OrderBookConfig book_config)
        : config(validate(book_config)),
          pool(config.max_orders),
          bids(price_level_count(config)),
          asks(price_level_count(config)),
          occupied_bids(price_level_count(config)),
          occupied_asks(price_level_count(config)),
          active_orders(config.max_orders)
    {
    }

    [[nodiscard]] std::size_t index_of(std::uint32_t price) const noexcept
    {
        assert(price >= config.min_price_ticks && price <= config.max_price_ticks);
        return static_cast<std::size_t>(price - config.min_price_ticks);
    }

    [[nodiscard]] std::uint32_t price_of(std::size_t index) const noexcept
    {
        assert(index < bids.size());
        return config.min_price_ticks + static_cast<std::uint32_t>(index);
    }

    [[nodiscard]] PriceLevel& level(Side side, std::size_t index) noexcept
    {
        return side == Side::buy ? bids[index] : asks[index];
    }

    [[nodiscard]] const PriceLevel& level(
        Side side,
        std::size_t index) const noexcept
    {
        return side == Side::buy ? bids[index] : asks[index];
    }

    [[nodiscard]] const OccupiedPriceSet& occupied(Side side) const noexcept
    {
        return side == Side::buy ? occupied_bids : occupied_asks;
    }

    [[nodiscard]] std::optional<std::size_t> best_level(Side side) const noexcept
    {
        return side == Side::buy ? best_bid : best_ask;
    }

    [[nodiscard]] std::optional<std::size_t> following_level(
        Side side,
        std::size_t index) const noexcept
    {
        return side == Side::buy
            ? occupied_bids.previous(index)
            : occupied_asks.next(index);
    }

    void occupy_level(Side side, std::size_t index) noexcept
    {
        if (side == Side::buy) {
            const bool changed = occupied_bids.set(index);
            assert(changed);
            (void)changed;
            if (!best_bid.has_value() || index > *best_bid) {
                best_bid = index;
            }
            return;
        }

        const bool changed = occupied_asks.set(index);
        assert(changed);
        (void)changed;
        if (!best_ask.has_value() || index < *best_ask) {
            best_ask = index;
        }
    }

    void vacate_level(Side side, std::size_t index) noexcept
    {
        if (side == Side::buy) {
            const bool changed = occupied_bids.clear(index);
            assert(changed);
            (void)changed;
            if (best_bid == index) {
                best_bid = occupied_bids.last();
            }
            return;
        }

        const bool changed = occupied_asks.clear(index);
        assert(changed);
        (void)changed;
        if (best_ask == index) {
            best_ask = occupied_asks.first();
        }
    }

    [[nodiscard]] bool valid_order(const NewOrder& order) const noexcept
    {
        return order.id != 0 && order.quantity != 0 &&
               order.quantity <= config.max_order_quantity &&
               order.price_ticks >= config.min_price_ticks &&
               order.price_ticks <= config.max_price_ticks &&
               is_valid(order.side) && is_valid(order.time_in_force);
    }

    [[nodiscard]] bool can_fill(const NewOrder& order) const noexcept
    {
        const Side maker_side = opposite(order.side);
        std::optional<std::size_t> current = best_level(maker_side);
        std::uint64_t quantity_needed = order.quantity;
        while (current.has_value()) {
            const std::uint32_t maker_price = price_of(*current);
            if (!crosses(order.side, order.price_ticks, maker_price)) {
                break;
            }
            const PriceLevel& maker_level = level(maker_side, *current);
            if (maker_level.total_quantity >= quantity_needed) {
                return true;
            }
            quantity_needed -= maker_level.total_quantity;
            current = following_level(maker_side, *current);
        }
        return false;
    }

    [[nodiscard]] std::uint64_t match(
        const NewOrder& taker,
        void* sink,
        TradeCallback on_trade) noexcept
    {
        const Side maker_side = opposite(taker.side);
        std::optional<std::size_t> current = best_level(maker_side);
        std::uint64_t remaining = taker.quantity;
        while (remaining != 0 && current.has_value()) {
            const std::uint32_t maker_price = price_of(*current);
            if (!crosses(taker.side, taker.price_ticks, maker_price)) {
                break;
            }

            PriceLevel& maker_level = level(maker_side, *current);
            assert(!maker_level.empty());
            const PoolIndex maker_index = maker_level.head;
            OrderNode& maker = pool[maker_index];
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
            maker_level.total_quantity -= traded;

            if (maker.remaining_quantity == 0) {
                const bool erased = active_orders.erase(maker.id);
                assert(erased);
                (void)erased;
                maker_level.unlink(pool, maker_index);
                pool.release(maker_index);
                if (maker_level.empty()) {
                    const std::size_t emptied_index = *current;
                    vacate_level(maker_side, emptied_index);
                    current = following_level(maker_side, emptied_index);
                }
            }

            on_trade(sink, trade);
        }
        return remaining;
    }

    OrderBookConfig config;
    OrderPool pool;
    std::vector<PriceLevel> bids;
    std::vector<PriceLevel> asks;
    OccupiedPriceSet occupied_bids;
    OccupiedPriceSet occupied_asks;
    std::optional<std::size_t> best_bid;
    std::optional<std::size_t> best_ask;
    ActiveIdIndex active_orders;
};

FastOrderBook::FastOrderBook(OrderBookConfig config)
    : impl_(std::make_unique<Impl>(config))
{
}

FastOrderBook::~FastOrderBook() = default;

SubmitResult FastOrderBook::submit_impl(
    NewOrder order,
    void* sink,
    TradeCallback on_trade) noexcept
{
    if (!impl_->valid_order(order)) {
        return {0, 0, 0, SubmitStatus::invalid};
    }
    const bool needs_preflight_id_lookup =
        order.time_in_force != TimeInForce::gtc ||
        impl_->active_orders.size() >= impl_->config.max_orders;
    if (needs_preflight_id_lookup && impl_->active_orders.contains(order.id)) {
        return {0, 0, 0, SubmitStatus::duplicate_id};
    }
    if (order.time_in_force == TimeInForce::fok && !impl_->can_fill(order)) {
        return {0, 0, order.quantity, SubmitStatus::accepted};
    }
    if (order.time_in_force == TimeInForce::gtc &&
        impl_->active_orders.size() >= impl_->config.max_orders) {
        return {0, 0, 0, SubmitStatus::capacity_exhausted};
    }

    PoolIndex incoming_index = invalid_index;
    if (order.time_in_force == TimeInForce::gtc) {
        const auto slot = impl_->pool.allocate();
        if (!slot.has_value()) {
            return {0, 0, 0, SubmitStatus::capacity_exhausted};
        }
        incoming_index = *slot;
        OrderNode& incoming = impl_->pool[incoming_index];
        incoming.id = order.id;
        incoming.remaining_quantity = order.quantity;
        incoming.price_ticks = order.price_ticks;
        incoming.side = order.side;

        const IdInsertResult inserted =
            impl_->active_orders.insert(order.id, incoming_index);
        if (inserted != IdInsertResult::inserted) {
            impl_->pool.release(incoming_index);
            return {
                0,
                0,
                0,
                inserted == IdInsertResult::duplicate
                    ? SubmitStatus::duplicate_id
                    : SubmitStatus::capacity_exhausted
            };
        }
    }

    const std::uint64_t remaining = impl_->match(order, sink, on_trade);
    const std::uint64_t executed = order.quantity - remaining;

    if (order.time_in_force == TimeInForce::gtc) {
        if (remaining != 0) {
            OrderNode& incoming = impl_->pool[incoming_index];
            incoming.remaining_quantity = remaining;
            const std::size_t price_index = impl_->index_of(order.price_ticks);
            PriceLevel& resting_level = impl_->level(order.side, price_index);
            const bool was_empty = resting_level.empty();
            resting_level.append(impl_->pool, incoming_index);
            if (was_empty) {
                impl_->occupy_level(order.side, price_index);
            }
            return {executed, remaining, 0, SubmitStatus::accepted};
        }

        const bool erased = impl_->active_orders.erase(order.id);
        assert(erased);
        (void)erased;
        impl_->pool.release(incoming_index);
    }

    if (order.time_in_force == TimeInForce::ioc) {
        return {executed, 0, remaining, SubmitStatus::accepted};
    }

    assert(remaining == 0);
    return {executed, 0, 0, SubmitStatus::accepted};
}

CancelResult FastOrderBook::cancel(OrderId id) noexcept
{
    if (id == 0) {
        return {0, CancelStatus::invalid};
    }
    const auto location = impl_->active_orders.find(id);
    if (!location.has_value()) {
        return {0, CancelStatus::not_found};
    }

    const PoolIndex index = *location;
    const OrderNode& node = impl_->pool[index];
    const Side side = node.side;
    const std::size_t price_index = impl_->index_of(node.price_ticks);
    const std::uint64_t canceled = node.remaining_quantity;
    PriceLevel& resting_level = impl_->level(side, price_index);
    resting_level.unlink(impl_->pool, index);
    if (resting_level.empty()) {
        impl_->vacate_level(side, price_index);
    }

    const bool erased = impl_->active_orders.erase(id);
    assert(erased);
    (void)erased;
    impl_->pool.release(index);
    return {canceled, CancelStatus::accepted};
}

Quote FastOrderBook::best_quote() const noexcept
{
    Quote result;
    if (impl_->best_bid.has_value()) {
        const PriceLevel& level = impl_->bids[*impl_->best_bid];
        result.bid = LevelQuote{level.total_quantity, impl_->price_of(*impl_->best_bid)};
    }
    if (impl_->best_ask.has_value()) {
        const PriceLevel& level = impl_->asks[*impl_->best_ask];
        result.ask = LevelQuote{level.total_quantity, impl_->price_of(*impl_->best_ask)};
    }
    return result;
}

BookSnapshot FastOrderBook::snapshot() const
{
    BookSnapshot result;
    result.bids.reserve(impl_->occupied_bids.count());
    result.asks.reserve(impl_->occupied_asks.count());

    const auto append_side = [&](Side side, auto& destination) {
        std::optional<std::size_t> current = impl_->best_level(side);
        while (current.has_value()) {
            const PriceLevel& level = impl_->level(side, *current);
            PriceLevelView view{
                .total_quantity = level.total_quantity,
                .price_ticks = impl_->price_of(*current),
                .orders = {}
            };
            view.orders.reserve(level.order_count);
            PoolIndex index = level.head;
            while (index != invalid_index) {
                const OrderNode& node = impl_->pool[index];
                view.orders.push_back({node.id, node.remaining_quantity});
                index = node.next;
            }
            destination.push_back(std::move(view));
            current = impl_->following_level(side, *current);
        }
    };

    append_side(Side::buy, result.bids);
    append_side(Side::sell, result.asks);
    return result;
}

BookStats FastOrderBook::stats() const noexcept
{
    BookStats result{};
    const auto sum_side = [&](Side side) {
        std::uint64_t quantity = 0;
        std::optional<std::size_t> current = impl_->best_level(side);
        while (current.has_value()) {
            quantity += impl_->level(side, *current).total_quantity;
            current = impl_->following_level(side, *current);
        }
        return quantity;
    };

    result.bid_quantity = sum_side(Side::buy);
    result.ask_quantity = sum_side(Side::sell);
    result.preallocated_storage_bytes = impl_->pool.storage_bytes() +
        impl_->bids.capacity() * sizeof(PriceLevel) +
        impl_->asks.capacity() * sizeof(PriceLevel) +
        impl_->occupied_bids.storage_bytes() +
        impl_->occupied_asks.storage_bytes() +
        impl_->active_orders.storage_bytes();
    result.total_probes = impl_->active_orders.total_probes();
    result.id_index_operations = impl_->active_orders.operation_count();
    result.id_index_capacity = impl_->active_orders.capacity();
    result.id_index_tombstones = impl_->active_orders.tombstones();
    result.max_probe_length = impl_->active_orders.max_probe_length();
    result.live_orders = impl_->pool.allocated_count();
    result.free_order_slots = impl_->pool.free_count();
    result.bid_levels = static_cast<std::uint32_t>(impl_->occupied_bids.count());
    result.ask_levels = static_cast<std::uint32_t>(impl_->occupied_asks.count());
    result.active_id_entries = static_cast<std::uint32_t>(impl_->active_orders.size());
    return result;
}

bool FastOrderBook::check_invariants() const
{
    if (!impl_->pool.check_invariants() ||
        !impl_->occupied_bids.check_invariants() ||
        !impl_->occupied_asks.check_invariants() ||
        !impl_->active_orders.check_invariants() ||
        impl_->pool.allocated_count() != impl_->active_orders.size() ||
        impl_->best_bid != impl_->occupied_bids.last() ||
        impl_->best_ask != impl_->occupied_asks.first()) {
        return false;
    }

    std::vector<std::uint8_t> membership(impl_->pool.capacity(), 0);
    const auto check_side = [&](Side expected_side) {
        const OccupiedPriceSet& occupied = impl_->occupied(expected_side);
        for (std::size_t price_index = 0;
             price_index < Impl::price_level_count(impl_->config);
             ++price_index) {
            const PriceLevel& level = impl_->level(expected_side, price_index);
            if (occupied.contains(price_index) == level.empty()) {
                return false;
            }
            if (level.empty()) {
                if (level.head != invalid_index || level.tail != invalid_index ||
                    level.total_quantity != 0 || level.order_count != 0) {
                    return false;
                }
                continue;
            }
            if (level.order_count == 0 || level.head >= impl_->pool.capacity() ||
                level.tail >= impl_->pool.capacity() ||
                impl_->pool[level.head].previous != invalid_index ||
                impl_->pool[level.tail].next != invalid_index) {
                return false;
            }

            const std::uint32_t price = impl_->price_of(price_index);
            std::uint64_t total = 0;
            std::uint32_t count = 0;
            PoolIndex previous = invalid_index;
            PoolIndex index = level.head;
            while (index != invalid_index) {
                if (index >= impl_->pool.capacity() || membership[index] != 0 ||
                    count >= impl_->pool.capacity()) {
                    return false;
                }
                membership[index] = 1;
                const OrderNode& node = impl_->pool[index];
                if (node.state != NodeState::live || node.id == 0 ||
                    node.remaining_quantity == 0 || node.side != expected_side ||
                    node.price_ticks != price || node.previous != previous ||
                    total > std::numeric_limits<std::uint64_t>::max() -
                                node.remaining_quantity) {
                    return false;
                }
                const auto active = impl_->active_orders.find(node.id);
                if (!active.has_value() || *active != index) {
                    return false;
                }
                total += node.remaining_quantity;
                previous = index;
                index = node.next;
                ++count;
            }
            if (previous != level.tail || count != level.order_count ||
                total != level.total_quantity) {
                return false;
            }
        }
        return true;
    };

    if (!check_side(Side::buy) || !check_side(Side::sell)) {
        return false;
    }

    std::uint32_t live_nodes = 0;
    for (PoolIndex index = 0; index < impl_->pool.capacity(); ++index) {
        const OrderNode& node = impl_->pool[index];
        if (node.state == NodeState::live) {
            if (membership[index] != 1) {
                return false;
            }
            ++live_nodes;
        } else if (node.state == NodeState::reserved || membership[index] != 0) {
            return false;
        }
    }

    bool active_entries_valid = true;
    impl_->active_orders.for_each([&](OrderId id, PoolIndex index) noexcept {
        if (index >= impl_->pool.capacity() ||
            impl_->pool[index].state != NodeState::live ||
            impl_->pool[index].id != id) {
            active_entries_valid = false;
        }
    });
    if (!active_entries_valid) {
        return false;
    }

    const bool uncrossed = !impl_->best_bid.has_value() ||
                           !impl_->best_ask.has_value() ||
                           *impl_->best_bid < *impl_->best_ask;
    return live_nodes == impl_->pool.allocated_count() && uncrossed;
}

} // namespace orderbook
