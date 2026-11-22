// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/i6s/touch.hpp"
#include "coretx/ui/translation.hpp"

namespace coretx::firmware::i6s {

[[nodiscard]] constexpr const char *error_message(const TouchError error) noexcept {
    namespace tr = coretx::ui::tr;
    switch (error) {
    case TouchError::none:
        return nullptr;
    case TouchError::not_initialized:
        return tr::not_initialized;
    case TouchError::timeout:
        return tr::timeout;
    case TouchError::nack:
        return tr::no_ack;
    case TouchError::bus:
        return tr::bus_error;
    case TouchError::arbitration:
        return tr::arbitration;
    case TouchError::overrun:
        return tr::overrun;
    case TouchError::packet:
        return tr::touch_packet_error;
    }
    return tr::unknown;
}

} // namespace coretx::firmware::i6s
