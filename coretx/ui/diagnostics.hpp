// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "coretx/ui/ui.hpp"

namespace coretx::ui {

template <std::size_t Count>
constexpr void render_adc_dashboard(const std::array<std::string_view, Count> &labels,
                                    const std::array<std::uint16_t, Count> &samples,
                                    const bool valid, const char *const error,
                                    Framebuffer &display) noexcept {
    static_assert((Count > 0) && (Count <= 4), "This dashboard page has room for four ADC inputs");
    display.clear();
    display.rectangle(0, 0, Framebuffer::width, 13);
    display.text(3, 3, "ADC RAW", false);
    if (error != nullptr) {
        display.text(3, 20, "ADC ERROR");
        display.text(3, 34, error);
        return;
    }
    for (std::size_t i = 0; i < Count; ++i) {
        const auto y = 18 + static_cast<int>(i) * 10;
        display.text(3, y, labels[i]);
        if (valid) {
            display.number(123, y, samples[i]);
        } else {
            display.text(100, y, "----");
        }
    }
}

} // namespace coretx::ui
