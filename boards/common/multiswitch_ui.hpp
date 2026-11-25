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
    MultiSwitchLayout layout{MultiSwitchLayout::rows2_columns4};
};

struct MultiSwitchRect {
    int left, top, width, height;
};

[[nodiscard]] constexpr MultiSwitchRect multiswitch_rect(const MultiSwitchLayout layout,
                                                         const std::size_t i) noexcept {
    const std::size_t columns = layout == MultiSwitchLayout::rows2_columns4 ? 4 : 2;
    const int width = columns == 4 ? 30 : 62;
    const int height = layout == MultiSwitchLayout::rows1_columns2 ? 35 : 17;
    return {1 + static_cast<int>(i % columns) * (width + 2),
            14 + static_cast<int>(i / columns) * 18, width, height};
}

[[nodiscard]] constexpr std::optional<std::size_t>
multiswitch_at(const int x, const int y,
               const MultiSwitchLayout layout = MultiSwitchLayout::rows2_columns4) noexcept {
    for (std::size_t i = 0; i < multiswitch_buttons(layout); ++i) {
        const auto rect = multiswitch_rect(layout, i);
        if ((x >= rect.left) && (x < rect.left + rect.width) && (y >= rect.top) &&
            (y < rect.top + rect.height))
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
    for (std::size_t i = 0; i < multiswitch_buttons(page.layout); ++i) {
        const auto [left, top, width, height] = multiswitch_rect(page.layout, i);
        const bool on = (switches & (1U << i)) != 0;
        display.rectangle(left, top, width, height);
        if (!on) {
            display.rectangle(left + 1, top + 1, width - 2, height - 2, false);
        }
        if ((page.momentary & (1U << i)) != 0) {
            display.rectangle(left + 2, top + 2, width - 4, height - 4, !on);
            display.rectangle(left + 3, top + 3, width - 6, height - 6, on);
        }
        std::array<char, 6> buffer{};
        const auto label = multiswitch_label(page.labels[i], buffer, i);
        const auto condition = page.controls[i].condition();
        const int offset = (height - 17) / 2;
        display.text(left + (width - static_cast<int>(label.size()) * 4) / 2,
                     top + offset + (condition.enabled ? 3 : 4), label, !on);
        if (condition.enabled) {
            const auto source = switch_condition_label(condition, buffer);
            display.text(left + (width - static_cast<int>(source.size()) * 4) / 2, top + offset + 9,
                         source, !on);
        } else if (selected == i)
            display.rectangle(left + 4, top + offset + 12, width - 8, 1, !on);
    }
    display.compact(false);
}

constexpr void highlight_multiswitch(const MultiSwitchLayout layout, const std::size_t index,
                                     coretx::ui::Framebuffer &display) noexcept {
    const auto rect = multiswitch_rect(layout, index);
    display.invert(rect.left, rect.top, rect.width, rect.height);
}

} // namespace coretx::firmware
