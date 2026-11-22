// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>

namespace coretx::ui::tr {

template <std::size_t Columns, std::size_t Size>
[[nodiscard]] consteval const char *text(const char (&value)[Size]) noexcept {
    static_assert((Size - 1) <= Columns, "Menu text exceeds its display field");
    return value;
}

} // namespace coretx::ui::tr

#if defined(CORETX_LANGUAGE_DE)
#include "coretx/ui/translations/DE.hpp"
#else
#include "coretx/ui/translations/EN.hpp"
#endif
