// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace coretx {

class MixValue {
  public:
    static constexpr unsigned fractional_bits = 8;
    static constexpr std::int32_t scale = 1 << fractional_bits;

    constexpr MixValue() noexcept = default;

    [[nodiscard]] static constexpr MixValue from_raw(const std::int32_t raw) noexcept {
        return MixValue{raw};
    }

    [[nodiscard]] static constexpr MixValue from_channel_units(const std::int16_t value) noexcept {
        return from_raw(static_cast<std::int32_t>(value) * scale);
    }

    [[nodiscard]] constexpr std::int32_t raw() const noexcept { return raw_; }

    friend constexpr bool operator==(const MixValue left, const MixValue right) noexcept {
        return left.raw_ == right.raw_;
    }

  private:
    constexpr explicit MixValue(const std::int32_t raw) noexcept : raw_{raw} {}

    std::int32_t raw_{};
};

static_assert(sizeof(MixValue) == sizeof(std::int32_t));

} // namespace coretx
