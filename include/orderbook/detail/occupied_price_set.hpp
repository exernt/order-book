#pragma once

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

namespace orderbook::detail {

class OccupiedPriceSet {
public:
    explicit OccupiedPriceSet(std::size_t price_count)
        : price_count_(validated_price_count(price_count))
    {
        levels_.emplace_back(word_count(price_count_), std::uint64_t{0});
        while (levels_.back().size() > 1) {
            levels_.emplace_back(
                word_count(levels_.back().size()), std::uint64_t{0});
        }
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return price_count_;
    }

    [[nodiscard]] std::size_t count() const noexcept
    {
        return occupied_count_;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return occupied_count_ == 0;
    }

    [[nodiscard]] std::size_t storage_bytes() const noexcept
    {
        std::size_t bytes = 0;
        for (const auto& level : levels_) {
            bytes += level.capacity() * sizeof(std::uint64_t);
        }
        return bytes;
    }

    [[nodiscard]] bool contains(std::size_t index) const noexcept
    {
        if (index >= price_count_) {
            return false;
        }
        return (levels_[0][index / bits_per_word] & bit(index)) != 0;
    }

    // Returns true only for an empty-to-occupied transition.
    bool set(std::size_t index) noexcept
    {
        assert(index < price_count_);
        std::size_t child_index = index;
        for (std::size_t level = 0; level < levels_.size(); ++level) {
            std::uint64_t& word = levels_[level][child_index / bits_per_word];
            const std::uint64_t mask = bit(child_index);
            if ((word & mask) != 0) {
                return false;
            }
            const bool was_empty = word == 0;
            word |= mask;
            if (!was_empty) {
                break;
            }
            child_index /= bits_per_word;
        }
        ++occupied_count_;
        return true;
    }

    // Returns true only for an occupied-to-empty transition.
    bool clear(std::size_t index) noexcept
    {
        assert(index < price_count_);
        std::size_t child_index = index;
        for (std::size_t level = 0; level < levels_.size(); ++level) {
            std::uint64_t& word = levels_[level][child_index / bits_per_word];
            const std::uint64_t mask = bit(child_index);
            if ((word & mask) == 0) {
                return false;
            }
            word &= ~mask;
            if (word != 0) {
                break;
            }
            child_index /= bits_per_word;
        }
        assert(occupied_count_ != 0);
        --occupied_count_;
        return true;
    }

    [[nodiscard]] std::optional<std::size_t> first() const noexcept
    {
        if (empty()) {
            return std::nullopt;
        }
        return descend(0, levels_.size() - 1, Direction::forward);
    }

    [[nodiscard]] std::optional<std::size_t> last() const noexcept
    {
        if (empty()) {
            return std::nullopt;
        }
        return descend(0, levels_.size() - 1, Direction::backward);
    }

    [[nodiscard]] std::optional<std::size_t> next(
        std::size_t index) const noexcept
    {
        if (index >= price_count_) {
            return std::nullopt;
        }
        return adjacent(index, Direction::forward);
    }

    [[nodiscard]] std::optional<std::size_t> previous(
        std::size_t index) const noexcept
    {
        if (index >= price_count_) {
            return std::nullopt;
        }
        return adjacent(index, Direction::backward);
    }

    [[nodiscard]] bool check_invariants() const noexcept
    {
        std::size_t counted = 0;
        for (const std::uint64_t word : levels_[0]) {
            counted += std::popcount(word);
        }
        if (counted != occupied_count_) {
            return false;
        }

        for (std::size_t level = 1; level < levels_.size(); ++level) {
            for (std::size_t child = 0; child < levels_[level - 1].size(); ++child) {
                const bool expected = levels_[level - 1][child] != 0;
                const bool actual =
                    (levels_[level][child / bits_per_word] & bit(child)) != 0;
                if (actual != expected) {
                    return false;
                }
            }
        }

        const std::size_t valid_bits =
            levels_.size() == 1 ? price_count_ : levels_[levels_.size() - 2].size();
        const std::uint64_t padding_mask = valid_bits % bits_per_word == 0
            ? std::uint64_t{0}
            : ~((std::uint64_t{1} << (valid_bits % bits_per_word)) - 1);
        return (levels_.back().back() & padding_mask) == 0;
    }

private:
    static constexpr std::size_t bits_per_word = 64;

    enum class Direction : std::uint8_t { forward, backward };

    [[nodiscard]] static std::size_t validated_price_count(std::size_t count)
    {
        if (count == 0) {
            throw std::invalid_argument("occupied price set cannot be empty");
        }
        return count;
    }

    [[nodiscard]] static constexpr std::size_t word_count(
        std::size_t bit_count) noexcept
    {
        return bit_count / bits_per_word + (bit_count % bits_per_word != 0 ? 1 : 0);
    }

    [[nodiscard]] static constexpr std::uint64_t bit(
        std::size_t index) noexcept
    {
        return std::uint64_t{1} << (index % bits_per_word);
    }

    [[nodiscard]] static unsigned selected_bit(
        std::uint64_t word,
        Direction direction) noexcept
    {
        assert(word != 0);
        if (direction == Direction::forward) {
            return std::countr_zero(word);
        }
        return 63U - std::countl_zero(word);
    }

    [[nodiscard]] std::size_t descend(
        std::size_t child_index,
        std::size_t level,
        Direction direction) const noexcept
    {
        while (true) {
            const std::uint64_t word = levels_[level][child_index];
            child_index = child_index * bits_per_word +
                          selected_bit(word, direction);
            if (level == 0) {
                assert(child_index < price_count_);
                return child_index;
            }
            --level;
        }
    }

    [[nodiscard]] std::optional<std::size_t> adjacent(
        std::size_t index,
        Direction direction) const noexcept
    {
        std::size_t child_index = index;
        for (std::size_t level = 0; level < levels_.size(); ++level) {
            const std::size_t word_index = child_index / bits_per_word;
            const unsigned offset = static_cast<unsigned>(child_index % bits_per_word);
            std::uint64_t candidates = levels_[level][word_index];
            if (direction == Direction::forward) {
                candidates &= offset == 63U
                    ? std::uint64_t{0}
                    : (~std::uint64_t{0} << (offset + 1U));
            } else {
                candidates &= offset == 0U
                    ? std::uint64_t{0}
                    : ((std::uint64_t{1} << offset) - 1U);
            }

            if (candidates != 0) {
                const std::size_t sibling = word_index * bits_per_word +
                    selected_bit(candidates, direction);
                if (level == 0) {
                    assert(sibling < price_count_);
                    return sibling;
                }
                return descend(sibling, level - 1, direction);
            }
            child_index = word_index;
        }
        return std::nullopt;
    }

    std::size_t price_count_;
    std::size_t occupied_count_{0};
    std::vector<std::vector<std::uint64_t>> levels_;
};

class FlatOccupiedPriceSet {
public:
    explicit FlatOccupiedPriceSet(std::size_t price_count)
        : price_count_(validated_price_count(price_count)),
          words_(word_count(price_count_), std::uint64_t{0})
    {
    }

    [[nodiscard]] std::size_t size() const noexcept { return price_count_; }
    [[nodiscard]] std::size_t count() const noexcept { return occupied_count_; }
    [[nodiscard]] bool empty() const noexcept { return occupied_count_ == 0; }
    [[nodiscard]] std::size_t storage_bytes() const noexcept
    {
        return words_.capacity() * sizeof(std::uint64_t);
    }

    [[nodiscard]] bool contains(std::size_t index) const noexcept
    {
        return index < price_count_ &&
               (words_[index / bits_per_word] & bit(index)) != 0;
    }

    bool set(std::size_t index) noexcept
    {
        assert(index < price_count_);
        std::uint64_t& word = words_[index / bits_per_word];
        const std::uint64_t mask = bit(index);
        if ((word & mask) != 0) {
            return false;
        }
        word |= mask;
        ++occupied_count_;
        return true;
    }

    bool clear(std::size_t index) noexcept
    {
        assert(index < price_count_);
        std::uint64_t& word = words_[index / bits_per_word];
        const std::uint64_t mask = bit(index);
        if ((word & mask) == 0) {
            return false;
        }
        word &= ~mask;
        --occupied_count_;
        return true;
    }

    [[nodiscard]] std::optional<std::size_t> first() const noexcept
    {
        for (std::size_t word = 0; word < words_.size(); ++word) {
            if (words_[word] != 0) {
                return word * bits_per_word + std::countr_zero(words_[word]);
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::size_t> last() const noexcept
    {
        for (std::size_t word = words_.size(); word != 0; --word) {
            const std::uint64_t value = words_[word - 1];
            if (value != 0) {
                return (word - 1) * bits_per_word + 63U - std::countl_zero(value);
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::size_t> next(std::size_t index) const noexcept
    {
        if (index >= price_count_) {
            return std::nullopt;
        }
        std::size_t word = index / bits_per_word;
        const unsigned offset = static_cast<unsigned>(index % bits_per_word);
        std::uint64_t candidates = offset == 63U
            ? std::uint64_t{0}
            : words_[word] & (~std::uint64_t{0} << (offset + 1U));
        if (candidates != 0) {
            return word * bits_per_word + std::countr_zero(candidates);
        }
        while (++word < words_.size()) {
            if (words_[word] != 0) {
                return word * bits_per_word + std::countr_zero(words_[word]);
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::size_t> previous(
        std::size_t index) const noexcept
    {
        if (index >= price_count_) {
            return std::nullopt;
        }
        std::size_t word = index / bits_per_word;
        const unsigned offset = static_cast<unsigned>(index % bits_per_word);
        std::uint64_t candidates = offset == 0U
            ? std::uint64_t{0}
            : words_[word] & ((std::uint64_t{1} << offset) - 1U);
        if (candidates != 0) {
            return word * bits_per_word + 63U - std::countl_zero(candidates);
        }
        while (word != 0) {
            --word;
            if (words_[word] != 0) {
                return word * bits_per_word + 63U - std::countl_zero(words_[word]);
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] bool check_invariants() const noexcept
    {
        std::size_t counted = 0;
        for (const std::uint64_t word : words_) {
            counted += std::popcount(word);
        }
        if (counted != occupied_count_) {
            return false;
        }
        const std::size_t valid = price_count_ % bits_per_word;
        const std::uint64_t padding_mask = valid == 0
            ? std::uint64_t{0}
            : ~((std::uint64_t{1} << valid) - 1U);
        return (words_.back() & padding_mask) == 0;
    }

private:
    static constexpr std::size_t bits_per_word = 64;

    [[nodiscard]] static std::size_t validated_price_count(std::size_t count)
    {
        if (count == 0) {
            throw std::invalid_argument("occupied price set cannot be empty");
        }
        return count;
    }

    [[nodiscard]] static constexpr std::size_t word_count(
        std::size_t bit_count) noexcept
    {
        return bit_count / bits_per_word + (bit_count % bits_per_word != 0 ? 1 : 0);
    }

    [[nodiscard]] static constexpr std::uint64_t bit(std::size_t index) noexcept
    {
        return std::uint64_t{1} << (index % bits_per_word);
    }

    std::size_t price_count_;
    std::size_t occupied_count_{0};
    std::vector<std::uint64_t> words_;
};

} // namespace orderbook::detail
