// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "coretx/radio/afhds2a.hpp"

namespace coretx::firmware {

enum class RadioOutput : std::uint8_t { off, internal, external };
struct RadioSettings {
    afhds2a::TxId transmitter{};
    afhds2a::RxId receiver{};
    RadioOutput output{};
    afhds2a::ChannelMode channel_mode{};
    friend constexpr bool operator==(const RadioSettings &, const RadioSettings &) = default;
};

} // namespace coretx::firmware
