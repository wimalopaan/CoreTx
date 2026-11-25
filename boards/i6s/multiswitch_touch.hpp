// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/common/multiswitch_ui.hpp"
#include "boards/i6s/touch_navigation.hpp"

namespace coretx::firmware {

// Touch gestures on the multiswitch page: tap or hold toggles a tile, swipes change the page.
class MultiSwitchTouch {
  public:
    // A toggle tile switches once the finger rests this long without leaving the tap slop, or on an
    // earlier lift. Swipes leave the slop sooner and therefore do not toggle the start tile.
    inline static constexpr DurationUs toggle_hold{100'000};
    constexpr void reset() noexcept {
        *this = {};
        blocked_ = true;
    }
    [[nodiscard]] constexpr Key
    service(const TimestampUs now, const i6s::TouchSample *sample, const bool healthy,
            const std::uint8_t momentary,
            const MultiSwitchLayout layout = MultiSwitchLayout::rows2_columns4) noexcept {
        if (layout_ != layout) {
            reset();
            layout_ = layout;
        }
        tapped_ = none;
        const bool expired =
            active_ && ((now - last_packet_) >= i6s::TouchNavigation::release_timeout_us);
        const bool inferred_release =
            expired && sample && sample->synthetic && (sample->contacts == 0);
        if (!healthy || (expired && !inferred_release)) {
            active_ = false;
            held_ = none;
            blocked_ = true;
        }
        if (healthy && active_ && !emitted_ && !momentary_ && !stroke_.moved &&
            ((now - stroke_.pressed_at) >= toggle_hold) && (!sample || (sample->contacts == 1))) {
            if (const auto start = tile(stroke_.start_x, stroke_.start_y); start != none) {
                if (sample)
                    stroke_.track(*sample);
                if (!stroke_.moved) {
                    tapped_ = start;
                    emitted_ = true;
                }
            }
        }
        if (!healthy || (sample == nullptr))
            return Key::none;
        const auto previous = last_packet_;
        last_packet_ = now;
        if (sample->contacts > 1) {
            active_ = false;
            held_ = none;
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
                stroke_.track(*sample);
                const bool endpoint = sample->position_valid && !sample->synthetic;
                key = stroke_.release(*sample, endpoint ? now : previous);
                if (!momentary_ && (!sample->synthetic || inferred_release) && (key == Key::none) &&
                    !stroke_.moved)
                    tapped_ = tile(stroke_.start_x, stroke_.start_y);
            }
            active_ = false;
            held_ = none;
            return key;
        }
        if (!active_) {
            active_ = true;
            emitted_ = false;
            stroke_.begin(*sample, now);
            const auto start = tile(stroke_.start_x, stroke_.start_y);
            momentary_ = (start != none) && ((momentary & (1U << start)) != 0);
            held_ = momentary_ ? start : none;
            const auto key = stroke_.drag(*sample);
            emitted_ = key != Key::none;
            if (emitted_)
                held_ = none;
            return key;
        }
        if (emitted_)
            return Key::none;
        stroke_.track(*sample);
        if (momentary_ && (held_ != tile(sample->x, sample->y)))
            held_ = none;
        auto key = stroke_.drag(*sample);
        if ((key == Key::none) && !stroke_.moved &&
            (tile(stroke_.start_x, stroke_.start_y) == none) &&
            ((now - stroke_.pressed_at) >= DurationUs{800'000}))
            key = Key::ok_long;
        if (key != Key::none) {
            emitted_ = true;
            held_ = none;
        }
        return key;
    }
    [[nodiscard]] constexpr std::optional<std::size_t> held() const noexcept {
        return index(held_);
    }
    [[nodiscard]] constexpr std::optional<std::size_t> tapped() const noexcept {
        return index(tapped_);
    }
    // A toggle tile that is touched and can still be tapped, for immediate visual feedback.
    [[nodiscard]] constexpr std::optional<std::size_t> pressed() const noexcept {
        return active_ && !emitted_ && !momentary_ && !stroke_.moved
                   ? index(tile(stroke_.start_x, stroke_.start_y))
                   : std::nullopt;
    }

  private:
    inline static constexpr std::uint8_t none = 0xFF;
    [[nodiscard]] constexpr std::uint8_t tile(const int x, const int y) const noexcept {
        const auto found = multiswitch_at(x, y, layout_);
        return found ? static_cast<std::uint8_t>(*found) : none;
    }
    [[nodiscard]] static constexpr std::optional<std::size_t>
    index(const std::uint8_t value) noexcept {
        return value != none ? std::optional<std::size_t>{value} : std::nullopt;
    }
    TimestampUs last_packet_{};
    i6s::TouchStroke stroke_{};
    std::uint8_t held_{none};
    std::uint8_t tapped_{none};
    MultiSwitchLayout layout_{MultiSwitchLayout::rows2_columns4};
    bool active_{};
    bool blocked_{true};
    bool emitted_{};
    bool momentary_{};
};

} // namespace coretx::firmware
