// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "coretx/ui/input.hpp"
#include "coretx/utility/time.hpp"

#include <algorithm>
#include <array>
#include <optional>

namespace coretx::firmware::i6s {

enum class Gesture : std::uint8_t { none, up, right, down, left };

struct TouchSample {
    std::uint16_t raw_x{};
    std::uint16_t raw_y{};
    std::uint8_t x{};
    std::uint8_t y{};
    std::uint8_t contacts{};
    Gesture gesture{};
    bool position_valid{};
    bool synthetic{};
};

// LCD pixels; the panel is twice as wide as tall, so vertical strokes need less travel.
inline constexpr int drag_x = 12, drag_y = 10;
// Quick strokes are accepted with shorter travel when one axis clearly dominates.
inline constexpr int flick_x = 8, flick_y = 7;
// Finger jitter below this travel never cancels a tap; a dominant flick still takes precedence.
inline constexpr int tap_slop = 12;
inline constexpr DurationUs flick_time{300'000};
// Header and footer rows are touch keys, as on Ferrox-RC: a tap in the header is Up, a tap in the
// footer is OK (left half) or Back (right half). Taps in the body never select anything, so list
// rows cannot be activated by accident. Contacts in these rows are distorted by the panel edge, so
// a stroke starting there stays a tap until it exceeds tap_slop.
inline constexpr int header_height = 15;
inline constexpr int footer_y = 46;
inline constexpr int footer_split = 64;

[[nodiscard]] constexpr Key gesture_key(const Gesture gesture) noexcept {
    switch (gesture) {
    case Gesture::up:
        return Key::up;
    case Gesture::right:
        return Key::ok;
    case Gesture::down:
        return Key::down;
    case Gesture::left:
        return Key::cancel;
    case Gesture::none:
        break;
    }
    return Key::none;
}

[[nodiscard]] constexpr Key direction_key(const int dx, const int dy, const int minimum_x,
                                          const int minimum_y) noexcept {
    const int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    // The dominant axis must exceed the other by half to reject diagonal strokes.
    if ((ax >= minimum_x) && (2 * ax >= 3 * ay))
        return dx > 0 ? Key::ok : Key::cancel;
    if ((ay >= minimum_y) && (2 * ay >= 3 * ax))
        return dy > 0 ? Key::down : Key::up;
    return Key::none;
}

[[nodiscard]] constexpr Key swipe_key(const TouchSample &sample, const std::uint8_t x,
                                      const std::uint8_t y) noexcept {
    if (const auto key = gesture_key(sample.gesture); key != Key::none)
        return key;
    if ((sample.contacts == 0) && !sample.position_valid)
        return Key::none;
    return direction_key(int{sample.x} - x, int{sample.y} - y, drag_x, drag_y);
}

struct TouchStroke {
    constexpr void begin(const TouchSample &sample, const TimestampUs now) noexcept {
        pressed_at = now;
        start_x = last_x = sample.x;
        start_y = last_y = sample.y;
        moved = false;
    }
    constexpr void track(const TouchSample &sample) noexcept {
        if ((sample.contacts == 0) && !sample.position_valid)
            return;
        last_x = sample.x;
        last_y = sample.y;
        const int dx = int{last_x} - start_x, dy = int{last_y} - start_y;
        moved =
            moved || (dx >= tap_slop) || (dx <= -tap_slop) || (dy >= tap_slop) || (dy <= -tap_slop);
    }
    [[nodiscard]] constexpr bool on_touch_bar() const noexcept {
        return (start_y < header_height) || (start_y >= footer_y);
    }
    [[nodiscard]] constexpr Key drag(const TouchSample &sample) const noexcept {
        return swipe_key(sample, start_x, start_y);
    }
    // Lift packets often carry no position; the last tracked point completes the stroke.
    [[nodiscard]] constexpr Key release(const TouchSample &sample,
                                        const TimestampUs ended_at) const noexcept {
        if (const auto key = gesture_key(sample.gesture); key != Key::none)
            return key;
        const bool quick = (ended_at - pressed_at) <= flick_time;
        return direction_key(int{last_x} - start_x, int{last_y} - start_y, quick ? flick_x : drag_x,
                             quick ? flick_y : drag_y);
    }
    TimestampUs pressed_at{};
    std::uint8_t start_x{}, start_y{}, last_x{}, last_y{};
    bool moved{};
};

[[nodiscard]] constexpr Key tap_key(const TouchStroke &stroke) noexcept {
    if (stroke.start_y < header_height) {
        return Key::up;
    }
    if (stroke.start_y >= footer_y) {
        return stroke.start_x < footer_split ? Key::ok : Key::cancel;
    }
    return Key::none;
}

[[nodiscard]] constexpr bool decode_touch(const std::array<std::uint8_t, 7> &bytes,
                                          TouchSample &sample) noexcept {
    if (((bytes[0] & 0x70U) != 0) || ((bytes[2] & 0x0FU) > 2)) {
        return false;
    }
    sample = {};
    sample.contacts = bytes[2] & 0x0FU;
    switch (bytes[1]) {
    case 0x10:
        sample.gesture = Gesture::up;
        break;
    case 0x14:
        sample.gesture = Gesture::right;
        break;
    case 0x18:
        sample.gesture = Gesture::down;
        break;
    case 0x1C:
        sample.gesture = Gesture::left;
        break;
    default:
        break;
    }
    const auto event = bytes[3] >> 6;
    if ((sample.contacts == 0) && (event != 1)) {
        return true;
    }
    if (event == 3) {
        return false;
    }
    sample.raw_x = static_cast<std::uint16_t>(((bytes[3] & 0x0FU) << 8) | bytes[4]);
    sample.raw_y = static_cast<std::uint16_t>(((bytes[5] & 0x0FU) << 8) | bytes[6]);
    sample.y = static_cast<std::uint8_t>(std::min<unsigned>(sample.raw_x / 2U, 63U));
    const auto scaled = sample.raw_y / 2U;
    sample.x = static_cast<std::uint8_t>(scaled >= 128U ? 0U : std::min(128U - scaled, 127U));
    sample.position_valid = true;
    if ((event == 1) && (sample.contacts == 1)) {
        sample.contacts = 0;
    }
    return true;
}

class TouchNavigation {
  public:
    // Safety deadline, not tap latency: the driver polls active contacts independently of INT.
    inline static constexpr DurationUs release_timeout_us{300'000};
    constexpr void reset() noexcept {
        *this = {};
        blocked_ = true;
    }

    [[nodiscard]] constexpr Key service(const TimestampUs now, const TouchSample *sample,
                                        const bool healthy, const bool taps = false) noexcept {
        const bool expired = active_ && ((now - last_packet_) >= release_timeout_us);
        // Consume a driver-qualified release before the stale-contact guard discards the tap.
        const bool inferred_release =
            expired && sample && sample->synthetic && (sample->contacts == 0);
        if (!healthy || (expired && !inferred_release)) {
            active_ = false;
            blocked_ = true;
        }
        if (!healthy || (sample == nullptr)) {
            return healthy ? long_press(now) : Key::none;
        }
        const auto previous = last_packet_;
        last_packet_ = now;
        if (sample->contacts > 1) {
            active_ = false;
            blocked_ = true;
            return Key::none;
        }
        if (blocked_) {
            if (sample->contacts == 0) {
                blocked_ = false;
            }
            return Key::none;
        }
        if (!active_ && (sample->contacts != 0)) {
            active_ = true;
            emitted_ = false;
            stroke_.begin(*sample, now);
        }
        if (!active_)
            return Key::none;
        stroke_.track(*sample);
        if (sample->contacts == 0) {
            active_ = false;
            if (emitted_)
                return Key::none;
            const bool endpoint = sample->position_valid && !sample->synthetic;
            const bool bar = taps && stroke_.on_touch_bar() && !stroke_.moved;
            auto key = bar ? Key::none : stroke_.release(*sample, endpoint ? now : previous);
            if (taps && (key == Key::none) && !stroke_.moved &&
                (!sample->synthetic || inferred_release)) {
                key = tap_key(stroke_);
            }
            return key;
        }
        if (!emitted_ && !(taps && stroke_.on_touch_bar() && !stroke_.moved)) {
            if (const auto key = stroke_.drag(*sample); key != Key::none) {
                emitted_ = true;
                return key;
            }
        }
        return long_press(now);
    }

  private:
    [[nodiscard]] constexpr Key long_press(const TimestampUs now) noexcept {
        if (active_ && !emitted_ && ((now - stroke_.pressed_at) >= DurationUs{800'000})) {
            emitted_ = true;
            return Key::ok_long;
        }
        return Key::none;
    }
    TimestampUs last_packet_{};
    TouchStroke stroke_{};
    bool active_{};
    bool emitted_{};
    bool blocked_{};
};

} // namespace coretx::firmware::i6s
