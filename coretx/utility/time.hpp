// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <cstdlib>
#include <limits>

namespace coretx {

class DurationUs {
  public:
    constexpr DurationUs() noexcept = default;
    constexpr explicit DurationUs(const std::uint32_t value) noexcept : value_(value) {}
    [[nodiscard]] constexpr std::uint32_t count() const noexcept { return value_; }
    constexpr bool operator==(const DurationUs &) const noexcept = default;
    // Direct comparisons avoid an out-of-line three-way comparison on Cortex-M0 (-Os).
    [[nodiscard]] constexpr bool operator<(const DurationUs other) const noexcept {
        return value_ < other.value_;
    }
    [[nodiscard]] constexpr bool operator<=(const DurationUs other) const noexcept {
        return value_ <= other.value_;
    }
    [[nodiscard]] constexpr bool operator>(const DurationUs other) const noexcept {
        return value_ > other.value_;
    }
    [[nodiscard]] constexpr bool operator>=(const DurationUs other) const noexcept {
        return value_ >= other.value_;
    }

    [[nodiscard]] friend constexpr DurationUs operator+(const DurationUs a,
                                                        const DurationUs b) noexcept {
        return DurationUs{a.value_ + b.value_};
    }
    [[nodiscard]] friend constexpr DurationUs operator%(const DurationUs a,
                                                        const DurationUs b) noexcept {
        return DurationUs{a.value_ % b.value_};
    }
    [[nodiscard]] friend constexpr DurationUs operator*(const std::uint32_t scale,
                                                        const DurationUs duration) noexcept {
        return DurationUs{scale * duration.value_};
    }

  private:
    std::uint32_t value_{};
};

class TimestampUs {
  public:
    constexpr TimestampUs() noexcept = default;
    constexpr explicit TimestampUs(const std::uint32_t value) noexcept : value_(value) {}
    [[nodiscard]] constexpr std::uint32_t count() const noexcept { return value_; }
    constexpr bool operator==(const TimestampUs &) const noexcept = default;

    // Ordering absolute timestamps is ambiguous across the counter wrap.
    [[nodiscard]] friend constexpr DurationUs operator-(const TimestampUs now,
                                                        const TimestampUs before) noexcept {
        return DurationUs{now.value_ - before.value_};
    }
    [[nodiscard]] friend constexpr TimestampUs operator+(const TimestampUs timestamp,
                                                         const DurationUs duration) noexcept {
        return TimestampUs{timestamp.value_ + duration.count()};
    }
    [[nodiscard]] friend constexpr TimestampUs operator-(const TimestampUs timestamp,
                                                         const DurationUs duration) noexcept {
        return TimestampUs{timestamp.value_ - duration.count()};
    }

  private:
    std::uint32_t value_{};
};

namespace literals {
namespace detail {
[[nodiscard]] consteval DurationUs duration(const unsigned long long value,
                                            const unsigned long long scale) noexcept {
    if (value > (std::numeric_limits<std::uint32_t>::max() / scale)) {
        std::abort();
    }
    return DurationUs{static_cast<std::uint32_t>(value * scale)};
}
} // namespace detail

[[nodiscard]] consteval DurationUs operator""_us(const unsigned long long value) noexcept {
    return detail::duration(value, 1);
}
[[nodiscard]] consteval DurationUs operator""_ms(const unsigned long long value) noexcept {
    return detail::duration(value, 1'000);
}
} // namespace literals

} // namespace coretx
