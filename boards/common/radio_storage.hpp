// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "coretx/radio/afhds2a.hpp"
#include "coretx/storage/storage.hpp"
#include "platform/stm32f072/eeprom.hpp"

namespace coretx::firmware {

enum class RadioOutput : std::uint8_t { off, internal, external };
struct RadioSettings {
    afhds2a::TxId transmitter{};
    afhds2a::RxId receiver{};
    RadioOutput output{};
    afhds2a::ChannelMode channel_mode{};
    friend constexpr bool operator==(const RadioSettings &, const RadioSettings &) = default;
};
enum class RadioStorageStatus : std::uint8_t {
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

namespace radio_storage {
inline constexpr std::uint16_t base_address = 2048;
inline constexpr std::size_t record_size = 64;
inline constexpr std::uint32_t magic = 0x31524652; // Little-endian "RFR1".
inline constexpr std::uint8_t version = 2;
inline constexpr std::uint8_t legacy_version = 1;
using Record = std::array<std::uint8_t, record_size>;
struct Decoded {
    RadioSettings settings{};
    std::uint32_t generation{};
    RadioStorageStatus status{RadioStorageStatus::invalid};
    [[nodiscard]] constexpr bool valid() const noexcept {
        return status == RadioStorageStatus::ready;
    }
};
[[nodiscard]] constexpr Record encode(const RadioSettings settings,
                                      const std::uint32_t generation) noexcept {
    Record bytes{};
    bytes.fill(0xFF);
    coretx::storage::detail::Writer writer{bytes};
    writer.word(magic);
    writer.field(version);
    writer.field(settings.output);
    writer.field(settings.channel_mode);
    writer.field(std::uint8_t{0});
    writer.word(generation);
    writer.word(settings.transmitter.value);
    writer.word(settings.receiver.value);
    coretx::storage::detail::Writer checksum{std::span{bytes}.last(4)};
    checksum.word(coretx::storage::crc32(std::span{bytes}.first(record_size - 4)));
    return bytes;
}
[[nodiscard]] constexpr Decoded decode(const Record &bytes) noexcept {
    Decoded decoded{};
    if (std::all_of(bytes.begin(), bytes.end(), [](const auto b) { return b == 0xFF; })) {
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
    std::uint8_t record_version{};
    std::uint8_t reserved{};
    reader.word(identifier);
    reader.field(record_version);
    reader.field(decoded.settings.output);
    reader.field(decoded.settings.channel_mode);
    reader.field(reserved);
    reader.word(decoded.generation);
    reader.word(decoded.settings.transmitter.value);
    reader.word(decoded.settings.receiver.value);
    if (identifier != magic)
        return decoded;
    if ((record_version != legacy_version) && (record_version != version)) {
        decoded.status = RadioStorageStatus::version;
        return decoded;
    }
    if ((decoded.settings.output > RadioOutput::external) || (reserved != 0) ||
        (decoded.settings.channel_mode > afhds2a::ChannelMode::extended16) ||
        ((record_version == legacy_version) &&
         (decoded.settings.channel_mode != afhds2a::ChannelMode::standard14)) ||
        !afhds2a::valid_id(decoded.settings.transmitter.value) ||
        ((decoded.settings.receiver.value != 0) &&
         !afhds2a::valid_id(decoded.settings.receiver.value)) ||
        !std::all_of(bytes.begin() + 20, bytes.end() - 4, [](const auto b) { return b == 0xFF; }))
        return decoded;
    if (record_version == legacy_version)
        decoded.settings.channel_mode = afhds2a::ChannelMode::extended16;
    decoded.status = RadioStorageStatus::ready;
    return decoded;
}
} // namespace radio_storage

template <typename Memory> class RadioStorage {
  public:
    constexpr void initialize(const afhds2a::TxId transmitter) noexcept {
        *this = {};
        settings_.transmitter = transmitter;
    }
    [[nodiscard]] constexpr bool owns_memory() const noexcept {
        return (stage_ != Stage::wait) && (stage_ != Stage::ready);
    }
    [[nodiscard]] constexpr bool loaded() const noexcept { return loaded_; }
    [[nodiscard]] constexpr RadioStorageStatus status() const noexcept { return status_; }
    [[nodiscard]] constexpr const RadioSettings &settings() const noexcept { return settings_; }
    constexpr void save(const RadioSettings settings) noexcept {
        settings_ = settings;
        dirty_ = true;
    }
    constexpr void service(const TimestampUs now, const i6x::StorageProbeResult probe) noexcept {
        if (stage_ == Stage::wait) {
            if ((probe.state == i6x::StorageProbeState::idle) ||
                (probe.state == i6x::StorageProbeState::scanning))
                return;
            loaded_ = true;
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
            loaded_ = false;
            stage_ = Stage::load_first;
        }
        if (stage_ == Stage::ready) {
            if (!dirty_ || !writable_)
                return;
            target_ = has_active_ ? 1U - active_ : 0U;
            record_ = radio_storage::encode(settings_, has_active_ ? generation_ + 1U : 0U);
            pending_settings_ = settings_;
            status_ = RadioStorageStatus::saving;
            stage_ = Stage::invalidate;
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
            switch (stage_) {
            case Stage::load_first:
                first_ = radio_storage::decode(record_);
                stage_ = Stage::load_second;
                break;
            case Stage::load_second:
                load(radio_storage::decode(record_));
                return;
            case Stage::invalidate:
                stage_ = Stage::body;
                break;
            case Stage::body:
                stage_ = Stage::commit;
                break;
            case Stage::commit:
                stage_ = Stage::verify;
                break;
            case Stage::verify:
                if (record_ !=
                    radio_storage::encode(pending_settings_, has_active_ ? generation_ + 1U : 0U)) {
                    writable_ = false;
                    finish(RadioStorageStatus::verify);
                    return;
                }
                generation_ = has_active_ ? generation_ + 1U : 0U;
                active_ = target_;
                has_active_ = true;
                dirty_ = settings_ != pending_settings_;
                finish(RadioStorageStatus::saved);
                return;
            case Stage::wait:
            case Stage::ready:
                break;
            }
        }
        const auto address = static_cast<std::uint16_t>(radio_storage::base_address +
                                                        (stage_ == Stage::load_second  ? 1U
                                                         : stage_ == Stage::load_first ? 0U
                                                                                       : target_) *
                                                            radio_storage::record_size);
        bool started{};
        if ((stage_ == Stage::load_first) || (stage_ == Stage::load_second) ||
            (stage_ == Stage::verify))
            started = Memory::read(address, record_);
        else if (stage_ == Stage::invalidate)
            started = Memory::write(address, invalid_);
        else if (stage_ == Stage::body)
            started = Memory::write(static_cast<std::uint16_t>(address + 1),
                                    std::span{record_}.subspan(1));
        else if (stage_ == Stage::commit)
            started = Memory::write(address, std::span{record_}.first(1));
        if (!started) {
            writable_ = false;
            finish(RadioStorageStatus::io_error);
        } else {
            pending_ = true;
        }
    }

  private:
    enum class Stage : std::uint8_t {
        wait,
        load_first,
        load_second,
        ready,
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
    constexpr void load(const radio_storage::Decoded second) noexcept {
        if ((first_.status == RadioStorageStatus::version) ||
            (second.status == RadioStorageStatus::version)) {
            finish(RadioStorageStatus::version);
            return;
        }
        if (!first_.valid() && !second.valid()) {
            writable_ = true;
            finish((first_.status == RadioStorageStatus::empty) &&
                           (second.status == RadioStorageStatus::empty)
                       ? RadioStorageStatus::empty
                       : RadioStorageStatus::invalid);
            return;
        }
        auto selected = first_;
        if (!first_.valid() ||
            (second.valid() && ((second.generation - first_.generation) < 0x80000000U) &&
             (second.generation != first_.generation))) {
            selected = second;
            active_ = 1;
        }
        if (first_.valid() && second.valid() &&
            (((second.generation - first_.generation) == 0x80000000U) ||
             ((first_.generation == second.generation) && (first_.settings != second.settings)))) {
            finish(RadioStorageStatus::invalid);
            return;
        }
        if (selected.settings.transmitter != settings_.transmitter) {
            finish(RadioStorageStatus::invalid);
            return;
        }
        settings_ = selected.settings;
        generation_ = selected.generation;
        has_active_ = true;
        writable_ = true;
        finish(RadioStorageStatus::ready);
    }
    RadioSettings settings_{};
    RadioSettings pending_settings_{};
    radio_storage::Record record_{};
    radio_storage::Decoded first_{};
    inline static constexpr std::array<std::uint8_t, 1> invalid_{0};
    std::uint32_t generation_{};
    std::size_t active_{};
    std::size_t target_{};
    Stage stage_{};
    RadioStorageStatus status_{};
    bool pending_{};
    bool loaded_{};
    bool writable_{};
    bool dirty_{};
    bool has_active_{};
};

} // namespace coretx::firmware
