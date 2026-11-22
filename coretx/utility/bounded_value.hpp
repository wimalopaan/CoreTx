// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <compare>
#include <concepts>
#include <cstdint>
#include <limits>

namespace coretx {

template <typename Tag, std::signed_integral Rep, std::int32_t Minimum, std::int32_t Maximum>
class BoundedValue {
    static_assert(Minimum <= Maximum);
    static_assert(Minimum >= std::numeric_limits<Rep>::min());
    static_assert(Maximum <= std::numeric_limits<Rep>::max());

  public:
    static constexpr std::int32_t minimum = Minimum;
    static constexpr std::int32_t maximum = Maximum;

    constexpr BoundedValue() noexcept
        requires((Minimum <= 0) && (Maximum >= 0))
    = default;

    template <std::int32_t Value>
        requires((Value >= Minimum) && (Value <= Maximum))
    [[nodiscard]] static constexpr BoundedValue constant() noexcept {
        return BoundedValue{static_cast<Rep>(Value)};
    }

    [[nodiscard]] constexpr bool try_set(const std::int32_t value) noexcept {
        if ((value < Minimum) || (value > Maximum)) {
            return false;
        }
        value_ = static_cast<Rep>(value);
        return true;
    }

    [[nodiscard]] constexpr Rep value() const noexcept { return value_; }

    friend constexpr auto operator<=>(const BoundedValue left, const BoundedValue right) noexcept
        requires(sizeof(Rep) <= sizeof(std::uint32_t))
    {
        return left.value_ <=> right.value_;
    }

    friend constexpr bool operator==(const BoundedValue left, const BoundedValue right) noexcept
        requires(sizeof(Rep) <= sizeof(std::uint32_t))
    {
        return left.value_ == right.value_;
    }

    friend constexpr auto operator<=>(const BoundedValue &, const BoundedValue &) noexcept
        requires(sizeof(Rep) > sizeof(std::uint32_t))
    = default;

  private:
    constexpr explicit BoundedValue(const Rep value) noexcept : value_{value} {}

    Rep value_{};
};

} // namespace coretx
