// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/i6s/hardware.hpp"
#include "boards/i6x/hardware.hpp"

#include <type_traits>

namespace coretx::firmware {

using Hardware = std::conditional_t<features::i6s, i6s::Hardware, i6x::Hardware>;

} // namespace coretx::firmware
