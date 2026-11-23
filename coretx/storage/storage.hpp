// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "coretx/core/controls.hpp"

#include <span>

namespace coretx::storage {

inline constexpr std::size_t slot_size = 1024;
inline constexpr std::size_t header_size = 16;
inline constexpr std::uint16_t oldest_format_version = 6;
inline constexpr std::uint16_t format_version = 7;
using Record = std::array<std::uint8_t, slot_size>;

struct State {
    Model model{};
    ControlSetup setup{};
};

enum class Error : std::uint8_t {
    none,
    empty,
    truncated,
    magic,
    version,
    obsolete,
    length,
    checksum,
    field,
    model,
    setup,
    no_valid_slot,
    generation
};

struct Status {
    Error code{Error::none};
    std::size_t offset{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return code == Error::none; }
};

struct SlotInfo {
    Status status{};
    std::uint32_t generation{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return static_cast<bool>(status);
    }
};

struct Selection {
    Status status{};
    std::array<Status, 2> slots{};
    std::uint8_t slot{};
    std::uint32_t generation{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return static_cast<bool>(status);
    }
};

[[nodiscard]] constexpr Status validate(const State &state) noexcept;
[[nodiscard]] constexpr std::uint32_t crc32(const std::span<const std::uint8_t> &data) noexcept;
[[nodiscard]] constexpr Status encode(const State &state, std::uint32_t generation,
                                      Record &record) noexcept;
// Failed decoding leaves state unchanged.
[[nodiscard]] constexpr SlotInfo decode(const std::span<const std::uint8_t> &record,
                                        State &state) noexcept;
[[nodiscard]] constexpr SlotInfo inspect(const std::span<const std::uint8_t> &record) noexcept;
[[nodiscard]] constexpr Selection select(const std::span<const std::uint8_t> &first,
                                         const std::span<const std::uint8_t> &second) noexcept;
// Scratch is overwritten even on failure; success leaves the selected state.
[[nodiscard]] constexpr Selection select_into(const std::span<const std::uint8_t> &first,
                                              const std::span<const std::uint8_t> &second,
                                              State &scratch) noexcept;
[[nodiscard]] constexpr const char *error_message(const Error &error) noexcept;

} // namespace coretx::storage

#include "coretx/storage/detail/storage_impl.hpp"
