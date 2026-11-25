// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/common/sender_input.hpp"
#include "boards/i6s/multiswitch_touch.hpp"
#include "boards/i6s/touch_errors.hpp"
#include "boards/i6s/touch_navigation.hpp"
#ifdef CORETX_I6S_TOUCH_TRACE
#include "platform/stm32f072/i2c.hpp"
#endif

namespace coretx::firmware {

namespace sender_input_detail {
template <typename Touch, bool MultiSwitch = features::multiswitch> struct TouchState {
    i6s::TouchNavigation navigation{};
    bool ready{};
};

template <typename Touch> struct TouchState<Touch, true> {
    i6s::TouchNavigation navigation{};
    MultiSwitchTouch multiswitch{};
    bool multiswitch_active{};
    bool ready{};
};
} // namespace sender_input_detail

struct NoSetupControls {
    constexpr void initialize() noexcept {}
    [[nodiscard]] constexpr Key service(TimestampUs, bool, std::int16_t, std::int16_t,
                                        bool) noexcept {
        return Key::none;
    }
};

template <typename Touch, typename SetupControls = NoSetupControls> class TouchInput {
  public:
    inline static constexpr bool needs_display = true;
    inline static constexpr bool touch = true;
    constexpr void initialize() noexcept {
        state_.navigation = {};
        if constexpr (features::multiswitch) {
            state_.multiswitch = {};
            state_.multiswitch_active = false;
        }
        controls_.initialize();
        Touch::initialize();
    }
    [[nodiscard]] constexpr Key service(const TimestampUs now, const bool enabled,
                                        const SenderInputContext context = {}) noexcept {
        const auto physical_key =
            controls_.service(now, enabled, context.vra, context.vrb, context.jog_enabled);
        const auto event = Touch::service(now);
        const bool ready = enabled && Touch::ready() && (Touch::error() == i6s::TouchError::none);
        const auto *const sample = (event == i6s::TouchEvent::sample) || (ready != state_.ready)
                                       ? &Touch::sample()
                                       : nullptr;
#ifdef CORETX_I6S_TOUCH_TRACE
        if (ready && state_.ready)
            trace_loop_gap_ = std::max(trace_loop_gap_, (now - trace_loop_at_).count());
        trace_loop_at_ = now;
        if (event == i6s::TouchEvent::sample) {
            i6s::TouchSample report{};
            if (i6s::decode_touch(Touch::packet(), report) &&
                (report.contacts == Touch::sample().contacts)) {
                if (trace_contact_ && (report.contacts != 0))
                    trace_gap_ = std::max(trace_gap_, (now - trace_packet_).count());
                trace_packet_ = now;
                trace_contact_ = report.contacts != 0;
            }
        }
#endif
        state_.ready = ready;
        if constexpr (features::multiswitch) {
            if (context.active != state_.multiswitch_active) {
                state_.multiswitch_active = context.active;
                state_.multiswitch.reset();
                state_.navigation.reset();
            }
            if (context.active) {
                const auto key = state_.multiswitch.service(now, sample, ready, context.momentary);
                return physical_key != Key::none ? physical_key : key;
            }
        }
        const auto key = state_.navigation.service(now, sample, ready, context.taps);
        return physical_key != Key::none ? physical_key : key;
    }
    constexpr void set_setup_mode(const TimestampUs now, const bool active) noexcept
        requires requires(SetupControls &controls) { controls.set_mode(now, active); }
    {
        controls_.set_mode(now, active);
    }
    [[nodiscard]] constexpr std::uint8_t rear_buttons() const noexcept
        requires requires(const SetupControls &controls) { controls.buttons(); }
    {
        return controls_.buttons();
    }
    [[nodiscard]] constexpr bool ok_held() const noexcept { return false; }
    [[nodiscard]] constexpr std::optional<std::size_t> held_switch() const noexcept {
        if constexpr (features::multiswitch)
            return state_.multiswitch_active ? state_.multiswitch.held() : std::nullopt;
        return std::nullopt;
    }
    [[nodiscard]] constexpr std::optional<std::size_t> pressed_switch() const noexcept {
        if constexpr (features::multiswitch)
            return state_.multiswitch_active ? state_.multiswitch.pressed() : std::nullopt;
        return std::nullopt;
    }
    [[nodiscard]] constexpr std::optional<std::size_t> tapped_switch() const noexcept {
        if constexpr (features::multiswitch)
            return state_.multiswitch_active ? state_.multiswitch.tapped() : std::nullopt;
        return std::nullopt;
    }
    [[nodiscard]] constexpr const char *message() const noexcept {
        return i6s::error_message(Touch::error());
    }
#ifdef CORETX_I6S_TOUCH_TRACE
    // Touch controller state of the coretx-i6s-touch-trace build.
    constexpr void render_trace(coretx::ui::Framebuffer &display, const Key key) const noexcept {
        display.rectangle(0, 48, 128, 16, false);
        display.compact();
        display.text(0, 48, "K");
        display.unsigned_number(15, 48, static_cast<unsigned>(key));
        display.text(24, 48, "C");
        display.unsigned_number(39, 48, Touch::sample().contacts);
        display.text(48, 48, "G");
        display.unsigned_number(79, 48, trace_gap_ / 1000U);
        i6s::TouchSample report{};
        const bool valid_report = i6s::decode_touch(Touch::packet(), report);
        display.text(88, 48, "R");
        display.unsigned_number(127, 48, valid_report ? report.contacts : 9U);
        display.text(0, 56, "L");
        display.unsigned_number(63, 56, trace_loop_gap_ / 1000U);
        display.compact(false);
    }
    // Adds calibration phase, menu visibility and the last failed EEPROM transfer.
    template <typename Memory>
    constexpr void render_trace(coretx::ui::Framebuffer &display, const Key key,
                                const unsigned phase, const unsigned menu) const noexcept {
        render_trace(display, key);
        display.compact();
        display.text(72, 56, "P");
        display.unsigned_number(95, 56, phase);
        display.text(104, 56, "M");
        display.unsigned_number(127, 56, menu);
        display.compact(false);
        if (Memory::error() == I2cError::none)
            return;
        const auto storage = Memory::trace();
        display.rectangle(0, 48, 128, 16, false);
        display.compact();
        display.text(0, 48, "EE S");
        display.unsigned_number(27, 48, storage.stage);
        display.text(36, 48, "A");
        display.unsigned_number(79, 48, storage.address);
        display.text(88, 48, "P");
        display.unsigned_number(127, 48, storage.position);
        display.text(0, 56, "E");
        display.unsigned_number(31, 56, static_cast<unsigned>(Memory::error()));
        display.text(40, 56, "ISR");
        display.unsigned_number(127, 56, storage.status);
        display.compact(false);
    }
#endif

  private:
    sender_input_detail::TouchState<Touch> state_{};
    [[no_unique_address]] SetupControls controls_{};
#ifdef CORETX_I6S_TOUCH_TRACE
    TimestampUs trace_packet_{};
    TimestampUs trace_loop_at_{};
    std::uint32_t trace_gap_{};
    std::uint32_t trace_loop_gap_{};
    bool trace_contact_{};
#endif
};

} // namespace coretx::firmware
