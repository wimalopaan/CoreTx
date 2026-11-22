// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace edgetx_reference {

std::int32_t input(std::int32_t value, int expo_percent, int weight_percent,
                   int offset_percent) noexcept;
std::int32_t contribution(std::int32_t value, int weight_percent, int offset_percent) noexcept;
std::int32_t multiply(std::int32_t accumulator, std::int32_t contribution) noexcept;
std::int16_t output(std::int32_t value, int minimum_permille, int maximum_permille,
                    int subtrim_permille, bool reverse) noexcept;

} // namespace edgetx_reference
