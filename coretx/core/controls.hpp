// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "coretx/core/core.hpp"

namespace coretx {

// Preserve the eight-axis legacy calibration journal; auxiliary setup is separate.
inline constexpr std::size_t calibration_count = 8;

struct AnalogCalibration {
    std::uint16_t minimum{};
    std::uint16_t center{};
    std::uint16_t maximum{};
    friend constexpr bool operator==(const AnalogCalibration &,
                                     const AnalogCalibration &) = default;
};

struct AnalogMapping {
    std::uint8_t physical_index{};
    bool reversed{};
};

struct ControlSetup {
    std::array<AnalogCalibration, calibration_count> calibration{};
    // Indexed by logical analog; calibration is indexed by physical analog.
    std::array<AnalogMapping, calibration_count> mapping{};

    constexpr ControlSetup() noexcept;
};

struct RawControls {
    std::array<std::uint16_t, calibration_count> analogs{};
    // Switches and trims already use logical order, like Controls.
    std::array<SwitchPosition, switch_count> switches{};
    std::array<TrimSteps, trim_count> trims{};
};

enum class PreparationError : std::uint8_t { none, calibration, mapping, switch_position };

struct PreparationStatus {
    PreparationError code{PreparationError::none};
    std::size_t index{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return code == PreparationError::none;
    }
};

struct PreparedControls {
    PreparationStatus status{};
    Controls controls{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return static_cast<bool>(status);
    }
};

[[nodiscard]] constexpr PreparationStatus validate(const ControlSetup &setup) noexcept;
// Do not evaluate or publish controls unless preparation reports success.
[[nodiscard]] constexpr PreparedControls prepare_controls(const ControlSetup &setup,
                                                          const RawControls &raw) noexcept;
[[nodiscard]] constexpr const char *
preparation_error_message(const PreparationError &code) noexcept;

} // namespace coretx

#include "coretx/core/detail/controls_impl.hpp"
