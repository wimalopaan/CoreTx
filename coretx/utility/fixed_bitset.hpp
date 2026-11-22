// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>

namespace coretx {

template <std::size_t Count> class FixedBitset {
    static_assert((Count > 0) && (Count <= 32), "FixedBitset requires 1 to 32 bits");

  public:
    [[nodiscard]] static constexpr std::size_t size() noexcept { return Count; }

    [[nodiscard]] constexpr bool test(const std::size_t index) const noexcept {
        return (bits_ & mask(index)) != 0;
    }

    [[nodiscard]] constexpr bool operator[](const std::size_t index) const noexcept {
        return test(index);
    }

    constexpr void set(const std::size_t index) noexcept { bits_ |= mask(index); }
    constexpr void reset(const std::size_t index) noexcept { bits_ &= ~mask(index); }
    constexpr void clear() noexcept { bits_ = 0; }

  private:
    // Indexed operations require index < Count, as with FixedVector.
    [[nodiscard]] static constexpr std::uint32_t mask(const std::size_t index) noexcept {
#if defined(__STDC_HOSTED__) && !__STDC_HOSTED__ && (defined(__GNUC__) || defined(__clang__))
        if (index >= Count) {
            __builtin_trap();
        }
#else
        assert(index < Count);
#endif
        return std::uint32_t{1} << index;
    }

    std::uint32_t bits_{};
};

} // namespace coretx
