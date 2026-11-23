// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/common/hardware.hpp"
#include "platform/stm32f072/radio_gpio.hpp"

namespace coretx::firmware {

inline constexpr auto i6x_radio_pins = i6x::Hardware::radio;
inline constexpr auto i6s_radio_pins = i6s::Hardware::radio;
inline constexpr auto external_module_power = Hardware::external_module_power;
inline constexpr auto external_module_active_high = Hardware::external_module_active_high;
inline constexpr auto external_module_gpio_clock = gpio_clock(external_module_power);

} // namespace coretx::firmware
