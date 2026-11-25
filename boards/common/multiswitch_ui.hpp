// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/common/switch_names.hpp"
#include "coretx/radio/multiswitch.hpp"
#include "coretx/ui/translation.hpp"
#include "coretx/ui/ui.hpp"

namespace coretx::firmware {

inline constexpr int multiswitch_header_height = 8;

struct SenderInputContext {
    bool active{};
    std::uint8_t momentary{};
    bool taps{};
    std::int16_t vra{}, vrb{};
    bool jog_enabled{};
};

[[nodiscard]] constexpr std::optional<std::size_t> multiswitch_at(const int x,
                                                                  const int y) noexcept {
    for (std::size_t i = 0; i < multiswitch::switch_count; ++i) {
        const int left = 1 + static_cast<int>(i % 4) * 32;
        const int top = 14 + static_cast<int>(i / 4) * 18;
        if ((x >= left) && (x < (left + 30)) && (y >= top) && (y < (top + 17)))
            return i;
    }
    return std::nullopt;
}

[[nodiscard]] constexpr std::string_view multiswitch_label(const std::array<char, 6> &label,
                                                           std::array<char, 6> &buffer,
                                                           const std::size_t index) noexcept {
    for (std::size_t i = 0; i < label.size(); ++i)
        buffer[i] = label[i] == 0 ? ' ' : label[i];
    std::size_t first{}, last = buffer.size();
    while ((first < last) && (buffer[first] == ' '))
        ++first;
    while ((last > first) && (buffer[last - 1] == ' '))
        --last;
    if (first == last) {
        buffer[0] = static_cast<char>('1' + index);
        return {buffer.data(), 1};
    }
    return {buffer.data() + first, last - first};
}

[[nodiscard]] constexpr std::string_view
switch_condition_label(const Condition &condition, std::array<char, 6> &buffer) noexcept {
    return coretx::ui::detail::condition_label(condition, buffer, switch_names);
}

constexpr void render_multiswitch(const MultiSwitchPage &page, const std::size_t slot,
                                  const std::uint8_t switches,
                                  const std::optional<std::size_t> selected,
                                  coretx::ui::Framebuffer &display,
                                  const bool direct_control = false) noexcept {
    display.compact();
    const bool active = selected.has_value() || direct_control;
    if (active)
        display.rectangle(0, 0, coretx::ui::Framebuffer::width, multiswitch_header_height);
    display.text(0, 0, "MultiSwitch", !active);
    display.unsigned_number(51, 0, static_cast<std::uint32_t>(slot + 1), !active);
    display.text(68, 0, coretx::ui::tr::multiswitch_address, !active);
    display.number(127, 0, page.address, !active);
    for (std::size_t i = 0; i < multiswitch::switch_count; ++i) {
        const int left = 1 + static_cast<int>(i % 4) * 32;
        const int top = 14 + static_cast<int>(i / 4) * 18;
        const bool on = (switches & (1U << i)) != 0;
        display.rectangle(left, top, 30, 17);
        if (!on) {
            display.rectangle(left + 1, top + 1, 28, 15, false);
        }
        if ((page.momentary & (1U << i)) != 0) {
            display.rectangle(left + 2, top + 2, 26, 13, !on);
            display.rectangle(left + 3, top + 3, 24, 11, on);
        }
        std::array<char, 6> buffer{};
        const auto label = multiswitch_label(page.labels[i], buffer, i);
        const auto condition = page.controls[i].condition();
        display.text(left + (30 - static_cast<int>(label.size()) * 4) / 2,
                     top + (condition.enabled ? 3 : 4), label, !on);
        if (condition.enabled) {
            const auto source = switch_condition_label(condition, buffer);
            display.text(left + (30 - static_cast<int>(source.size()) * 4) / 2, top + 9, source,
                         !on);
        } else if (selected == i)
            display.rectangle(left + 4, top + 12, 22, 1, !on);
    }
    display.compact(false);
}

constexpr void highlight_multiswitch(const std::size_t index,
                                     coretx::ui::Framebuffer &display) noexcept {
    display.invert(1 + static_cast<int>(index % 4) * 32, 14 + static_cast<int>(index / 4) * 18, 30,
                   17);
}

} // namespace coretx::firmware
