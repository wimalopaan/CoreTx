// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/i6s/hardware.hpp"
#include "platform/stm32f072/clock.hpp"

namespace coretx::firmware::i6s {

using stm32f072::cpu_hz;
using stm32f072::HardwareError;
using stm32f072::ready_timeout;
using stm32f072::timer_hz;
using stm32f072::wait_for;
namespace registers = stm32f072::registers;

class PowerButton {
  public:
    inline static constexpr DurationUs hold_duration{1'500'000};
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
                return true;
            }
            break;
        case State::requested:
            break;
        }
        return false;
    }
    [[nodiscard]] constexpr bool requested() const noexcept { return state_ == State::requested; }

  private:
    enum class State : std::uint8_t { boot, release, ready, held, requested };
    TimestampUs since_{};
    State state_{};
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
