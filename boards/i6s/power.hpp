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
