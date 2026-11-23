// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/i6s/switch_names.hpp"
#include "boards/i6x/switch_names.hpp"
#include "coretx/features.hpp"

namespace coretx::firmware {

// Names of the selected board; free of hardware headers so host-only UI code can use them.
inline constexpr const ui::SwitchNames &switch_names =
    features::i6s ? i6s::switch_names : i6x::switch_names;

} // namespace coretx::firmware
