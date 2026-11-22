// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/i6s/touch_errors.hpp"
#include "coretx/ui/ui.hpp"
#include "tests/translation.hpp"

namespace coretx::firmware::i6s {

[[nodiscard]] constexpr const char *key_name(const Key key) noexcept {
    namespace tr = coretx::ui::tr;
    switch (key) {
    case Key::none:
        return tr::key_none;
    case Key::down:
        return tr::key_down;
    case Key::up:
        return tr::key_up;
    case Key::ok:
        return tr::status_ok;
    case Key::cancel:
        return tr::key_cancel;
    case Key::ok_long:
    case Key::cancel_long:
        return tr::unknown;
    }
    return tr::unknown;
}

constexpr void render_touch(coretx::ui::Framebuffer &display, const TouchSample &sample,
                            const bool ready, const bool received, const TouchError error,
                            const Key last_key, const std::uint32_t actions,
                            const std::uint32_t watchdog_resets = 0) noexcept {
    namespace tr = coretx::ui::tr;
    display.clear();
    display.text(0, 0, "I6S TOUCH");
    display.text(0, 9, tr::watchdog_resets);
    display.unsigned_number(127, 9, watchdog_resets);
    const auto message = error_message(error);
    display.text(0, 18, message != nullptr ? message : ready ? tr::touch_ready : tr::touch_start);
    display.text(0, 27, tr::raw);
    display.text(0, 36, "LCD");
    for (const int y : {27, 36}) {
        display.text(24, y, "X");
        display.text(80, y, "Y");
        if (received && (error == TouchError::none)) {
            display.number(73, y, y == 27 ? sample.raw_x : sample.x);
            display.number(127, y, y == 27 ? sample.raw_y : sample.y);
        } else {
            display.text(49, y, "----");
            display.text(103, y, "----");
        }
    }
    display.text(0, 45,
                 (error != TouchError::none) || !ready ? "---"
                 : sample.contacts == 0                ? tr::touch_released
                 : sample.contacts == 1                ? tr::touch_contact
                                                       : tr::touch_multi);
    display.text(0, 54, tr::key);
    display.text(36, 54, key_name(last_key));
    display.number(127, 54, static_cast<std::int32_t>(actions % 10'000U));
}

} // namespace coretx::firmware::i6s
