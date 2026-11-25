// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/common/slot_transfer.hpp"

namespace coretx::firmware {

enum class SettingsStatus : std::uint8_t {
    loading,
    ready,
    empty,
    saving,
    saved,
    missing,
    multiple,
    io_error,
    invalid,
    version,
    verify
};

// Journaled A/B storage of one small settings record (calibration, input setup).
//
// Codec requirements:
//   using Value; using Record = std::array<std::uint8_t, N>; using Decoded;
//   base_address                      first of two consecutive record slots
//   overwrite_invalid                 damaged records may be replaced without recover()
//   encode(const Value &, generation) -> Record
//   decode(const Record &)            -> Decoded {generation, status, valid()}
//   value(const Decoded &)            -> const Value &
//   valid(const Value &)              -> bool, checked before saving
template <typename Memory, typename Codec> class SettingsStorage {
  public:
    using Value = typename Codec::Value;
    using Record = typename Codec::Record;

    constexpr SettingsStorage() noexcept = default;
    constexpr explicit SettingsStorage(const Value &initial) noexcept : value_(initial) {}

    [[nodiscard]] constexpr bool owns_memory() const noexcept {
        return (stage_ != Stage::wait) && (stage_ != Stage::ready);
    }
    [[nodiscard]] constexpr bool loaded() const noexcept { return loaded_; }
    [[nodiscard]] constexpr bool busy() const noexcept {
        return (stage_ != Stage::ready) || (dirty_ && writable_);
    }
    [[nodiscard]] constexpr bool writable() const noexcept { return writable_; }
    [[nodiscard]] constexpr bool can_recover() const noexcept {
        return loaded_ && (stage_ == Stage::ready) && (status_ == SettingsStatus::invalid);
    }
    [[nodiscard]] constexpr bool recover() noexcept {
        if (!can_recover())
            return false;
        record_ = Codec::encode(value_, 0);
        pending_checksum_ = checksum(record_);
        has_active_ = false;
        generation_ = 0;
        target_ = 0;
        loaded_ = false;
        dirty_ = true;
        status_ = SettingsStatus::saving;
        stage_ = Stage::invalidate_other;
        transfer_.erase(address(1));
        return true;
    }
    [[nodiscard]] constexpr SettingsStatus status() const noexcept { return status_; }
    [[nodiscard]] constexpr std::uint32_t generation() const noexcept { return generation_; }
    [[nodiscard]] constexpr const Value &value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool save(const Value &value) noexcept {
        if (!writable_ || !Codec::valid(value))
            return false;
        value_ = value;
        dirty_ = true;
        return true;
    }
    constexpr void service(const TimestampUs now, const StorageProbeResult probe) noexcept {
        if (stage_ == Stage::wait) {
            if (probe.state == StorageProbeState::failed) {
                finish(SettingsStatus::io_error);
                return;
            }
            const auto selection = select_memory<Memory>(probe);
            if (selection == ProbeSelection::wait)
                return;
            if (selection != ProbeSelection::selected) {
                finish(selection == ProbeSelection::missing ? SettingsStatus::missing
                                                            : SettingsStatus::multiple);
                return;
            }
            status_ = SettingsStatus::loading;
            stage_ = Stage::first;
            transfer_.read(address(0), record_);
        }
        if (stage_ == Stage::ready) {
            if (!dirty_ || !writable_)
                return;
            target_ = has_active_ ? 1U - active_ : 0U;
            record_ = Codec::encode(value_, has_active_ ? generation_ + 1U : 0U);
            pending_checksum_ = checksum(record_);
            status_ = SettingsStatus::saving;
            stage_ = Stage::write;
            transfer_.write(address(target_), record_, 1);
        }
        for (;;) {
            switch (transfer_.service(now)) {
            case Transfer::Result::busy:
                return;
            case Transfer::Result::failed:
                writable_ = false;
                finish(SettingsStatus::io_error);
                return;
            case Transfer::Result::mismatch:
                writable_ = false;
                finish(SettingsStatus::verify);
                return;
            case Transfer::Result::done:
                break;
            }
            switch (stage_) {
            case Stage::invalidate_other:
                stage_ = Stage::write;
                transfer_.write(address(target_), record_, 1);
                continue;
            case Stage::first: {
                const auto decoded = Codec::decode(record_);
                first_ = {decoded.generation, checksum(record_), decoded.status};
                stage_ = Stage::second;
                transfer_.read(address(1), record_);
                continue;
            }
            case Stage::second:
                if (!load(Codec::decode(record_)))
                    return;
                transfer_.read(address(0), record_);
                continue;
            case Stage::reload: {
                const auto decoded = Codec::decode(record_);
                if (!decoded.valid() || (decoded.generation != first_.generation) ||
                    (checksum(record_) != first_.checksum)) {
                    finish(SettingsStatus::invalid);
                    return;
                }
                select(decoded, 0);
                return;
            }
            case Stage::write:
                if (!Codec::decode(record_).valid()) {
                    writable_ = false;
                    finish(SettingsStatus::verify);
                    return;
                }
                generation_ = has_active_ ? generation_ + 1U : 0U;
                active_ = target_;
                has_active_ = true;
                writable_ = true;
                dirty_ = checksum(Codec::encode(value_, generation_)) != pending_checksum_;
                finish(SettingsStatus::saved);
                return;
            case Stage::wait:
            case Stage::ready:
                return;
            }
        }
    }

  protected:
    // Rejects a loaded record that the owner cannot apply; a replacement goes to the other slot.
    constexpr void reject() noexcept {
        writable_ = Codec::overwrite_invalid;
        finish(SettingsStatus::invalid);
    }

  private:
    enum class Stage : std::uint8_t { wait, first, second, reload, ready, invalidate_other, write };
    using Transfer = SlotTransfer<Memory>;
    [[nodiscard]] static constexpr std::uint16_t address(const std::size_t slot) noexcept {
        return static_cast<std::uint16_t>(Codec::base_address + slot * std::tuple_size_v<Record>);
    }
    [[nodiscard]] static constexpr std::uint32_t checksum(const Record &record) noexcept {
        return static_cast<std::uint32_t>(record[record.size() - 4]) |
               (static_cast<std::uint32_t>(record[record.size() - 3]) << 8) |
               (static_cast<std::uint32_t>(record[record.size() - 2]) << 16) |
               (static_cast<std::uint32_t>(record[record.size() - 1]) << 24);
    }
    constexpr void finish(const SettingsStatus status) noexcept {
        status_ = status;
        stage_ = Stage::ready;
        loaded_ = true;
    }
    // Slot 0 is reduced to a summary so that only one decoded value exists at a time.
    struct Summary {
        std::uint32_t generation{};
        std::uint32_t checksum{};
        SettingsStatus status{SettingsStatus::invalid};
        [[nodiscard]] constexpr bool valid() const noexcept {
            return status == SettingsStatus::ready;
        }
    };
    constexpr void select(const typename Codec::Decoded &selected,
                          const std::size_t bank) noexcept {
        value_ = Codec::value(selected);
        generation_ = selected.generation;
        active_ = bank;
        has_active_ = true;
        writable_ = true;
        finish(SettingsStatus::ready);
    }
    // Returns true when slot 0 won and has to be read again.
    [[nodiscard]] constexpr bool load(const typename Codec::Decoded &second) noexcept {
        if ((first_.status == SettingsStatus::version) ||
            (second.status == SettingsStatus::version)) {
            finish(SettingsStatus::version);
            return false;
        }
        if (!first_.valid() && !second.valid()) {
            const bool empty = (first_.status == SettingsStatus::empty) &&
                               (second.status == SettingsStatus::empty);
            writable_ = empty || Codec::overwrite_invalid;
            finish(empty ? SettingsStatus::empty : SettingsStatus::invalid);
            return false;
        }
        if (first_.valid() && second.valid() &&
            (((second.generation - first_.generation) == 0x80000000U) ||
             ((first_.generation == second.generation) &&
              (first_.checksum != checksum(record_))))) {
            finish(SettingsStatus::invalid);
            return false;
        }
        if (!first_.valid() || (second.valid() && (second.generation != first_.generation) &&
                                ((second.generation - first_.generation) < 0x80000000U))) {
            select(second, 1);
            return false;
        }
        stage_ = Stage::reload;
        return true;
    }
    Value value_{};
    Record record_{};
    Summary first_{};
    std::uint32_t pending_checksum_{};
    Transfer transfer_{};
    std::uint32_t generation_{};
    std::size_t active_{};
    std::size_t target_{};
    Stage stage_{};
    SettingsStatus status_{SettingsStatus::loading};
    bool loaded_{};
    bool writable_{};
    bool dirty_{};
    bool has_active_{};
};

} // namespace coretx::firmware
