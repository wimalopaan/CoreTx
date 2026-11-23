// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/common/calibration_storage.hpp"
#include "boards/common/input_storage.hpp"
#include "boards/common/model_storage.hpp"
#include "boards/common/radio_errors.hpp"
#include "boards/common/radio_storage.hpp"

namespace coretx::firmware {

// All EEPROM-backed records share one I2C memory. StorageSet arbitrates which record may use
// the bus and summarizes whether the radio can be switched off without losing data.
template <typename Memory, typename Probe> struct StorageSet {
    explicit constexpr StorageSet(const bool sb_three_position) noexcept
        : input{sb_three_position} {}

    constexpr void initialize(const afhds2a::TxId transmitter_id) noexcept {
        Probe::initialize();
        radio.initialize(transmitter_id);
        models.initialize(transmitter_id);
    }

    // Calibration, input or radio settings are being written or own the memory.
    [[nodiscard]] constexpr bool settings_busy() const noexcept {
        return calibration.busy() || radio.owns_memory() || input.owns_memory();
    }
    [[nodiscard]] constexpr bool memory_busy() const noexcept {
        return settings_busy() || models.owns_memory();
    }

    // Probe the memory, then load calibration and input setup (in that priority).
    constexpr void service_inputs(const TimestampUs now, Diagnostics &diagnostics) noexcept {
        Probe::service(now);
        const bool model_memory = models.owns_memory();
        if (!radio.owns_memory() && !model_memory && !input.owns_memory())
            calibration.service(now, Probe::result(), diagnostics, models.legacy_workspace());
        if (!calibration.busy() && !model_memory && !radio.owns_memory())
            input.service(now, Probe::result());
    }
    // Radio settings are loaded only after the input setup has been applied.
    constexpr void service_radio(const TimestampUs now) noexcept {
        if (!calibration.busy() && !models.owns_memory() && !input.owns_memory())
            radio.service(now, Probe::result());
    }
    constexpr void service_models(const TimestampUs now, const Model &active,
                                  const RadioSettings &settings) noexcept {
        if (!calibration.busy() && !radio.owns_memory() && !input.owns_memory())
            models.service(now, Probe::result(),
                           calibration.legacy_loaded() ? models.legacy_workspace() : active,
                           settings);
    }

    [[nodiscard]] constexpr bool shutdown_ready() const noexcept {
        return !calibration.busy() && !radio.busy() && !input.busy() && !Memory::busy() &&
               !models.busy();
    }
    [[nodiscard]] constexpr const char *shutdown_error() const noexcept {
        if (Memory::error() != I2cError::none)
            return error_message(Memory::error());
        if ((Probe::state() == StorageProbeState::not_found) ||
            (Probe::state() == StorageProbeState::failed))
            return coretx::ui::tr::models_unique_eeprom;
        const auto calibration_state = calibration.state();
        if ((calibration_state == CalibrationStorageState::multiple) ||
            (calibration_state == CalibrationStorageState::io_error) ||
            (calibration_state == CalibrationStorageState::invalid) ||
            (calibration_state == CalibrationStorageState::verify_error))
            return coretx::ui::tr::power_off_error;
        const auto failed = [](const RadioStorageStatus status) constexpr {
            return (status == RadioStorageStatus::missing) ||
                   (status == RadioStorageStatus::multiple) ||
                   (status == RadioStorageStatus::io_error) ||
                   (status == RadioStorageStatus::invalid) ||
                   (status == RadioStorageStatus::version) ||
                   (status == RadioStorageStatus::verify);
        };
        if (failed(input.status()))
            return input.message();
        if (failed(radio.status()))
            return coretx::ui::tr::power_off_error;
        if (models.loaded() && !models.editable())
            return models.message() != nullptr ? models.message() : coretx::ui::tr::power_off_error;
        return nullptr;
    }

    CalibrationStorage<Memory> calibration{};
    RadioStorage<Memory> radio{};
    InputStorage<Memory> input;
    ModelStorage<Memory> models{};
};

} // namespace coretx::firmware
