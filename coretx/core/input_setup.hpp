// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "coretx/core/controls.hpp"
#include <algorithm>
#include <string_view>

namespace coretx {

enum class InputMode : std::uint8_t {
    off,
    pot,
    pot_detent,
    two_position,
    three_position,
    six_position
};
enum class DigitalInputMode : std::uint8_t { off, two_position, three_position };
inline constexpr std::size_t digital_input_count = 5;
inline constexpr std::array<std::string_view, 3> digital_mode_names{"Off", "2pos", "3pos"};
inline constexpr std::size_t auxiliary_count = 11;
inline constexpr std::array<std::string_view, 6> input_mode_names{"Off",  "Pot",  "Pot-D",
                                                                  "2pos", "3pos", "6pos"};
inline constexpr std::array<std::string_view, auxiliary_count> auxiliary_names{
    "SA", "SB", "SC", "SD", "VRA", "VRB", "VRC", "VRD", "VRE", "VRF", "VRG"};
inline constexpr std::array<std::size_t, auxiliary_count> auxiliary_analog_indices{
    6, 7, 8, 9, 4, 5, 10, 11, 12, 13, 14};

struct InputSetup {
    bool extra_analog{};
    bool external_module{true};
    std::array<InputMode, auxiliary_count> modes{
        InputMode::two_position, InputMode::two_position, InputMode::three_position,
        InputMode::two_position, InputMode::pot,          InputMode::pot};
    bool trim_beep{true};
    // Two bits per GPIO keep the sender-wide setup copies small on the F072.
    std::uint16_t digital_modes{};
    std::array<AnalogCalibration, auxiliary_count> calibration = [] {
        std::array<AnalogCalibration, auxiliary_count> values{};
        values.fill({0, 2048, 4095});
        return values;
    }();
    constexpr explicit InputSetup(const bool i6s = false) noexcept {
        if (i6s)
            modes[1] = InputMode::three_position;
    }
    [[nodiscard]] constexpr DigitalInputMode digital_mode(const std::size_t index) const noexcept {
        return static_cast<DigitalInputMode>((digital_modes >> (2U * index)) & 3U);
    }
    constexpr void set_digital_mode(const std::size_t index, const DigitalInputMode mode) noexcept {
        const auto shift = 2U * index;
        digital_modes = static_cast<std::uint16_t>((digital_modes & ~(3U << shift)) |
                                                   (static_cast<unsigned>(mode) << shift));
    }
    friend constexpr bool operator==(const InputSetup &, const InputSetup &) = default;
};

[[nodiscard]] constexpr bool is_pot(const InputMode mode) noexcept {
    return (mode == InputMode::pot) || (mode == InputMode::pot_detent);
}
[[nodiscard]] constexpr bool valid_input_setup(const InputSetup &setup) noexcept {
    if ((setup.digital_modes >> (2U * digital_input_count)) != 0)
        return false;
    for (std::size_t i = 0; i < digital_input_count; ++i) {
        const auto mode = setup.digital_mode(i);
        if ((mode > DigitalInputMode::three_position) ||
            ((mode == DigitalInputMode::three_position) &&
             (((i != 0) && (i != 2)) || (setup.digital_mode(i + 1) != DigitalInputMode::off))))
            return false;
    }
    for (std::size_t i = 0; i < auxiliary_count; ++i) {
        const auto &axis = setup.calibration[i];
        if ((setup.modes[i] > InputMode::six_position) || (axis.minimum >= axis.center) ||
            (axis.center >= axis.maximum) || (axis.maximum > 4095))
            return false;
    }
    return true;
}
[[nodiscard]] constexpr std::int16_t
decode_pot(const std::uint16_t raw, const AnalogCalibration axis, const bool detent) noexcept {
    const std::int32_t center =
        detent ? axis.center
               : static_cast<std::int32_t>((std::uint32_t{axis.minimum} + axis.maximum) / 2U);
    const auto sample = std::clamp<std::int32_t>(raw, axis.minimum, axis.maximum);
    const auto distance = sample < center ? center - sample : sample - center;
    const auto span = sample < center ? center - axis.minimum : axis.maximum - center;
    const auto magnitude = (distance * resolution + span / 2) / span;
    return static_cast<std::int16_t>(sample < center ? -magnitude : magnitude);
}
[[nodiscard]] constexpr SwitchPosition decode_input_switch(const std::uint16_t raw,
                                                           const InputMode mode,
                                                           const AnalogCalibration axis) noexcept {
    if (mode == InputMode::two_position)
        return raw < axis.center ? SwitchPosition::up : SwitchPosition::down;
    const auto sample = std::clamp<std::int32_t>(raw, axis.minimum, axis.maximum) - axis.minimum;
    const auto span = static_cast<std::int32_t>(axis.maximum) - axis.minimum + 1;
    if (mode == InputMode::six_position)
        return static_cast<SwitchPosition>(2 + std::min<std::int32_t>(5, sample * 6 / span));
    return (sample * 3) < span         ? SwitchPosition::up
           : (sample * 3) < (span * 2) ? SwitchPosition::middle
                                       : SwitchPosition::down;
}

} // namespace coretx
