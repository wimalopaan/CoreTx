// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/i6s/power.hpp"
#include "coretx/ui/input.hpp"

namespace coretx::firmware::i6s {

class SetupNavigation {
  public:
    constexpr void set_mode(const TimestampUs now, const bool active) noexcept {
        if (active == active_)
            return;
        active_ = active;
        blocked_ = true;
        jog_armed_ = false;
        long_sent_ = false;
        led_on_ = true;
        blink_at_ = now;
    }
    [[nodiscard]] constexpr Key service(const TimestampUs now, const std::uint8_t buttons,
                                        const bool enabled, const std::int16_t vra,
                                        const std::int16_t vrb, const bool jog_enabled) noexcept {
        if (buttons != candidate_) {
            candidate_ = buttons;
            changed_at_ = now;
        }
        const auto previous = held_;
        if ((now - changed_at_) >= 20'000_us)
            held_ = candidate_;
        if (!active_ || !enabled) {
            blocked_ = true;
            jog_armed_ = false;
            return Key::none;
        }
        if ((now - blink_at_) >= 250'000_us) {
            blink_at_ = now;
            led_on_ = !led_on_;
        }
        Key key{};
        if (blocked_ || (held_ == 3)) {
            blocked_ = (held_ != 0) || (candidate_ != 0);
        } else if (held_ != previous) {
            if (held_ != 0) {
                pressed_at_ = now;
                long_sent_ = false;
                blocked_ = previous != 0;
            } else if (!long_sent_)
                key = previous == 1 ? Key::ok : Key::cancel;
        } else if ((held_ != 0) && (candidate_ == held_) && !long_sent_ &&
                   ((now - pressed_at_) >= 800'000_us)) {
            long_sent_ = true;
            key = held_ == 1 ? Key::ok_long : Key::cancel_long;
        }
        if (!jog_enabled) {
            jog_armed_ = false;
            return key;
        }
        const int direction = vra > 300 ? 1 : vra < -300 ? -1 : vrb > 300 ? 1 : vrb < -300 ? -1 : 0;
        if (direction == 0)
            jog_armed_ = true;
        else {
            if (jog_armed_ && (key == Key::none))
                key = direction > 0 ? Key::down : Key::up;
            jog_armed_ = false;
        }
        return key;
    }
    [[nodiscard]] constexpr std::uint8_t buttons() const noexcept { return held_; }
    [[nodiscard]] constexpr bool led_on() const noexcept { return led_on_; }

  private:
    TimestampUs changed_at_{}, pressed_at_{}, blink_at_{};
    std::uint8_t candidate_{}, held_{};
    bool active_{}, blocked_{true}, long_sent_{}, jog_armed_{}, led_on_{true};
};

template <typename Io, typename Assignment = Hardware> class SetupControls {
  public:
    constexpr void initialize() noexcept {
        navigation_ = {};
        using namespace stm32f072::registers;
        Io::write(rcc_ahbenr, Io::read(rcc_ahbenr) | gpio_clocks(Assignment::rear_buttons));
        (void)Io::read(rcc_ahbenr);
        for (const auto pin : Assignment::rear_buttons)
            configure_input<Io>(pin);
        initialize_power_leds<Io, Assignment>();
    }
    constexpr void set_mode(const TimestampUs now, const bool active) noexcept {
        const auto was_on = navigation_.led_on();
        navigation_.set_mode(now, active);
        if (was_on != navigation_.led_on())
            set_power_leds<Io, Assignment>(navigation_.led_on());
    }
    [[nodiscard]] static constexpr std::uint8_t sample_buttons() noexcept {
        std::uint8_t pressed{};
        for (std::size_t i = 0; i < Assignment::rear_buttons.size(); ++i) {
            const auto pin = Assignment::rear_buttons[i];
            if ((Io::read(pin.port + offsetof(GPIO_TypeDef, IDR)) & (1U << pin.index)) == 0)
                pressed |= static_cast<std::uint8_t>(1U << i);
        }
        return pressed;
    }
    [[nodiscard]] constexpr Key service(const TimestampUs now, const bool enabled,
                                        const std::int16_t vra, const std::int16_t vrb,
                                        const bool jog_enabled) noexcept {
        const auto was_on = navigation_.led_on();
        const auto key = navigation_.service(now, sample_buttons(), enabled, vra, vrb, jog_enabled);
        if (was_on != navigation_.led_on())
            set_power_leds<Io, Assignment>(navigation_.led_on());
        return key;
    }
    [[nodiscard]] constexpr std::uint8_t buttons() const noexcept { return navigation_.buttons(); }

  private:
    SetupNavigation navigation_{};
};

} // namespace coretx::firmware::i6s
