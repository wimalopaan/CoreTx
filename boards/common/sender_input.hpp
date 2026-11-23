// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/common/multiswitch_ui.hpp"
#include "coretx/ui/input.hpp"

namespace coretx::firmware {

template <typename Keys> class PhysicalInput {
  public:
    inline static constexpr bool needs_display = false;
    inline static constexpr bool touch = false;
    constexpr void initialize() noexcept { Keys::initialize(); }
    [[nodiscard]] constexpr Key service(const TimestampUs now, const bool,
                                        const SenderInputContext = {}) noexcept {
        return Keys::service(now);
    }
    [[nodiscard]] constexpr bool ok_held() const noexcept {
        if constexpr (requires { Keys::ok_held(); })
            return Keys::ok_held();
        else
            return false;
    }
    [[nodiscard]] constexpr std::uint8_t trim_buttons() const noexcept {
        if constexpr (requires { Keys::trim_buttons(); })
            return Keys::trim_buttons();
        else
            return 0;
    }
    [[nodiscard]] constexpr std::optional<std::size_t> held_switch() const noexcept {
        return std::nullopt;
    }
    [[nodiscard]] constexpr std::optional<std::size_t> tapped_switch() const noexcept {
        return std::nullopt;
    }
    [[nodiscard]] constexpr std::optional<std::size_t> pressed_switch() const noexcept {
        return std::nullopt;
    }
    [[nodiscard]] constexpr const char *message() const noexcept { return nullptr; }
};

} // namespace coretx::firmware
