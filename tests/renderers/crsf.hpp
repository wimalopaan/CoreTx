// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "coretx/ui/ui.hpp"
#include "tests/translation.hpp"

namespace coretx::test {

struct CrsfInfo {
    std::uint32_t baudrate{};
    std::uint32_t actual_baudrate{};
    std::uint32_t frames{};
    std::uint32_t receive_errors_per_second{};
    const char *message{coretx::ui::tr::not_initialized};
};

constexpr void render_crsf(const CrsfInfo &crsf, coretx::ui::Framebuffer &display) noexcept {
    display.text(0, 3, coretx::ui::tr::crsf_baud);
    display.text(0, 15, "BAUD");
    display.number(127, 15, static_cast<std::int32_t>(crsf.baudrate));
    display.text(0, 25, coretx::ui::tr::actual);
    display.number(127, 25, static_cast<std::int32_t>(crsf.actual_baudrate));
    display.text(0, 35, "TX");
    display.number(127, 35, static_cast<std::int32_t>(crsf.frames & 0x7FFFFFFFU));
    display.text(0, 45, coretx::ui::tr::rx_errors);
    display.number(127, 45,
                   static_cast<std::int32_t>(crsf.receive_errors_per_second & 0x7FFFFFFFU));
    display.small_text(0, 58, crsf.message);
}

} // namespace coretx::test
