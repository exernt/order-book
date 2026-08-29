#pragma once

#include "types.hpp"

#include <concepts>
#include <memory>

namespace orderbook {

    template <typename T>
    concept TradeSink = requires(T& sink, const Trade& trade) {
        { sink.on_trade(trade) } noexcept -> std::same_as<void>;
    };

    class OrderBook {
    public:

        // non-movable
        OrderBook(OrderBook&&) = delete;
        OrderBook& operator=(OrderBook&&) = delete;
        // non-copyable
        OrderBook(const OrderBook&) = delete;
        OrderBook& operator=(const OrderBook&) = delete;

        explicit OrderBook(OrderBookConfig config);
        ~OrderBook();

        template <TradeSink Sink>
        [[nodiscard]]
        SubmitResult submit(NewOrder order, Sink& sink) noexcept {
            return submit_impl(
                order,
                &sink,
                [](void* context, const Trade& trade) noexcept {
                    static_cast<Sink*>(context)->on_trade(trade);
                });
        }

        [[nodiscard]] CancelResult cancel(OrderId id) noexcept;
        [[nodiscard]] Quote best_quote() const noexcept;
        [[nodiscard]] BookSnapshot snapshot() const;
        [[nodiscard]] BookStats stats() const noexcept;
        [[nodiscard]] bool check_invariants() const;

    private:
        using TradeCallback = void (*)(void*, const Trade&) noexcept;

        struct Impl;

        [[nodiscard]] SubmitResult submit_impl(
            NewOrder order,
            void* sink,
            TradeCallback on_trade) noexcept;

        std::unique_ptr<Impl> impl_;
    };
}



