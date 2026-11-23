// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "coretx/ui/detail/font_6x10.hpp"
#include "coretx/ui/detail/micro_font.hpp"
#include "coretx/ui/ui.hpp"

#include <algorithm>
#include <type_traits>

namespace coretx::ui {
namespace detail {

inline constexpr int header_height = 16;
inline constexpr int footer_y = 48;
inline constexpr int row_height = 10;
inline constexpr std::size_t visible_rows = 3;
inline constexpr int header_split = 56;

template <typename Value>
[[nodiscard]] constexpr const char *set_bounded(Value &target, const int value) noexcept {
    return target.try_set(value) ? nullptr : "VALUE OUT OF RANGE";
}

constexpr int touch_zone(const int x, const int y) noexcept {
    return y < header_height ? x < header_split ? 0 : 1
           : y >= footer_y   ? 2 + x / 32
                             : 6 + (y - header_height) / row_height;
}

inline constexpr std::array<std::string_view, analog_count> analog_names{
    "AIL", "ELE", "THR", "RUD", "VRA", "VRB", "SA",  "SB",
    "SC",  "SD",  "VRC", "VRD", "VRE", "VRF", "VRG", "A16"};
inline constexpr std::array<std::string_view, 11> input_fields{
    "SOURCE", "WEIGHT %", "EXPO %", "OFFSET %", "TRIM",  "SIDE",
    "INVERT", "TARGET",   "SWITCH", "SW POS",   "SW NOT"};
inline constexpr std::array<std::string_view, 10> mix_fields{
    "SOURCE", "WEIGHT %", "OFFSET %", "MODE",   "TRIM",
    "INVERT", "TARGET",   "SWITCH",   "SW POS", "SW NOT"};
inline constexpr std::array<std::string_view, 4> output_fields{"MIN 0.1%", "MAX 0.1%", "SUB 0.1%",
                                                               "REVERSE"};
inline constexpr std::array<std::string_view, 3> monitor_fields{"INPUT", "MIX", "OUTPUT"};
inline constexpr std::array<std::string_view, 5> action_names{"ADD", "DUPLICATE", "DELETE",
                                                              "MOVE UP", "MOVE DOWN"};

constexpr int extra_field(const Page &page, const std::uint8_t field) noexcept {
    return page == Page::inputs  ? static_cast<int>(field) - 7
           : page == Page::mixes ? static_cast<int>(field) - 6
                                 : -1;
}

template <typename Line, std::size_t Capacity>
constexpr std::size_t retarget(FixedVector<Line, Capacity> &lines, const std::size_t selected,
                               const int destination) noexcept {
    const auto count = lines.size();
    if (lines[selected].destination == destination) {
        return selected;
    }
    auto line = lines[selected];
    line.destination = static_cast<std::uint8_t>(destination);
    for (auto i = selected; (i + 1) < count; ++i) {
        lines[i] = lines[i + 1];
    }
    std::size_t insertion{};
    while (((insertion + 1) < count) && (lines[insertion].destination <= destination)) {
        ++insertion;
    }
    for (auto i = count - 1; i > insertion; --i) {
        lines[i] = lines[i - 1];
    }
    lines[insertion] = line;
    return insertion;
}

struct ActionResult {
    std::size_t selected{};
    const char *error{};
};

template <typename Line, std::size_t Capacity>
constexpr ActionResult edit_lines(FixedVector<Line, Capacity> &lines, const std::size_t selected,
                                  const LineAction &action) noexcept {
    const auto count = lines.size();
    if ((action == LineAction::add) || (action == LineAction::duplicate)) {
        if (count == Capacity) {
            return {selected, "LIST FULL"};
        }
        if ((action == LineAction::duplicate) && (count == 0)) {
            return {0, "NO LINE"};
        }
        Line line{};
        std::size_t insertion{};
        if (action == LineAction::duplicate) {
            line = lines[selected];
            insertion = selected + 1;
        } else {
            line.weight_percent = {};
            if constexpr (std::is_same_v<Line, InputLine>) {
                line.trim = TrimSource::none;
            }
            line.destination = count == 0 ? 0 : lines[selected].destination;
            while ((insertion < count) && (lines[insertion].destination <= line.destination)) {
                ++insertion;
            }
        }
        if (!lines.try_insert(insertion, line)) {
            return {selected, "CANNOT INSERT"};
        }
        return {insertion, nullptr};
    }
    if (count == 0) {
        return {0, "NO LINE"};
    }
    if (action == LineAction::erase) {
        if (!lines.try_erase(selected)) {
            return {selected, "CANNOT DELETE"};
        }
        return {lines.empty() ? 0 : std::min(selected, lines.size() - 1), nullptr};
    }
    if (action == LineAction::move_up) {
        if ((selected == 0) || (lines[selected - 1].destination != lines[selected].destination)) {
            return {selected, "FIRST IN TARGET"};
        }
        std::swap(lines[selected], lines[selected - 1]);
        return {selected - 1, nullptr};
    }
    if (((selected + 1) >= count) ||
        (lines[selected + 1].destination != lines[selected].destination)) {
        return {selected, "LAST IN TARGET"};
    }
    std::swap(lines[selected], lines[selected + 1]);
    return {selected + 1, nullptr};
}

constexpr std::size_t page_index(const Page &page) noexcept {
    return static_cast<std::size_t>(page);
}

constexpr int field_count(const Page &page) noexcept {
    switch (page) {
    case Page::inputs:
        return static_cast<int>(input_fields.size());
    case Page::mixes:
        return static_cast<int>(mix_fields.size());
    case Page::outputs:
        return static_cast<int>(output_fields.size());
    case Page::monitor:
        return static_cast<int>(monitor_fields.size());
    case Page::controls:
        return 0;
    }
    return 0;
}

constexpr int source_choice(const Source &source, const bool allow_inputs) noexcept {
    switch (source.kind) {
    case SourceKind::analog:
        return source.index;
    case SourceKind::switch_position:
        return static_cast<int>(analog_count) + source.index;
    case SourceKind::maximum:
        return static_cast<int>(analog_count + switch_count);
    case SourceKind::input:
        return allow_inputs ? static_cast<int>(analog_count + switch_count + 1) + source.index : 0;
    }
    return 0;
}

constexpr Source select_source(int choice, const bool inverted) noexcept {
    if (choice < static_cast<int>(analog_count)) {
        return {SourceKind::analog, static_cast<std::uint8_t>(choice), inverted};
    }
    choice -= static_cast<int>(analog_count);
    if (choice < static_cast<int>(switch_count)) {
        return {SourceKind::switch_position, static_cast<std::uint8_t>(choice), inverted};
    }
    if (choice == static_cast<int>(switch_count)) {
        return {SourceKind::maximum, 0, inverted};
    }
    return {SourceKind::input,
            static_cast<std::uint8_t>(choice - static_cast<int>(switch_count) - 1), inverted};
}

constexpr void indexed_label(Framebuffer &display, const int x, const int y,
                             const std::string_view &prefix, const std::size_t index,
                             const bool ink) noexcept {
    display.text(x, y, prefix, ink);
    const int right =
        x + (static_cast<int>(prefix.size()) + (index >= 9 ? 2 : 1)) * display.character_width();
    display.number(right, y, static_cast<std::int32_t>(index + 1), ink);
}

// Switch sources need the board's names; without them they are shown as "?".
constexpr void draw_source(Framebuffer &display, const int x, const int y, const Source &source,
                           const bool ink, const SwitchNames *const names) noexcept {
    switch (source.kind) {
    case SourceKind::analog:
        display.text(x, y, source.index < analog_count ? analog_names[source.index] : "?", ink);
        break;
    case SourceKind::input:
        indexed_label(display, x, y, "I", source.index, ink);
        break;
    case SourceKind::switch_position: {
        if ((source.index >= switch_count) &&
            (source.index < (switch_count + logical_switch_count)))
            indexed_label(display, x, y, "L", source.index - switch_count, ink);
        else
            display.text(
                x, y, names && (source.index < switch_count) ? (*names)[source.index] : "?", ink);
        break;
    }
    case SourceKind::maximum:
        display.text(x, y, "MAX", ink);
        break;
    }
}

constexpr void control_label(Framebuffer &display, const int y, const std::size_t index,
                             const bool ink, const std::size_t analogs,
                             const SwitchNames *const names) noexcept {
    if (index < analogs) {
        display.text(1, y, analog_names[index], ink);
    } else if (index < (analogs + switch_count)) {
        draw_source(
            display, 1, y,
            {SourceKind::switch_position, static_cast<std::uint8_t>(index - analogs), false}, ink,
            names);
    } else {
        indexed_label(display, 1, y, "TRM", index - analogs - switch_count, ink);
    }
}

constexpr void bar(Framebuffer &display, const int y, const std::int32_t value, const int range,
                   const bool ink) noexcept {
    constexpr int center = 52;
    const int amount =
        static_cast<int>(std::clamp<std::int32_t>(value, -range, range) * 20 / range);
    display.rectangle(center, y + 1, 1, 5, ink);
    display.rectangle(amount < 0 ? center + amount : center, y + 2,
                      amount < 0 ? -amount + 1 : amount + 1, 3, ink);
}

constexpr void draw_trim(Framebuffer &display, const int y, const TrimSource &source,
                         const bool ink) noexcept {
    if (source == TrimSource::automatic) {
        display.text(98, y, "AUTO", ink);
    } else if (source == TrimSource::none) {
        display.text(98, y, "NONE", ink);
    } else {
        draw_source(display, 98, y,
                    {SourceKind::analog,
                     static_cast<std::uint8_t>(static_cast<unsigned>(source) -
                                               static_cast<unsigned>(TrimSource::stick1))},
                    ink, nullptr);
    }
}

} // namespace detail

constexpr void Framebuffer::clip_page(const std::uint8_t page) noexcept {
    clipped_page_ = std::min<std::uint8_t>(page, height / 8 - 1);
}

constexpr void Framebuffer::clear() noexcept {
    if (clipped_page_ == (height / 8))
        pixels_.fill(0);
    else
        std::fill_n(pixels_.begin() + static_cast<std::size_t>(clipped_page_) * width, width, 0);
}

constexpr void Framebuffer::pixel(const int x, const int y, const bool ink) noexcept {
    if ((x < 0) || (y < 0) || (x >= width) || (y >= height)) {
        return;
    }
    if ((clipped_page_ != (height / 8)) && ((y / 8) != clipped_page_))
        return;
    auto &byte = pixels_[static_cast<std::size_t>(x + (y / 8) * width)];
    const auto mask = static_cast<std::uint8_t>(1U << (y % 8));
    byte = ink ? static_cast<std::uint8_t>(byte | mask)
               : static_cast<std::uint8_t>(byte & static_cast<std::uint8_t>(~mask));
}

constexpr bool Framebuffer::pixel_at(const int x, const int y) const noexcept {
    if ((x < 0) || (y < 0) || (x >= width) || (y >= height)) {
        return false;
    }
    return (pixels_[static_cast<std::size_t>(x + (y / 8) * width)] & (1U << (y % 8))) != 0;
}

constexpr void Framebuffer::rectangle(const int x, const int y, const int w, const int h,
                                      const bool ink) noexcept {
    fill(x, y, w, h, ink ? Fill::set : Fill::clear);
}

constexpr void Framebuffer::invert(const int x, const int y, const int w, const int h) noexcept {
    fill(x, y, w, h, Fill::invert);
}

constexpr void Framebuffer::fill(const int x, const int y, const int w, const int h,
                                 const Fill mode) noexcept {
    if ((w <= 0) || (h <= 0) || (x >= width) || (y >= height))
        return;
    const int left = std::max(x, 0);
    const int clip_top = clipped_page_ == (height / 8) ? 0 : clipped_page_ * 8;
    const int clip_bottom = clipped_page_ == (height / 8) ? height : clip_top + 8;
    const int top = std::max(y, clip_top);
    const int right = x >= (width - w) ? width : x + w;
    const int bottom = std::min(y >= (height - h) ? height : y + h, clip_bottom);
    if ((right <= left) || (bottom <= top))
        return;
    for (int page = top / 8; page <= ((bottom - 1) / 8); ++page) {
        const auto first = static_cast<unsigned>(std::max(top - page * 8, 0));
        const auto last = static_cast<unsigned>(std::min(bottom - page * 8, 8));
        const auto mask = static_cast<std::uint8_t>((0xFFU << first) & (0xFFU >> (8U - last)));
        for (int column = left; column < right; ++column) {
            auto &byte = pixels_[static_cast<std::size_t>(page * width + column)];
            byte = mode == Fill::set     ? static_cast<std::uint8_t>(byte | mask)
                   : mode == Fill::clear ? static_cast<std::uint8_t>(byte & ~mask)
                                         : static_cast<std::uint8_t>(byte ^ mask);
        }
    }
}

constexpr void Framebuffer::text(int x, const int y, const std::string_view &value,
                                 const bool ink) noexcept {
    if ((y <= -(compact_ ? 6 : 9)) || (y >= height))
        return;
    if ((clipped_page_ != (height / 8)) &&
        ((y >= ((clipped_page_ + 1) * 8)) || (y <= (clipped_page_ * 8 - (compact_ ? 6 : 9)))))
        return;
    const auto page = static_cast<std::size_t>(std::max(y, 0) / 8);
    const auto shift = static_cast<unsigned>(std::max(y, 0) % 8);
    const auto clipped = static_cast<unsigned>(std::max(-y, 0));
    const auto paint = [&](const auto &columns) constexpr noexcept {
        for (int column = 0; column < static_cast<int>(columns.size()); ++column) {
            if (((x + column) < 0) || ((x + column) >= width))
                continue;
            const auto bits = static_cast<std::uint16_t>(
                (columns[static_cast<std::size_t>(column)] >> clipped) << shift);
            if (bits == 0)
                continue;
            const auto offset = page * width + static_cast<std::size_t>(x + column);
            const auto low = static_cast<std::uint8_t>(bits);
            if ((clipped_page_ == (height / 8)) || (page == clipped_page_))
                pixels_[offset] = ink ? static_cast<std::uint8_t>(pixels_[offset] | low)
                                      : static_cast<std::uint8_t>(pixels_[offset] & ~low);
            const auto high = static_cast<std::uint8_t>(bits >> 8);
            if ((high != 0) && ((page + 1) < (height / 8)) &&
                ((clipped_page_ == (height / 8)) || ((page + 1) == clipped_page_)))
                pixels_[offset + width] =
                    ink ? static_cast<std::uint8_t>(pixels_[offset + width] | high)
                        : static_cast<std::uint8_t>(pixels_[offset + width] & ~high);
        }
    };
    for (const char ch : value) {
        if (x >= width)
            break;
        if (compact_)
            paint(detail::micro_glyph(ch));
        else
            paint(detail::glyph_columns(ch));
        x += character_width();
    }
}

constexpr void Framebuffer::number(const int right, const int y, const std::int32_t value,
                                   const bool ink) noexcept {
    const auto magnitude =
        value < 0 ? 0U - static_cast<std::uint32_t>(value) : static_cast<std::uint32_t>(value);
    render_number(right, y, magnitude, value < 0, ink);
}

constexpr void Framebuffer::unsigned_number(const int right, const int y, const std::uint32_t value,
                                            const bool ink) noexcept {
    render_number(right, y, value, false, ink);
}

constexpr void Framebuffer::render_number(const int right, const int y, std::uint32_t magnitude,
                                          const bool negative, const bool ink) noexcept {
    char buffer[12]{};
    std::size_t start = sizeof(buffer);
    do {
        buffer[--start] = static_cast<char>('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude != 0);
    if (negative) {
        buffer[--start] = '-';
    }
    const std::string_view number{buffer + start, sizeof(buffer) - start};
    text(right - static_cast<int>(number.size()) * character_width(), y, number, ink);
}

constexpr std::size_t Interface::row_count(const Model &model) const noexcept {
    switch (page_) {
    case Page::controls:
        return control_analog_count() + switch_count + trim_count;
    case Page::inputs:
        return model.inputs.size();
    case Page::mixes:
        return model.mixes.size();
    case Page::outputs:
        return channel_count;
    case Page::monitor:
        return fields_[4] == 0 ? input_count : channel_count;
    }
    return 0;
}

constexpr std::size_t Interface::row(const Model &model) const noexcept {
    const auto count = row_count(model);
    return count == 0 ? 0 : std::min<std::size_t>(rows_[detail::page_index(page_)], count - 1);
}

constexpr std::size_t Interface::first_row(const Model &model) const noexcept {
    const auto count = row_count(model);
    return std::min<std::size_t>(first_rows_[detail::page_index(page_)],
                                 count > detail::visible_rows ? count - detail::visible_rows : 0);
}

constexpr void Interface::reveal_row(const Model &model) noexcept {
    auto first = first_row(model);
    const auto selected = row(model);
    if (selected < first) {
        first = selected;
    } else if (selected >= (first + detail::visible_rows)) {
        first = selected - detail::visible_rows + 1;
    }
    first_rows_[detail::page_index(page_)] = static_cast<std::uint8_t>(first);
}

constexpr int Interface::value(const Model &model, const std::uint8_t field) const noexcept {
    const auto selected = row(model);
    const int extra = detail::extra_field(page_, field);
    if (extra >= 0) {
        const auto condition = page_ == Page::inputs ? model.inputs[selected].condition
                                                     : model.mixes[selected].condition;
        switch (extra) {
        case 0:
            return editing_ && (field == selected_field()) ? pending_destination_
                   : page_ == Page::inputs                 ? model.inputs[selected].destination
                                                           : model.mixes[selected].destination;
        case 1:
            return condition.enabled ? condition.index + 1 : 0;
        case 2:
            return static_cast<int>(condition.position);
        case 3:
            return condition.inverted;
        }
    }
    if (page_ == Page::inputs) {
        const auto &line = model.inputs[selected];
        switch (field) {
        case 0:
            return detail::source_choice(line.source, false);
        case 1:
            return line.weight_percent.value();
        case 2:
            return line.expo_percent.value();
        case 3:
            return line.offset_percent.value();
        case 4:
            return static_cast<int>(line.trim);
        case 5:
            return static_cast<int>(line.side);
        case 6:
            return line.source.inverted;
        }
    } else if (page_ == Page::mixes) {
        const auto &line = model.mixes[selected];
        switch (field) {
        case 0:
            return detail::source_choice(line.source, true);
        case 1:
            return line.weight_percent.value();
        case 2:
            return line.offset_percent.value();
        case 3:
            return static_cast<int>(line.multiplex);
        case 4:
            return line.carry_trim;
        case 5:
            return line.source.inverted;
        }
    } else if (page_ == Page::outputs) {
        const auto &output = model.outputs[selected];
        switch (field) {
        case 0:
            return output.minimum_permille.value();
        case 1:
            return output.maximum_permille.value();
        case 2:
            return output.subtrim_permille.value();
        case 3:
            return output.reversed;
        }
    }
    return 0;
}

constexpr void Interface::set_value(Model &model, const int value) noexcept {
    const auto selected = row(model);
    const auto field = fields_[detail::page_index(page_)];
    const int extra = detail::extra_field(page_, field);
    if (extra >= 0) {
        auto &condition = page_ == Page::inputs ? model.inputs[selected].condition
                                                : model.mixes[selected].condition;
        switch (extra) {
        case 0:
            pending_destination_ = value;
            break;
        case 1:
            condition.enabled = value != 0;
            if (condition.enabled) {
                condition.index = static_cast<std::uint8_t>(value - 1);
                const bool six = (six_position_sources_ & (1U << condition.index)) != 0;
                condition.position =
                    condition.index >= switch_count
                        ? SwitchPosition::down
                        : static_cast<SwitchPosition>(std::clamp(
                              static_cast<int>(condition.position), six ? 2 : -1, six ? 7 : 1));
            }
            break;
        case 2:
            condition.position = static_cast<SwitchPosition>(value);
            break;
        case 3:
            condition.inverted = value != 0;
            break;
        }
        return;
    }
    if (page_ == Page::inputs) {
        auto &line = model.inputs[selected];
        switch (field) {
        case 0:
            line.source = detail::select_source(value, line.source.inverted);
            break;
        case 1:
            notice_ = detail::set_bounded(line.weight_percent, value);
            break;
        case 2:
            notice_ = detail::set_bounded(line.expo_percent, value);
            break;
        case 3:
            notice_ = detail::set_bounded(line.offset_percent, value);
            break;
        case 4:
            line.trim = static_cast<TrimSource>(value);
            break;
        case 5:
            line.side = static_cast<InputSide>(value);
            break;
        case 6:
            line.source.inverted = value != 0;
            break;
        }
    } else if (page_ == Page::mixes) {
        auto &line = model.mixes[selected];
        switch (field) {
        case 0:
            line.source = detail::select_source(value, line.source.inverted);
            break;
        case 1:
            notice_ = detail::set_bounded(line.weight_percent, value);
            break;
        case 2:
            notice_ = detail::set_bounded(line.offset_percent, value);
            break;
        case 3:
            line.multiplex = static_cast<Multiplex>(value);
            break;
        case 4:
            line.carry_trim = value != 0;
            break;
        case 5:
            line.source.inverted = value != 0;
            break;
        }
    } else if (page_ == Page::outputs) {
        auto &output = model.outputs[selected];
        switch (field) {
        case 0:
            notice_ = detail::set_bounded(output.minimum_permille, value);
            break;
        case 1:
            notice_ = detail::set_bounded(output.maximum_permille, value);
            break;
        case 2:
            notice_ = detail::set_bounded(output.subtrim_permille, value);
            break;
        case 3:
            output.reversed = value != 0;
            break;
        }
    }
}

constexpr void Interface::change(Model &model, const int direction, const bool coarse) noexcept {
    const auto field = fields_[detail::page_index(page_)];
    int minimum{};
    int maximum{1};
    int step{1};
    const int extra = detail::extra_field(page_, field);
    if (extra == 0) {
        maximum = static_cast<int>(page_ == Page::inputs ? input_count : channel_count) - 1;
    } else if (extra == 1) {
        maximum = static_cast<int>(switch_count + logical_switch_count);
    } else if (extra == 2) {
        const auto &condition = page_ == Page::inputs ? model.inputs[row(model)].condition
                                                      : model.mixes[row(model)].condition;
        const bool six = (six_position_sources_ & (1U << condition.index)) != 0;
        minimum = six ? 2 : -1;
        maximum = six ? 7 : 1;
        if (condition.index >= switch_count)
            step = 2;
    } else if (extra == 3) {
        maximum = 1;
    } else if (page_ == Page::inputs) {
        if (field == 0) {
            maximum = static_cast<int>(analog_count + switch_count);
        } else if ((field >= 1) && (field <= 3)) {
            constexpr std::array minima{InputWeight::minimum, ExpoPercent::minimum,
                                        InputOffset::minimum};
            constexpr std::array maxima{InputWeight::maximum, ExpoPercent::maximum,
                                        InputOffset::maximum};
            minimum = minima[field - 1U];
            maximum = maxima[field - 1U];
            step = coarse ? 10 : 1;
        } else if (field == 4) {
            maximum = static_cast<int>(TrimSource::stick4);
        } else if (field == 5) {
            maximum = static_cast<int>(InputSide::positive);
        }
    } else if (page_ == Page::mixes) {
        if (field == 0) {
            maximum = static_cast<int>(analog_count + switch_count + input_count);
        } else if ((field == 1) || (field == 2)) {
            minimum = field == 1 ? MixWeight::minimum : MixOffset::minimum;
            maximum = field == 1 ? MixWeight::maximum : MixOffset::maximum;
            step = coarse ? 10 : 1;
        } else if (field == 3) {
            maximum = static_cast<int>(Multiplex::replace);
        }
    } else if ((page_ == Page::outputs) && (field < 3)) {
        const auto &output = model.outputs[row(model)];
        minimum = field == 0   ? TravelPermille::minimum
                  : field == 1 ? output.minimum_permille.value()
                               : SubtrimPermille::minimum;
        maximum = field == 0   ? output.maximum_permille.value()
                  : field == 1 ? TravelPermille::maximum
                               : SubtrimPermille::maximum;
        step = coarse ? 100 : 10;
    }
    int next = std::clamp(value(model) + direction * step, minimum, maximum);
    if ((extra == 1) || (line_page() && (field == 0))) {
        for (;;) {
            bool available{};
            if (extra == 1)
                available =
                    (next == 0) ||
                    ((next > static_cast<int>(switch_count)) &&
                     (model.logical_switches[static_cast<std::size_t>(next - 1) - switch_count]
                          .function != LogicalFunction::disabled)) ||
                    ((next <= static_cast<int>(switch_count)) &&
                     ((switch_sources_ & (1U << (next - 1))) != 0));
            else {
                const auto source = detail::select_source(next, false);
                available = source.kind == SourceKind::analog
                                ? (analog_sources_ & (1U << source.index)) != 0
                            : source.kind == SourceKind::switch_position
                                ? (switch_sources_ & (1U << source.index)) != 0
                                : true;
            }
            if (available)
                break;
            next += direction;
            if ((next < minimum) || (next > maximum))
                return;
        }
    }
    set_value(model, next);
}

constexpr bool Interface::line_page() const noexcept {
    return (page_ == Page::inputs) || (page_ == Page::mixes);
}

constexpr void Interface::begin_edit(const Model &model) noexcept {
    if (line_page() && !validate(model)) {
        dialog_ = Dialog::actions;
        notice_ = "INVALID MODEL";
        return;
    }
    saved_value_ = value(model);
    pending_destination_ = saved_value_;
    if (line_page()) {
        saved_condition_ = page_ == Page::inputs ? model.inputs[row(model)].condition
                                                 : model.mixes[row(model)].condition;
    }
    editing_ = true;
}

constexpr void Interface::cancel_edit(Model &model) noexcept {
    set_value(model, saved_value_);
    if (line_page()) {
        auto &condition = page_ == Page::inputs ? model.inputs[row(model)].condition
                                                : model.mixes[row(model)].condition;
        condition = saved_condition_;
    }
    editing_ = false;
}

constexpr void Interface::commit_edit(Model &model) noexcept {
    model_changed_ = model_changed_ || (value(model) != saved_value_);
    if (detail::extra_field(page_, fields_[detail::page_index(page_)]) == 0) {
        const auto selected = row(model);
        const auto next = page_ == Page::inputs
                              ? detail::retarget(model.inputs, selected, pending_destination_)
                              : detail::retarget(model.mixes, selected, pending_destination_);
        rows_[detail::page_index(page_)] = static_cast<std::uint8_t>(next);
    }
    editing_ = false;
    reveal_row(model);
}

constexpr void Interface::perform_action(Model &model) noexcept {
    if (!validate(model)) {
        notice_ = "INVALID MODEL";
        return;
    }
    if ((action_ == LineAction::erase) && (dialog_ != Dialog::delete_line)) {
        if (row_count(model) == 0) {
            notice_ = "NO LINE";
            return;
        }
        confirm_delete_ = false;
        dialog_ = Dialog::delete_line;
        return;
    }
    const auto selected = row(model);
    const auto result = page_ == Page::inputs ? detail::edit_lines(model.inputs, selected, action_)
                                              : detail::edit_lines(model.mixes, selected, action_);
    notice_ = result.error;
    if (!notice_) {
        model_changed_ = true;
        rows_[detail::page_index(page_)] = static_cast<std::uint8_t>(result.selected);
        dialog_ = Dialog::none;
        reveal_row(model);
    }
}

constexpr void Interface::adjust_control(Controls &controls, const int direction,
                                         const bool coarse) noexcept {
    const auto selected = static_cast<std::size_t>(rows_[0]);
    const auto analogs = control_analog_count();
    if (selected < analogs) {
        const int step = coarse ? 128 : 16;
        controls.analogs[selected] = static_cast<std::int16_t>(
            std::clamp(controls.analogs[selected] + direction * step, -1024, 1024));
    } else if (selected < (analogs + switch_count)) {
        auto &position = controls.switches[selected - analogs];
        position =
            static_cast<SwitchPosition>(std::clamp(static_cast<int>(position) + direction, -1, 1));
    } else {
        auto &trim = controls.trims[selected - analogs - switch_count];
        notice_ =
            detail::set_bounded(trim, std::clamp<int>(trim.value() + direction * (coarse ? 8 : 1),
                                                      TrimSteps::minimum, TrimSteps::maximum));
    }
}

constexpr void Interface::handle(const Key &key, Model &model, Controls &controls,
                                 const bool coarse) noexcept {
    if (key == Key::none) {
        return;
    }
    pointer_active_ = false;
    const auto index = detail::page_index(page_);
    if (dialog_ != Dialog::none) {
        if ((key == Key::next_page) || (key == Key::previous_page)) {
            dialog_ = Dialog::none;
            notice_ = nullptr;
        } else {
            if ((key == Key::back) || (key == Key::menu)) {
                dialog_ = (key == Key::back) && (dialog_ == Dialog::delete_line) ? Dialog::actions
                                                                                 : Dialog::none;
                notice_ = nullptr;
            } else if (dialog_ == Dialog::actions) {
                if ((key == Key::left) || (key == Key::up) || (key == Key::right) ||
                    (key == Key::down)) {
                    const int direction = (key == Key::left) || (key == Key::up) ? -1 : 1;
                    action_ =
                        static_cast<LineAction>((static_cast<int>(action_) + direction +
                                                 static_cast<int>(detail::action_names.size())) %
                                                static_cast<int>(detail::action_names.size()));
                    notice_ = nullptr;
                } else if (key == Key::enter) {
                    perform_action(model);
                }
            } else if ((key == Key::left) || (key == Key::up)) {
                confirm_delete_ = false;
            } else if ((key == Key::right) || (key == Key::down)) {
                confirm_delete_ = true;
            } else if (key == Key::enter) {
                if (confirm_delete_) {
                    perform_action(model);
                } else {
                    dialog_ = Dialog::actions;
                }
            }
            return;
        }
    }
    if ((key == Key::back) || (key == Key::next_page) || (key == Key::previous_page)) {
        if (editing_) {
            cancel_edit(model);
        }
        if (key != Key::back) {
            page_ =
                static_cast<Page>((static_cast<int>(index) + (key == Key::next_page ? 1 : 4)) % 5);
        }
        reveal_row(model);
        return;
    }
    if (key == Key::menu) {
        if (line_page() && !editing_) {
            dialog_ = Dialog::actions;
            action_ = LineAction::add;
            notice_ = nullptr;
        }
        return;
    }
    const auto count = row_count(model);
    if (count == 0) {
        return;
    }
    rows_[index] = static_cast<std::uint8_t>(row(model));
    if (editing_) {
        if ((key == Key::left) || (key == Key::right)) {
            change(model, key == Key::right ? 1 : -1, coarse);
        } else if (key == Key::enter) {
            commit_edit(model);
        }
        return;
    }
    if ((key == Key::up) && (rows_[index] > 0)) {
        --rows_[index];
    } else if ((key == Key::down) && ((rows_[index] + 1U) < count)) {
        ++rows_[index];
    } else if ((key == Key::left) || (key == Key::right)) {
        const int direction = key == Key::right ? 1 : -1;
        if (page_ == Page::controls) {
            adjust_control(controls, direction, coarse);
        } else {
            const int fields = detail::field_count(page_);
            fields_[index] =
                static_cast<std::uint8_t>((fields_[index] + direction + fields) % fields);
            rows_[index] = static_cast<std::uint8_t>(row(model));
        }
    } else if (key == Key::enter) {
        if (page_ == Page::controls) {
            const auto selected = row(model);
            const auto analogs = control_analog_count();
            if (selected < analogs) {
                controls.analogs[selected] = selected == 2 ? -resolution : 0;
            } else if (selected < (analogs + switch_count)) {
                controls.switches[selected - analogs] = SwitchPosition::middle;
            } else {
                controls.trims[selected - analogs - switch_count] = {};
            }
        } else if (page_ != Page::monitor) {
            begin_edit(model);
        }
    }
    reveal_row(model);
}

template <typename State, typename... Context>
constexpr void Interface::handle_pointer(const PointerEvent &event, Model &model, State &controls,
                                         const bool coarse, Context &...context) noexcept {
    const auto dispatch = [&](const Key &key, const bool large = false) constexpr {
        handle(key, model, controls, context..., large);
    };
    if ((event.kind == PointerKind::cancel) || (event.x < 0) || (event.y < 0) ||
        (event.x >= Framebuffer::width) || (event.y >= Framebuffer::height)) {
        pointer_active_ = false;
        return;
    }
    if (event.kind == PointerKind::press) {
        if (!pointer_active_) {
            pointer_active_ = true;
            pointer_dragged_ = false;
            pointer_x_ = event.x;
            pointer_y_ = event.y;
            scroll_y_ = event.y;
        }
        return;
    }
    if (!pointer_active_) {
        return;
    }
    const int origin_zone = detail::touch_zone(pointer_x_, pointer_y_);
    if ((origin_zone < 6) && (detail::touch_zone(event.x, event.y) != origin_zone)) {
        pointer_active_ = false;
        return;
    }
    const int dx = event.x - pointer_x_;
    const int dy = event.y - pointer_y_;
    if ((dx <= -4) || (dx >= 4) || (dy <= -4) || (dy >= 4)) {
        pointer_dragged_ = true;
    }
    if (pointer_dragged_ && !editing_ && (dialog_ == Dialog::none) &&
        (pointer_y_ >= detail::header_height) && (pointer_y_ < detail::footer_y) &&
        (event.y >= detail::header_height) && (event.y < detail::footer_y)) {
        const int steps = (scroll_y_ - event.y) / detail::row_height;
        if (steps != 0) {
            const auto count = row_count(model);
            const int maximum =
                count > detail::visible_rows ? static_cast<int>(count - detail::visible_rows) : 0;
            const int first = std::clamp(static_cast<int>(first_row(model)) + steps, 0, maximum);
            const auto index = detail::page_index(page_);
            first_rows_[index] = static_cast<std::uint8_t>(first);
            if (count != 0) {
                rows_[index] = static_cast<std::uint8_t>(
                    std::clamp<int>(static_cast<int>(row(model)), first,
                                    std::min(first + static_cast<int>(detail::visible_rows),
                                             static_cast<int>(count)) -
                                        1));
            }
            scroll_y_ = event.y;
        }
    }
    if (event.kind != PointerKind::release) {
        return;
    }
    pointer_active_ = false;
    if (pointer_dragged_) {
        return;
    }
    if (origin_zone != detail::touch_zone(event.x, event.y)) {
        return;
    }
    if (dialog_ != Dialog::none) {
        if (event.y >= detail::footer_y) {
            constexpr std::array keys{Key::back, Key::left, Key::right, Key::enter};
            dispatch(keys[static_cast<std::size_t>(event.x / 32)]);
        }
        return;
    }
    if (event.y < detail::header_height) {
        if (event.x < detail::header_split) {
            dispatch(Key::next_page);
        } else if (!editing_ && (page_ != Page::controls)) {
            dispatch(Key::right);
        } else if constexpr (std::is_same_v<State, RawControls>) {
            if (page_ == Page::controls) {
                dispatch(Key::menu);
            }
        }
    } else if (event.y >= detail::footer_y) {
        switch (event.x / 32) {
        case 0:
            dispatch(editing_ ? Key::back : Key::previous_page);
            break;
        case 1:
            dispatch(Key::left, coarse);
            break;
        case 2:
            dispatch(Key::right, coarse);
            break;
        case 3:
            dispatch(page_ == Page::monitor     ? Key::next_page
                     : line_page() && !editing_ ? Key::menu
                                                : Key::enter);
            break;
        }
    } else if (!editing_ &&
               (event.y < (detail::header_height +
                           (detail::row_height * static_cast<int>(detail::visible_rows))))) {
        const auto selected =
            first_row(model) +
            static_cast<std::size_t>((event.y - detail::header_height) / detail::row_height);
        if (selected < row_count(model)) {
            rows_[detail::page_index(page_)] = static_cast<std::uint8_t>(selected);
            if ((pointer_x_ >= 90) && (event.x >= 90) && (page_ != Page::controls) &&
                (page_ != Page::monitor)) {
                dispatch(Key::enter);
            }
        }
    }
}

constexpr void Interface::handle(const PointerEvent &event, Model &model, Controls &controls,
                                 const bool coarse) noexcept {
    handle_pointer(event, model, controls, coarse);
}

constexpr void Interface::render(const Model &model, const Controls &controls,
                                 const Evaluation &evaluation,
                                 Framebuffer &display) const noexcept {
    display.clear();
    display.rectangle(0, 0, 128, detail::header_height);
    const auto index = detail::page_index(page_);
    const auto field = fields_[index];
    constexpr std::array<std::string_view, 5> names{"CONTROLS", "INPUT", "MIXER", "OUTPUT", "MON"};
    display.text(1, 4, names[index], false);
    display.rectangle(detail::header_split - 1, 0, 1, detail::header_height, false);
    constexpr int field_x = detail::header_split + 2;
    switch (page_) {
    case Page::inputs:
        display.text(field_x, 4, detail::input_fields[field], false);
        break;
    case Page::mixes:
        display.text(field_x, 4, detail::mix_fields[field], false);
        break;
    case Page::outputs:
        display.text(field_x, 4, detail::output_fields[field], false);
        break;
    case Page::monitor:
        display.text(field_x, 4, detail::monitor_fields[field], false);
        break;
    case Page::controls:
        break;
    }
    display.number(126, 4, static_cast<std::int32_t>(index + 1), false);
    display.rectangle(0, detail::footer_y, 128, 1);
    display.text(4, 53, "BACK");
    display.text(dialog_ == Dialog::delete_line ? 36 : 45, 53,
                 dialog_ == Dialog::delete_line ? "NO" : "-");
    display.text(dialog_ == Dialog::delete_line ? 68 : 77, 53,
                 dialog_ == Dialog::delete_line ? "YES" : "+");
    display.text(100, 53,
                 editing_ || (dialog_ != Dialog::none) ? "OK"
                 : page_ == Page::controls             ? "RST"
                 : page_ == Page::monitor              ? "PAGE"
                 : line_page()                         ? "MENU"
                                                       : "EDIT");
    for (int x = 32; x < 128; x += 32) {
        display.rectangle(x, detail::footer_y, 1, 16);
    }
    if (dialog_ != Dialog::none) {
        display.rectangle(0, 0, 128, detail::header_height);
        display.text(1, 4, dialog_ == Dialog::actions ? "LINE ACTIONS" : "DELETE LINE?", false);
        if (dialog_ == Dialog::actions) {
            display.number(126, 4, static_cast<int>(action_) + 1, false);
            display.text(4, 20, detail::action_names[static_cast<std::size_t>(action_)]);
        } else {
            display.text(4, 20, confirm_delete_ ? "YES - DELETE" : "NO - KEEP");
        }
        if (row_count(model) == 0) {
            display.text(4, 30, "EMPTY LIST");
        } else {
            detail::indexed_label(display, 4, 30, "LINE ", row(model), true);
            const auto destination = page_ == Page::inputs ? model.inputs[row(model)].destination
                                                           : model.mixes[row(model)].destination;
            detail::indexed_label(display, 76, 30, page_ == Page::inputs ? "I" : "CH", destination,
                                  true);
        }
        if (notice_) {
            display.text(1, 40, notice_);
        }
        return;
    }
    if (!evaluation) {
        display.text(1, 18, "CALCULATION ERROR");
        display.text(1, 28, stage_name(evaluation.status.stage));
        display.number(126, 28, static_cast<std::int32_t>(evaluation.status.index));
        display.text(1, 38, error_message(evaluation.status.code));
        return;
    }
    const auto selected = row(model);
    const auto first = first_row(model);
    for (std::size_t line = first;
         (line < row_count(model)) && (line < (first + detail::visible_rows)); ++line) {
        const int y =
            detail::header_height + 1 + static_cast<int>(line - first) * detail::row_height;
        const bool ink = line != selected;
        if (!ink) {
            display.rectangle(0, y - 1, 128, detail::row_height);
        }
        if (page_ == Page::controls) {
            const auto analogs = control_analog_count();
            detail::control_label(display, y, line, ink, analogs, switch_names_);
            int current{};
            int range{resolution};
            if (line < analogs) {
                current = controls.analogs[line];
            } else if (line < (analogs + switch_count)) {
                current = static_cast<int>(controls.switches[line - analogs]);
                range = 1;
            } else {
                current = controls.trims[line - analogs - switch_count].value();
                range = TrimSteps::maximum;
            }
            detail::bar(display, y, current, range, ink);
            display.number(126, y, current, ink);
        } else if (page_ == Page::monitor) {
            const auto &frame = evaluation.frame;
            const auto current = field == 0   ? frame.inputs[line]
                                 : field == 1 ? frame.mixes[line].raw() / MixValue::scale
                                              : frame.channels[line];
            detail::indexed_label(display, 1, y, field == 0 ? "I" : "CH", line, ink);
            detail::bar(display, y, current, 1536, ink);
            display.number(126, y, current, ink);
        } else if ((page_ == Page::inputs) || (page_ == Page::mixes)) {
            const bool input = page_ == Page::inputs;
            const auto destination =
                input ? model.inputs[line].destination : model.mixes[line].destination;
            const auto source = input ? model.inputs[line].source : model.mixes[line].source;
            detail::indexed_label(display, 1, y, input ? "I" : "CH", destination, ink);
            detail::draw_source(display, 31, y, source, ink, switch_names_);
            const auto condition =
                input ? model.inputs[line].condition : model.mixes[line].condition;
            const int extra = detail::extra_field(page_, field);
            if (extra == 0) {
                const auto shown = editing_ && (line == selected)
                                       ? static_cast<std::size_t>(pending_destination_)
                                       : destination;
                detail::indexed_label(display, 98, y, input ? "I" : "CH", shown, ink);
            } else if (extra == 1) {
                if (condition.enabled) {
                    detail::draw_source(display, 98, y,
                                        {SourceKind::switch_position, condition.index, false}, ink,
                                        switch_names_);
                } else {
                    display.text(104, y, "OFF", ink);
                }
            } else if (extra == 2) {
                if (condition.position >= SwitchPosition::pos1) {
                    const char position[]{
                        static_cast<char>('1' + static_cast<int>(condition.position) -
                                          static_cast<int>(SwitchPosition::pos1))};
                    display.text(98, y, {position, sizeof(position)}, ink);
                } else
                    display.text(98, y,
                                 condition.position == SwitchPosition::up       ? "UP"
                                 : condition.position == SwitchPosition::middle ? "MID"
                                                                                : "DOWN",
                                 ink);
            } else if (extra == 3) {
                display.text(104, y, condition.inverted ? "YES" : "NO", ink);
            } else if (field == 0) {
                detail::draw_source(display, 98, y, source, ink, switch_names_);
            } else if (input) {
                const auto &item = model.inputs[line];
                switch (field) {
                case 1:
                    display.number(126, y, item.weight_percent.value(), ink);
                    break;
                case 2:
                    display.number(126, y, item.expo_percent.value(), ink);
                    break;
                case 3:
                    display.number(126, y, item.offset_percent.value(), ink);
                    break;
                case 4:
                    detail::draw_trim(display, y, item.trim, ink);
                    break;
                case 5:
                    display.text(98, y,
                                 item.side == InputSide::both       ? "BOTH"
                                 : item.side == InputSide::negative ? "NEG"
                                                                    : "POS",
                                 ink);
                    break;
                case 6:
                    display.text(104, y, source.inverted ? "YES" : "NO", ink);
                    break;
                }
            } else {
                const auto &item = model.mixes[line];
                switch (field) {
                case 1:
                    display.number(126, y, item.weight_percent.value(), ink);
                    break;
                case 2:
                    display.number(126, y, item.offset_percent.value(), ink);
                    break;
                case 3:
                    display.text(98, y,
                                 item.multiplex == Multiplex::add        ? "ADD"
                                 : item.multiplex == Multiplex::multiply ? "MUL"
                                                                         : "REPL",
                                 ink);
                    break;
                case 4:
                    display.text(104, y, item.carry_trim ? "YES" : "NO", ink);
                    break;
                case 5:
                    display.text(104, y, source.inverted ? "YES" : "NO", ink);
                    break;
                }
            }
        } else {
            detail::indexed_label(display, 1, y, "CH", line, ink);
            const auto &output = model.outputs[line];
            if (field == 3) {
                display.text(104, y, output.reversed ? "YES" : "NO", ink);
            } else {
                display.number(126, y,
                               field == 0   ? output.minimum_permille.value()
                               : field == 1 ? output.maximum_permille.value()
                                            : output.subtrim_permille.value(),
                               ink);
            }
        }
    }
    if (row_count(model) == 0) {
        display.text(1, 20, "NO LINES");
    }
}

constexpr Model simulator_model() noexcept {
    Model model{};
    model.inputs.resize<8>();
    model.mixes.resize<8>();
    for (std::uint8_t i = 0; i < 8; ++i) {
        model.inputs[i].destination = i;
        model.inputs[i].source.index = i;
        model.mixes[i].destination = i;
        model.mixes[i].source.index = i;
    }
    model.inputs[0].expo_percent = ExpoPercent::constant<30>();
    model.inputs[1].expo_percent = ExpoPercent::constant<30>();
    model.inputs[6].source = {SourceKind::switch_position, 0, false};
    model.inputs[7].source = {SourceKind::maximum, 0, false};
    return model;
}

constexpr Controls simulator_controls() noexcept {
    Controls controls{};
    controls.analogs[2] = -resolution;
    return controls;
}

constexpr ControlSetup simulator_setup() noexcept {
    ControlSetup setup{};
    setup.calibration.fill({0, 2048, 4095});
    return setup;
}

constexpr RawControls simulator_raw_controls() noexcept {
    RawControls raw{};
    raw.analogs.fill(2048);
    raw.analogs[2] = 0;
    return raw;
}

} // namespace coretx::ui

#include "coretx/ui/detail/calibration_ui_impl.hpp"
