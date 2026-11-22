// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) EdgeTX
// Based on OpenTX, th9x, er9x and gruvin9x.
// Modified 2026-11-19 to isolate the supported arithmetic for host comparisons.
// Extracted arithmetic reference: EdgeTX revision 90e8f0c55.
// radio/src/mixer.cpp:146-202, 238-248, 293-362, 1058-1139;
// radio/src/edgetx_helpers.h:52-59 and radio/src/edgetx.h:369-382.
// The original evaluation order is deliberately retained for comparison.
#include "edgetx_reference.hpp"

#include <algorithm>
#include <cstdint>

namespace edgetx_reference {
namespace {

int divRoundClosest(const int n, const int d) noexcept {
    return ((n < 0) ^ (d < 0)) ? ((n - d / 2) / d) : ((n + d / 2) / d);
}

int calc100to256(const int x) noexcept { return divRoundClosest(x * 256, 100); }
int calc100toRESX(const int x) noexcept { return divRoundClosest(x * 1024, 100); }
int calc1000toRESX(const int x) noexcept { return divRoundClosest(x * 1024, 1000); }

unsigned int expou(const unsigned int x, unsigned int k) noexcept {
    k = static_cast<unsigned int>(calc100to256(static_cast<int>(k)));
    std::uint32_t value = x * x;
    value *= k;
    value >>= 8;
    value *= x;
    value >>= 12;
    value += (256 - k) * x + 128;
    return value >> 8;
}

int expo(int x, const int k) noexcept {
    if (k == 0) {
        return x;
    }
    const bool neg = x < 0;
    if (neg) {
        x = -x;
    }
    x = std::min(x, 1024);
    const auto magnitude = static_cast<unsigned int>(x);
    const auto y = k < 0 ? 1024 - expou(1024 - magnitude, static_cast<unsigned int>(-k))
                         : expou(magnitude, static_cast<unsigned int>(k));
    return neg ? -static_cast<int>(y) : static_cast<int>(y);
}

} // namespace

std::int32_t input(std::int32_t value, const int expo_percent, const int weight_percent,
                   const int offset_percent) noexcept {
    value = expo(value, expo_percent);
    value = divRoundClosest(value * (weight_percent * 10), 1000);
    if (offset_percent != 0) {
        value += divRoundClosest(calc100toRESX(offset_percent * 10), 10);
    }
    return value;
}

std::int32_t contribution(const std::int32_t value, const int weight_percent,
                          const int offset_percent) noexcept {
    const int weight = calc100to256(weight_percent * 10);
    auto dv = divRoundClosest(value * weight, 10);
    if (offset_percent != 0) {
        // Multiplication avoids a left shift of a negative signed offset.
        dv += divRoundClosest(calc100toRESX(offset_percent * 10), 10) * 256;
    }
    return dv;
}

std::int32_t multiply(const std::int32_t accumulator, const std::int32_t contribution) noexcept {
    // Widen only the intermediate to avoid the original 32-bit multiplication overflow.
    const std::int64_t dv = contribution >> 8;
    return static_cast<std::int32_t>((dv * accumulator) >> 10);
}

std::int16_t output(std::int32_t value, const int minimum_permille, const int maximum_permille,
                    const int subtrim_permille, const bool reverse) noexcept {
    int ofs = calc1000toRESX(subtrim_permille);
    const int lim_p = calc1000toRESX(maximum_permille);
    const int lim_n = calc1000toRESX(minimum_permille);
    if (ofs > lim_p) {
        ofs = lim_p;
    }
    if (ofs < lim_n) {
        ofs = lim_n;
    }
    value = std::clamp(value, -1024 * 256, 1024 * 256);
    if (value != 0) {
        const int tmp = value > 0 ? lim_p - ofs : -lim_n + ofs;
        value *= tmp;
        ofs += (value + (value < 0 ? (1 << 17) - 1 : (1 << 17))) >> 18;
    }
    if (ofs > lim_p) {
        ofs = lim_p;
    }
    if (ofs < lim_n) {
        ofs = lim_n;
    }
    if (reverse) {
        ofs = -ofs;
    }
    return static_cast<std::int16_t>(ofs);
}

} // namespace edgetx_reference
