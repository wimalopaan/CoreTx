// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/common/radio_storage.hpp"
#include "coretx/core/input_setup.hpp"
#include "coretx/ui/translation.hpp"

namespace coretx::firmware {
namespace input_storage {

inline constexpr std::uint16_t base_address = 14784;
inline constexpr std::size_t record_size = 128;
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
    writer.field(std::uint8_t{1});
    writer.field(setup.extra_analog);
    writer.field(setup.extra_digital);
    writer.field(std::uint8_t{0});
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
[[nodiscard]] constexpr Decoded decode(const Record &bytes) noexcept {
    Decoded decoded{};
    if (std::all_of(bytes.begin(), bytes.end(), [](const auto value) { return value == 0xFF; })) {
        decoded.status = RadioStorageStatus::empty;
        return decoded;
    }
    coretx::storage::detail::Reader checksum{std::span{bytes}.last(4)};
    std::uint32_t stored{};
    checksum.word(stored);
    if (stored != coretx::storage::crc32(std::span{bytes}.first(record_size - 4)))
        return decoded;
    coretx::storage::detail::Reader reader{bytes};
    std::uint32_t identifier{};
    std::uint8_t version{};
    std::uint8_t reserved{};
    reader.word(identifier);
    reader.field(version);
    reader.field(decoded.setup.extra_analog);
    reader.field(decoded.setup.extra_digital);
    reader.field(reserved);
    reader.word(decoded.generation);
    if (identifier != magic)
        return decoded;
    if (version != 1) {
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
    if (!reader.good || (reserved != 0) || !valid_input_setup(decoded.setup) ||
        !std::all_of(bytes.begin() + static_cast<std::ptrdiff_t>(reader.position), bytes.end() - 4,
                     [](const auto value) { return value == 0xFF; }))
        return decoded;
    decoded.status = RadioStorageStatus::ready;
    return decoded;
}
} // namespace input_storage

template <typename Memory> class InputStorage {
  public:
    constexpr explicit InputStorage(const bool i6s = false) noexcept : setup_(i6s) {}
    [[nodiscard]] constexpr bool owns_memory() const noexcept {
        return (stage_ != Stage::wait) && (stage_ != Stage::ready);
    }
    [[nodiscard]] constexpr bool loaded() const noexcept { return loaded_; }
    [[nodiscard]] constexpr bool writable() const noexcept { return writable_; }
    [[nodiscard]] constexpr bool can_recover() const noexcept {
        return loaded_ && (stage_ == Stage::ready) && (status_ == RadioStorageStatus::invalid);
    }
    [[nodiscard]] constexpr bool recover() noexcept {
        if (!can_recover())
            return false;
        pending_setup_ = setup_;
        record_ = input_storage::encode(pending_setup_, 0);
        has_active_ = false;
        generation_ = 0;
        target_ = 1;
        offset_ = 0;
        loaded_ = false;
        dirty_ = true;
        status_ = RadioStorageStatus::saving;
        stage_ = Stage::invalidate_other;
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
    constexpr void service(const TimestampUs now, const i6x::StorageProbeResult probe) noexcept {
        if (stage_ == Stage::wait) {
            if ((probe.state == i6x::StorageProbeState::idle) ||
                (probe.state == i6x::StorageProbeState::scanning))
                return;
            if (probe.state == i6x::StorageProbeState::failed) {
                finish(RadioStorageStatus::io_error);
                return;
            }
            if (probe.state != i6x::StorageProbeState::found) {
                finish(RadioStorageStatus::missing);
                return;
            }
            if (std::popcount(probe.addresses) != 1) {
                finish(RadioStorageStatus::multiple);
                return;
            }
            Memory::select(static_cast<std::uint8_t>(i6x::StorageProbeResult::first_address +
                                                     std::countr_zero(probe.addresses)));
            stage_ = Stage::first;
        }
        if (stage_ == Stage::ready) {
            if (!dirty_ || !writable_)
                return;
            target_ = has_active_ ? 1U - active_ : 0U;
            pending_setup_ = setup_;
            record_ = input_storage::encode(pending_setup_, has_active_ ? generation_ + 1U : 0U);
            status_ = RadioStorageStatus::saving;
            stage_ = Stage::invalidate;
            offset_ = 0;
        }
        if (pending_) {
            Memory::service(now);
            if (Memory::busy())
                return;
            pending_ = false;
            if (Memory::error() != i6x::I2cError::none) {
                writable_ = false;
                finish(RadioStorageStatus::io_error);
                return;
            }
            if (stage_ == Stage::invalidate_other) {
                target_ = 0;
                offset_ = 0;
                stage_ = Stage::invalidate;
            } else if (stage_ == Stage::invalidate) {
                offset_ = 1;
                stage_ = Stage::body;
            } else if (stage_ == Stage::commit) {
                offset_ = 0;
                stage_ = Stage::verify;
            } else {
                offset_ += count_;
                if (offset_ == input_storage::record_size) {
                    offset_ = 0;
                    switch (stage_) {
                    case Stage::first:
                        first_ = input_storage::decode(record_);
                        stage_ = Stage::second;
                        break;
                    case Stage::second:
                        load(input_storage::decode(record_));
                        return;
                    case Stage::body:
                        stage_ = Stage::commit;
                        break;
                    case Stage::verify:
                        if (record_ != input_storage::encode(pending_setup_,
                                                             has_active_ ? generation_ + 1U : 0U)) {
                            writable_ = false;
                            finish(RadioStorageStatus::verify);
                            return;
                        }
                        generation_ = has_active_ ? generation_ + 1U : 0U;
                        active_ = target_;
                        has_active_ = true;
                        writable_ = true;
                        dirty_ = setup_ != pending_setup_;
                        finish(RadioStorageStatus::saved);
                        return;
                    default:
                        break;
                    }
                }
            }
        }
        const auto bank = stage_ == Stage::first ? 0U : stage_ == Stage::second ? 1U : target_;
        const auto address = static_cast<std::uint16_t>(
            input_storage::base_address + bank * input_storage::record_size + offset_);
        count_ = std::min(Memory::page_size - offset_ % Memory::page_size,
                          input_storage::record_size - offset_);
        bool started{};
        if ((stage_ == Stage::first) || (stage_ == Stage::second) || (stage_ == Stage::verify))
            started = Memory::read(address, std::span{record_}.subspan(offset_, count_));
        else if ((stage_ == Stage::invalidate) || (stage_ == Stage::invalidate_other))
            started = Memory::write(address, invalid_);
        else if (stage_ == Stage::commit)
            started = Memory::write(address, std::span{record_}.first(1));
        else
            started = Memory::write(address, std::span{record_}.subspan(offset_, count_));
        if (!started) {
            writable_ = false;
            finish(RadioStorageStatus::io_error);
        } else
            pending_ = true;
    }

  private:
    enum class Stage : std::uint8_t {
        wait,
        first,
        second,
        ready,
        invalidate_other,
        invalidate,
        body,
        commit,
        verify
    };
    constexpr void finish(const RadioStorageStatus status) noexcept {
        status_ = status;
        stage_ = Stage::ready;
        loaded_ = true;
    }
    constexpr void load(const input_storage::Decoded second) noexcept {
        if ((first_.status == RadioStorageStatus::version) ||
            (second.status == RadioStorageStatus::version)) {
            finish(RadioStorageStatus::version);
            return;
        }
        if (!first_.valid() && !second.valid()) {
            writable_ = (first_.status == RadioStorageStatus::empty) &&
                        (second.status == RadioStorageStatus::empty);
            finish(writable_ ? RadioStorageStatus::empty : RadioStorageStatus::invalid);
            return;
        }
        if (first_.valid() && second.valid() &&
            (((second.generation - first_.generation) == 0x80000000U) ||
             ((first_.generation == second.generation) && (first_.setup != second.setup)))) {
            finish(RadioStorageStatus::invalid);
            return;
        }
        auto selected = first_;
        if (!first_.valid() || (second.valid() && (second.generation != first_.generation) &&
                                ((second.generation - first_.generation) < 0x80000000U))) {
            selected = second;
            active_ = 1;
        }
        setup_ = selected.setup;
        generation_ = selected.generation;
        has_active_ = true;
        writable_ = true;
        finish(RadioStorageStatus::ready);
    }
    InputSetup setup_{};
    InputSetup pending_setup_{};
    input_storage::Record record_{};
    input_storage::Decoded first_{};
    inline static constexpr std::array<std::uint8_t, 1> invalid_{0};
    std::uint32_t generation_{};
    std::size_t active_{};
    std::size_t target_{};
    std::size_t offset_{};
    std::size_t count_{};
    Stage stage_{};
    RadioStorageStatus status_{RadioStorageStatus::loading};
    bool pending_{};
    bool loaded_{};
    bool writable_{};
    bool dirty_{};
    bool has_active_{};
};

} // namespace coretx::firmware
