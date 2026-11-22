// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>

namespace coretx {

constexpr ControlSetup::ControlSetup() noexcept {
    for (std::size_t i = 0; i < calibration_count; ++i) {
        mapping[i].physical_index = static_cast<std::uint8_t>(i);
    }
}

constexpr PreparationStatus validate(const ControlSetup &setup) noexcept {
    for (std::size_t i = 0; i < calibration_count; ++i) {
        const auto &calibration = setup.calibration[i];
        if ((calibration.minimum >= calibration.center) ||
            (calibration.center >= calibration.maximum)) {
            return {PreparationError::calibration, i};
        }
    }
    for (std::size_t i = 0; i < calibration_count; ++i) {
        if (setup.mapping[i].physical_index >= calibration_count) {
            return {PreparationError::mapping, i};
        }
    }
    return {};
}

constexpr PreparedControls prepare_controls(const ControlSetup &setup,
                                            const RawControls &raw) noexcept {
    if (const auto status = validate(setup); !status) {
        return {status, {}};
    }
    for (std::size_t i = 0; i < switch_count; ++i) {
        if (!valid_switch_position(raw.switches[i])) {
            return {{PreparationError::switch_position, i}, {}};
        }
    }

    PreparedControls result{};
    result.controls.active_analogs = static_cast<std::uint16_t>((1U << calibration_count) - 1U);
    result.controls.switches = raw.switches;
    result.controls.trims = raw.trims;
    for (std::size_t i = 0; i < calibration_count; ++i) {
        const auto &mapping = setup.mapping[i];
        const auto &calibration = setup.calibration[mapping.physical_index];
        const auto sample = std::clamp<std::int32_t>(raw.analogs[mapping.physical_index],
                                                     calibration.minimum, calibration.maximum);
        const std::int32_t center = calibration.center;
        const auto distance = sample < center ? center - sample : sample - center;
        const auto span =
            sample < center ? center - calibration.minimum : calibration.maximum - center;
        // Round magnitudes to nearest, ties away from zero, on both sides.
        const auto magnitude = (distance * resolution + span / 2) / span;
        const auto value = sample < center ? -magnitude : magnitude;
        result.controls.analogs[i] = static_cast<std::int16_t>(mapping.reversed ? -value : value);
    }
    return result;
}

constexpr const char *preparation_error_message(const PreparationError &code) noexcept {
    switch (code) {
    case PreparationError::none:
        return "no error";
    case PreparationError::calibration:
        return "expected calibration minimum < center < maximum";
    case PreparationError::mapping:
        return "physical analog index out of range";
    case PreparationError::switch_position:
        return "switch position out of range";
    }
    return "unknown preparation error";
}

} // namespace coretx
