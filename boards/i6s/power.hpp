// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/i6s/hardware.hpp"
#include "coretx/ui/ui.hpp"
#include "platform/stm32f072/clock.hpp"

#include <algorithm>
#include <array>

namespace coretx::firmware::i6s {

using stm32f072::cpu_hz;
using stm32f072::HardwareError;
using stm32f072::ready_timeout;
using stm32f072::timer_hz;
using stm32f072::wait_for;
namespace registers = stm32f072::registers;

// After a 1.5 s hold the radio shuts down: a falling three-tone signal plays and the screen
// collapses like a CRT (to a line, then to a dot) before the supply latch is released.
class PowerButton {
  public:
    struct Tone {
        std::uint16_t hz{};
        DurationUs duration{};
    };
    inline static constexpr DurationUs hold_duration{1'500'000};
    inline static constexpr DurationUs tone_step{130'000};
    inline static constexpr std::array<std::uint16_t, 3> farewell{1'760, 1'320, 880};
    inline static constexpr DurationUs collapse_vertical{450'000};
    inline static constexpr DurationUs collapse_horizontal{250'000};
    inline static constexpr DurationUs animation_duration{800'000};
    [[nodiscard]] constexpr bool update(const TimestampUs now, const bool pressed) noexcept {
        switch (state_) {
        case State::boot:
            if (!pressed) {
                since_ = now;
                state_ = State::release;
            }
            break;
        case State::release:
            if (pressed)
                state_ = State::boot;
            else if ((now - since_) >= 30'000_us)
                state_ = State::ready;
            break;
        case State::ready:
            if (pressed) {
                since_ = now;
                state_ = State::held;
            }
            break;
        case State::held:
            if (!pressed)
                state_ = State::ready;
            else if ((now - since_) >= hold_duration) {
                state_ = State::requested;
                since_ = now;
                tones_ = 0;
                return true;
            }
            break;
        case State::requested:
            break;
        }
        return false;
    }
    [[nodiscard]] constexpr bool requested() const noexcept { return state_ == State::requested; }
    // Time since the shutdown request; meaningful only while requested().
    [[nodiscard]] constexpr DurationUs elapsed(const TimestampUs now) const noexcept {
        return now - since_;
    }
    [[nodiscard]] constexpr bool animation_done(const TimestampUs now) const noexcept {
        return requested() && (elapsed(now) >= animation_duration);
    }
    // Returns each farewell tone once when it is due.
    [[nodiscard]] constexpr Tone next_tone(const TimestampUs now) noexcept {
        if (!requested() || (tones_ >= farewell.size()) ||
            (elapsed(now) < DurationUs{tones_ * tone_step.count()}))
            return {};
        return {farewell[tones_++], DurationUs{100'000}};
    }
    // Shutdown animation: a full-screen block collapses to a centre line, then to a dot.
    constexpr void render(coretx::ui::Framebuffer &display, const TimestampUs now) const noexcept {
        constexpr auto vertical = collapse_vertical.count();
        constexpr auto horizontal = collapse_horizontal.count();
        const auto t = elapsed(now).count();
        int height = 2, width = 2;
        if (t < vertical) {
            height = std::max(2, static_cast<int>(64U - (62U * t) / vertical));
            width = 128;
        } else if (t < (vertical + horizontal))
            width = std::max(2, static_cast<int>(128U - (126U * (t - vertical)) / horizontal));
        display.rectangle((128 - width) / 2, (64 - height) / 2, width, height);
    }

  private:
    enum class State : std::uint8_t { boot, release, ready, held, requested };
    TimestampUs since_{};
    State state_{};
    std::uint8_t tones_{};
};

template <typename Io, typename Assignment = Hardware>
constexpr void initialize_power_button() noexcept {
    using namespace registers;
    constexpr auto pin = Assignment::power_button;
    Io::write(rcc_ahbenr, Io::read(rcc_ahbenr) | gpio_clock(pin));
    (void)Io::read(rcc_ahbenr);
    configure_input<Io>(pin);
}

template <typename Io, typename Assignment = Hardware>
[[nodiscard]] constexpr bool power_button_pressed() noexcept {
    constexpr auto pin = Assignment::power_button;
    return (Io::read(pin.port + offsetof(GPIO_TypeDef, IDR)) & (std::uint32_t{1} << pin.index)) ==
           0;
}

template <typename Io, typename Assignment = Hardware>
constexpr void set_power_leds(const bool on) noexcept {
    for (const auto pin : Assignment::power_leds)
        drive<Io>(pin, on);
}

template <typename Io, typename Assignment = Hardware>
constexpr void initialize_power_leds() noexcept {
    using namespace registers;
    Io::write(rcc_ahbenr, Io::read(rcc_ahbenr) | gpio_clocks(Assignment::power_leds));
    (void)Io::read(rcc_ahbenr);
    set_power_leds<Io, Assignment>(true);
    for (const auto pin : Assignment::power_leds)
        configure<Io>(pin, false);
}

template <typename Io, typename Assignment = Hardware> constexpr void release_power() noexcept {
    set_power_leds<Io, Assignment>(false);
    drive<Io>(Assignment::hold_power, false);
}

template <typename Io, typename Assignment = Hardware> constexpr void hold_power() noexcept {
    using namespace registers;
    constexpr auto pin = Assignment::hold_power;
    Io::write(rcc_ahbenr, Io::read(rcc_ahbenr) | gpio_clock(pin));
    (void)Io::read(rcc_ahbenr);
    // Load PB15 high before enabling its output driver to avoid a low latch pulse.
    drive<Io>(pin, true);
    constexpr auto otyper = pin.port + offsetof(GPIO_TypeDef, OTYPER);
    constexpr auto moder = pin.port + offsetof(GPIO_TypeDef, MODER);
    Io::write(otyper, Io::read(otyper) & ~(std::uint32_t{GPIO_OTYPER_OT_0} << pin.index));
    Io::write(moder, (Io::read(moder) & ~(std::uint32_t{GPIO_MODER_MODER0} << (2U * pin.index))) |
                         (std::uint32_t{GPIO_MODER_MODER0_0} << (2U * pin.index)));
}

template <typename Io>
[[nodiscard]] constexpr HardwareError
initialize(const std::uint32_t attempts = ready_timeout) noexcept {
    hold_power<Io>();
    return coretx::firmware::Clock<Io>::initialize(attempts);
}

} // namespace coretx::firmware::i6s
