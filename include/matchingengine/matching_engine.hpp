#pragma once

#include "orderbook/order_book.hpp"

#include <concepts>
#include <cstdint>
#include <optional>
#include <vector>

namespace matchingengine {

    using ClientId = std::uint32_t;
    using ClientOrderId = std::uint32_t;

    struct ClientOrderKey {
        ClientId client_id;
        ClientOrderId order_id;
    };

    struct ClientOrder {
        ClientOrderId order_id;
        std::uint64_t quantity;
        std::uint32_t price_ticks;
        orderbook::Side side;
        orderbook::TimeInForce time_in_force;
    };
    
    struct ClientTrade {
        ClientOrderKey maker;
        ClientOrderKey taker;
        std::uint64_t quantity;
        std::uint32_t price_ticks;
    };

    struct ClientRestingOrder {
        ClientOrderKey key;
        std::uint64_t remaining_quantity;
    };

    struct ClientPriceLevel {
        std::uint64_t total_quantity;
        std::uint32_t price_ticks;
        std::vector<ClientRestingOrder> orders;
    };

    struct ClientBookSnapshot {
        // Both sides are best-to-worst; orders retain FIFO priority.
        std::vector<ClientPriceLevel> bids;
        std::vector<ClientPriceLevel> asks;
    };


    constexpr orderbook::OrderId make_order_id(ClientId client_id, ClientOrderId client_order_id) noexcept {
        return (static_cast<orderbook::OrderId>(client_id) << 32) | static_cast<orderbook::OrderId>(client_order_id);
    }

    constexpr ClientOrderKey split_order_id(orderbook::OrderId id) noexcept {
        return {
            .client_id = static_cast<ClientId>(id >> 32),
            .order_id = static_cast<ClientOrderId>(id)
        };
    }

    template <typename T>
    concept ClientTradeSink = requires(T& sink, const ClientTrade& trade) {
        { sink.on_trade(trade) } noexcept -> std::same_as<void>;
    };

    class MatchingEngine {
    public:
        explicit MatchingEngine(orderbook::OrderBookConfig config)
            : book_(config) {}

        [[nodiscard]]
        std::optional<ClientId> register_client() noexcept;

        template <ClientTradeSink Sink>
        [[nodiscard]]
        orderbook::SubmitResult submit(
            ClientId client_id,
            ClientOrder order,
            Sink& sink) noexcept {
            if (!is_registered(client_id) || order.order_id == 0) {
                return {
                    .executed_quantity = 0,
                    .resting_quantity = 0,
                    .canceled_quantity = 0,
                    .status = orderbook::SubmitStatus::invalid
                };
            }

            struct SinkAdapter {
                Sink& destination;

                void on_trade(const orderbook::Trade& trade) noexcept
                {
                    destination.on_trade(ClientTrade{
                        .maker = split_order_id(trade.maker_id),
                        .taker = split_order_id(trade.taker_id),
                        .quantity = trade.quantity,
                        .price_ticks = trade.price_ticks
                    });
                }
            };

            SinkAdapter adapter{sink};
            
            return book_.submit(orderbook::NewOrder{
                .id = make_order_id(client_id, order.order_id),
                .quantity = order.quantity,
                .price_ticks = order.price_ticks,
                .side = order.side,
                .time_in_force = order.time_in_force
            }, adapter);
        }

        [[nodiscard]]
        orderbook::CancelResult cancel(
            ClientId client_id,
            ClientOrderId order_id) noexcept;

        [[nodiscard]] orderbook::Quote best_quote() const noexcept {
            return book_.best_quote();
        }
        [[nodiscard]] ClientBookSnapshot snapshot() const;
        [[nodiscard]] orderbook::BookStats stats() const noexcept {
            return book_.stats();
        }
        [[nodiscard]] bool check_invariants() const {
            return book_.check_invariants();
        }

    private:
        [[nodiscard]]
        bool is_registered(ClientId id) const noexcept;

        std::uint64_t next_client_id_{1};
        orderbook::OrderBook book_;
    };

}
