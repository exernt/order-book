#include "orderbook/detail/active_id_index.hpp"
#include "orderbook/detail/order_pool.hpp"
#include "orderbook/detail/occupied_price_set.hpp"
#include "orderbook/fast_order_book.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using orderbook::Side;
using orderbook::FastOrderBook;
using orderbook::detail::ActiveIdIndex;
using orderbook::detail::IdInsertResult;
using orderbook::detail::NodeState;
using orderbook::detail::OccupiedPriceSet;
using orderbook::detail::OrderNode;
using orderbook::detail::OrderPool;
using orderbook::detail::PoolIndex;
using orderbook::detail::PriceLevel;
using orderbook::detail::invalid_index;

[[noreturn]] void fail(std::string_view expression, int line)
{
    std::cerr << "component test failure at line " << line
              << ": " << expression << '\n';
    std::exit(1);
}

#define REQUIRE(expression) \
    do { if (!(expression)) fail(#expression, __LINE__); } while (false)

void initialize(
    OrderNode& node,
    std::uint64_t id,
    std::uint64_t quantity,
    std::uint32_t price,
    Side side)
{
    REQUIRE(node.state == NodeState::reserved);
    node.id = id;
    node.remaining_quantity = quantity;
    node.price_ticks = price;
    node.side = side;
}

void test_pool_exhaustion_and_slot_reuse()
{
    OrderPool pool(3);
    REQUIRE(pool.capacity() == 3);
    REQUIRE(pool.allocated_count() == 0);
    REQUIRE(pool.free_count() == 3);
    REQUIRE(pool.check_invariants());

    const auto first = pool.allocate();
    const auto second = pool.allocate();
    const auto third = pool.allocate();
    REQUIRE(first.has_value() && *first == 0);
    REQUIRE(second.has_value() && *second == 1);
    REQUIRE(third.has_value() && *third == 2);
    REQUIRE(!pool.allocate().has_value());
    REQUIRE(pool.allocated_count() == 3);
    REQUIRE(pool.free_count() == 0);
    REQUIRE(pool.check_invariants());

    pool.release(*second);
    REQUIRE(pool.allocated_count() == 2);
    REQUIRE(pool.free_count() == 1);
    REQUIRE(pool.check_invariants());

    const auto reused = pool.allocate();
    REQUIRE(reused.has_value() && *reused == *second);
    REQUIRE(pool[*reused].state == NodeState::reserved);
    REQUIRE(pool[*reused].previous == invalid_index);
    REQUIRE(pool[*reused].next == invalid_index);
    REQUIRE(pool.check_invariants());

    pool.release(*first);
    pool.release(*third);
    pool.release(*reused);
    REQUIRE(pool.free_count() == 3);
    REQUIRE(pool.check_invariants());
}

void test_invalid_pool_capacity()
{
    bool rejected_zero = false;
    try {
        OrderPool pool(0);
        (void)pool;
    } catch (const std::invalid_argument&) {
        rejected_zero = true;
    }
    REQUIRE(rejected_zero);

    bool rejected_sentinel = false;
    try {
        OrderPool pool(std::numeric_limits<std::uint32_t>::max());
        (void)pool;
    } catch (const std::invalid_argument&) {
        rejected_sentinel = true;
    }
    REQUIRE(rejected_sentinel);
}

void test_fifo_append_and_every_unlink_position()
{
    OrderPool pool(4);
    PriceLevel level;
    const PoolIndex first = *pool.allocate();
    const PoolIndex middle = *pool.allocate();
    const PoolIndex last = *pool.allocate();
    initialize(pool[first], 1, 10, 100, Side::buy);
    initialize(pool[middle], 2, 20, 100, Side::buy);
    initialize(pool[last], 3, 30, 100, Side::buy);

    level.append(pool, first);
    level.append(pool, middle);
    level.append(pool, last);
    REQUIRE(level.head == first);
    REQUIRE(level.tail == last);
    REQUIRE(level.order_count == 3);
    REQUIRE(level.total_quantity == 60);
    REQUIRE(pool[first].previous == invalid_index);
    REQUIRE(pool[first].next == middle);
    REQUIRE(pool[middle].previous == first);
    REQUIRE(pool[middle].next == last);
    REQUIRE(pool[last].previous == middle);
    REQUIRE(pool[last].next == invalid_index);

    // A partial fill changes quantities but not FIFO position.
    pool[first].remaining_quantity -= 4;
    level.total_quantity -= 4;
    REQUIRE(level.head == first);
    REQUIRE(level.total_quantity == 56);

    level.unlink(pool, middle);
    REQUIRE(level.head == first);
    REQUIRE(level.tail == last);
    REQUIRE(level.order_count == 2);
    REQUIRE(level.total_quantity == 36);
    REQUIRE(pool[first].next == last);
    REQUIRE(pool[last].previous == first);
    REQUIRE(pool[middle].state == NodeState::reserved);
    pool.release(middle);

    level.unlink(pool, first);
    REQUIRE(level.head == last);
    REQUIRE(level.tail == last);
    REQUIRE(level.order_count == 1);
    REQUIRE(level.total_quantity == 30);
    REQUIRE(pool[last].previous == invalid_index);
    pool.release(first);

    level.unlink(pool, last);
    REQUIRE(level.empty());
    REQUIRE(level.head == invalid_index);
    REQUIRE(level.tail == invalid_index);
    REQUIRE(level.order_count == 0);
    REQUIRE(level.total_quantity == 0);
    pool.release(last);
    REQUIRE(pool.check_invariants());
}

void test_one_element_removal_and_reappend()
{
    OrderPool pool(1);
    PriceLevel level;
    const PoolIndex index = *pool.allocate();
    initialize(pool[index], 9, 99, 101, Side::sell);

    level.append(pool, index);
    REQUIRE(level.head == index && level.tail == index);
    level.unlink(pool, index);
    REQUIRE(level.empty());
    REQUIRE(pool[index].state == NodeState::reserved);

    // A detached reserved node can be initialized and appended again without
    // changing its stable pool index.
    pool[index].remaining_quantity = 7;
    level.append(pool, index);
    REQUIRE(level.total_quantity == 7);
    REQUIRE(level.head == index && level.tail == index);
    level.unlink(pool, index);
    pool.release(index);
    REQUIRE(pool.check_invariants());
}

[[nodiscard]] std::optional<std::size_t> expected_first(
    const std::set<std::size_t>& reference)
{
    return reference.empty()
        ? std::nullopt
        : std::optional<std::size_t>(*reference.begin());
}

[[nodiscard]] std::optional<std::size_t> expected_last(
    const std::set<std::size_t>& reference)
{
    return reference.empty()
        ? std::nullopt
        : std::optional<std::size_t>(*reference.rbegin());
}

void verify_occupied_set(
    const OccupiedPriceSet& occupied,
    const std::set<std::size_t>& reference)
{
    REQUIRE(occupied.count() == reference.size());
    REQUIRE(occupied.empty() == reference.empty());
    REQUIRE(occupied.first() == expected_first(reference));
    REQUIRE(occupied.last() == expected_last(reference));
    REQUIRE(occupied.check_invariants());

    for (std::size_t index = 0; index < occupied.size(); ++index) {
        REQUIRE(occupied.contains(index) == reference.contains(index));

        const auto next = reference.upper_bound(index);
        const std::optional<std::size_t> expected_next = next == reference.end()
            ? std::nullopt
            : std::optional<std::size_t>(*next);
        REQUIRE(occupied.next(index) == expected_next);

        const auto lower = reference.lower_bound(index);
        const std::optional<std::size_t> expected_previous =
            lower == reference.begin()
                ? std::nullopt
                : std::optional<std::size_t>(*std::prev(lower));
        REQUIRE(occupied.previous(index) == expected_previous);
    }
}

void test_occupied_price_set_exhaustively()
{
    for (const std::size_t size : {1U, 2U, 63U, 64U, 65U, 127U, 128U, 129U}) {
        OccupiedPriceSet occupied(size);
        std::set<std::size_t> reference;
        verify_occupied_set(occupied, reference);

        for (std::size_t index = 0; index < size; ++index) {
            REQUIRE(occupied.set(index) == reference.insert(index).second);
            REQUIRE(!occupied.set(index));
            verify_occupied_set(occupied, reference);
        }
        for (std::size_t index = 0; index < size; index += 2) {
            REQUIRE(occupied.clear(index) == (reference.erase(index) == 1));
            REQUIRE(!occupied.clear(index));
            verify_occupied_set(occupied, reference);
        }
        for (std::size_t index = 1; index < size; index += 2) {
            REQUIRE(occupied.clear(index) == (reference.erase(index) == 1));
            verify_occupied_set(occupied, reference);
        }
    }
}

void test_occupied_price_set_hierarchy_and_sparse_boundaries()
{
    constexpr std::size_t size = 64U * 64U + 65U;
    OccupiedPriceSet occupied(size);
    std::set<std::size_t> reference;
    const std::vector<std::size_t> indices{
        0, 1, 62, 63, 64, 65, 127, 128, 4'095, 4'096, size - 2, size - 1
    };

    for (const std::size_t index : indices) {
        REQUIRE(occupied.set(index));
        reference.insert(index);
    }
    verify_occupied_set(occupied, reference);

    for (auto index = indices.rbegin(); index != indices.rend(); ++index) {
        REQUIRE(occupied.clear(*index));
        reference.erase(*index);
        verify_occupied_set(occupied, reference);
    }
}

void test_occupied_price_set_rejects_zero_size()
{
    bool rejected = false;
    try {
        OccupiedPriceSet occupied(0);
        (void)occupied;
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    REQUIRE(rejected);
}

void test_dense_ladder_configuration_limit()
{
    bool rejected = false;
    try {
        FastOrderBook book({
            .max_order_quantity = 1,
            .max_orders = 1,
            .min_price_ticks = 1,
            .max_price_ticks = FastOrderBook::max_dense_price_levels + 1
        });
        (void)book;
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    REQUIRE(rejected);
}

[[nodiscard]] std::uint64_t test_id_hash(std::uint64_t value) noexcept
{
    return ::rapidhashNano_withSeed(
        &value, sizeof(value), ActiveIdIndex::hash_seed);
}

void test_active_id_index_basic_operations()
{
    ActiveIdIndex index(7);
    REQUIRE(index.max_entries() == 7);
    REQUIRE(index.capacity() == ActiveIdIndex::required_capacity(7));
    REQUIRE(index.size() == 0);
    REQUIRE(index.tombstones() == 0);
    REQUIRE(!index.find(42).has_value());
    REQUIRE(!index.erase(42));

    REQUIRE(index.insert(42, 3) == IdInsertResult::inserted);
    REQUIRE(index.insert(42, 9) == IdInsertResult::duplicate);
    REQUIRE(index.contains(42));
    REQUIRE(index.find(42) == PoolIndex{3});
    REQUIRE(index.size() == 1);
    REQUIRE(index.erase(42));
    REQUIRE(!index.contains(42));
    REQUIRE(!index.erase(42));
    REQUIRE(index.size() == 0);

    // A deleted probe-chain slot must be reusable without losing later keys.
    REQUIRE(index.insert(42, 8) == IdInsertResult::inserted);
    REQUIRE(index.find(42) == PoolIndex{8});
    REQUIRE(index.check_invariants());
#if ORDERBOOK_ENABLE_ID_INDEX_DIAGNOSTICS
    REQUIRE(index.operation_count() != 0);
    REQUIRE(index.total_probes() >= index.operation_count());
    REQUIRE(index.max_probe_length() >= 1);
#else
    REQUIRE(index.operation_count() == 0);
    REQUIRE(index.total_probes() == 0);
    REQUIRE(index.max_probe_length() == 0);
#endif
}

void test_active_id_index_wraparound_and_backward_shift()
{
    ActiveIdIndex index(3);
    REQUIRE(index.capacity() == ActiveIdIndex::required_capacity(3));

    std::vector<std::uint64_t> colliding_ids;
    for (std::uint64_t id = 1; colliding_ids.size() != 3; ++id) {
        const std::uint64_t bucket =
            test_id_hash(id) & (index.capacity() - 1U);
        if (bucket == index.capacity() - 1U) {
            colliding_ids.push_back(id);
        }
    }

    REQUIRE(index.insert(colliding_ids[0], 10) == IdInsertResult::inserted);
    REQUIRE(index.insert(colliding_ids[1], 11) == IdInsertResult::inserted);
    REQUIRE(index.insert(colliding_ids[2], 12) == IdInsertResult::inserted);
#if ORDERBOOK_ENABLE_ID_INDEX_DIAGNOSTICS
    REQUIRE(index.max_probe_length() == 3);
#endif

    // The cluster occupies buckets 4, 0, and 1. Removing its head must shift
    // the wrapped entries backward so that lookup can still stop at an empty slot.
    REQUIRE(index.erase(colliding_ids[0]));
    REQUIRE(index.find(colliding_ids[1]) == PoolIndex{11});
    REQUIRE(index.find(colliding_ids[2]) == PoolIndex{12});
    REQUIRE(index.insert(colliding_ids[0], 13) == IdInsertResult::inserted);
    REQUIRE(index.find(colliding_ids[0]) == PoolIndex{13});
    REQUIRE(index.check_invariants());
}

void test_active_id_index_capacity_boundaries()
{
    ActiveIdIndex index(10);
    for (std::uint64_t id = 1; id <= 10; ++id) {
        REQUIRE(index.insert(id, static_cast<PoolIndex>(id)) ==
                IdInsertResult::inserted);
    }
    REQUIRE(index.size() == 10);
    REQUIRE(index.insert(5, 99) == IdInsertResult::duplicate);
    REQUIRE(index.insert(11, 11) == IdInsertResult::full);
    REQUIRE(index.erase(6));
    REQUIRE(index.insert(11, 11) == IdInsertResult::inserted);
    REQUIRE(index.find(11) == PoolIndex{11});
    REQUIRE(index.check_invariants());

    bool rejected_zero = false;
    try {
        ActiveIdIndex invalid(0);
        (void)invalid;
    } catch (const std::invalid_argument&) {
        rejected_zero = true;
    }
    REQUIRE(rejected_zero);

    bool rejected_overflow = false;
    try {
        ActiveIdIndex invalid(std::numeric_limits<std::uint32_t>::max());
        (void)invalid;
    } catch (const std::invalid_argument&) {
        rejected_overflow = true;
    }
    REQUIRE(rejected_overflow);

    bool rejected_book_overflow = false;
    try {
        FastOrderBook invalid({
            .max_order_quantity = 1,
            .max_orders = std::numeric_limits<std::uint32_t>::max() - 1U,
            .min_price_ticks = 1,
            .max_price_ticks = 1
        });
        (void)invalid;
    } catch (const std::invalid_argument&) {
        rejected_book_overflow = true;
    }
    REQUIRE(rejected_book_overflow);
}

void test_active_id_index_adversarial_patterns()
{
    constexpr std::uint32_t count = 1'024;
    ActiveIdIndex index(count);
    for (std::uint32_t value = 0; value < count; ++value) {
        const std::uint64_t id = (static_cast<std::uint64_t>(value) << 32U) | 7U;
        REQUIRE(index.insert(id, value) == IdInsertResult::inserted);
    }
    for (std::uint32_t value = 0; value < count; ++value) {
        const std::uint64_t id = (static_cast<std::uint64_t>(value) << 32U) | 7U;
        REQUIRE(index.find(id) == value);
    }
    for (std::uint32_t value = 0; value < count; value += 2) {
        const std::uint64_t id = (static_cast<std::uint64_t>(value) << 32U) | 7U;
        REQUIRE(index.erase(id));
    }
    for (std::uint32_t value = 0; value < count; value += 2) {
        const std::uint64_t id = (static_cast<std::uint64_t>(value) << 32U) | 9U;
        REQUIRE(index.insert(id, value) == IdInsertResult::inserted);
    }
    REQUIRE(index.size() == count);
    REQUIRE(index.check_invariants());
}

} // namespace

int main()
{
    test_active_id_index_adversarial_patterns();
    test_active_id_index_capacity_boundaries();
    test_active_id_index_wraparound_and_backward_shift();
    test_active_id_index_basic_operations();
    test_dense_ladder_configuration_limit();
    test_occupied_price_set_rejects_zero_size();
    test_occupied_price_set_exhaustively();
    test_occupied_price_set_hierarchy_and_sparse_boundaries();
    test_invalid_pool_capacity();
    test_pool_exhaustion_and_slot_reuse();
    test_fifo_append_and_every_unlink_position();
    test_one_element_removal_and_reappend();
    std::cout << "all component tests passed\n";
    return 0;
}
