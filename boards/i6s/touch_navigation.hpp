// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "coretx/ui/input.hpp"
#include "coretx/utility/time.hpp"

#include <algorithm>
#include <array>

namespace coretx::firmware::i6s {

enum class Gesture : std::uint8_t { none, up, right, down, left };

struct TouchSample {
    std::uint16_t raw_x{};
    std::uint16_t raw_y{};
    std::uint8_t x{};
    std::uint8_t y{};
    std::uint8_t contacts{};
    Gesture gesture{};
};

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
    if (sample.contacts == 0) {
        return true;
    }
    const auto event = bytes[3] >> 6;
    if (event == 3) {
        return false;
    }
    sample.raw_x = static_cast<std::uint16_t>(((bytes[3] & 0x0FU) << 8) | bytes[4]);
    sample.raw_y = static_cast<std::uint16_t>(((bytes[5] & 0x0FU) << 8) | bytes[6]);
    sample.y = static_cast<std::uint8_t>(std::min<unsigned>(sample.raw_x / 2U, 63U));
    const auto scaled = sample.raw_y / 2U;
    sample.x = static_cast<std::uint8_t>(scaled >= 128U ? 0U : std::min(128U - scaled, 127U));
    if ((event == 1) && (sample.contacts == 1)) {
        sample.contacts = 0;
    }
    return true;
}

class TouchNavigation {
  public:
    inline static constexpr DurationUs release_timeout_us{40'000};

    [[nodiscard]] constexpr Key service(const TimestampUs now, const TouchSample *sample,
                                        const bool healthy) noexcept {
        if (!healthy || (active_ && ((now - last_packet_) >= release_timeout_us))) {
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
        Key key = Key::none;
        if (active_ && !emitted_) {
            switch (sample->gesture) {
            case Gesture::up:
                key = Key::up;
                break;
            case Gesture::right:
                key = Key::ok;
                break;
            case Gesture::down:
                key = Key::down;
                break;
            case Gesture::left:
                key = Key::cancel;
                break;
            case Gesture::none:
                break;
            }
        }
        if (sample->contacts == 0) {
            active_ = false;
            return key;
        }
        if (!active_) {
            active_ = true;
            emitted_ = false;
            start_x_ = sample->x;
            start_y_ = sample->y;
            pressed_at_ = now;
        } else if (!emitted_ && (key == Key::none)) {
            const int dx = int{sample->x} - start_x_;
            const int dy = int{sample->y} - start_y_;
            const int ax = dx < 0 ? -dx : dx;
            const int ay = dy < 0 ? -dy : dy;
            if ((ax >= 12) && (ax >= (ay + 4))) {
                key = dx > 0 ? Key::ok : Key::cancel;
            } else if ((ay >= 12) && (ay >= (ax + 4))) {
                key = dy > 0 ? Key::down : Key::up;
            }
        }
        if (key != Key::none) {
            emitted_ = true;
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
};

} // namespace coretx::firmware::i6s
