// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/common/controls.hpp"
#include "boards/common/radio_storage.hpp"
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
    [[nodiscard]] constexpr InputMenuCommand handle(const Key key, i6x::Diagnostics &diagnostics,
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
            const int maximum = selected_ < extension_rows ? 1 : 5;
            draft_ = (draft_ + (key == Key::up ? 1 : maximum)) % (maximum + 1);
        } else if (key == Key::ok) {
            auto setup = diagnostics.input_setup();
            if constexpr (features::extended_inputs) {
                if (selected_ == 0)
                    setup.extra_digital = draft_ != 0;
                else if (selected_ == 1)
                    setup.extra_analog = draft_ != 0;
                else
                    setup.modes[selected_ - extension_rows] = static_cast<InputMode>(draft_);
            } else
                setup.modes[selected_] = static_cast<InputMode>(draft_);
            if (!diagnostics.set_input_setup(setup)) {
                notice_ = coretx::ui::tr::bad_request;
                return {};
            }
            editing_ = false;
            return InputMenuCommand::save;
        }
        return {};
    }
    constexpr void render(const i6x::Diagnostics &diagnostics, coretx::ui::Framebuffer &display,
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
            const bool calibration = (i + 1) == count(diagnostics);
            const std::string_view label =
                calibration          ? tr::menu_calibration
                : i < extension_rows ? (i == 0 ? tr::menu_extra_digital : tr::menu_extra_analog)
                                     : auxiliary_names[i - extension_rows];
            display.text(0, y, label, ink);
            if (!calibration) {
                const int field = editing_ && (selected_ == i) ? draft_ : value(diagnostics, i);
                const std::string_view text =
                    i < extension_rows ? (field == 0 ? tr::menu_no : tr::menu_yes)
                                       : input_mode_names[static_cast<std::size_t>(field)];
                display.text(128 - static_cast<int>(text.size()) * 6, y, text, ink);
            }
        }
        display.small_text(0, 52, tr::menu_input_setup);
        display.small_text(0, 58,
                           notice_ != nullptr ? notice_
                           : editing_ && features::i6s && features::extended_inputs &&
                                   (selected_ == 0) && (draft_ != 0)
                               ? tr::inputs_swd_warning
                           : storage_message != nullptr ? storage_message
                                                        : tr::menu_help);
    }

  private:
    inline static constexpr std::size_t extension_rows = features::extended_inputs ? 2U : 0U;
    [[nodiscard]] static constexpr std::size_t count(const i6x::Diagnostics &diagnostics) noexcept {
        return extension_rows + 6 +
               (diagnostics.input_setup().extra_analog ? i6x::extra_adc_count : 0) + 1;
    }
    [[nodiscard]] static constexpr int value(const i6x::Diagnostics &diagnostics,
                                             const std::size_t row) noexcept {
        if constexpr (features::extended_inputs)
            if (row < extension_rows)
                return row == 0 ? diagnostics.input_setup().extra_digital
                                : diagnostics.input_setup().extra_analog;
        return static_cast<int>(diagnostics.input_setup().modes[row - extension_rows]);
    }
    [[nodiscard]] constexpr int value(const i6x::Diagnostics &diagnostics) const noexcept {
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
