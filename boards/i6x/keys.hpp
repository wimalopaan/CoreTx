// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/i6x/hardware.hpp"
#include "coretx/ui/input.hpp"
#include "platform/stm32f072/gpio.hpp"

namespace coretx::firmware::i6x {

using coretx::firmware::Key;

namespace detail {

template <typename Io, typename Assignment = Hardware> class KeysState {
    static constexpr auto first_column = Assignment::key_columns[0];
    inline static constexpr std::uint16_t trim_mask = 0xFFU;
    inline static constexpr std::uint16_t navigation_mask = 0xF00U;
    static_assert(
        [] {
            for (std::size_t i = 0; i < Assignment::key_columns.size(); ++i)
                if ((Assignment::key_columns[i].port != first_column.port) ||
                    (Assignment::key_columns[i].index != (first_column.index + i)))
                    return false;
            return Assignment::key_columns.size() == 3;
        }(),
        "Key columns must be three consecutive pins on one port");

  public:
    constexpr void initialize() noexcept {
        *this = {};
        using namespace stm32f072::registers;
        Io::write(rcc_ahbenr, Io::read(rcc_ahbenr) | gpio_clocks(Assignment::key_columns) |
                                  gpio_clock({Assignment::key_rows_port, 0}));
        (void)Io::read(rcc_ahbenr);
        for (std::uint8_t i = first_column.index; i < (first_column.index + 3U); ++i) {
            const Pin column{first_column.port, i};
            drive<Io>(column, true);
            configure<Io>(column, false);
            // Released columns must not short a selected column through multiple keys.
            const auto otyper = column.port + offsetof(GPIO_TypeDef, OTYPER);
            Io::write(otyper, Io::read(otyper) | (std::uint32_t{GPIO_OTYPER_OT_0} << column.index));
        }
        for (std::uint8_t i = 0; i < 4; ++i) {
            configure_input<Io>({Assignment::key_rows_port,
                                 static_cast<std::uint8_t>(Assignment::key_rows_first + i)});
        }
        drive<Io>({first_column.port, first_column.index}, false);
        changed_ = Io::now_us();
        selected_at_ = changed_;
    }
    [[nodiscard]] constexpr Key service(const TimestampUs now) noexcept {
        if ((now - selected_at_) < 1'000_us) {
            return Key::none;
        }
        const auto rows = (~Io::read(Assignment::key_rows_port + offsetof(GPIO_TypeDef, IDR)) >>
                           Assignment::key_rows_first) &
                          0xFU;
        pending_ |= static_cast<std::uint16_t>(rows << (column_ * 4U));
        drive<Io>({first_column.port, static_cast<std::uint8_t>(first_column.index + column_)},
                  true);
        column_ = static_cast<std::uint8_t>((column_ + 1U) % 3U);
        drive<Io>({first_column.port, static_cast<std::uint8_t>(first_column.index + column_)},
                  false);
        selected_at_ = now;
        if (column_ != 0) {
            return Key::none;
        }
        bool ghosted{};
        const auto matrix = static_cast<unsigned>(pending_);
        for (unsigned a = 0; a < Assignment::key_columns.size(); ++a)
            for (unsigned b = a + 1; b < Assignment::key_columns.size(); ++b)
                ghosted = ghosted ||
                          (std::popcount(((matrix >> (a * 4)) & (matrix >> (b * 4))) & 0xFU) >= 2);
        const auto navigation = static_cast<std::uint16_t>(pending_ & navigation_mask);
        const bool ambiguous = ghosted || (std::popcount(navigation) > 1);
        if (ambiguous)
            suppress_release_ = true;
        const auto sample = ghosted ? std::uint16_t{} : pending_;
        pending_ = 0;
        if (sample != candidate_) {
            candidate_ = sample;
            changed_ = now;
        }
        if ((now - changed_) < 20'000_us) {
            return Key::none;
        }
        if (!ambiguous && (sample == 0) && (held_ == 0))
            suppress_release_ = false;
        const auto previous = static_cast<std::uint16_t>(held_ & navigation_mask);
        const auto current =
            ambiguous ? std::uint16_t{} : static_cast<std::uint16_t>(sample & navigation_mask);
        held_ = static_cast<std::uint16_t>((sample & trim_mask) | previous);
        if (current == previous) {
            if (!long_sent_ && ((previous == (1U << 10)) || (previous == (1U << 11))) &&
                ((now - pressed_at_) >= 800'000_us)) {
                long_sent_ = true;
                return previous == (1U << 10) ? Key::ok_long : Key::cancel_long;
            }
            if (((previous == (1U << 8)) || (previous == (1U << 9))) &&
                ((now - pressed_at_) >= 400'000_us) && ((now - repeated_at_) >= 100'000_us)) {
                repeated_at_ = now;
                return previous == (1U << 8) ? Key::down : Key::up;
            }
            return Key::none;
        }
        const auto released = previous;
        const bool short_press = !long_sent_ && !suppress_release_;
        const auto pressed = static_cast<std::uint16_t>(current & ~previous);
        held_ = static_cast<std::uint16_t>((sample & trim_mask) | current);
        pressed_at_ = repeated_at_ = now;
        long_sent_ = false;
        if ((current == 0) && !ambiguous)
            suppress_release_ = false;
        if ((current == 0) && short_press) {
            if (released == (1U << 10))
                return Key::ok;
            if (released == (1U << 11))
                return Key::cancel;
        }
        switch (pressed) {
        case std::uint16_t{1} << 8:
            return Key::down;
        case std::uint16_t{1} << 9:
            return Key::up;
        default:
            return Key::none;
        }
    }

    [[nodiscard]] constexpr bool ok_held() const noexcept {
        return ((held_ & navigation_mask) == (1U << 10)) &&
               ((candidate_ & navigation_mask) == (1U << 10)) && !suppress_release_;
    }
    [[nodiscard]] constexpr std::uint8_t trim_buttons() const noexcept {
        return static_cast<std::uint8_t>(held_ & candidate_ & trim_mask);
    }

  private:
    TimestampUs selected_at_{};
    TimestampUs changed_{};
    TimestampUs pressed_at_{};
    TimestampUs repeated_at_{};
    std::uint16_t pending_{};
    std::uint16_t candidate_{};
    std::uint16_t held_{};
    std::uint8_t column_{};
    bool long_sent_{};
    bool suppress_release_{};
};

} // namespace detail

template <typename Io, typename Assignment = Hardware> class Keys {
  public:
    Keys() = delete;

    static void initialize() noexcept { state_.initialize(); }
    [[nodiscard]] static Key service(const TimestampUs now) noexcept { return state_.service(now); }
    [[nodiscard]] static bool ok_held() noexcept { return state_.ok_held(); }
    [[nodiscard]] static std::uint8_t trim_buttons() noexcept { return state_.trim_buttons(); }

  private:
    inline static constinit detail::KeysState<Io, Assignment> state_{};
};

} // namespace coretx::firmware::i6x
