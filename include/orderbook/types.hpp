#pragma once

#include <cstdint>
#include <optional>
#include <vector>


namespace orderbook{
    
    using OrderId = std::uint64_t;

    enum class Side : std::uint8_t {
        buy,
        sell
    };

    enum class TimeInForce : std::uint8_t {
        gtc,
        ioc,
        fok
    };

    enum class SubmitStatus : std::uint8_t {
        accepted,
        invalid,
        duplicate_id,
        capacity_exhausted
    };

    enum class CancelStatus : std::uint8_t {
        accepted,
        invalid,
        not_found
    };


    struct NewOrder {
        OrderId id;
        std::uint64_t quantity;
        std::uint32_t price_ticks;
        Side side;
        TimeInForce time_in_force;
    };

    struct Trade {
        OrderId maker_id;
        OrderId taker_id;
        std::uint64_t quantity;
        std::uint32_t price_ticks;
    };

    struct SubmitResult {
        std::uint64_t executed_quantity;
        std::uint64_t resting_quantity;
        std::uint64_t canceled_quantity;
        SubmitStatus status;
    };

    struct CancelResult {
        std::uint64_t canceled_quantity;
        CancelStatus status;
    };

    struct LevelQuote {
        std::uint64_t quantity;
        std::uint32_t price_ticks;
    };

    struct Quote {
        std::optional<LevelQuote> bid;
        std::optional<LevelQuote> ask;
    };

    struct RestingOrderView {
        OrderId id;
        std::uint64_t remaining_quantity;
    };

    struct PriceLevelView {
        std::uint64_t total_quantity;
        std::uint32_t price_ticks;
        std::vector<RestingOrderView> orders;
    };

    struct BookSnapshot {
        // Both sides are best-to-worst; orders retain FIFO priority.
        std::vector<PriceLevelView> bids;
        std::vector<PriceLevelView> asks;
    };

    struct BookStats {
        std::uint64_t bid_quantity;
        std::uint64_t ask_quantity;
        std::uint64_t preallocated_storage_bytes;

        std::uint64_t total_probes;
        std::uint64_t id_index_operations;
        std::uint32_t id_index_capacity;
        std::uint32_t id_index_tombstones;
        std::uint32_t max_probe_length;

        std::uint32_t live_orders;
        std::uint32_t free_order_slots;
        std::uint32_t bid_levels;
        std::uint32_t ask_levels;
        std::uint32_t active_id_entries;
    };

    struct OrderBookConfig {
        std::uint64_t max_order_quantity;
        std::uint32_t max_orders;
        std::uint32_t min_price_ticks;
        std::uint32_t max_price_ticks;
    };

}
