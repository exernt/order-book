#include "matchingengine/matching_engine.hpp"
#include "orderbook/order_book.hpp"

#include <limits>
#include <utility>

std::optional<matchingengine::ClientId> matchingengine::MatchingEngine::register_client() noexcept {
    constexpr auto max = std::numeric_limits<matchingengine::ClientId>::max();

    if (next_client_id_ > max) {
        return std::nullopt;
    }

    return static_cast<matchingengine::ClientId>(next_client_id_++);
}

bool matchingengine::MatchingEngine::is_registered(matchingengine::ClientId id) const noexcept {
    return id != 0 && static_cast<std::uint64_t>(id) < next_client_id_;
}

orderbook::CancelResult matchingengine::MatchingEngine::cancel(ClientId client_id, ClientOrderId order_id) noexcept {
    if (!is_registered(client_id) || order_id == 0) {
        return {
            .canceled_quantity = 0,
            .status = orderbook::CancelStatus::invalid
        };
    }

    return book_.cancel(make_order_id(client_id, order_id));
}

matchingengine::ClientBookSnapshot matchingengine::MatchingEngine::snapshot() const {
    const orderbook::BookSnapshot book_snapshot = book_.snapshot();
    ClientBookSnapshot result;
    result.bids.reserve(book_snapshot.bids.size());
    result.asks.reserve(book_snapshot.asks.size());

    const auto append_levels = [](const auto& source, auto& destination) {
        for (const orderbook::PriceLevelView& level : source) {
            ClientPriceLevel client_level{
                .total_quantity = level.total_quantity,
                .price_ticks = level.price_ticks
            };
            client_level.orders.reserve(level.orders.size());
            for (const orderbook::RestingOrderView& order : level.orders) {
                client_level.orders.push_back({
                    .key = split_order_id(order.id),
                    .remaining_quantity = order.remaining_quantity
                });
            }
            destination.push_back(std::move(client_level));
        }
    };

    append_levels(book_snapshot.bids, result.bids);
    append_levels(book_snapshot.asks, result.asks);
    return result;
}
