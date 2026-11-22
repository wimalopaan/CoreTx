// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "coretx/ui/translation.hpp"
#include "platform/stm32f072/afhds2a.hpp"

namespace coretx::firmware {

[[nodiscard]] constexpr const char *error_message(const A7105Error error) noexcept {
    namespace tr = coretx::ui::tr;
    switch (error) {
    case A7105Error::none:
        return nullptr;
    case A7105Error::not_initialized:
        return tr::not_initialized;
    case A7105Error::spi:
        return tr::spi_error;
    case A7105Error::timeout:
        return tr::timeout;
    case A7105Error::signature:
        return tr::a7105_signature;
    case A7105Error::verify:
        return tr::a7105_verify;
    case A7105Error::sequence:
        return tr::bad_request;
    }
    return tr::unknown;
}

[[nodiscard]] constexpr const char *error_message(const Afhds2aError error) noexcept {
    namespace tr = coretx::ui::tr;
    switch (error) {
    case Afhds2aError::none:
        return nullptr;
    case Afhds2aError::identity:
        return tr::radio_identity;
    case Afhds2aError::bus:
        return tr::spi_error;
    case Afhds2aError::calibration:
        return tr::radio_calibration;
    case Afhds2aError::preparation_timeout:
        return tr::radio_preparation_timeout;
    case Afhds2aError::tx_timeout:
        return tr::tx_timeout;
    case Afhds2aError::bind_timeout:
        return tr::bind_timeout;
    }
    return tr::unknown;
}

} // namespace coretx::firmware
