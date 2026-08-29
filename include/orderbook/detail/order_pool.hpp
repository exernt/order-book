#pragma once

#include "orderbook/types.hpp"

#include <cassert>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace orderbook::detail {

using PoolIndex = std::uint32_t;
inline constexpr PoolIndex invalid_index = std::numeric_limits<PoolIndex>::max();

enum class NodeState : std::uint8_t {
    free,
    reserved,
    live
};

struct OrderNode {
    OrderId id{0};
    std::uint64_t remaining_quantity{0};
    std::uint32_t price_ticks{0};
    PoolIndex previous{invalid_index};
    PoolIndex next{invalid_index};
    Side side{Side::buy};
    NodeState state{NodeState::free};
};

static_assert(std::is_trivially_destructible_v<OrderNode>);
static_assert(sizeof(OrderNode) <= 32, "OrderNode no longer fits its intended budget");

class OrderPool {
public:
    explicit OrderPool(std::uint32_t capacity)
        : nodes_(validated_capacity(capacity))
    {
        for (PoolIndex index = 0; index < capacity; ++index) {
            nodes_[index].next = index + 1 < capacity ? index + 1 : invalid_index;
        }
        free_head_ = 0;
    }

    [[nodiscard]] std::optional<PoolIndex> allocate() noexcept
    {
        if (free_head_ == invalid_index) {
            return std::nullopt;
        }

        const PoolIndex result = free_head_;
        OrderNode& slot = nodes_[result];
        assert(slot.state == NodeState::free);
        free_head_ = slot.next;
        slot.previous = invalid_index;
        slot.next = invalid_index;
        slot.state = NodeState::reserved;
        ++allocated_count_;
        return result;
    }

    void release(PoolIndex index) noexcept
    {
        assert(index < nodes_.size());
        OrderNode& slot = nodes_[index];
        assert(slot.state == NodeState::reserved);

        slot = OrderNode{};
        slot.next = free_head_;
        free_head_ = index;
        assert(allocated_count_ != 0);
        --allocated_count_;
    }

    [[nodiscard]] OrderNode& operator[](PoolIndex index) noexcept
    {
        assert(index < nodes_.size());
        return nodes_[index];
    }

    [[nodiscard]] const OrderNode& operator[](PoolIndex index) const noexcept
    {
        assert(index < nodes_.size());
        return nodes_[index];
    }

    [[nodiscard]] std::uint32_t capacity() const noexcept
    {
        return static_cast<std::uint32_t>(nodes_.size());
    }

    [[nodiscard]] std::uint32_t allocated_count() const noexcept
    {
        return allocated_count_;
    }

    [[nodiscard]] std::uint32_t free_count() const noexcept
    {
        return capacity() - allocated_count_;
    }

    [[nodiscard]] std::size_t storage_bytes() const noexcept
    {
        return nodes_.capacity() * sizeof(OrderNode);
    }

    [[nodiscard]] bool check_invariants() const
    {
        if (allocated_count_ > nodes_.size()) {
            return false;
        }

        std::vector<bool> seen(nodes_.size(), false);
        std::uint32_t free_nodes = 0;
        PoolIndex index = free_head_;
        while (index != invalid_index) {
            if (index >= nodes_.size() || seen[index] ||
                nodes_[index].state != NodeState::free ||
                nodes_[index].previous != invalid_index) {
                return false;
            }
            seen[index] = true;
            ++free_nodes;
            index = nodes_[index].next;
        }

        for (PoolIndex slot = 0; slot < nodes_.size(); ++slot) {
            if ((nodes_[slot].state == NodeState::free) != seen[slot]) {
                return false;
            }
        }
        return free_nodes == free_count();
    }

private:
    [[nodiscard]] static std::uint32_t validated_capacity(std::uint32_t capacity)
    {
        if (capacity == 0 ||
            capacity == std::numeric_limits<std::uint32_t>::max()) {
            throw std::invalid_argument("invalid order pool capacity");
        }
        return capacity;
    }

    std::vector<OrderNode> nodes_;
    PoolIndex free_head_{invalid_index};
    std::uint32_t allocated_count_{0};
};

struct PriceLevel {
    PoolIndex head{invalid_index};
    PoolIndex tail{invalid_index};
    std::uint64_t total_quantity{0};
    std::uint32_t order_count{0};

    [[nodiscard]] bool empty() const noexcept
    {
        return head == invalid_index;
    }

    void append(OrderPool& pool, PoolIndex index) noexcept
    {
        OrderNode& node = pool[index];
        assert(node.state == NodeState::reserved);
        assert(node.remaining_quantity != 0);
        assert(node.previous == invalid_index && node.next == invalid_index);
        assert(total_quantity <=
               std::numeric_limits<std::uint64_t>::max() - node.remaining_quantity);

        node.previous = tail;
        if (tail == invalid_index) {
            assert(head == invalid_index && order_count == 0);
            head = index;
        } else {
            pool[tail].next = index;
        }
        tail = index;
        node.state = NodeState::live;
        total_quantity += node.remaining_quantity;
        ++order_count;
    }

    void unlink(OrderPool& pool, PoolIndex index) noexcept
    {
        OrderNode& node = pool[index];
        assert(node.state == NodeState::live);
        assert(total_quantity >= node.remaining_quantity);
        assert(order_count != 0);

        if (node.previous == invalid_index) {
            assert(head == index);
            head = node.next;
        } else {
            pool[node.previous].next = node.next;
        }

        if (node.next == invalid_index) {
            assert(tail == index);
            tail = node.previous;
        } else {
            pool[node.next].previous = node.previous;
        }

        total_quantity -= node.remaining_quantity;
        --order_count;
        node.previous = invalid_index;
        node.next = invalid_index;
        node.state = NodeState::reserved;

        assert((order_count == 0) == (head == invalid_index));
        assert((order_count == 0) == (tail == invalid_index));
    }
};

} // namespace orderbook::detail
