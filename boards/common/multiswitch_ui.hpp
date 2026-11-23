// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/i6s/touch_navigation.hpp"
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
    if (!condition.enabled)
        return {};
    std::size_t size{};
    if (condition.inverted)
        buffer[size++] = '!';
    if ((condition.index >= switch_count) &&
        (condition.index < (switch_count + logical_switch_count))) {
        buffer[size++] = 'L';
        buffer[size++] = static_cast<char>('1' + (condition.index - switch_count));
        buffer[size++] = condition.position == SwitchPosition::down ? '1' : '0';
    } else if ((condition.index < switch_count) && valid_switch_position(condition.position)) {
        const auto name = coretx::ui::detail::switch_names[condition.index];
        for (const auto character : name)
            buffer[size++] = character;
        buffer[size++] = condition.position == SwitchPosition::up     ? '^'
                         : condition.position == SwitchPosition::down ? 'v'
                         : condition.position == SwitchPosition::middle
                             ? '-'
                             : static_cast<char>('1' + static_cast<int>(condition.position) - 2);
    } else
        buffer[size++] = '?';
    return {buffer.data(), size};
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

class MultiSwitchTouch {
  public:
    constexpr void reset() noexcept {
        *this = {};
        blocked_ = true;
    }
    [[nodiscard]] constexpr Key service(const TimestampUs now, const i6s::TouchSample *sample,
                                        const bool healthy, const std::uint8_t momentary) noexcept {
        tapped_.reset();
        const bool expired =
            active_ && ((now - last_packet_) >= i6s::TouchNavigation::release_timeout_us);
        const bool inferred_release =
            expired && sample && sample->synthetic && (sample->contacts == 0);
        if (!healthy || (expired && !inferred_release)) {
            active_ = false;
            held_.reset();
            blocked_ = true;
        }
        if (!healthy || (sample == nullptr))
            return Key::none;
        last_packet_ = now;
        if (sample->contacts > 1) {
            active_ = false;
            held_.reset();
            blocked_ = true;
        }
        if (blocked_) {
            if (sample->contacts == 0)
                blocked_ = false;
            return Key::none;
        }
        if (sample->contacts == 0) {
            Key key{};
            if (active_ && !emitted_) {
                key = i6s::swipe_key(*sample, start_x_, start_y_);
                const bool moved = sample->position_valid &&
                                   ((int{sample->x} - start_x_ >= i6s::swipe_distance) ||
                                    (int{sample->x} - start_x_ <= -i6s::swipe_distance) ||
                                    (int{sample->y} - start_y_ >= i6s::swipe_distance) ||
                                    (int{sample->y} - start_y_ <= -i6s::swipe_distance));
                if (!momentary_ && (!sample->synthetic || inferred_release) && (key == Key::none) &&
                    !tap_cancelled_ && !moved) {
                    tapped_ = multiswitch_at(start_x_, start_y_);
                }
            }
            active_ = false;
            held_.reset();
            return key;
        }
        if (!active_) {
            active_ = true;
            emitted_ = false;
            tap_cancelled_ = false;
            start_x_ = sample->x;
            start_y_ = sample->y;
            pressed_at_ = now;
            held_ = multiswitch_at(start_x_, start_y_);
            momentary_ = held_ && ((momentary & (1U << *held_)) != 0);
            if (!momentary_)
                held_.reset();
            const auto key = i6s::swipe_key(*sample, start_x_, start_y_);
            emitted_ = key != Key::none;
            if (emitted_)
                held_.reset();
            return key;
        }
        if (emitted_)
            return Key::none;
        if (momentary_) {
            if (held_ != multiswitch_at(sample->x, sample->y))
                held_.reset();
        }
        const int dx = int{sample->x} - start_x_;
        const int dy = int{sample->y} - start_y_;
        auto key = i6s::swipe_key(*sample, start_x_, start_y_);
        if ((key == Key::none) && !tap_cancelled_ && !multiswitch_at(start_x_, start_y_) &&
            ((now - pressed_at_) >= DurationUs{800'000}))
            key = Key::ok_long;
        if (key != Key::none) {
            emitted_ = true;
            held_.reset();
        }
        if ((dx >= i6s::swipe_distance) || (dx <= -i6s::swipe_distance) ||
            (dy >= i6s::swipe_distance) || (dy <= -i6s::swipe_distance))
            tap_cancelled_ = true;
        return key;
    }
    [[nodiscard]] constexpr std::optional<std::size_t> held() const noexcept { return held_; }
    [[nodiscard]] constexpr std::optional<std::size_t> tapped() const noexcept { return tapped_; }

  private:
    TimestampUs last_packet_{};
    TimestampUs pressed_at_{};
    std::optional<std::size_t> held_{};
    std::optional<std::size_t> tapped_{};
    std::uint8_t start_x_{};
    std::uint8_t start_y_{};
    bool active_{};
    bool blocked_{true};
    bool emitted_{};
    bool tap_cancelled_{};
    bool momentary_{};
};

} // namespace coretx::firmware
