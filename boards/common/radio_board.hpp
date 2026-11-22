// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/common/hardware.hpp"

namespace coretx::firmware {

inline constexpr auto i6x_radio_pins = i6x::Hardware::radio;
inline constexpr auto i6s_radio_pins = i6s::Hardware::radio;
inline constexpr auto external_module_power = Hardware::external_module_power;
inline constexpr auto external_module_active_high = Hardware::external_module_active_high;
inline constexpr auto external_module_gpio_clock = gpio_clock(external_module_power);

template <typename Io, typename Assignment = Hardware>
constexpr void external_module_off() noexcept {
    namespace reg = stm32f072::registers;
    Io::write(reg::rcc_ahbenr,
              Io::read(reg::rcc_ahbenr) | gpio_clock(Assignment::external_module_power));
    (void)Io::read(reg::rcc_ahbenr);
    drive<Io>(Assignment::external_module_power, !Assignment::external_module_active_high);
    configure<Io>(Assignment::external_module_power, false);
}

} // namespace coretx::firmware
