// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/common/hardware.hpp"
#include "boards/common/radio_storage.hpp"
#include "coretx/core/input_setup.hpp"
#include "coretx/ui/translation.hpp"

namespace coretx::firmware {
namespace input_storage {

inline constexpr std::uint16_t base_address = 14784;
inline constexpr std::size_t record_size = 128;
inline constexpr std::uint8_t format_version = 3;
inline constexpr std::uint32_t magic = 0x31504E49; // Little-endian "INP1".
using Record = std::array<std::uint8_t, record_size>;
static_assert((base_address >= 14720) && ((base_address + (2 * record_size)) <= 16384));

struct Decoded {
    InputSetup setup{};
    std::uint32_t generation{};
    RadioStorageStatus status{RadioStorageStatus::invalid};
    [[nodiscard]] constexpr bool valid() const noexcept {
        return status == RadioStorageStatus::ready;
    }
};
[[nodiscard]] constexpr Record encode(const InputSetup &setup,
                                      const std::uint32_t generation) noexcept {
    Record bytes{};
    bytes.fill(0xFF);
    coretx::storage::detail::Writer writer{bytes};
    writer.word(magic);
    writer.field(format_version);
    writer.field(setup.extra_analog);
    writer.field(static_cast<std::uint8_t>(setup.digital_modes & 0xFFU));
    writer.field(
        static_cast<std::uint8_t>((setup.trim_beep ? 0U : 1U) | (setup.external_module ? 0U : 2U) |
                                  ((static_cast<unsigned>(setup.digital_modes) >> 8U) << 2U)));
    writer.word(generation);
    for (const auto mode : setup.modes)
        writer.field(mode);
    for (const auto axis : setup.calibration) {
        writer.field(axis.minimum);
        writer.field(axis.center);
        writer.field(axis.maximum);
    }
    coretx::storage::detail::Writer checksum{std::span{bytes}.last(4)};
    checksum.word(coretx::storage::crc32(std::span{bytes}.first(record_size - 4)));
    return bytes;
}
[[nodiscard]] constexpr std::uint32_t checksum(const Record &bytes) noexcept {
    std::uint32_t stored{};
    coretx::storage::detail::Reader reader{std::span{bytes}.last(4)};
    reader.word(stored);
    return stored;
}
[[nodiscard]] constexpr Decoded decode(const Record &bytes) noexcept {
    Decoded decoded{};
    if (std::all_of(bytes.begin(), bytes.end(), [](const auto value) { return value == 0xFF; })) {
        decoded.status = RadioStorageStatus::empty;
        return decoded;
    }
    if (checksum(bytes) != coretx::storage::crc32(std::span{bytes}.first(record_size - 4)))
        return decoded;
    coretx::storage::detail::Reader reader{bytes};
    std::uint32_t identifier{};
    std::uint8_t version{};
    std::uint8_t flags{};
    std::uint8_t digital{};
    reader.word(identifier);
    reader.field(version);
    reader.field(decoded.setup.extra_analog);
    reader.field(digital);
    reader.field(flags);
    reader.word(decoded.generation);
    if (identifier != magic)
        return decoded;
    if ((version != 1) && (version != 2) && (version != format_version)) {
        decoded.status = RadioStorageStatus::version;
        return decoded;
    }
    for (auto &mode : decoded.setup.modes)
        reader.field(mode);
    for (auto &axis : decoded.setup.calibration) {
        reader.field(axis.minimum);
        reader.field(axis.center);
        reader.field(axis.maximum);
    }
    decoded.setup.trim_beep = (flags & 1U) == 0;
    if (version < 3) {
        if (digital > 1)
            return decoded;
        if (digital != 0) {
            for (std::size_t i = 0; i < board::digital_inputs; ++i)
                decoded.setup.set_digital_mode(i, DigitalInputMode::two_position);
            decoded.setup.external_module = board::external_module_digital_mask == 0;
        }
    } else {
        decoded.setup.digital_modes = static_cast<std::uint16_t>(digital | ((flags >> 2U) << 8U));
        decoded.setup.external_module = (flags & 2U) == 0;
    }
    if (!reader.good ||
        (flags > (version == 1   ? 0U
                  : version == 2 ? 1U
                                 : 15U)) ||
        !valid_input_setup(decoded.setup) ||
        !std::all_of(bytes.begin() + static_cast<std::ptrdiff_t>(reader.position), bytes.end() - 4,
                     [](const auto value) { return value == 0xFF; }))
        return decoded;
    decoded.status = RadioStorageStatus::ready;
    return decoded;
}
} // namespace input_storage

template <typename Memory> class InputStorage {
  public:
    constexpr explicit InputStorage(const bool sb_three_position = false) noexcept
        : setup_(sb_three_position) {}
    [[nodiscard]] constexpr bool owns_memory() const noexcept {
        return (stage_ != Stage::wait) && (stage_ != Stage::ready);
    }
    [[nodiscard]] constexpr bool loaded() const noexcept { return loaded_; }
    [[nodiscard]] constexpr bool busy() const noexcept {
        return (stage_ != Stage::ready) || dirty_;
    }
    [[nodiscard]] constexpr bool writable() const noexcept { return writable_; }
    [[nodiscard]] constexpr bool can_recover() const noexcept {
        return loaded_ && (stage_ == Stage::ready) && (status_ == RadioStorageStatus::invalid);
    }
    [[nodiscard]] constexpr bool recover() noexcept {
        if (!can_recover())
            return false;
        record_ = input_storage::encode(setup_, 0);
        pending_checksum_ = input_storage::checksum(record_);
        has_active_ = false;
        generation_ = 0;
        target_ = 0;
        loaded_ = false;
        dirty_ = true;
        status_ = RadioStorageStatus::saving;
        stage_ = Stage::invalidate_other;
        transfer_.erase(address(1));
        return true;
    }
    [[nodiscard]] constexpr RadioStorageStatus status() const noexcept { return status_; }
    [[nodiscard]] constexpr const InputSetup &setup() const noexcept { return setup_; }
    [[nodiscard]] constexpr const char *message() const noexcept {
        namespace tr = coretx::ui::tr;
        switch (status_) {
        case RadioStorageStatus::ready:
        case RadioStorageStatus::empty:
        case RadioStorageStatus::saved:
            return nullptr;
        case RadioStorageStatus::loading:
            return tr::inputs_loading;
        case RadioStorageStatus::saving:
            return tr::saving;
        case RadioStorageStatus::missing:
            return tr::storage_no_eeprom;
        case RadioStorageStatus::multiple:
            return tr::storage_multiple;
        case RadioStorageStatus::version:
            return tr::storage_bad_version;
        case RadioStorageStatus::verify:
            return tr::storage_verify_error;
        case RadioStorageStatus::io_error:
            return tr::inputs_io_error;
        case RadioStorageStatus::invalid:
            return tr::inputs_invalid;
        }
        return tr::inputs_invalid;
    }
    [[nodiscard]] constexpr bool save(const InputSetup &setup) noexcept {
        if (!writable_ || !valid_input_setup(setup))
            return false;
        setup_ = setup;
        dirty_ = true;
        return true;
    }
    constexpr void service(const TimestampUs now, const StorageProbeResult probe) noexcept {
        if (stage_ == Stage::wait) {
            if (probe.state == StorageProbeState::failed) {
                finish(RadioStorageStatus::io_error);
                return;
            }
            const auto selection = select_memory<Memory>(probe);
            if (selection == ProbeSelection::wait)
                return;
            if (selection != ProbeSelection::selected) {
                finish(selection == ProbeSelection::missing ? RadioStorageStatus::missing
                                                            : RadioStorageStatus::multiple);
                return;
            }
            stage_ = Stage::first;
            transfer_.read(address(0), record_);
        }
        if (stage_ == Stage::ready) {
            if (!dirty_ || !writable_)
                return;
            target_ = has_active_ ? 1U - active_ : 0U;
            write();
        }
        for (;;) {
            switch (transfer_.service(now)) {
            case Transfer::Result::busy:
                return;
            case Transfer::Result::failed:
                writable_ = false;
                finish(RadioStorageStatus::io_error);
                return;
            case Transfer::Result::mismatch:
                writable_ = false;
                finish(RadioStorageStatus::verify);
                return;
            case Transfer::Result::done:
                break;
            }
            switch (stage_) {
            case Stage::invalidate_other:
                stage_ = Stage::write;
                transfer_.write(address(target_), record_, 1);
                continue;
            case Stage::first:
                first_ = summary(input_storage::decode(record_));
                stage_ = Stage::second;
                transfer_.read(address(1), record_);
                continue;
            case Stage::second:
                if (!load(input_storage::decode(record_)))
                    return;
                transfer_.read(address(0), record_);
                continue;
            case Stage::reload: {
                const auto decoded = input_storage::decode(record_);
                if (!decoded.valid() || (decoded.generation != first_.generation) ||
                    (input_storage::checksum(record_) != first_.checksum)) {
                    finish(RadioStorageStatus::invalid);
                    return;
                }
                select(decoded, 0);
                return;
            }
            case Stage::write:
                if (!input_storage::decode(record_).valid()) {
                    writable_ = false;
                    finish(RadioStorageStatus::verify);
                    return;
                }
                generation_ = has_active_ ? generation_ + 1U : 0U;
                active_ = target_;
                has_active_ = true;
                writable_ = true;
                dirty_ = input_storage::checksum(input_storage::encode(setup_, generation_)) !=
                         pending_checksum_;
                finish(RadioStorageStatus::saved);
                return;
            case Stage::wait:
            case Stage::ready:
                return;
            }
        }
    }

  private:
    enum class Stage : std::uint8_t { wait, first, second, reload, ready, invalidate_other, write };
    using Transfer = SlotTransfer<Memory>;
    [[nodiscard]] static constexpr std::uint16_t address(const std::size_t slot) noexcept {
        return static_cast<std::uint16_t>(input_storage::base_address +
                                          slot * input_storage::record_size);
    }
    constexpr void write() noexcept {
        record_ = input_storage::encode(setup_, has_active_ ? generation_ + 1U : 0U);
        pending_checksum_ = input_storage::checksum(record_);
        status_ = RadioStorageStatus::saving;
        stage_ = Stage::write;
        transfer_.write(address(target_), record_, 1);
    }
    constexpr void finish(const RadioStorageStatus status) noexcept {
        status_ = status;
        stage_ = Stage::ready;
        loaded_ = true;
    }
    // Slot 0 is reduced to a summary so that only one decoded setup exists at a time.
    struct Summary {
        std::uint32_t generation{};
        std::uint32_t checksum{};
        RadioStorageStatus status{RadioStorageStatus::invalid};
        [[nodiscard]] constexpr bool valid() const noexcept {
            return status == RadioStorageStatus::ready;
        }
    };
    [[nodiscard]] constexpr Summary summary(const input_storage::Decoded &decoded) const noexcept {
        return {decoded.generation, input_storage::checksum(record_), decoded.status};
    }
    constexpr void select(const input_storage::Decoded &selected, const std::size_t bank) noexcept {
        setup_ = selected.setup;
        generation_ = selected.generation;
        active_ = bank;
        has_active_ = true;
        writable_ = true;
        finish(RadioStorageStatus::ready);
    }
    // Returns true when slot 0 won and has to be read again.
    [[nodiscard]] constexpr bool load(const input_storage::Decoded &second) noexcept {
        if ((first_.status == RadioStorageStatus::version) ||
            (second.status == RadioStorageStatus::version)) {
            finish(RadioStorageStatus::version);
            return false;
        }
        if (!first_.valid() && !second.valid()) {
            writable_ = (first_.status == RadioStorageStatus::empty) &&
                        (second.status == RadioStorageStatus::empty);
            finish(writable_ ? RadioStorageStatus::empty : RadioStorageStatus::invalid);
            return false;
        }
        if (first_.valid() && second.valid() &&
            (((second.generation - first_.generation) == 0x80000000U) ||
             ((first_.generation == second.generation) &&
              (first_.checksum != input_storage::checksum(record_))))) {
            finish(RadioStorageStatus::invalid);
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
    InputSetup setup_{};
    input_storage::Record record_{};
    Summary first_{};
    std::uint32_t pending_checksum_{};
    Transfer transfer_{};
    std::uint32_t generation_{};
    std::size_t active_{};
    std::size_t target_{};
    Stage stage_{};
    RadioStorageStatus status_{RadioStorageStatus::loading};
    bool loaded_{};
    bool writable_{};
    bool dirty_{};
    bool has_active_{};
};

} // namespace coretx::firmware
