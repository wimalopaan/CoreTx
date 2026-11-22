// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/common/controls.hpp"
#include "coretx/storage/storage.hpp"
#include "coretx/ui/translation.hpp"
#include "platform/stm32f072/eeprom.hpp"

namespace coretx::firmware::i6x {

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
    [[nodiscard]] constexpr const Model &legacy_model() const noexcept { return snapshot_.model; }
    constexpr void save_setup(const ControlSetup &setup) noexcept {
        if (!busy())
            save(setup);
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
            save(diagnostics.setup());
        }
    }
    constexpr void service(const TimestampUs now, const StorageProbeResult probe,
                           Diagnostics &diagnostics) noexcept {
        if (stage_ == Stage::wait_probe) {
            if ((probe.state == StorageProbeState::idle) ||
                (probe.state == StorageProbeState::scanning)) {
                return;
            }
            if (probe.state != StorageProbeState::found) {
                state_ = CalibrationStorageState::not_found;
                stage_ = Stage::ready;
                return;
            }
            const auto mask = probe.addresses;
            if (std::popcount(mask) != 1) {
                state_ = CalibrationStorageState::multiple;
                stage_ = Stage::ready;
                return;
            }
            Memory::select(static_cast<std::uint8_t>(StorageProbeResult::first_address +
                                                     std::countr_zero(mask)));
            state_ = CalibrationStorageState::loading;
            stage_ = Stage::load;
        }
        if (stage_ == Stage::ready) {
            return;
        }
        if (transferring_) {
            Memory::service(now);
            if (Memory::busy()) {
                return;
            }
            transferring_ = false;
            error_ = Memory::error();
            if (error_ != I2cError::none) {
                finish(CalibrationStorageState::io_error);
                return;
            }
            switch (stage_) {
            case Stage::load:
                offset_ += Memory::page_size;
                if (offset_ == coretx::storage::slot_size) {
                    offset_ = 0;
                    if (++slot_ == 2) {
                        load(diagnostics);
                        return;
                    }
                }
                break;
            case Stage::invalidate:
                offset_ = Memory::page_size;
                stage_ = Stage::body;
                break;
            case Stage::body:
                offset_ += Memory::page_size;
                if (offset_ == coretx::storage::slot_size) {
                    offset_ = 0;
                    stage_ = Stage::commit;
                }
                break;
            case Stage::commit:
                stage_ = Stage::verify;
                offset_ = 0;
                break;
            case Stage::verify:
                if (!std::equal(verify_.begin(), verify_.end(),
                                records_[slot_].begin() + offset_)) {
                    finish(CalibrationStorageState::verify_error);
                    return;
                }
                offset_ += Memory::page_size;
                if (offset_ == coretx::storage::slot_size) {
                    active_ = slot_;
                    has_active_ = true;
                    generation_ = pending_generation_;
                    record_error_ = coretx::storage::Error::none;
                    finish(CalibrationStorageState::saved);
                    return;
                }
                break;
            default:
                break;
            }
        }
        const auto address =
            static_cast<std::uint16_t>(slot_ * coretx::storage::slot_size + offset_);
        bool started{};
        switch (stage_) {
        case Stage::load:
            started = Memory::read(address,
                                   std::span{records_[slot_]}.subspan(offset_, Memory::page_size));
            break;
        case Stage::invalidate:
            started = Memory::write(static_cast<std::uint16_t>(slot_ * coretx::storage::slot_size),
                                    invalid_magic_);
            break;
        case Stage::body:
        case Stage::commit:
            started = Memory::write(address,
                                    std::span{records_[slot_]}.subspan(offset_, Memory::page_size));
            break;
        case Stage::verify:
            started = Memory::read(address, verify_);
            break;
        default:
            return;
        }
        transferring_ = started;
        if (!started) {
            error_ = Memory::error();
            finish(CalibrationStorageState::io_error);
        }
    }

  private:
    enum class Stage : std::uint8_t { wait_probe, load, ready, invalidate, body, commit, verify };

    constexpr void finish(const CalibrationStorageState state) noexcept {
        state_ = state;
        stage_ = Stage::ready;
    }

    constexpr void load(Diagnostics &diagnostics) noexcept {
        // The enlarged model must not add a full State temporary to the main-loop stack.
        const auto selection = coretx::storage::select_into(records_[0], records_[1], snapshot_);
        record_error_ = selection.status.code;
        if (!selection) {
            finish(record_error_ == coretx::storage::Error::empty
                       ? CalibrationStorageState::empty
                       : CalibrationStorageState::invalid);
            return;
        }
        if (!diagnostics.restore_calibration(snapshot_.setup)) {
            record_error_ = coretx::storage::Error::setup;
            finish(CalibrationStorageState::invalid);
            return;
        }
        active_ = selection.slot;
        generation_ = selection.generation;
        has_active_ = true;
        finish(CalibrationStorageState::loaded);
    }

    constexpr void save(const ControlSetup &setup) noexcept {
        if ((state_ == CalibrationStorageState::not_found) ||
            (state_ == CalibrationStorageState::multiple) ||
            (state_ == CalibrationStorageState::io_error) ||
            (state_ == CalibrationStorageState::verify_error) ||
            (record_error_ == coretx::storage::Error::version) ||
            (record_error_ == coretx::storage::Error::generation)) {
            return; // Keep the explicit failure visible; never overwrite a newer/ambiguous format.
        }
        snapshot_.model = diagnostic_model;
        snapshot_.setup = setup;
        slot_ = has_active_ ? 1U - active_ : 0U;
        pending_generation_ = has_active_ ? generation_ + 1U : 0U;
        const auto encoded =
            coretx::storage::encode(snapshot_, pending_generation_, records_[slot_]);
        if (!encoded) {
            record_error_ = encoded.code;
            finish(CalibrationStorageState::invalid);
            return;
        }
        offset_ = 0;
        transferring_ = false;
        state_ = CalibrationStorageState::saving;
        stage_ = Stage::invalidate;
    }

    std::array<coretx::storage::Record, 2> records_{};
    coretx::storage::State snapshot_{};
    std::array<std::uint8_t, Memory::page_size> verify_{};
    inline static constexpr std::array<std::uint8_t, 1> invalid_magic_{0};
    std::size_t offset_{};
    std::size_t slot_{};
    std::size_t active_{};
    std::uint32_t generation_{};
    std::uint32_t pending_generation_{};
    Stage stage_{Stage::wait_probe};
    CalibrationStorageState state_{};
    coretx::storage::Error record_error_{};
    I2cError error_{};
    bool transferring_{};
    bool has_active_{};
};

} // namespace coretx::firmware::i6x
