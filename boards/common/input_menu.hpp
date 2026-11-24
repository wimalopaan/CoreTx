// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/common/controls.hpp"
#include "boards/common/radio_storage.hpp"
#include "boards/common/switch_names.hpp"
#include "coretx/ui/translation.hpp"
#include "coretx/ui/ui.hpp"

namespace coretx::firmware {

enum class InputMenuCommand : std::uint8_t { none, save, calibrate, recover };

class InputMenu {
  public:
    constexpr void open() noexcept {
        *this = {};
        visible_ = true;
    }
    [[nodiscard]] constexpr bool visible() const noexcept { return visible_; }
    [[nodiscard]] constexpr bool editing() const noexcept { return editing_; }
    [[nodiscard]] constexpr InputMenuCommand handle(const Key key, Diagnostics &diagnostics,
                                                    const RadioOutput output, const bool writable,
                                                    const bool busy,
                                                    const char *const storage_message,
                                                    const bool can_recover = false) noexcept {
        if (key == Key::none)
            return {};
        if ((key == Key::cancel) && confirming_) {
            confirming_ = false;
            return {};
        }
        if ((key == Key::cancel_long) || ((key == Key::cancel) && !editing_)) {
            visible_ = false;
            confirming_ = false;
            return {};
        }
        if (key == Key::cancel) {
            editing_ = false;
            return {};
        }
        notice_ = nullptr;
        if (key == Key::ok) {
            if (features::secure_ui && (output != RadioOutput::off)) {
                notice_ = coretx::ui::tr::menu_rf_off;
                return {};
            }
            if (busy || (!writable && !can_recover)) {
                notice_ = busy ? coretx::ui::tr::saving : storage_message;
                return {};
            }
        }
        if (can_recover) {
            if (key == Key::ok) {
                if (!confirming_) {
                    confirming_ = true;
                    confirm_reset_ = false;
                } else {
                    confirming_ = false;
                    if (confirm_reset_)
                        return InputMenuCommand::recover;
                }
            } else if (confirming_ && ((key == Key::up) || (key == Key::down)))
                confirm_reset_ = !confirm_reset_;
            return {};
        }
        confirming_ = false;
        if (!editing_) {
            if ((key == Key::up) && (selected_ > 0))
                --selected_;
            else if ((key == Key::down) && ((selected_ + 1) < count(diagnostics)))
                ++selected_;
            else if (key == Key::ok) {
                if ((selected_ + 1) == count(diagnostics)) {
                    visible_ = false;
                    return InputMenuCommand::calibrate;
                }
                draft_ = value(diagnostics);
                editing_ = true;
            }
        } else if ((key == Key::up) || (key == Key::down)) {
            const auto selected = entry(diagnostics, selected_);
            const int maximum = selected.kind == Kind::analog_enable ? 1
                                : selected.kind == Kind::digital
                                    ? ((selected.index == 0) || (selected.index == 2) ? 2 : 1)
                                    : 5;
            draft_ = (draft_ + (key == Key::up ? 1 : maximum)) % (maximum + 1);
        } else if (key == Key::ok) {
            auto setup = diagnostics.input_setup();
            const auto selected = entry(diagnostics, selected_);
            if (selected.kind == Kind::analog_enable)
                setup.extra_analog = draft_ != 0;
            else if (selected.kind == Kind::digital) {
                setup.set_digital_mode(selected.index, static_cast<DigitalInputMode>(draft_));
                if (draft_ == static_cast<int>(DigitalInputMode::three_position))
                    setup.set_digital_mode(selected.index + 1, DigitalInputMode::off);
            } else
                setup.modes[selected.index] = static_cast<InputMode>(draft_);
            if (!diagnostics.set_input_setup(setup)) {
                notice_ = coretx::ui::tr::bad_request;
                return {};
            }
            editing_ = false;
            return InputMenuCommand::save;
        }
        return {};
    }
    constexpr void render(const Diagnostics &diagnostics, coretx::ui::Framebuffer &display,
                          const char *const storage_message,
                          const bool can_recover = false) const noexcept {
        namespace tr = coretx::ui::tr;
        display.clear();
        display.compact(false);
        display.text(0, 0, tr::menu_radio_setup);
        display.rectangle(0, 10, 128, 1);
        if (can_recover) {
            display.text(0, 14, tr::menu_input_setup);
            display.small_text(0, 26, tr::inputs_invalid);
            if (confirming_) {
                display.text(0, 34, tr::inputs_reset_confirm);
                display.rectangle(0, 44, 128, 10);
                display.text(0, 45, confirm_reset_ ? tr::menu_yes : tr::menu_no, false);
            } else {
                display.rectangle(0, 36, 128, 10);
                display.text(0, 37, tr::inputs_reset, false);
            }
            display.small_text(0, 58, notice_ != nullptr ? notice_ : tr::inputs_reset_help);
            return;
        }
        const auto first = selected_ >= 4 ? selected_ - 3 : 0;
        for (auto i = first; (i < count(diagnostics)) && (i < (first + 4)); ++i) {
            const int y = 12 + static_cast<int>(i - first) * 10;
            const bool ink = i != selected_;
            if (!ink)
                display.rectangle(0, y - 1, 128, 10);
            const auto item = entry(diagnostics, i);
            const bool calibration = item.kind == Kind::calibration;
            const std::string_view label =
                calibration                        ? tr::menu_calibration
                : item.kind == Kind::analog_enable ? tr::menu_extra_analog
                : item.kind == Kind::digital       ? switch_names[auxiliary_count + item.index]
                                                   : auxiliary_names[item.index];
            display.text(0, y, label, ink);
            if (!calibration) {
                const int field = editing_ && (selected_ == i) ? draft_ : value(diagnostics, i);
                const std::string_view text =
                    item.kind == Kind::analog_enable ? (field == 0 ? tr::menu_no : tr::menu_yes)
                    : item.kind == Kind::digital
                        ? digital_mode_names[static_cast<std::size_t>(field)]
                        : input_mode_names[static_cast<std::size_t>(field)];
                display.text(128 - static_cast<int>(text.size()) * 6, y, text, ink);
            }
        }
        display.small_text(0, 52, tr::menu_input_setup);
        const auto selected = entry(diagnostics, selected_);
        display.small_text(0, 58,
                           notice_ != nullptr ? notice_
                           : editing_ && (selected.kind == Kind::digital) &&
                                   board::swd_input(selected.index) && (draft_ != 0)
                               ? tr::inputs_swd_warning
                           : editing_ && (selected.kind == Kind::digital) &&
                                   (draft_ == static_cast<int>(DigitalInputMode::three_position))
                               ? tr::inputs_pair_warning
                           : storage_message != nullptr ? storage_message
                                                        : tr::menu_help);
    }

  private:
    enum class Kind : std::uint8_t { analog_enable, analog, digital, calibration };
    struct Entry {
        Kind kind{};
        std::size_t index{};
    };
    [[nodiscard]] static constexpr bool digital_visible(const Diagnostics &diagnostics,
                                                        const std::size_t index) noexcept {
        if (board::external_module_input(index) && diagnostics.input_setup().external_module)
            return false;
        return ((index != 1) && (index != 3)) ||
               (diagnostics.input_setup().digital_mode(index - 1) !=
                DigitalInputMode::three_position);
    }
    [[nodiscard]] static constexpr Entry entry(const Diagnostics &diagnostics,
                                               std::size_t row) noexcept {
        if constexpr (features::extended_inputs) {
            if (row == 0)
                return {Kind::analog_enable};
            --row;
        }
        const auto analogs = 6 + (diagnostics.input_setup().extra_analog ? extra_adc_count : 0);
        if (row < analogs)
            return {Kind::analog, row};
        row -= analogs;
        if constexpr (features::extended_inputs)
            for (std::size_t i = 0; i < board::digital_inputs; ++i) {
                if (!digital_visible(diagnostics, i))
                    continue;
                if (row == 0)
                    return {Kind::digital, i};
                --row;
            }
        return {Kind::calibration};
    }
    [[nodiscard]] static constexpr std::size_t count(const Diagnostics &diagnostics) noexcept {
        std::size_t result = 7 + (diagnostics.input_setup().extra_analog ? extra_adc_count : 0);
        if constexpr (features::extended_inputs) {
            ++result;
            for (std::size_t i = 0; i < board::digital_inputs; ++i)
                result += digital_visible(diagnostics, i) ? 1U : 0U;
        }
        return result;
    }
    [[nodiscard]] static constexpr int value(const Diagnostics &diagnostics,
                                             const std::size_t row) noexcept {
        const auto item = entry(diagnostics, row);
        return item.kind == Kind::analog_enable ? diagnostics.input_setup().extra_analog
               : item.kind == Kind::digital
                   ? static_cast<int>(diagnostics.input_setup().digital_mode(item.index))
                   : static_cast<int>(diagnostics.input_setup().modes[item.index]);
    }
    [[nodiscard]] constexpr int value(const Diagnostics &diagnostics) const noexcept {
        return value(diagnostics, selected_);
    }
    std::size_t selected_{};
    int draft_{};
    const char *notice_{};
    bool visible_{};
    bool editing_{};
    bool confirming_{};
    bool confirm_reset_{};
};

} // namespace coretx::firmware
