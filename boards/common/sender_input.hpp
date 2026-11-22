// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/common/multiswitch_ui.hpp"
#include "boards/i6s/touch_errors.hpp"
#include "boards/i6s/touch_navigation.hpp"

namespace coretx::firmware {

template <typename Keys> class PhysicalInput {
  public:
    inline static constexpr bool needs_display = false;
    inline static constexpr bool touch = false;
    constexpr void initialize() noexcept { Keys::initialize(); }
    [[nodiscard]] constexpr Key service(const TimestampUs now, const bool,
                                        const MultiSwitchInput = {}) noexcept {
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
    [[nodiscard]] constexpr const char *message() const noexcept { return nullptr; }
};

template <typename Touch> class TouchInput {
  public:
    inline static constexpr bool needs_display = true;
    inline static constexpr bool touch = true;
    constexpr void initialize() noexcept {
        navigation_ = {};
#if CORETX_MULTISWITCH
        multiswitch_ = {};
        multiswitch_active_ = false;
#endif
        Touch::initialize();
    }
    [[nodiscard]] constexpr Key service(const TimestampUs now, const bool enabled,
                                        const MultiSwitchInput multiswitch = {}) noexcept {
        const auto event = Touch::service(now);
        const bool ready = enabled && Touch::ready() && (Touch::error() == i6s::TouchError::none);
        const auto *const sample =
            (event == i6s::TouchEvent::sample) || (ready != ready_) ? &Touch::sample() : nullptr;
        ready_ = ready;
#if CORETX_MULTISWITCH
        if (multiswitch.active != multiswitch_active_) {
            multiswitch_active_ = multiswitch.active;
            multiswitch_.reset();
            navigation_ = {};
        }
        if (multiswitch.active)
            return multiswitch_.service(now, sample, ready, multiswitch.momentary);
#else
        (void)multiswitch;
#endif
        return navigation_.service(now, sample, ready);
    }
    [[nodiscard]] constexpr bool ok_held() const noexcept { return false; }
    [[nodiscard]] constexpr std::optional<std::size_t> held_switch() const noexcept {
#if CORETX_MULTISWITCH
        return multiswitch_active_ ? multiswitch_.held() : std::nullopt;
#else
        return std::nullopt;
#endif
    }
    [[nodiscard]] constexpr std::optional<std::size_t> tapped_switch() const noexcept {
#if CORETX_MULTISWITCH
        return multiswitch_active_ ? multiswitch_.tapped() : std::nullopt;
#else
        return std::nullopt;
#endif
    }
    [[nodiscard]] constexpr const char *message() const noexcept {
        return i6s::error_message(Touch::error());
    }

  private:
    i6s::TouchNavigation navigation_{};
#if CORETX_MULTISWITCH
    MultiSwitchTouch multiswitch_{};
    bool multiswitch_active_{};
#endif
    bool ready_{};
};

} // namespace coretx::firmware
