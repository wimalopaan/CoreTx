// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/common/controls.hpp"
#include "boards/common/settings_storage.hpp"
#include "coretx/storage/storage.hpp"
#include "coretx/ui/translation.hpp"

namespace coretx::firmware {
namespace calibration_storage {

// Two journaled 64-byte records directly below the model area.
inline constexpr std::uint16_t base_address = 1920;
inline constexpr std::size_t record_size = 64;
inline constexpr std::uint32_t magic = 0x314C4143; // Little-endian "CAL1".
inline constexpr std::uint32_t family_mask = 0x00FFFFFF;
using Record = std::array<std::uint8_t, record_size>;
static_assert((base_address + (2 * record_size)) <= 2048);

struct Decoded {
    ControlSetup setup{};
    std::uint32_t generation{};
    SettingsStatus status{SettingsStatus::invalid};
    [[nodiscard]] constexpr bool valid() const noexcept { return status == SettingsStatus::ready; }
};
// Layout: magic, generation, 8 x {minimum, center, maximum}, 8 mapping nibbles
// (bits 0-2 physical index, bit 3 reversed), CRC-32 over the preceding 60 bytes.
[[nodiscard]] constexpr Record encode(const ControlSetup &setup,
                                      const std::uint32_t generation) noexcept {
    Record bytes{};
    coretx::storage::detail::Writer writer{bytes};
    writer.word(magic);
    writer.word(generation);
    for (const auto &axis : setup.calibration) {
        writer.field(axis.minimum);
        writer.field(axis.center);
        writer.field(axis.maximum);
    }
    std::uint32_t mapping{};
    for (std::size_t i = 0; i < calibration_count; ++i)
        mapping |= static_cast<std::uint32_t>((setup.mapping[i].physical_index & 7U) |
                                              (setup.mapping[i].reversed ? 8U : 0U))
                   << (4U * i);
    writer.word(mapping);
    writer.word(coretx::storage::crc32(std::span{bytes}.first(record_size - 4)));
    return bytes;
}
[[nodiscard]] constexpr Decoded decode(const Record &bytes) noexcept {
    Decoded decoded{};
    if (std::all_of(bytes.begin(), bytes.end(), [](const auto value) { return value == 0xFF; })) {
        decoded.status = SettingsStatus::empty;
        return decoded;
    }
    coretx::storage::detail::Reader reader{bytes};
    std::uint32_t identifier{};
    reader.word(identifier);
    reader.word(decoded.generation);
    for (auto &axis : decoded.setup.calibration) {
        reader.field(axis.minimum);
        reader.field(axis.center);
        reader.field(axis.maximum);
    }
    std::uint32_t mapping{};
    std::uint32_t stored{};
    reader.word(mapping);
    reader.word(stored);
    if (stored != coretx::storage::crc32(std::span{bytes}.first(record_size - 4)))
        return decoded;
    if (identifier != magic) {
        if ((identifier & family_mask) == (magic & family_mask))
            decoded.status = SettingsStatus::version;
        return decoded;
    }
    for (std::size_t i = 0; i < calibration_count; ++i) {
        const auto nibble = (mapping >> (4U * i)) & 0xFU;
        decoded.setup.mapping[i] = {static_cast<std::uint8_t>(nibble & 7U), (nibble & 8U) != 0};
    }
    if (coretx::validate(decoded.setup))
        decoded.status = SettingsStatus::ready;
    return decoded;
}
struct Codec {
    using Value = ControlSetup;
    using Record = calibration_storage::Record;
    using Decoded = calibration_storage::Decoded;
    static constexpr std::uint16_t base_address = calibration_storage::base_address;
    // Damaged calibration is replaced by the next successful calibration run.
    static constexpr bool overwrite_invalid = true;
    static constexpr Record encode(const ControlSetup &setup,
                                   const std::uint32_t generation) noexcept {
        return calibration_storage::encode(setup, generation);
    }
    static constexpr Decoded decode(const Record &bytes) noexcept {
        return calibration_storage::decode(bytes);
    }
    static constexpr const ControlSetup &value(const Decoded &decoded) noexcept {
        return decoded.setup;
    }
    static constexpr bool valid(const ControlSetup &setup) noexcept {
        return Diagnostics::valid_setup(setup);
    }
};
} // namespace calibration_storage

template <typename Memory>
class CalibrationStorage : public SettingsStorage<Memory, calibration_storage::Codec> {
    using Base = SettingsStorage<Memory, calibration_storage::Codec>;

  public:
    constexpr void save_setup(const Diagnostics &diagnostics) noexcept {
        if (!Base::busy())
            (void)Base::save(diagnostics.setup());
    }
    [[nodiscard]] constexpr const char *label(const bool calibrated) const noexcept {
        const auto status = Base::status();
        if (status == SettingsStatus::loading)
            return coretx::ui::tr::load_cal;
        if (status == SettingsStatus::saving)
            return coretx::ui::tr::saving;
        if (!calibrated)
            return coretx::ui::tr::need_cal;
        return (status == SettingsStatus::saved) || (status == SettingsStatus::ready)
                   ? coretx::ui::tr::ok_cal
                   : coretx::ui::tr::ram_cal;
    }
    constexpr void handle(const Key key, Diagnostics &diagnostics) noexcept {
        if ((key == Key::ok) && Base::busy())
            return;
        const auto phase = diagnostics.phase();
        diagnostics.handle(key);
        if ((key == Key::ok) && (phase == CalibrationPhase::range) &&
            (diagnostics.phase() == CalibrationPhase::idle) && diagnostics.calibrated())
            (void)Base::save(diagnostics.setup());
    }
    constexpr void service(const TimestampUs now, const StorageProbeResult probe,
                           Diagnostics &diagnostics) noexcept {
        Base::service(now, probe);
        if (applied_ || !Base::loaded() || Base::owns_memory())
            return;
        applied_ = true;
        if ((Base::status() == SettingsStatus::ready) &&
            !diagnostics.restore_calibration(Base::value()))
            Base::reject();
    }

  private:
    bool applied_{};
};

} // namespace coretx::firmware
