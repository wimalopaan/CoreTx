// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Historical screens remain pure renderer fixtures, never dashboard routes.

#include "boards/common/radio_errors.hpp"
#include "boards/common/radio_storage.hpp"
#include "coretx/ui/ui.hpp"
#include "platform/stm32f072/afhds2a.hpp"
#include "tests/translation.hpp"

namespace coretx::firmware {

struct RadioInfo {
    RadioOutput output{};
    RadioStorageStatus storage{};
    afhds2a::RxId receiver{};
    std::uint32_t frames{};
    const char *message{};
    bool ready{};
    bool binding{};
    bool linked{};
    afhds2a::ChannelMode channel_mode{};
};
[[nodiscard]] constexpr const char *storage_message(const RadioStorageStatus state) noexcept {
    namespace tr = coretx::ui::tr;
    switch (state) {
    case RadioStorageStatus::loading:
        return tr::radio_loading;
    case RadioStorageStatus::ready:
    case RadioStorageStatus::saved:
        return tr::radio_saved;
    case RadioStorageStatus::empty:
        return tr::radio_empty;
    case RadioStorageStatus::saving:
        return tr::radio_saving;
    case RadioStorageStatus::missing:
        return tr::radio_missing;
    case RadioStorageStatus::multiple:
        return tr::radio_multiple;
    case RadioStorageStatus::io_error:
        return tr::i2c_error;
    case RadioStorageStatus::invalid:
        return tr::radio_invalid;
    case RadioStorageStatus::version:
        return tr::radio_version;
    case RadioStorageStatus::verify:
        return tr::radio_verify;
    }
    return tr::unknown;
}
constexpr void render_receiver(const afhds2a::RxId receiver, coretx::ui::Framebuffer &display,
                               const int y) noexcept {
    display.text(0, y, "RX ID");
    constexpr std::string_view hex = "0123456789ABCDEF";
    std::array<char, 8> id{};
    for (std::size_t i = 0; i < id.size(); ++i)
        id[i] = hex[(receiver.value >> ((id.size() - i - 1) * 4)) & 15U];
    display.text(79, y, {id.data(), id.size()});
}
constexpr void render_radio(const RadioInfo &info, coretx::ui::Framebuffer &display,
                            const std::uint32_t watchdog_resets) noexcept {
    namespace tr = coretx::ui::tr;
    display.clear();
    display.text(0, 0, tr::radio_output);
    display.text(0, 10, tr::watchdog_resets);
    display.unsigned_number(127, 10, watchdog_resets);
    display.text(0, 20,
                 info.output == RadioOutput::internal   ? tr::radio_internal
                 : info.output == RadioOutput::external ? tr::radio_external
                                                        : tr::radio_off);
    display.text(0, 30, storage_message(info.storage));
    render_receiver(info.receiver, display, 40);
    if (info.message != nullptr)
        display.small_text(0, 52, info.message);
    display.small_text(0, 58, tr::radio_change);
}
constexpr void render_afhds2a(const RadioInfo &info, coretx::ui::Framebuffer &display,
                              const std::uint32_t watchdog_resets) noexcept {
    namespace tr = coretx::ui::tr;
    display.clear();
    display.text(0, 0, "AFHDS 2A");
    display.text(56, 0,
                 info.channel_mode == afhds2a::ChannelMode::extended16
                     ? tr::radio_channels_extended
                     : tr::radio_channels_standard);
    display.text(0, 10, tr::watchdog_resets);
    display.unsigned_number(127, 10, watchdog_resets);
    display.text(0, 20,
                 info.message != nullptr                   ? info.message
                 : !info.ready                             ? tr::a7105_start
                 : info.binding                            ? tr::radio_binding
                 : !afhds2a::valid_id(info.receiver.value) ? tr::radio_unbound
                 : info.output != RadioOutput::internal    ? tr::radio_off
                 : info.linked                             ? tr::live
                                                           : tr::radio_waiting_rx);
    render_receiver(info.receiver, display, 30);
    display.text(0, 40, "TX");
    display.unsigned_number(127, 40, info.frames);
    display.small_text(0, 52,
                       info.output == RadioOutput::off ? storage_message(info.storage)
                                                       : tr::radio_center_failsafe);
    display.small_text(0, 58,
                       info.output == RadioOutput::off        ? tr::radio_format
                       : info.output == RadioOutput::internal ? tr::radio_bind
                                                              : tr::radio_select_internal);
}

} // namespace coretx::firmware
