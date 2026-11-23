// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/i6s/hardware.hpp"
#include "boards/i6x/hardware.hpp"
#include "coretx/core/input_setup.hpp"

#include <type_traits>

namespace coretx::firmware {

using Hardware = std::conditional_t<features::i6s, i6s::Hardware, i6x::Hardware>;

inline constexpr auto gimbal_inputs = Hardware::gimbal_inputs;
inline constexpr auto stock_inputs = Hardware::stock_inputs;
inline constexpr std::size_t battery_index = stock_inputs.size();
inline constexpr auto sender_inputs = Hardware::sender_inputs;
inline constexpr std::size_t extra_adc_count = sender_inputs.size() - stock_inputs.size() - 1;

// Board differences used by the shared firmware; derived from the selected Hardware.
namespace board {
inline constexpr std::size_t digital_inputs = Hardware::digital_inputs.size();
static_assert(digital_inputs <= digital_input_count);
inline constexpr bool trim_buttons = Hardware::trim_buttons;
inline constexpr std::uint8_t external_module_digital_mask = Hardware::external_module_digital_mask;
// Digital inputs on the SWD pins; enabling them disconnects a debugger.
inline constexpr std::uint8_t swd_digital_mask = [] {
    std::uint8_t mask{};
    for (std::size_t i = 0; i < digital_inputs; ++i)
        if (Hardware::digital_inputs[i].released != DigitalInput::Released::analog)
            mask |= static_cast<std::uint8_t>(1U << i);
    return mask;
}();
[[nodiscard]] constexpr bool external_module_input(const std::size_t index) noexcept {
    return ((external_module_digital_mask >> index) & 1U) != 0;
}
[[nodiscard]] constexpr bool swd_input(const std::size_t index) noexcept {
    return ((swd_digital_mask >> index) & 1U) != 0;
}
} // namespace board

} // namespace coretx::firmware
