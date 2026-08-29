#pragma once

#include "orderbook/types.hpp"

#include <cstdint>
#include <concepts>
#include <memory>

namespace orderbook {

class FastOrderBook {
public:
    // Two dense PriceLevel arrays are allocated at construction. Wider price
    // domains should use a sparse book representation instead.
    static constexpr std::uint32_t max_dense_price_levels = 1'000'000;

    FastOrderBook(FastOrderBook&&) = delete;
    FastOrderBook& operator=(FastOrderBook&&) = delete;
    FastOrderBook(const FastOrderBook&) = delete;
    FastOrderBook& operator=(const FastOrderBook&) = delete;

    explicit FastOrderBook(OrderBookConfig config);
    ~FastOrderBook();

    template <typename Sink>
        requires requires(Sink& sink, const Trade& trade) {
            { sink.on_trade(trade) } noexcept -> std::same_as<void>;
        }
    [[nodiscard]] SubmitResult submit(NewOrder order, Sink& sink) noexcept
    {
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

} // namespace orderbook
