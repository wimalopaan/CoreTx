// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/common/controls.hpp"
#include "boards/common/slot_transfer.hpp"
#include "coretx/storage/storage.hpp"
#include "coretx/ui/translation.hpp"
#include "platform/stm32f072/eeprom.hpp"

namespace coretx::firmware {

enum class CalibrationStorageState : std::uint8_t {
    waiting,
    loading,
    loaded,
    empty,
    saving,
    saved,
    not_found,
    multiple,
    io_error,
    invalid,
    verify_error
};

template <typename Memory> class CalibrationStorage {
  public:
    [[nodiscard]] constexpr CalibrationStorageState state() const noexcept { return state_; }
    [[nodiscard]] constexpr I2cError error() const noexcept { return error_; }
    [[nodiscard]] constexpr coretx::storage::Error record_error() const noexcept {
        return record_error_;
    }
    [[nodiscard]] constexpr std::uint32_t generation() const noexcept { return generation_; }
    [[nodiscard]] constexpr bool busy() const noexcept { return stage_ != Stage::ready; }
    // True after a successful load; the caller's workspace then holds the stored legacy model.
    [[nodiscard]] constexpr bool legacy_loaded() const noexcept { return legacy_loaded_; }
    constexpr void save_setup(const Diagnostics &diagnostics) noexcept {
        if (!busy())
            save(diagnostics);
    }

    [[nodiscard]] constexpr const char *label(const bool calibrated) const noexcept {
        if ((state_ == CalibrationStorageState::waiting) ||
            (state_ == CalibrationStorageState::loading)) {
            return coretx::ui::tr::load_cal;
        }
        if (state_ == CalibrationStorageState::saving) {
            return coretx::ui::tr::saving;
        }
        if (!calibrated) {
            return coretx::ui::tr::need_cal;
        }
        return (state_ == CalibrationStorageState::saved) ||
                       (state_ == CalibrationStorageState::loaded)
                   ? coretx::ui::tr::ok_cal
                   : coretx::ui::tr::ram_cal;
    }

    constexpr void handle(const Key key, Diagnostics &diagnostics) noexcept {
        if ((key == Key::ok) && busy()) {
            return;
        }
        const auto phase = diagnostics.phase();
        diagnostics.handle(key);
        if ((key == Key::ok) && (phase == CalibrationPhase::range) &&
            (diagnostics.phase() == CalibrationPhase::idle) && diagnostics.calibrated()) {
            save(diagnostics);
        }
    }
    // The legacy record format contains a full model. It is decoded into the caller-owned
    // workspace (the model storage's not yet loaded model) instead of a private State copy.
    constexpr void service(const TimestampUs now, const StorageProbeResult probe,
                           Diagnostics &diagnostics, Model &workspace) noexcept {
        if (stage_ == Stage::wait_probe) {
            const auto selection = select_memory<Memory>(probe);
            if (selection == ProbeSelection::wait)
                return;
            if (selection != ProbeSelection::selected) {
                finish(selection == ProbeSelection::missing ? CalibrationStorageState::not_found
                                                            : CalibrationStorageState::multiple);
                return;
            }
            state_ = CalibrationStorageState::loading;
            stage_ = Stage::load;
            read();
        }
        for (;;) {
            switch (transfer_.service(now)) {
            case Transfer::Result::busy:
                return;
            case Transfer::Result::failed:
                error_ = Memory::error();
                finish(CalibrationStorageState::io_error);
                return;
            case Transfer::Result::mismatch:
                finish(CalibrationStorageState::verify_error);
                return;
            case Transfer::Result::done:
                break;
            }
            if (stage_ == Stage::load) {
                if (!load(diagnostics, workspace))
                    return;
                read();
                continue;
            }
            if (stage_ == Stage::write) {
                active_ = slot_;
                has_active_ = true;
                generation_ = pending_generation_;
                record_error_ = coretx::storage::Error::none;
                finish(CalibrationStorageState::saved);
            }
            return;
        }
    }

  private:
    enum class Stage : std::uint8_t { wait_probe, load, ready, write };
    using Transfer = SlotTransfer<Memory>;
    [[nodiscard]] constexpr std::uint16_t address() const noexcept {
        return static_cast<std::uint16_t>(slot_ * coretx::storage::slot_size);
    }
    constexpr void read() noexcept { transfer_.read(address(), record_); }

    constexpr void finish(const CalibrationStorageState state) noexcept {
        state_ = state;
        stage_ = Stage::ready;
    }

    [[nodiscard]] constexpr std::uint32_t record_checksum() const noexcept {
        return static_cast<std::uint32_t>(record_[12]) |
               (static_cast<std::uint32_t>(record_[13]) << 8) |
               (static_cast<std::uint32_t>(record_[14]) << 16) |
               (static_cast<std::uint32_t>(record_[15]) << 24);
    }

    // Both slots share one buffer: slot 0 and slot 1 are inspected in turn, and slot 0 is read
    // again if it wins. Returns true while further pages have to be read.
    constexpr bool load(Diagnostics &diagnostics, Model &workspace) noexcept {
        ControlSetup setup{};
        const auto info = coretx::storage::decode_parts(record_, workspace, setup);
        if (reload_) {
            if (!info || (info.generation != generation_) ||
                (record_checksum() != first_checksum_)) {
                record_error_ = info ? coretx::storage::Error::generation : info.status.code;
                finish(CalibrationStorageState::invalid);
                return false;
            }
            return restore(diagnostics, setup, 0, generation_);
        }
        if (slot_ == 0) {
            first_ = info;
            first_checksum_ = record_checksum();
            slot_ = 1;
            return true;
        }
        const auto selection =
            coretx::storage::select_slots(first_, info, first_checksum_ == record_checksum());
        record_error_ = selection.status.code;
        if (!selection) {
            finish(record_error_ == coretx::storage::Error::empty
                       ? CalibrationStorageState::empty
                       : CalibrationStorageState::invalid);
            return false;
        }
        if (selection.slot == 1)
            return restore(diagnostics, setup, 1, selection.generation);
        generation_ = selection.generation;
        reload_ = true;
        slot_ = 0;
        return true;
    }

    constexpr bool restore(Diagnostics &diagnostics, const ControlSetup &setup,
                           const std::size_t slot, const std::uint32_t generation) noexcept {
        reload_ = false;
        if (!diagnostics.restore_calibration(setup)) {
            record_error_ = coretx::storage::Error::setup;
            finish(CalibrationStorageState::invalid);
            return false;
        }
        active_ = slot;
        generation_ = generation;
        has_active_ = true;
        legacy_loaded_ = true;
        finish(CalibrationStorageState::loaded);
        return false;
    }

    // Format 7 still carries a model for older firmware; the active model is stored there.
    constexpr void save(const Diagnostics &diagnostics) noexcept {
        if ((state_ == CalibrationStorageState::not_found) ||
            (state_ == CalibrationStorageState::multiple) ||
            (state_ == CalibrationStorageState::io_error) ||
            (state_ == CalibrationStorageState::verify_error) ||
            (record_error_ == coretx::storage::Error::version) ||
            (record_error_ == coretx::storage::Error::generation)) {
            return; // Keep the explicit failure visible; never overwrite a newer/ambiguous format.
        }
        slot_ = has_active_ ? 1U - active_ : 0U;
        pending_generation_ = has_active_ ? generation_ + 1U : 0U;
        const auto encoded = coretx::storage::encode(diagnostics.model(), diagnostics.setup(),
                                                     pending_generation_, record_);
        if (!encoded) {
            record_error_ = encoded.code;
            finish(CalibrationStorageState::invalid);
            return;
        }
        state_ = CalibrationStorageState::saving;
        stage_ = Stage::write;
        // The first page holds the header; writing it last commits the record.
        transfer_.write(address(), record_, Memory::page_size);
    }

    coretx::storage::Record record_{};
    coretx::storage::SlotInfo first_{};
    std::uint32_t first_checksum_{};
    Transfer transfer_{};
    std::size_t slot_{};
    std::size_t active_{};
    std::uint32_t generation_{};
    std::uint32_t pending_generation_{};
    Stage stage_{Stage::wait_probe};
    CalibrationStorageState state_{};
    coretx::storage::Error record_error_{};
    I2cError error_{};
    bool has_active_{};
    bool reload_{};
    bool legacy_loaded_{};
};

} // namespace coretx::firmware
