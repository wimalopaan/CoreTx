// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace coretx::firmware {

enum class Key : std::uint8_t { none, down, up, ok, cancel, ok_long, cancel_long };

} // namespace coretx::firmware
