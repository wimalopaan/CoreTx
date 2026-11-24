// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/common/model_format.hpp"

namespace coretx::firmware::model_migration {
inline constexpr std::uint16_t address = models::directory_base + 128;
inline constexpr std::size_t record_size = 64;
inline constexpr std::uint32_t magic = 0x3156474D; // MGV1.
using Record = std::array<std::uint8_t, record_size>;
enum class Kind : std::uint8_t { conversion = 1, reset = 2 };
enum class Status : std::uint8_t { empty, valid, invalid };
struct Info {
    Kind kind{};
    std::uint16_t target{};
};
static_assert((address % record_size) == 0);
static_assert(address + record_size <= 16'384);

constexpr void encode(const Kind kind, const std::uint16_t target, Record &record) noexcept {
    record.fill(0xFF);
    coretx::storage::detail::Writer out{record};
    out.word(magic);
    out.field(std::uint8_t{1});
    out.field(kind);
    out.field(target);
    coretx::storage::detail::Writer crc{std::span{record}.last(4)};
    crc.word(coretx::storage::crc32(std::span{record}.first(record_size - 4)));
}

[[nodiscard]] constexpr Status decode(const Record &record, Info &info) noexcept {
    if (std::all_of(record.begin(), record.end(), [](const auto b) { return b == 0xFF; }))
        return Status::empty;
    coretx::storage::detail::Reader in{record};
    coretx::storage::detail::Reader crc{std::span{record}.last(4)};
    std::uint32_t identifier{}, checksum{};
    std::uint8_t version{};
    in.word(identifier);
    in.field(version);
    in.field(info.kind);
    in.field(info.target);
    crc.word(checksum);
    if ((identifier != magic) || (version != 1) ||
        ((info.kind != Kind::conversion) && (info.kind != Kind::reset)) ||
        !std::all_of(record.begin() + 8, record.end() - 4,
                     [](const auto b) { return b == 0xFF; }) ||
        (checksum != coretx::storage::crc32(std::span{record}.first(record_size - 4))))
        return Status::invalid;
    return Status::valid;
}
} // namespace coretx::firmware::model_migration
