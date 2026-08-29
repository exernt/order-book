#pragma once

#include "orderbook/detail/order_pool.hpp"
#include "orderbook/detail/rapidhash.h"
#include "orderbook/types.hpp"

#include <cassert>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace orderbook::detail {

#ifndef ORDERBOOK_ENABLE_ID_INDEX_DIAGNOSTICS
#define ORDERBOOK_ENABLE_ID_INDEX_DIAGNOSTICS 1
#endif

enum class IdInsertResult : std::uint8_t {
    inserted,
    duplicate,
    full
};

class ActiveIdIndex {
public:
    static constexpr std::uint32_t max_load_numerator = 80;
    static constexpr std::uint32_t max_load_denominator = 100;
    static constexpr std::uint64_t hash_seed = 0x6a09e667f3bcc909ULL;

    explicit ActiveIdIndex(std::uint32_t max_entries)
        : entries_(required_capacity(max_entries)),
          max_entries_(max_entries)
    {
    }

    [[nodiscard]] static std::uint32_t required_capacity(
        std::uint32_t max_entries)
    {
        if (max_entries == 0) {
            throw std::invalid_argument("active ID capacity must be positive");
        }
        const std::uint64_t buckets =
            (static_cast<std::uint64_t>(max_entries) * max_load_denominator +
             max_load_numerator - 1U) /
            max_load_numerator;
        const std::uint64_t actual_buckets = std::bit_ceil(buckets);
        if (actual_buckets > std::numeric_limits<std::uint32_t>::max()) {
            throw std::invalid_argument("active ID table capacity overflows");
        }
        return static_cast<std::uint32_t>(actual_buckets);
    }

    [[nodiscard]] IdInsertResult insert(OrderId id, PoolIndex index) noexcept
    {
        const std::size_t start = bucket_for(id);
        std::size_t bucket = start;
        std::uint32_t probes = 0;
        do {
            ++probes;
            Entry& entry = entries_[bucket];
            if (!entry.occupied) {
                if (size_ == max_entries_) {
                    record_probes(probes);
                    return IdInsertResult::full;
                }
                entry.id = id;
                entry.pool_index = index;
                entry.occupied = true;
                ++size_;
                record_probes(probes);
                return IdInsertResult::inserted;
            }
            if (entry.id == id) {
                record_probes(probes);
                return IdInsertResult::duplicate;
            }
            bucket = next_bucket(bucket);
        } while (bucket != start);

        record_probes(probes);
        return IdInsertResult::full;
    }

    [[nodiscard]] std::optional<PoolIndex> find(OrderId id) const noexcept
    {
        const std::size_t start = bucket_for(id);
        std::size_t bucket = start;
        std::uint32_t probes = 0;
        do {
            ++probes;
            const Entry& entry = entries_[bucket];
            if (!entry.occupied) {
                record_probes(probes);
                return std::nullopt;
            }
            if (entry.id == id) {
                record_probes(probes);
                return entry.pool_index;
            }
            bucket = next_bucket(bucket);
        } while (bucket != start);

        record_probes(probes);
        return std::nullopt;
    }

    [[nodiscard]] bool contains(OrderId id) const noexcept
    {
        return find(id).has_value();
    }

    [[nodiscard]] bool erase(OrderId id) noexcept
    {
        const std::size_t start = bucket_for(id);
        std::size_t bucket = start;
        std::uint32_t probes = 0;
        do {
            ++probes;
            Entry& entry = entries_[bucket];
            if (!entry.occupied) {
                record_probes(probes);
                return false;
            }
            if (entry.id == id) {
                erase_at(bucket);
                assert(size_ != 0);
                --size_;
                record_probes(probes);
                return true;
            }
            bucket = next_bucket(bucket);
        } while (bucket != start);

        record_probes(probes);
        return false;
    }

    [[nodiscard]] std::uint32_t size() const noexcept
    {
        return size_;
    }

    [[nodiscard]] std::uint32_t max_entries() const noexcept
    {
        return max_entries_;
    }

    [[nodiscard]] std::uint32_t capacity() const noexcept
    {
        return static_cast<std::uint32_t>(entries_.size());
    }

    [[nodiscard]] std::size_t storage_bytes() const noexcept
    {
        return entries_.capacity() * sizeof(Entry);
    }

    [[nodiscard]] constexpr std::uint32_t tombstones() const noexcept
    {
        return 0;
    }

    [[nodiscard]] std::uint64_t total_probes() const noexcept
    {
#if ORDERBOOK_ENABLE_ID_INDEX_DIAGNOSTICS
        return total_probes_;
#else
        return 0;
#endif
    }

    [[nodiscard]] std::uint64_t operation_count() const noexcept
    {
#if ORDERBOOK_ENABLE_ID_INDEX_DIAGNOSTICS
        return operation_count_;
#else
        return 0;
#endif
    }

    [[nodiscard]] std::uint32_t max_probe_length() const noexcept
    {
#if ORDERBOOK_ENABLE_ID_INDEX_DIAGNOSTICS
        return max_probe_length_;
#else
        return 0;
#endif
    }

    template <typename Function>
    void for_each(Function&& function) const
        noexcept(noexcept(function(OrderId{}, PoolIndex{})))
    {
        for (const Entry& entry : entries_) {
            if (entry.occupied) {
                function(entry.id, entry.pool_index);
            }
        }
    }

    [[nodiscard]] bool check_invariants() const noexcept
    {
        std::uint32_t occupied_count = 0;
        for (std::size_t slot = 0; slot < entries_.size(); ++slot) {
            const Entry& entry = entries_[slot];
            if (!entry.occupied) {
                continue;
            }
            ++occupied_count;

            // An empty bucket before this entry would make it unreachable.
            std::size_t bucket = bucket_for(entry.id);
            while (bucket != slot && entries_[bucket].occupied) {
                if (entries_[bucket].id == entry.id) {
                    return false;
                }
                bucket = next_bucket(bucket);
            }
            if (bucket != slot) {
                return false;
            }
        }
        return occupied_count == size_ && size_ <= max_entries_;
    }

private:
    struct Entry {
        OrderId id{0};
        PoolIndex pool_index{invalid_index};
        bool occupied{false};
    };

    static_assert(std::is_trivially_destructible_v<Entry>);
    static_assert(sizeof(Entry) <= 16, "ActiveIdIndex entry exceeds its memory budget");

    [[nodiscard]] static RAPIDHASH_INLINE std::uint64_t mixed_hash(
        std::uint64_t value) noexcept
    {
        return ::rapidhashNano_withSeed(
            &value, sizeof(value), hash_seed);
    }

    [[nodiscard]] std::size_t bucket_for(OrderId id) const noexcept
    {
        return static_cast<std::size_t>(mixed_hash(id)) & (entries_.size() - 1U);
    }

    [[nodiscard]] std::size_t next_bucket(std::size_t bucket) const noexcept
    {
        ++bucket;
        return bucket == entries_.size() ? 0 : bucket;
    }

    [[nodiscard]] std::size_t probe_distance(
        std::size_t home,
        std::size_t slot) const noexcept
    {
        return slot >= home ? slot - home : entries_.size() - home + slot;
    }

    void erase_at(std::size_t hole) noexcept
    {
        std::size_t scan = next_bucket(hole);
        while (entries_[scan].occupied) {
            const std::size_t home = bucket_for(entries_[scan].id);
            if (probe_distance(home, hole) < probe_distance(home, scan)) {
                entries_[hole] = entries_[scan];
                hole = scan;
            }
            scan = next_bucket(scan);
        }
        entries_[hole] = Entry{};
    }

    void record_probes(std::uint32_t probes) const noexcept
    {
#if ORDERBOOK_ENABLE_ID_INDEX_DIAGNOSTICS
        total_probes_ += probes;
        ++operation_count_;
        if (probes > max_probe_length_) {
            max_probe_length_ = probes;
        }
#else
        (void)probes;
#endif
    }

    std::vector<Entry> entries_;
    std::uint32_t max_entries_{0};
    std::uint32_t size_{0};
#if ORDERBOOK_ENABLE_ID_INDEX_DIAGNOSTICS
    mutable std::uint64_t total_probes_{0};
    mutable std::uint64_t operation_count_{0};
    mutable std::uint32_t max_probe_length_{0};
#endif
};

} // namespace orderbook::detail
