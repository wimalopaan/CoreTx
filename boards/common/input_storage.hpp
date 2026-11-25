// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/common/hardware.hpp"
#include "boards/common/settings_storage.hpp"
#include "coretx/core/input_setup.hpp"
#include "coretx/storage/storage.hpp"
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
    SettingsStatus status{SettingsStatus::invalid};
    [[nodiscard]] constexpr bool valid() const noexcept { return status == SettingsStatus::ready; }
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
        decoded.status = SettingsStatus::empty;
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
        decoded.status = SettingsStatus::version;
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
    decoded.status = SettingsStatus::ready;
    return decoded;
}
struct Codec {
    using Value = InputSetup;
    using Record = input_storage::Record;
    using Decoded = input_storage::Decoded;
    static constexpr std::uint16_t base_address = input_storage::base_address;
    static constexpr bool overwrite_invalid = false;
    static constexpr Record encode(const InputSetup &setup,
                                   const std::uint32_t generation) noexcept {
        return input_storage::encode(setup, generation);
    }
    static constexpr Decoded decode(const Record &bytes) noexcept {
        return input_storage::decode(bytes);
    }
    static constexpr const InputSetup &value(const Decoded &decoded) noexcept {
        return decoded.setup;
    }
    static constexpr bool valid(const InputSetup &setup) noexcept {
        return valid_input_setup(setup);
    }
};
} // namespace input_storage

template <typename Memory>
class InputStorage : public SettingsStorage<Memory, input_storage::Codec> {
    using Base = SettingsStorage<Memory, input_storage::Codec>;

  public:
    constexpr explicit InputStorage(const bool sb_three_position = false) noexcept
        : Base(InputSetup{sb_three_position}) {}
    [[nodiscard]] constexpr const InputSetup &setup() const noexcept { return Base::value(); }
    [[nodiscard]] constexpr const char *message() const noexcept {
        namespace tr = coretx::ui::tr;
        switch (Base::status()) {
        case SettingsStatus::ready:
        case SettingsStatus::empty:
        case SettingsStatus::saved:
            return nullptr;
        case SettingsStatus::loading:
            return tr::inputs_loading;
        case SettingsStatus::saving:
            return tr::saving;
        case SettingsStatus::missing:
            return tr::storage_no_eeprom;
        case SettingsStatus::multiple:
            return tr::storage_multiple;
        case SettingsStatus::version:
            return tr::storage_bad_version;
        case SettingsStatus::verify:
            return tr::storage_verify_error;
        case SettingsStatus::io_error:
            return tr::inputs_io_error;
        case SettingsStatus::invalid:
            return tr::inputs_invalid;
        }
        return tr::inputs_invalid;
    }
};

} // namespace coretx::firmware
