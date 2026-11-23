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

struct TouchPoint {
    std::uint8_t x{}, y{};
};
inline constexpr int swipe_distance = 12;

[[nodiscard]] constexpr Key swipe_key(const TouchSample &sample, const std::uint8_t x,
                                      const std::uint8_t y) noexcept {
    switch (sample.gesture) {
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
    if ((sample.contacts == 0) && !sample.position_valid)
        return Key::none;
    const int dx = int{sample.x} - x, dy = int{sample.y} - y;
    const int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    if ((ax >= swipe_distance) && (ax >= ay + 4))
        return dx > 0 ? Key::ok : Key::cancel;
    if ((ay >= swipe_distance) && (ay >= ax + 4))
        return dy > 0 ? Key::down : Key::up;
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
    [[nodiscard]] constexpr std::optional<TouchPoint> tapped() const noexcept {
        return tapped_ ? std::optional<TouchPoint>{{start_x_, start_y_}} : std::nullopt;
    }

    [[nodiscard]] constexpr Key service(const TimestampUs now, const TouchSample *sample,
                                        const bool healthy, const bool taps = false) noexcept {
        tapped_ = false;
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
            moved_ = false;
            start_x_ = sample->x;
            start_y_ = sample->y;
            pressed_at_ = now;
        }
        if (active_ && ((sample->contacts != 0) || sample->position_valid)) {
            const int dx = int{sample->x} - start_x_;
            const int dy = int{sample->y} - start_y_;
            const int ax = dx < 0 ? -dx : dx;
            const int ay = dy < 0 ? -dy : dy;
            moved_ = moved_ || (ax >= swipe_distance) || (ay >= swipe_distance);
        }
        auto key = active_ && !emitted_ ? swipe_key(*sample, start_x_, start_y_) : Key::none;
        if (taps && active_ && !emitted_ && !moved_ && (key == Key::none) &&
            (sample->contacts == 0) && (!sample->synthetic || inferred_release)) {
            tapped_ = true;
            key = Key::ok;
        }
        if (key != Key::none) {
            emitted_ = true;
        }
        if (sample->contacts == 0) {
            active_ = false;
            return key;
        }
        return key != Key::none ? key : long_press(now);
    }

  private:
    [[nodiscard]] constexpr Key long_press(const TimestampUs now) noexcept {
        if (active_ && !emitted_ && ((now - pressed_at_) >= DurationUs{800'000})) {
            emitted_ = true;
            return Key::ok_long;
        }
        return Key::none;
    }
    TimestampUs last_packet_{};
    TimestampUs pressed_at_{};
    std::uint8_t start_x_{};
    std::uint8_t start_y_{};
    bool active_{};
    bool emitted_{};
    bool blocked_{};
    bool tapped_{};
    bool moved_{};
};

} // namespace coretx::firmware::i6s
