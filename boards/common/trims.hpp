// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "coretx/core/controls.hpp"
#include "coretx/utility/time.hpp"

#include <algorithm>

namespace coretx::firmware {

// i6X matrix bits: RH +/- , RV +/- , LV +/- , LH +/- .
[[nodiscard]] constexpr SwitchPosition trim_position(const std::uint8_t buttons,
                                                     const std::size_t physical) noexcept {
    const auto pair = (static_cast<unsigned>(buttons) >> (physical * 2)) & 3U;
    return pair == 1   ? SwitchPosition::down
           : pair == 2 ? SwitchPosition::up
                       : SwitchPosition::middle;
}

[[nodiscard]] constexpr std::size_t trim_axis(const ControlSetup &setup,
                                              const std::size_t physical) noexcept {
    for (std::size_t i = 0; i < trim_count; ++i)
        if (setup.mapping[i].physical_index == physical)
            return i;
    return trim_count;
}

class TrimKeys {
  public:
    inline static constexpr DurationUs repeat_delay{400'000};
    inline static constexpr DurationUs repeat_period{100'000};

    constexpr void reset() noexcept { *this = {}; }
    [[nodiscard]] constexpr bool update(Model &model, const ControlSetup &setup,
                                        const std::uint8_t buttons,
                                        const TimestampUs now) noexcept {
        if (modes_ != model.trim_switches) {
            ready_ = false;
            modes_ = model.trim_switches;
        }
        if (!ready_) {
            previous_ = buttons;
            ready_ = buttons == 0;
            return false;
        }
        bool repeat{};
        if (buttons != previous_)
            pressed_at_ = repeated_at_ = now;
        else if ((buttons != 0) && ((now - pressed_at_) >= repeat_delay) &&
                 ((now - repeated_at_) >= repeat_period)) {
            repeat = true;
            repeated_at_ = now;
        }
        bool changed{};
        for (std::size_t physical = 0; physical < trim_count; ++physical) {
            if ((model.trim_switches & (1U << physical)) != 0)
                continue;
            const auto position = trim_position(buttons, physical);
            const auto before = trim_position(previous_, physical);
            if ((position == SwitchPosition::middle) || (!repeat && (position == before)))
                continue;
            const auto axis = trim_axis(setup, physical);
            if (axis == trim_count)
                continue;
            auto &trim = model.trims[axis];
            const auto value =
                std::clamp(trim.value() + (position == SwitchPosition::down ? 1 : -1),
                           int{TrimSteps::minimum}, int{TrimSteps::maximum});
            if (value != trim.value()) {
                (void)trim.try_set(value);
                changed = true;
            }
        }
        previous_ = buttons;
        return changed;
    }

  private:
    TimestampUs pressed_at_{};
    TimestampUs repeated_at_{};
    std::uint8_t previous_{};
    std::uint8_t modes_{};
    bool ready_{};
};

} // namespace coretx::firmware
