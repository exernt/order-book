#include "orderbook/order_book.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

using orderbook::NewOrder;
using orderbook::OrderBook;
using orderbook::Side;
using orderbook::SubmitStatus;
using orderbook::TimeInForce;
using orderbook::Trade;

struct RecordingSink {
    std::vector<Trade> trades;

    void on_trade(const Trade& trade) noexcept
    {
        trades.push_back(trade);
    }
};

[[noreturn]] void fail(std::string_view expression, int line)
{
    std::cerr << "test failure at line " << line << ": " << expression << '\n';
    std::exit(1);
}

#define REQUIRE(expression) \
    do { if (!(expression)) fail(#expression, __LINE__); } while (false)

[[nodiscard]] OrderBook make_book(std::uint32_t max_orders = 100)
{
    return OrderBook({
        .max_order_quantity = 1'000,
        .max_orders = max_orders,
        .min_price_ticks = 90,
        .max_price_ticks = 110
    });
}

[[nodiscard]] NewOrder order(
    std::uint64_t id,
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

void require_trade(
    const Trade& trade,
    std::uint64_t maker,
    std::uint64_t taker,
    std::uint32_t price,
    std::uint64_t quantity)
{
    REQUIRE(trade.maker_id == maker);
    REQUIRE(trade.taker_id == taker);
    REQUIRE(trade.price_ticks == price);
    REQUIRE(trade.quantity == quantity);
}

void test_validation_and_duplicate_ids()
{
    auto book = make_book();
    RecordingSink sink;
    sink.trades.reserve(8);

    REQUIRE(book.submit(order(0, Side::buy, 100, 1), sink).status == SubmitStatus::invalid);
    REQUIRE(book.submit(order(1, Side::buy, 100, 0), sink).status == SubmitStatus::invalid);
    REQUIRE(book.submit(order(1, Side::buy, 89, 1), sink).status == SubmitStatus::invalid);
    REQUIRE(book.submit(order(1, Side::buy, 111, 1), sink).status == SubmitStatus::invalid);
    REQUIRE(book.submit(order(1, Side::buy, 100, 1'001), sink).status == SubmitStatus::invalid);
    REQUIRE(book.stats().live_orders == 0);

    const auto accepted = book.submit(order(7, Side::buy, 99, 10), sink);
    REQUIRE(accepted.status == SubmitStatus::accepted);
    REQUIRE(accepted.resting_quantity == 10);

    const auto duplicate = book.submit(
        order(7, Side::sell, 99, 10, TimeInForce::ioc), sink);
    REQUIRE(duplicate.status == SubmitStatus::duplicate_id);
    REQUIRE(duplicate.executed_quantity == 0);
    REQUIRE(duplicate.resting_quantity == 0);
    REQUIRE(duplicate.canceled_quantity == 0);
    REQUIRE(book.check_invariants());
}

void test_price_time_priority_and_maker_prices()
{
    auto book = make_book();
    RecordingSink sink;
    sink.trades.reserve(8);

    (void)book.submit(order(1, Side::sell, 101, 4), sink);
    (void)book.submit(order(2, Side::sell, 100, 3), sink);
    (void)book.submit(order(3, Side::sell, 100, 5), sink);

    const auto result = book.submit(
        order(10, Side::buy, 101, 10, TimeInForce::ioc), sink);
    REQUIRE(result.status == SubmitStatus::accepted);
    REQUIRE(result.executed_quantity == 10);
    REQUIRE(result.resting_quantity == 0);
    REQUIRE(result.canceled_quantity == 0);
    REQUIRE(sink.trades.size() == 3);
    require_trade(sink.trades[0], 2, 10, 100, 3);
    require_trade(sink.trades[1], 3, 10, 100, 5);
    require_trade(sink.trades[2], 1, 10, 101, 2);

    const auto quote = book.best_quote();
    REQUIRE(!quote.bid.has_value());
    REQUIRE(quote.ask.has_value());
    REQUIRE(quote.ask->price_ticks == 101);
    REQUIRE(quote.ask->quantity == 2);
    REQUIRE(book.check_invariants());
}

void test_partial_fill_keeps_fifo_position()
{
    auto book = make_book();
    RecordingSink sink;
    sink.trades.reserve(8);

    (void)book.submit(order(1, Side::sell, 100, 10), sink);
    (void)book.submit(order(2, Side::sell, 100, 10), sink);
    (void)book.submit(order(3, Side::buy, 100, 4, TimeInForce::ioc), sink);
    (void)book.submit(order(4, Side::buy, 100, 8, TimeInForce::ioc), sink);

    REQUIRE(sink.trades.size() == 3);
    require_trade(sink.trades[0], 1, 3, 100, 4);
    require_trade(sink.trades[1], 1, 4, 100, 6);
    require_trade(sink.trades[2], 2, 4, 100, 2);
    REQUIRE(book.best_quote().ask->quantity == 8);
    REQUIRE(book.check_invariants());
}

void test_gtc_remainder_rests_at_tail()
{
    auto book = make_book();
    RecordingSink sink;
    sink.trades.reserve(8);

    (void)book.submit(order(1, Side::sell, 99, 3), sink);
    (void)book.submit(order(2, Side::buy, 100, 4), sink);
    (void)book.submit(order(3, Side::buy, 100, 2), sink);

    REQUIRE(sink.trades.size() == 1);
    require_trade(sink.trades[0], 1, 2, 99, 3);
    const auto first = book.submit(
        order(4, Side::sell, 100, 2, TimeInForce::ioc), sink);
    REQUIRE(first.executed_quantity == 2);
    const auto second = book.submit(
        order(5, Side::sell, 100, 2, TimeInForce::ioc), sink);
    REQUIRE(second.executed_quantity == 1);
    REQUIRE(second.canceled_quantity == 1);

    REQUIRE(sink.trades.size() == 4);
    require_trade(sink.trades[1], 2, 4, 100, 1);
    require_trade(sink.trades[2], 3, 4, 100, 1);
    require_trade(sink.trades[3], 3, 5, 100, 1);
    REQUIRE(book.check_invariants());
}

void test_ioc_cancels_only_the_remainder()
{
    auto book = make_book();
    RecordingSink sink;
    sink.trades.reserve(4);

    (void)book.submit(order(1, Side::sell, 100, 3), sink);
    (void)book.submit(order(2, Side::sell, 101, 4), sink);
    const auto result = book.submit(
        order(3, Side::buy, 100, 10, TimeInForce::ioc), sink);

    REQUIRE(result.executed_quantity == 3);
    REQUIRE(result.resting_quantity == 0);
    REQUIRE(result.canceled_quantity == 7);
    REQUIRE(book.stats().live_orders == 1);
    REQUIRE(book.best_quote().ask->price_ticks == 101);
    REQUIRE(book.check_invariants());
}

void test_fok_preflight_is_atomic()
{
    auto book = make_book();
    RecordingSink sink;
    sink.trades.reserve(8);

    (void)book.submit(order(1, Side::sell, 100, 3), sink);
    (void)book.submit(order(2, Side::sell, 101, 4), sink);
    const auto before = book.stats();

    const auto failed = book.submit(
        order(3, Side::buy, 101, 8, TimeInForce::fok), sink);
    REQUIRE(failed.status == SubmitStatus::accepted);
    REQUIRE(failed.executed_quantity == 0);
    REQUIRE(failed.resting_quantity == 0);
    REQUIRE(failed.canceled_quantity == 8);
    REQUIRE(sink.trades.empty());
    REQUIRE(book.stats().live_orders == before.live_orders);
    REQUIRE(book.stats().ask_quantity == before.ask_quantity);

    const auto filled = book.submit(
        order(4, Side::buy, 101, 7, TimeInForce::fok), sink);
    REQUIRE(filled.executed_quantity == 7);
    REQUIRE(filled.canceled_quantity == 0);
    REQUIRE(sink.trades.size() == 2);
    require_trade(sink.trades[0], 1, 4, 100, 3);
    require_trade(sink.trades[1], 2, 4, 101, 4);
    REQUIRE(book.stats().live_orders == 0);
    REQUIRE(book.check_invariants());
}

void test_cancel_and_id_reuse()
{
    auto book = make_book();
    RecordingSink sink;
    sink.trades.reserve(4);

    (void)book.submit(order(11, Side::buy, 99, 6), sink);
    const auto canceled = book.cancel(11);
    REQUIRE(canceled.status == orderbook::CancelStatus::accepted);
    REQUIRE(canceled.canceled_quantity == 6);
    REQUIRE(book.cancel(11).status == orderbook::CancelStatus::not_found);

    const auto reused = book.submit(order(11, Side::sell, 101, 7), sink);
    REQUIRE(reused.status == SubmitStatus::accepted);
    REQUIRE(reused.resting_quantity == 7);
    REQUIRE(book.check_invariants());
}

void test_capacity_rejection_changes_nothing()
{
    auto book = make_book(1);
    RecordingSink sink;
    sink.trades.reserve(2);

    (void)book.submit(order(1, Side::sell, 100, 5), sink);
    const auto rejected = book.submit(order(2, Side::buy, 100, 5), sink);
    REQUIRE(rejected.status == SubmitStatus::capacity_exhausted);
    REQUIRE(rejected.executed_quantity == 0);
    REQUIRE(sink.trades.empty());
    REQUIRE(book.best_quote().ask->quantity == 5);
    REQUIRE(book.check_invariants());
}

void test_snapshot_preserves_price_time_priority()
{
    auto book = make_book();
    RecordingSink sink;
    sink.trades.reserve(4);

    (void)book.submit(order(1, Side::buy, 99, 3), sink);
    (void)book.submit(order(2, Side::buy, 100, 4), sink);
    (void)book.submit(order(3, Side::buy, 100, 2), sink);
    (void)book.submit(order(4, Side::sell, 100, 5, TimeInForce::ioc), sink);
    (void)book.submit(order(5, Side::sell, 102, 7), sink);
    (void)book.submit(order(6, Side::sell, 101, 6), sink);

    const orderbook::BookSnapshot snapshot = book.snapshot();
    REQUIRE(snapshot.bids.size() == 2);
    REQUIRE(snapshot.bids[0].price_ticks == 100);
    REQUIRE(snapshot.bids[0].total_quantity == 1);
    REQUIRE(snapshot.bids[0].orders.size() == 1);
    REQUIRE(snapshot.bids[0].orders[0].id == 3);
    REQUIRE(snapshot.bids[0].orders[0].remaining_quantity == 1);
    REQUIRE(snapshot.bids[1].price_ticks == 99);
    REQUIRE(snapshot.bids[1].total_quantity == 3);

    REQUIRE(snapshot.asks.size() == 2);
    REQUIRE(snapshot.asks[0].price_ticks == 101);
    REQUIRE(snapshot.asks[0].total_quantity == 6);
    REQUIRE(snapshot.asks[0].orders[0].id == 6);
    REQUIRE(snapshot.asks[1].price_ticks == 102);
    REQUIRE(snapshot.asks[1].total_quantity == 7);
    REQUIRE(snapshot.asks[1].orders[0].id == 5);
    REQUIRE(book.check_invariants());
}

} // namespace

int main()
{
    test_validation_and_duplicate_ids();
    test_price_time_priority_and_maker_prices();
    test_partial_fill_keeps_fifo_position();
    test_gtc_remainder_rests_at_tail();
    test_ioc_cancels_only_the_remainder();
    test_fok_preflight_is_atomic();
    test_cancel_and_id_reuse();
    test_capacity_rejection_changes_nothing();
    test_snapshot_preserves_price_time_priority();
    std::cout << "all order-book tests passed\n";
    return 0;
}
