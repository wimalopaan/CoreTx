// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "coretx/storage/storage.hpp"

namespace coretx::firmware::models {
inline constexpr std::size_t count = 8;
inline constexpr std::size_t record_size = 768;
inline constexpr std::uint8_t format_version = coretx::storage::format_version;
inline constexpr std::uint16_t base = 2304;
inline constexpr std::size_t memory_size = 16'384;
inline constexpr std::uint16_t directory_base = base + count * 2 * record_size;
inline constexpr std::uint32_t magic = 0x314C444D;           // MDL1.
inline constexpr std::uint32_t directory_magic = 0x31524944; // DIR1.
} // namespace coretx::firmware::models
