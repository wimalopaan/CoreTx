// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/common/model_storage.hpp"
#include "boards/common/multiswitch_ui.hpp"
#include "boards/common/switch_names.hpp"
#include "coretx/ui/translation.hpp"
#include "coretx/ui/ui.hpp"

namespace coretx::firmware {

enum class MenuCommand : std::uint8_t {
    none,
    output,
    format,
    bind,
    baud,
    calibrate,
    stick_mode,
    crsf_config,
    input_setup,
    trim_beep,
    external_module,
    initialize_models
};
enum class MenuScreen : std::uint8_t {
    dashboard,
    root,
    models,
    model_actions,
    rename,
    delete_model,
    initialize_models,
    setup,
    radio,
    rf,
    editor,
    dashboard_channels,
    logical_switches,
    logical_editor,
    multiswitch,
    multiswitch_page,
    multiswitch_button,
    trims,
    curves
};

namespace menu_detail {
inline constexpr std::size_t trim_beep_row = features::crsf ? 4U : 3U;
inline constexpr std::size_t external_module_row = trim_beep_row + 1;
inline constexpr std::size_t multiswitch_row = 6;
inline constexpr std::size_t crsf_dashboard_row =
    multiswitch_row + (features::multiswitch ? 1U : 0U);
inline constexpr std::size_t trim_row = crsf_dashboard_row + (features::crsf ? 1U : 0U);
inline constexpr std::size_t curves_row = trim_row + 1;
inline constexpr std::size_t tank_row = curves_row + 1;
namespace tr = coretx::ui::tr;
inline constexpr std::size_t visible_rows = 4;
inline constexpr int list_y = 12;
inline constexpr int row_height = 10;
inline constexpr std::array input_fields{tr::menu_source,     tr::menu_weight,     tr::menu_expo,
                                         tr::menu_offset,     tr::menu_trim,       tr::menu_side,
                                         tr::menu_invert,     tr::menu_target,     tr::menu_switch,
                                         tr::menu_switch_pos, tr::menu_switch_not, tr::menu_curve};
inline constexpr std::array mix_fields{tr::menu_source,     tr::menu_weight, tr::menu_offset,
                                       tr::menu_multiplex,  tr::menu_trim,   tr::menu_invert,
                                       tr::menu_target,     tr::menu_switch, tr::menu_switch_pos,
                                       tr::menu_switch_not, tr::menu_curve};
inline constexpr std::array output_fields{tr::menu_minimum, tr::menu_maximum, tr::menu_subtrim,
                                          tr::menu_reverse};
inline constexpr std::array actions{tr::menu_new, tr::menu_copy, tr::menu_delete, tr::menu_move_up,
                                    tr::menu_move_down};
#if CORETX_MODUL_TANKMIX
inline constexpr std::array tank_fields{tr::tank_enabled,    tr::tank_drive,   tr::tank_steering,
                                        tr::tank_left,       tr::tank_right,   tr::tank_gain,
                                        tr::tank_hysteresis, tr::tank_reversal};

constexpr const char *tank_label(const MixRole role) noexcept {
    return role == MixRole::tank_drive     ? tr::tank_drive_line
           : role == MixRole::tank_forward ? tr::tank_forward
                                           : tr::tank_reverse;
}
#endif
inline constexpr std::array<std::string_view, 11> logical_functions{
    "--", "A > X", "A < X", "A = X", "A != X", "A >= X", "A <= X", "AND", "OR", "XOR", "SRFF"};
inline constexpr std::array logical_fields{
    tr::menu_function,   tr::menu_operand_a, tr::menu_switch_pos,
    tr::menu_switch_not, tr::menu_operand_b, tr::menu_switch_pos,
    tr::menu_switch_not, tr::menu_threshold, tr::menu_invert};

constexpr void row(coretx::ui::Framebuffer &display, const std::size_t index,
                   const std::size_t selected, const std::size_t first,
                   const std::string_view label) noexcept {
    if ((index < first) || (index >= (first + visible_rows)))
        return;
    const int y = list_y + static_cast<int>(index - first) * row_height;
    if (index == selected)
        display.rectangle(0, y - 1, 128, row_height);
    display.text(1, y, label, index != selected);
}

constexpr void percent(coretx::ui::Framebuffer &display, const int right, const int y,
                       const int value, const bool ink) noexcept {
    char buffer[10]{};
    std::size_t start = sizeof(buffer);
    auto magnitude = static_cast<unsigned>(value < 0 ? -value : value);
    buffer[--start] = static_cast<char>('0' + magnitude % 10);
    buffer[--start] = '.';
    magnitude /= 10;
    do {
        buffer[--start] = static_cast<char>('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude != 0);
    if (value < 0)
        buffer[--start] = '-';
    const std::string_view text{buffer + start, sizeof(buffer) - start};
    display.text(right - static_cast<int>(text.size()) * display.character_width(), y, text, ink);
}

constexpr void condition(coretx::ui::Framebuffer &display, const int x, const int y,
                         const Condition &condition, const bool ink) noexcept {
    if (!condition.enabled)
        return;
    std::array<char, 6> buffer{};
    const auto label = switch_condition_label(condition, buffer);
    const bool logical = (condition.index >= switch_count) &&
                         (condition.index < (switch_count + logical_switch_count));
    const int left =
        std::min(x + ((logical && !condition.inverted) ? display.character_width() : 0),
                 coretx::ui::Framebuffer::width -
                     static_cast<int>(label.size()) * display.character_width());
    display.text(left, y, label, ink);
}

inline constexpr int condition_positions =
    static_cast<int>(SwitchPosition::pos6) - static_cast<int>(SwitchPosition::up) + 1;

constexpr int condition_choice(const Condition &condition) noexcept {
    return condition.enabled ? (int{condition.index} * condition_positions +
                                static_cast<int>(condition.position) + 1)
                             : -1;
}

constexpr Condition select_condition(const int choice, const bool inverted) noexcept {
    return {choice >= 0, static_cast<std::uint8_t>(choice < 0 ? 0 : choice / condition_positions),
            static_cast<SwitchPosition>(choice < 0 ? -1 : choice % condition_positions - 1),
            inverted};
}

constexpr int change_condition(const int choice, const int direction,
                               const Diagnostics &diagnostics) noexcept {
    constexpr int maximum =
        static_cast<int>(switch_count + logical_switch_count) * condition_positions - 1;
    const auto switches = diagnostics.switch_mask();
    const auto six = diagnostics.six_mask();
    int next = choice;
    for (;;) {
        const int candidate = std::clamp(next + direction, -1, maximum);
        if (candidate == next)
            return choice;
        next = candidate;
        if (next < 0)
            return next;
        const auto condition = select_condition(next, false);
        const auto index = condition.index;
        const auto position = condition.position;
        if (index >= switch_count) {
            if ((diagnostics.model().logical_switches[index - switch_count].function !=
                 LogicalFunction::disabled) &&
                ((position == SwitchPosition::up) || (position == SwitchPosition::down)))
                return next;
        } else if ((switches & (1U << index)) != 0) {
            if ((six & (1U << index)) != 0) {
                if (position >= SwitchPosition::pos1)
                    return next;
            } else if ((position == SwitchPosition::up) || (position == SwitchPosition::down) ||
                       ((position == SwitchPosition::middle) &&
                        ((index >= physical_switch_count) || diagnostics.three_position(index))))
                return next;
        }
    }
}
} // namespace menu_detail

class Menu {
  public:
    [[nodiscard]] constexpr bool visible() const noexcept {
        return screen_ != MenuScreen::dashboard;
    }
    [[nodiscard]] constexpr MenuScreen screen() const noexcept { return screen_; }
    [[nodiscard]] constexpr bool pending_model_edit() const noexcept {
        return (screen_ == MenuScreen::editor) && editor_.editing();
    }
    [[nodiscard]] constexpr RadioOutput requested_output() const noexcept {
        return static_cast<RadioOutput>(draft_value_);
    }
    [[nodiscard]] constexpr afhds2a::ChannelMode requested_format() const noexcept {
        return static_cast<afhds2a::ChannelMode>(draft_value_);
    }
    [[nodiscard]] constexpr std::uint8_t requested_baud() const noexcept {
        return static_cast<std::uint8_t>(draft_value_);
    }
    constexpr void open() noexcept {
        screen_ = MenuScreen::root;
        selected_ = 0;
        notice_ = nullptr;
    }
    constexpr void model_changed(const std::size_t index) noexcept {
        if (screen_ == MenuScreen::models)
            selected_ = index;
    }

    template <typename Store>
    constexpr MenuCommand handle(const Key key, Store &store, Diagnostics &diagnostics,
                                 const RadioOutput output) noexcept {
        editor_.set_sources(diagnostics.analog_mask(), diagnostics.switch_mask(),
                            diagnostics.six_mask());
        can_initialize_ = store.can_initialize();
        if (key == Key::none)
            return {};
        auto &model = store.data().model;
        if (key == Key::cancel_long) {
            if (editor_.editing())
                editor_.handle(coretx::ui::Key::back, model, dummy_);
            diagnostics.refresh_model();
            screen_ = MenuScreen::dashboard;
            editing_ = false;
            return {};
        }
        if (key == Key::cancel) {
            if (screen_ == MenuScreen::editor) {
                if (editor_.editing() || (editor_.dialog() != coretx::ui::Dialog::none)) {
                    editor_.handle(coretx::ui::Key::back, model, dummy_);
                    diagnostics.refresh_model();
                } else if (details_)
                    details_ = false;
                else if (editor_.page() == coretx::ui::Page::curves) {
                    screen_ = MenuScreen::curves;
                    selected_ = editor_.selected_curve();
#if CORETX_MODUL_TANKMIX
                } else if (editor_.page() == coretx::ui::Page::tank) {
                    screen_ = MenuScreen::setup;
                    selected_ = menu_detail::tank_row;
#endif
                } else {
                    screen_ = MenuScreen::setup;
                    selected_ = static_cast<std::size_t>(editor_.page()) - 1;
                }
            } else if (editing_) {
                if (screen_ == MenuScreen::logical_editor)
                    logical_draft_ = logical_saved_field_;
                editing_ = false;
            } else if (screen_ == MenuScreen::logical_editor) {
                screen_ = MenuScreen::logical_switches;
                selected_ = logical_slot_;
            } else if (screen_ == MenuScreen::multiswitch_button) {
                screen_ = MenuScreen::multiswitch_page;
                selected_ = multiswitch_button_ + 1;
            } else if (screen_ == MenuScreen::multiswitch_page) {
                screen_ = MenuScreen::multiswitch;
                selected_ = multiswitch_slot_;
            } else {
                const bool from_model_actions = screen_ == MenuScreen::model_actions;
                screen_ =
                    screen_ == MenuScreen::root            ? MenuScreen::dashboard
                    : screen_ == MenuScreen::model_actions ? MenuScreen::models
                    : (screen_ == MenuScreen::rename) || (screen_ == MenuScreen::delete_model) ||
                            (screen_ == MenuScreen::initialize_models)
                        ? MenuScreen::model_actions
                    : (screen_ == MenuScreen::rf) || (screen_ == MenuScreen::dashboard_channels) ||
                            (screen_ == MenuScreen::logical_switches) ||
                            (screen_ == MenuScreen::multiswitch) ||
                            (screen_ == MenuScreen::trims) || (screen_ == MenuScreen::curves)
                        ? MenuScreen::setup
                        : MenuScreen::root;
                selected_ = from_model_actions ? model_slot_ : 0;
            }
            notice_ = nullptr;
            return {};
        }
        notice_ = nullptr;
        if (screen_ == MenuScreen::trims) {
            if (!store.editable()) {
                notice_ = store.message();
                return {};
            }
            const auto physical = board::trim_buttons ? selected_ / 2 : selected_;
            const bool mode = board::trim_buttons && ((selected_ % 2) == 0);
            const auto axis = trim_axis(diagnostics.setup(), physical);
            if (axis == trim_count) {
                notice_ = coretx::ui::tr::inputs_invalid;
                return {};
            }
            if (!editing_) {
                if (key == Key::ok) {
                    if (features::secure_ui && mode && (output != RadioOutput::off)) {
                        notice_ = coretx::ui::tr::menu_rf_off;
                        return {};
                    }
                    draft_value_ =
                        mode ? static_cast<int>(
                                   (static_cast<unsigned>(model.trim_switches) >> physical) & 1U)
                             : model.trims[axis].value();
                    editing_ = true;
                } else
                    move_selection(key, board::trim_buttons ? (trim_count * 2) : trim_count);
            } else if ((key == Key::up) || (key == Key::down)) {
                draft_value_ = std::clamp(draft_value_ + (key == Key::up ? 1 : -1),
                                          mode ? 0 : int{TrimSteps::minimum},
                                          mode ? 1 : int{TrimSteps::maximum});
            } else if (key == Key::ok) {
                const auto old_modes = model.trim_switches;
                const auto old_value = model.trims[axis];
                if (mode)
                    model.trim_switches = static_cast<std::uint8_t>(
                        (model.trim_switches & ~(1U << physical)) |
                        (static_cast<unsigned>(draft_value_) << physical));
                else
                    (void)model.trims[axis].try_set(draft_value_);
                if (!store.save()) {
                    model.trim_switches = old_modes;
                    model.trims[axis] = old_value;
                    notice_ = store.message();
                } else
                    editing_ = false;
                diagnostics.refresh_model();
            }
            return {};
        }
        if ((screen_ == MenuScreen::multiswitch) || (screen_ == MenuScreen::multiswitch_page) ||
            (screen_ == MenuScreen::multiswitch_button)) {
            if (store.busy() || !store.editable()) {
                notice_ = store.message();
                return {};
            }
            if (screen_ == MenuScreen::multiswitch) {
                if ((key == Key::ok) && (selected_ == multiswitch_page_count)) {
                    const bool old = model.multiswitch_beep;
                    model.multiswitch_beep = !old;
                    if (!store.save()) {
                        model.multiswitch_beep = old;
                        notice_ = store.message();
                    }
                } else if (key == Key::ok) {
                    multiswitch_slot_ = static_cast<std::uint8_t>(selected_);
                    selected_ = 0;
                    screen_ = MenuScreen::multiswitch_page;
                } else
                    move_selection(key, row_count());
                return {};
            }
            auto &page = model.multiswitch[multiswitch_slot_];
            if (!editing_) {
                if (key != Key::ok) {
                    move_selection(key, row_count());
                    return {};
                }
                if ((screen_ == MenuScreen::multiswitch_page) && (selected_ != 0)) {
                    multiswitch_button_ = static_cast<std::uint8_t>(selected_ - 1);
                    screen_ = MenuScreen::multiswitch_button;
                    selected_ = 0;
                    return {};
                }
                editing_ = true;
                multiswitch_control_ = page.controls[multiswitch_button_];
                if (screen_ == MenuScreen::multiswitch_page)
                    draft_value_ = page.address;
                else if (selected_ == 0) {
                    multiswitch_label_ = page.labels[multiswitch_button_];
                    multiswitch_cursor_ = 0;
                } else if (selected_ == 1)
                    draft_value_ = static_cast<int>(
                        (static_cast<unsigned>(page.momentary) >> multiswitch_button_) & 1U);
                else if (selected_ == 2)
                    draft_value_ = multiswitch_control_.output();
                else if (selected_ == 3)
                    draft_value_ = multiswitch_control_.address();
                else
                    draft_value_ = menu_detail::condition_choice(multiswitch_control_.condition());
                return {};
            }
            const bool text = (screen_ == MenuScreen::multiswitch_button) && (selected_ == 0);
            if (text && (key == Key::ok)) {
                multiswitch_cursor_ = static_cast<std::uint8_t>((multiswitch_cursor_ + 1) %
                                                                multiswitch_label_.size());
                return {};
            }
            if ((key == Key::up) || (key == Key::down)) {
                const int direction = key == Key::up ? 1 : -1;
                if (text) {
                    constexpr std::string_view alphabet =
                        " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.";
                    auto index = alphabet.find(multiswitch_label_[multiswitch_cursor_]);
                    if (index == std::string_view::npos)
                        index = 0;
                    index = (index + (direction > 0 ? 1 : alphabet.size() - 1)) % alphabet.size();
                    multiswitch_label_[multiswitch_cursor_] = alphabet[index];
                } else if ((screen_ == MenuScreen::multiswitch_button) && (selected_ == 4)) {
                    draft_value_ =
                        menu_detail::change_condition(draft_value_, direction, diagnostics);
                } else {
                    const bool address =
                        (screen_ == MenuScreen::multiswitch_page) || (selected_ == 3);
                    draft_value_ = std::clamp(draft_value_ + direction, address ? -1 : 0,
                                              address          ? 255
                                              : selected_ == 2 ? 7
                                                               : 1);
                }
                return {};
            }
            if ((text && (key == Key::ok_long)) || (!text && (key == Key::ok))) {
                const auto old = page;
                if (text)
                    page.labels[multiswitch_button_] = multiswitch_label_;
                else if (screen_ == MenuScreen::multiswitch_page)
                    page.address = static_cast<std::int16_t>(draft_value_);
                else if (selected_ == 1) {
                    const auto bit = static_cast<std::uint8_t>(1U << multiswitch_button_);
                    page.momentary = static_cast<std::uint8_t>((page.momentary & ~bit) |
                                                               (draft_value_ != 0 ? bit : 0U));
                } else {
                    auto &control = page.controls[multiswitch_button_];
                    bool valid{};
                    if (selected_ == 2)
                        valid = control.set_output(static_cast<std::uint8_t>(draft_value_));
                    else if (selected_ == 3)
                        valid = control.set_address(draft_value_);
                    else {
                        valid = control.set_condition(menu_detail::select_condition(
                            draft_value_, control.condition().inverted));
                    }
                    if (!valid) {
                        page = old;
                        notice_ = coretx::ui::tr::multiswitch_invalid;
                        return {};
                    }
                }
                if (const auto status = coretx::validate(diagnostics.model()); !status) {
                    page = old;
                    notice_ = status.code == ErrorCode::multiswitch_duplicate
                                  ? coretx::ui::tr::multiswitch_duplicate
                                  : coretx::ui::tr::multiswitch_invalid;
                    return {};
                }
                if (!store.save()) {
                    page = old;
                    notice_ = store.message();
                } else
                    editing_ = false;
                diagnostics.constrain_page();
                diagnostics.refresh_model();
            }
            return {};
        }
        if ((screen_ == MenuScreen::dashboard_channels) ||
            (screen_ == MenuScreen::logical_switches) || (screen_ == MenuScreen::logical_editor)) {
            if (store.busy() || !store.editable()) {
                notice_ = store.message();
                return {};
            }
            if (screen_ == MenuScreen::dashboard_channels) {
                if (editing_) {
                    if ((key == Key::up) || (key == Key::down))
                        draft_value_ = std::clamp(draft_value_ + (key == Key::up ? 1 : -1), 0, 16);
                    else if (key == Key::ok) {
                        const auto old = model.dashboard_channels[selected_];
                        model.dashboard_channels[selected_] =
                            static_cast<std::uint8_t>(draft_value_);
                        if (!store.save()) {
                            model.dashboard_channels[selected_] = old;
                            notice_ = store.message();
                        } else
                            editing_ = false;
                        diagnostics.refresh_model();
                    }
                } else if (key == Key::ok) {
                    draft_value_ = model.dashboard_channels[selected_];
                    editing_ = true;
                } else
                    move_selection(key, 8);
            } else if (screen_ == MenuScreen::logical_switches) {
                if (key == Key::ok) {
                    logical_slot_ = selected_;
                    logical_draft_ = model.logical_switches[selected_];
                    screen_ = MenuScreen::logical_editor;
                    selected_ = 0;
                    editing_ = false;
                } else
                    move_selection(key, logical_switch_count);
            } else if (key == Key::ok_long) {
                const auto old = model.logical_switches[logical_slot_];
                model.logical_switches[logical_slot_] = logical_draft_;
                const auto status = coretx::validate(model);
                if (!status || !store.save()) {
                    model.logical_switches[logical_slot_] = old;
                    notice_ = !status ? coretx::error_message(status.code) : store.message();
                } else {
                    screen_ = MenuScreen::logical_switches;
                    selected_ = logical_slot_;
                    editing_ = false;
                }
                diagnostics.refresh_model();
            } else if (key == Key::ok) {
                if (!editing_)
                    logical_saved_field_ = logical_draft_;
                editing_ = !editing_;
            } else if (editing_) {
                if ((key == Key::up) || (key == Key::down))
                    change_logical(key == Key::up ? 1 : -1);
            } else
                move_selection(key, menu_detail::logical_fields.size());
            return {};
        }
        if (screen_ == MenuScreen::editor) {
            if ((store.busy() && (!editor_.editing() || store.owns_memory())) ||
                !store.editable()) {
                notice_ = store.message();
                return {};
            }
            if ((editor_.page() == coretx::ui::Page::curves)
#if CORETX_MODUL_TANKMIX
                || (editor_.page() == coretx::ui::Page::tank)
#endif
            ) {
                editor_.handle(
                    key == Key::up
                        ? (editor_.editing() ? coretx::ui::Key::right : coretx::ui::Key::up)
                    : key == Key::down
                        ? (editor_.editing() ? coretx::ui::Key::left : coretx::ui::Key::down)
                    : key == Key::ok                              ? coretx::ui::Key::enter
                    : (key == Key::ok_long) && !editor_.editing() ? coretx::ui::Key::right
                                                                  : coretx::ui::Key::none,
                    model, dummy_);
            } else if ((key == Key::ok_long) && !details_)
                editor_.handle(coretx::ui::Key::menu, model, dummy_);
            else if (editor_.dialog() != coretx::ui::Dialog::none)
                editor_.handle(key == Key::up     ? coretx::ui::Key::up
                               : key == Key::down ? coretx::ui::Key::down
                               : key == Key::ok   ? coretx::ui::Key::enter
                                                  : coretx::ui::Key::none,
                               model, dummy_);
            else if (details_)
                editor_.handle(key == Key::up     ? (editor_.editing() ? coretx::ui::Key::right
                                                                       : coretx::ui::Key::left)
                               : key == Key::down ? (editor_.editing() ? coretx::ui::Key::left
                                                                       : coretx::ui::Key::right)
                               : key == Key::ok   ? coretx::ui::Key::enter
                                                  : coretx::ui::Key::none,
                               model, dummy_);
            else if (key == Key::ok) {
                if ((editor_.page() == coretx::ui::Page::outputs) ||
                    (!model.inputs.empty() && (editor_.page() == coretx::ui::Page::inputs)) ||
                    (!model.mixes.empty() && (editor_.page() == coretx::ui::Page::mixes)))
                    details_ = true;
            } else
                editor_.handle(key == Key::up     ? coretx::ui::Key::up
                               : key == Key::down ? coretx::ui::Key::down
                                                  : coretx::ui::Key::none,
                               model, dummy_);
            diagnostics.refresh_model();
            if (editor_.take_changed() && !store.save())
                notice_ = store.message();
            return {};
        }
        if (screen_ == MenuScreen::rename) {
            constexpr std::string_view alphabet =
                " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
            if (key == Key::ok)
                selected_ = (selected_ + 1) % (name_.size() - 1);
            else if ((key == Key::up) || (key == Key::down)) {
                auto index = alphabet.find(name_[selected_]);
                if (index == std::string_view::npos)
                    index = 0;
                index = (index + (key == Key::up ? 1 : alphabet.size() - 1)) % alphabet.size();
                name_[selected_] = alphabet[index];
            } else if (key == Key::ok_long) {
                for (auto i = name_.size() - 1; i > 0; --i) {
                    if ((name_[i - 1] != ' ') && (name_[i - 1] != 0))
                        break;
                    name_[i - 1] = 0;
                }
                if (store.rename(model_slot_, name_)) {
                    screen_ = MenuScreen::models;
                    selected_ = model_slot_;
                } else
                    notice_ = store.message();
            }
            return {};
        }
        if (editing_) {
            if ((key == Key::up) || (key == Key::down)) {
                const auto maximum = screen_ == MenuScreen::radio ? 4
                                     : rf_field(selected_) == 0   ? 2
                                     : rf_field(selected_) == 1   ? 1
                                                                  : 7;
                const int minimum = screen_ == MenuScreen::radio ? 1 : 0;
                draft_value_ += key == Key::up ? 1 : -1;
                if (draft_value_ < minimum)
                    draft_value_ = maximum;
                if (draft_value_ > maximum)
                    draft_value_ = minimum;
                if ((screen_ == MenuScreen::rf) && (selected_ == 0)) {
                    for (int i = 0;
                         (i < 3) &&
                         (((draft_value_ == 1) && !features::a7105) ||
                          ((draft_value_ == 2) &&
                           (!features::crsf || !diagnostics.input_setup().external_module)));
                         ++i)
                        draft_value_ = (draft_value_ + (key == Key::up ? 1 : 2)) % 3;
                }
            } else if (key == Key::ok) {
                editing_ = false;
                if (screen_ == MenuScreen::radio) {
                    if (!diagnostics.set_stick_mode(static_cast<std::size_t>(draft_value_))) {
                        notice_ = coretx::ui::tr::bad_request;
                        return {};
                    }
                    return MenuCommand::stick_mode;
                }
                return rf_field(selected_) == 0   ? MenuCommand::output
                       : rf_field(selected_) == 1 ? MenuCommand::format
                                                  : MenuCommand::baud;
            }
            return {};
        }
        const auto size = row_count();
        if ((key == Key::up) && (selected_ > 0))
            --selected_;
        else if ((key == Key::down) && ((selected_ + 1) < size))
            ++selected_;
        else if ((screen_ == MenuScreen::models) && ((key == Key::ok) || (key == Key::ok_long))) {
            model_slot_ = selected_;
            screen_ = MenuScreen::model_actions;
            selected_ = 0;
        } else if (key == Key::ok) {
            notice_ = nullptr;
            switch (screen_) {
            case MenuScreen::root:
                screen_ = selected_ == 0   ? MenuScreen::models
                          : selected_ == 1 ? MenuScreen::setup
                                           : MenuScreen::radio;
                selected_ = 0;
                break;
            case MenuScreen::setup:
#if CORETX_MODUL_TANKMIX
                if (selected_ == menu_detail::tank_row) {
                    editor_.open(coretx::ui::Page::tank);
                    screen_ = MenuScreen::editor;
                    details_ = false;
                } else
#endif
                    if (selected_ == menu_detail::curves_row) {
                    screen_ = MenuScreen::curves;
                    selected_ = 0;
                } else if (selected_ == menu_detail::trim_row) {
                    screen_ = MenuScreen::trims;
                    selected_ = 0;
                    editing_ = false;
                } else if (features::crsf && (selected_ == menu_detail::crsf_dashboard_row)) {
                    const bool old = model.crsf_dashboard;
                    model.crsf_dashboard = !old;
                    if (!store.save()) {
                        model.crsf_dashboard = old;
                        notice_ = store.message();
                    }
                    diagnostics.constrain_page();
                } else if (selected_ < 3) {
                    editor_.open(static_cast<coretx::ui::Page>(selected_ + 1));
                    screen_ = MenuScreen::editor;
                    details_ = false;
                } else if (selected_ == 3) {
                    screen_ = MenuScreen::rf;
                    selected_ = 0;
                } else {
                    screen_ = selected_ == 4   ? MenuScreen::dashboard_channels
                              : selected_ == 5 ? MenuScreen::logical_switches
                                               : MenuScreen::multiswitch;
                    selected_ = 0;
                    editing_ = false;
                }
                break;
            case MenuScreen::curves:
                editor_.open_curve(static_cast<std::uint8_t>(selected_));
                screen_ = MenuScreen::editor;
                details_ = false;
                break;
            case MenuScreen::radio:
                if (features::crsf && (selected_ == 3))
                    return MenuCommand::crsf_config;
                if (features::secure_ui && (output != RadioOutput::off)) {
                    notice_ = coretx::ui::tr::menu_rf_off;
                    break;
                }
                if (store.busy()) {
                    notice_ = store.message();
                    break;
                }
                if (selected_ == 1)
                    return MenuCommand::calibrate;
                if (selected_ == 2)
                    return MenuCommand::input_setup;
                if (selected_ == menu_detail::trim_beep_row)
                    return MenuCommand::trim_beep;
                if (selected_ == menu_detail::external_module_row) {
                    if (output == RadioOutput::external) {
                        notice_ = coretx::ui::tr::menu_rf_off;
                        break;
                    }
                    return MenuCommand::external_module;
                }
                draft_value_ = static_cast<int>(diagnostics.stick_mode());
                editing_ = true;
                break;
            case MenuScreen::rf:
                if (!store.editable() && (rf_field(selected_) == 0) &&
                    (output != RadioOutput::off)) {
                    draft_value_ = 0;
                    return MenuCommand::output;
                }
                if (!store.editable() || (store.busy() && !store.background_loading())) {
                    notice_ = store.message();
                    break;
                }
                if ((rf_field(selected_) != 0) && (rf_field(selected_) != 2) &&
                    features::secure_ui && (output != RadioOutput::off)) {
                    notice_ = coretx::ui::tr::menu_rf_off;
                    break;
                }
                if (rf_field(selected_) == 2)
                    return MenuCommand::bind;
                draft_value_ =
                    rf_field(selected_) == 0   ? static_cast<int>(store.data().radio.output)
                    : rf_field(selected_) == 1 ? static_cast<int>(store.data().radio.channel_mode)
                                               : store.data().baud_index;
                editing_ = true;
                break;
            case MenuScreen::model_actions:
                if (features::secure_ui && (output != RadioOutput::off)) {
                    notice_ = coretx::ui::tr::menu_rf_off;
                    break;
                }
                if (selected_ == 5) {
                    screen_ = MenuScreen::initialize_models;
                    selected_ = 0;
                } else if (selected_ == 4) {
                    screen_ = MenuScreen::delete_model;
                    selected_ = 0;
                } else if (selected_ == 3) {
                    if (!store.entries()[model_slot_].present) {
                        notice_ = coretx::ui::tr::menu_empty;
                        break;
                    }
                    name_ = store.entries()[model_slot_].name;
                    for (std::size_t i = 0; (i + 1) < name_.size(); ++i)
                        if (name_[i] == 0)
                            name_[i] = ' ';
                    screen_ = MenuScreen::rename;
                    selected_ = 0;
                } else {
                    const bool done = selected_ == 0 ? store.select(model_slot_)
                                                     : store.create(selected_ == 2, model_slot_);
                    if (done) {
                        screen_ = MenuScreen::models;
                        selected_ = model_slot_;
                    } else
                        notice_ = store.message();
                }
                break;
            case MenuScreen::delete_model:
                if (selected_ == 1) {
                    if (store.erase(model_slot_)) {
                        screen_ = MenuScreen::models;
                        selected_ = model_slot_;
                    } else
                        notice_ = store.message();
                } else {
                    screen_ = MenuScreen::model_actions;
                    selected_ = 0;
                }
                break;
            case MenuScreen::initialize_models:
                if (selected_ == 1) {
                    screen_ = MenuScreen::models;
                    selected_ = 0;
                    return MenuCommand::initialize_models;
                }
                screen_ = MenuScreen::model_actions;
                selected_ = 0;
                break;
            default:
                break;
            }
        }
        return {};
    }

    template <typename Store>
    constexpr void render(const Store &store, const Diagnostics &diagnostics,
                          const RadioOutput output, coretx::ui::Framebuffer &display,
                          const bool binding = false) const noexcept {
        namespace tr = coretx::ui::tr;
        using menu_detail::row;
        display.clear();
        display.compact(false);
        display.text(0, 0, title());
        if (screen_ == MenuScreen::logical_editor)
            coretx::ui::detail::indexed_label(display, 110, 0, "L", logical_slot_, true);
        if ((screen_ == MenuScreen::editor) && (editor_.page() == coretx::ui::Page::curves)) {
            coretx::ui::detail::indexed_label(display, 74, 0, "C", editor_.selected_curve(), true);
            display.text(98, 0, editor_.selected_field() == 0 ? "X %" : "Y %");
        }
#if CORETX_MODUL_TANKMIX
        if ((screen_ == MenuScreen::editor) && (editor_.page() == coretx::ui::Page::tank))
            display.small_text(100, 2, diagnostics.evaluation().frame.tank_reverse ? "REV" : "FWD");
#endif
        display.rectangle(0, 10, 128, 1);
        const auto first =
            selected_ >= menu_detail::visible_rows ? selected_ - menu_detail::visible_rows + 1 : 0;
        if (screen_ == MenuScreen::trims) {
            for (std::size_t i = first;
                 (i < row_count()) && (i < (first + menu_detail::visible_rows)); ++i) {
                const auto physical = board::trim_buttons ? i / 2 : i;
                const bool mode = board::trim_buttons && ((i % 2) == 0);
                const auto axis = trim_axis(diagnostics.setup(), physical);
                const int y =
                    menu_detail::list_y + static_cast<int>(i - first) * menu_detail::row_height;
                const bool ink = i != selected_;
                row(display, i, selected_, first, "");
                coretx::ui::detail::indexed_label(display, 1, y, "T", physical, ink);
                display.text(22, y, mode ? tr::trim_mode : tr::trim_value, ink);
                if (!mode && (axis < trim_count))
                    coretx::ui::detail::draw_source(
                        display, 56, y, {SourceKind::analog, static_cast<std::uint8_t>(axis)}, ink,
                        &switch_names);
                const int value =
                    editing_ && (i == selected_) ? draft_value_
                    : mode ? static_cast<int>(
                                 (static_cast<unsigned>(store.data().model.trim_switches) >>
                                  physical) &
                                 1U)
                    : axis < trim_count ? store.data().model.trims[axis].value()
                                        : 0;
                if (mode)
                    display.text(88, y, value ? tr::trim_switch : tr::menu_trim, ink);
                else
                    display.number(127, y, value, ink);
            }
        } else if ((screen_ == MenuScreen::multiswitch) ||
                   (screen_ == MenuScreen::multiswitch_page) ||
                   (screen_ == MenuScreen::multiswitch_button)) {
            render_multiswitch_setup(store.data().model, display, first);
        } else if (screen_ == MenuScreen::editor) {
            render_editor(store.data().model, diagnostics.evaluation().frame, display);
        } else if ((screen_ == MenuScreen::dashboard_channels) ||
                   (screen_ == MenuScreen::logical_switches) ||
                   (screen_ == MenuScreen::logical_editor)) {
            for (std::size_t i = first;
                 (i < row_count()) && (i < (first + menu_detail::visible_rows)); ++i) {
                const int y =
                    menu_detail::list_y + (static_cast<int>(i - first) * menu_detail::row_height);
                const bool ink = i != selected_;
                row(display, i, selected_, first,
                    screen_ == MenuScreen::logical_editor ? menu_detail::logical_fields[i] : "");
                if (screen_ == MenuScreen::dashboard_channels) {
                    display.text(1, y, "BAR", ink);
                    display.number(30, y, static_cast<std::int32_t>(i + 1), ink);
                    const int channel = (editing_ && (i == selected_))
                                            ? draft_value_
                                            : store.data().model.dashboard_channels[i];
                    if (channel == 0)
                        display.text(111, y, "--", ink);
                    else {
                        display.text(90, y, "CH", ink);
                        display.number(127, y, channel, ink);
                    }
                } else if (screen_ == MenuScreen::logical_switches) {
                    coretx::ui::detail::indexed_label(display, 1, y, "L", i, ink);
                    display.text(48, y,
                                 menu_detail::logical_functions[static_cast<std::size_t>(
                                     store.data().model.logical_switches[i].function)],
                                 ink);
                } else
                    render_logical_field(display, i, y, ink);
            }
        } else if (screen_ == MenuScreen::models) {
            for (std::size_t i = first;
                 (i < models::count) && (i < (first + menu_detail::visible_rows)); ++i) {
                const auto &entry = store.entries()[i];
                row(display, i, selected_, first, "");
                const int y =
                    menu_detail::list_y + static_cast<int>(i - first) * menu_detail::row_height;
                const bool ink = i != selected_;
                display.text(0, y, i == store.active() ? "*" : " ", ink);
                display.number(18, y, static_cast<std::int32_t>(i + 1), ink);
                display.text(26, y, entry.present ? entry.name.data() : tr::menu_empty, ink);
            }
        } else if (screen_ == MenuScreen::rename) {
            display.text(0, 17, name_.data());
            display.rectangle(static_cast<int>(selected_) * 6, 27, 5, 1);
        } else {
            for (std::size_t i = first;
                 (i < row_count()) && (i < (first + menu_detail::visible_rows)); ++i) {
                const char *label{};
                if (screen_ == MenuScreen::root) {
                    constexpr std::array labels{tr::menu_models, tr::menu_model_setup,
                                                tr::menu_radio_setup};
                    label = labels[i];
                } else if (screen_ == MenuScreen::setup) {
                    constexpr std::array labels{tr::menu_inputs,      tr::menu_mixers,
                                                tr::menu_outputs,     tr::menu_radio_config,
                                                tr::menu_dashboard,   tr::menu_logical,
                                                tr::menu_multiswitch, tr::menu_crsf_dashboard};
                    label =
#if CORETX_MODUL_TANKMIX
                        i == menu_detail::tank_row ? tr::menu_tank :
#endif
                        i == menu_detail::curves_row ? tr::menu_curves
                        : i == menu_detail::trim_row ? tr::menu_trims
                        : (i == menu_detail::crsf_dashboard_row) && features::crsf
                            ? tr::menu_crsf_dashboard
                            : labels[i];
                } else if (screen_ == MenuScreen::curves)
                    label = tr::menu_curve;
                else if (screen_ == MenuScreen::radio)
                    label = i == 0                                  ? tr::menu_stick_mode
                            : i == 1                                ? tr::menu_calibration
                            : i == 2                                ? tr::menu_input_setup
                            : i == menu_detail::trim_beep_row       ? tr::menu_trim_beep
                            : i == menu_detail::external_module_row ? tr::menu_external_module
                                                                    : tr::menu_crsf_config;
                else if (screen_ == MenuScreen::model_actions) {
                    constexpr std::array labels{tr::menu_select, tr::menu_new,
                                                tr::menu_copy,   tr::menu_rename,
                                                tr::menu_delete, tr::menu_initialize_models};
                    label = labels[i];
                } else if ((screen_ == MenuScreen::delete_model) ||
                           (screen_ == MenuScreen::initialize_models))
                    label = i == 0 ? tr::menu_no : tr::menu_yes;
                else if (screen_ == MenuScreen::rf) {
                    constexpr std::array labels{tr::radio_output, tr::menu_format, tr::menu_bind,
                                                tr::menu_baud};
                    label = labels[rf_field(i)];
                }
                if (screen_ == MenuScreen::initialize_models)
                    display.small_text(0, 36, tr::menu_erase_models);
                if (screen_ == MenuScreen::delete_model)
                    display.text(0, 36, store.entries()[model_slot_].name.data());
                row(display, i, selected_, first, label == nullptr ? "" : label);
                const int y =
                    menu_detail::list_y + static_cast<int>(i - first) * menu_detail::row_height;
                const bool ink = i != selected_;
                if (screen_ == MenuScreen::curves)
                    coretx::ui::detail::indexed_label(display, 110, y, "C", i, ink);
                if ((screen_ == MenuScreen::radio) && (i == 0))
                    display.number(
                        127, y,
                        editing_ ? draft_value_ : static_cast<int>(diagnostics.stick_mode()), ink);
                if ((screen_ == MenuScreen::radio) && ((i == menu_detail::trim_beep_row) ||
                                                       (i == menu_detail::external_module_row))) {
                    const std::string_view value = (i == menu_detail::trim_beep_row
                                                        ? diagnostics.input_setup().trim_beep
                                                        : diagnostics.input_setup().external_module)
                                                       ? tr::menu_yes
                                                       : tr::menu_no;
                    display.text(127 - static_cast<int>(value.size()) * 6, y, value, ink);
                }
                if ((screen_ == MenuScreen::setup) && features::crsf &&
                    (i == menu_detail::crsf_dashboard_row)) {
                    const std::string_view value =
                        store.data().model.crsf_dashboard ? tr::menu_yes : tr::menu_no;
                    display.text(127 - static_cast<int>(value.size()) * 6, y, value, ink);
                }
                if (screen_ == MenuScreen::rf) {
                    const auto field = rf_field(i);
                    const int value = editing_ && (selected_ == i) ? draft_value_
                                      : field == 0 ? static_cast<int>(store.data().radio.output)
                                      : field == 1
                                          ? static_cast<int>(store.data().radio.channel_mode)
                                          : store.data().baud_index;
                    if (field == 0) {
                        const std::string_view backend =
                            value == 0   ? tr::radio_off
                            : value == 1 ? (output == RadioOutput::off ? "AFHDS2A OFF" : "AFHDS2A")
                                         : (output == RadioOutput::off ? "CRSF OFF" : "CRSF");
                        display.text(128 - static_cast<int>(backend.size()) * 6, y, backend, ink);
                    } else if (field == 1)
                        display.text(98, y, value == 0 ? "14 RC" : "16 RC", ink);
                    else if ((field == 2) && binding)
                        display.small_text(96, y + 2, tr::radio_binding, ink);
                    else if (field == 3) {
                        display.unsigned_number(
                            127, y, crsf_baudrates[static_cast<std::size_t>(value)].rate, ink);
                    }
                }
            }
        }
#if CORETX_MODUL_TANKMIX
        const bool managed =
            (screen_ == MenuScreen::editor) && editor_.managed_mix(store.data().model);
        if (managed) {
            const auto &line = store.data().model.mixes[editor_.selected_row(store.data().model)];
            display.small_text(0, 52, menu_detail::tank_label(line.role));
            display.small_text(
                104, 52, tank_mix_active(line.role, diagnostics.evaluation().frame) ? "ON" : "OFF");
        } else
#endif
            display.small_text(0, 52,
                               (screen_ == MenuScreen::model_actions) ||
                                       (screen_ == MenuScreen::delete_model)
                                   ? (store.entries()[model_slot_].present
                                          ? store.entries()[model_slot_].name.data()
                                          : tr::menu_empty)
                                   : store.data().name.data());
        display.small_text(
            0, 58,
            notice_ != nullptr                  ? notice_
            : store.busy() || !store.editable() ? store.message()
#if CORETX_MODUL_TANKMIX
            : managed ? tr::tank_readonly
            : (screen_ == MenuScreen::editor) && (editor_.page() == coretx::ui::Page::tank)
                ? (editor_.notice() ? editor_.notice() : tr::tank_disable_help)
#endif
            : screen_ == MenuScreen::rename ? tr::menu_name_help
            : (screen_ == MenuScreen::multiswitch_button) && editing_ && (selected_ == 0)
                ? tr::menu_name_help
            : screen_ == MenuScreen::logical_editor ? tr::menu_logical_help
            : (screen_ == MenuScreen::editor) && (editor_.page() == coretx::ui::Page::curves)
                ? tr::menu_curve_help
            : (screen_ == MenuScreen::editor) && !details_ ? tr::menu_hold_help
                                                           : tr::menu_help);
    }

  private:
    constexpr void render_multiswitch_setup(const Model &model, coretx::ui::Framebuffer &display,
                                            const std::size_t first) const noexcept {
        namespace tr = coretx::ui::tr;
        for (std::size_t i = first; (i < row_count()) && (i < (first + menu_detail::visible_rows));
             ++i) {
            const int y =
                menu_detail::list_y + static_cast<int>(i - first) * menu_detail::row_height;
            const bool ink = i != selected_;
            menu_detail::row(display, i, selected_, first, "");
            if (screen_ == MenuScreen::multiswitch) {
                if (i == multiswitch_page_count) {
                    display.text(1, y, tr::menu_multiswitch_beep, ink);
                    const std::string_view text =
                        model.multiswitch_beep ? tr::menu_yes : tr::menu_no;
                    display.text(128 - static_cast<int>(text.size()) * 6, y, text, ink);
                    continue;
                }
                display.text(1, y, "MS", ink);
                display.number(30, y, static_cast<std::int32_t>(i + 1), ink);
                const auto address = model.multiswitch[i].address;
                if (address < 0)
                    display.text(128 - static_cast<int>(std::string_view{tr::radio_off}.size()) * 6,
                                 y, tr::radio_off, ink);
                else
                    display.number(127, y, address, ink);
                continue;
            }
            const auto &page = model.multiswitch[multiswitch_slot_];
            if ((screen_ == MenuScreen::multiswitch_page) && (i == 0)) {
                display.text(1, y, tr::multiswitch_address, ink);
                const int address = editing_ && (selected_ == 0) ? draft_value_ : page.address;
                if (address < 0)
                    display.text(128 - static_cast<int>(std::string_view{tr::radio_off}.size()) * 6,
                                 y, tr::radio_off, ink);
                else
                    display.number(127, y, address, ink);
            } else if (screen_ == MenuScreen::multiswitch_page) {
                const auto &control = page.controls[i - 1];
                const int address = control.target_address(page.address);
                const int digits = address < 0 ? 3 : address >= 100 ? 3 : address >= 10 ? 2 : 1;
                display.number(7, y, control.output() + 1, ink);
                display.text(7, y, "@", ink);
                if (address < 0)
                    display.text(13, y, tr::radio_off, ink);
                else
                    display.number(13 + digits * 6, y, address, ink);
                std::array<char, 6> buffer{};
                const auto label = multiswitch_label(page.labels[i - 1], buffer, i - 1);
                display.text(38, y, label, ink);
                display.compact();
                menu_detail::condition(display, 84, y + 2, control.condition(), ink);
                display.compact(false);
                display.text(120, y, (page.momentary & (1U << (i - 1))) != 0 ? "T" : "S", ink);
            } else if (i == 0) {
                display.text(1, y, tr::multiswitch_text, ink);
                std::array<char, 6> label = editing_ && (selected_ == 0)
                                                ? multiswitch_label_
                                                : page.labels[multiswitch_button_];
                for (auto &character : label)
                    if (character == 0)
                        character = ' ';
                display.text(90, y, {label.data(), label.size()}, ink);
                if (editing_ && (selected_ == 0))
                    display.rectangle(90 + static_cast<int>(multiswitch_cursor_) * 6, y + 8, 5, 1,
                                      ink);
            } else if (i == 1) {
                display.text(1, y, tr::multiswitch_type, ink);
                const bool momentary = editing_ && (selected_ == 1)
                                           ? draft_value_ != 0
                                           : (page.momentary & (1U << multiswitch_button_)) != 0;
                const std::string_view type =
                    momentary ? tr::multiswitch_momentary : tr::multiswitch_toggle;
                display.compact();
                display.text(127 - static_cast<int>(type.size()) * 4, y + 2, type, ink);
                display.compact(false);
            } else {
                const auto &control = page.controls[multiswitch_button_];
                const bool editing = editing_ && (selected_ == i);
                display.text(1, y,
                             i == 2   ? tr::multiswitch_output
                             : i == 3 ? tr::multiswitch_address
                                      : tr::multiswitch_source,
                             ink);
                if (i == 2)
                    display.number(127, y, (editing ? draft_value_ : control.output()) + 1, ink);
                else if (i == 3) {
                    const auto address = editing ? draft_value_ : control.address();
                    if (address < 0) {
                        const std::string_view inherited{tr::multiswitch_inherit};
                        display.text(127 - static_cast<int>(inherited.size()) * 6, y, inherited,
                                     ink);
                    } else
                        display.number(127, y, address, ink);
                } else {
                    auto condition = control.condition();
                    if (editing)
                        condition = menu_detail::select_condition(draft_value_, condition.inverted);
                    if (condition.enabled)
                        menu_detail::condition(display, 101, y, condition, ink);
                    else
                        display.text(109, y, "---", ink);
                }
            }
        }
        if (screen_ != MenuScreen::multiswitch) {
            display.compact();
            display.text(100, 2, "MS");
            display.number(115, 2, multiswitch_slot_ + 1);
            if (screen_ == MenuScreen::multiswitch_button)
                display.number(127, 2, multiswitch_button_ + 1);
            display.compact(false);
        }
    }
    constexpr void move_selection(const Key key, const std::size_t count) noexcept {
        if ((key == Key::up) && (selected_ > 0))
            --selected_;
        else if ((key == Key::down) && ((selected_ + 1) < count))
            ++selected_;
    }
    constexpr void change_logical(const int direction) noexcept {
        auto &line = logical_draft_;
        const auto change = [direction](const int value, const int minimum,
                                        const int maximum) constexpr {
            return std::clamp(value + direction, minimum, maximum);
        };
        if (selected_ == 0) {
            line.function =
                static_cast<LogicalFunction>(change(static_cast<int>(line.function), 0, 10));
            // Changing the function starts with safe physical operands.
            line.a = line.b = 0;
            line.position_a = line.position_b = SwitchPosition::up;
        } else if ((selected_ == 1) || (selected_ == 4)) {
            auto &index = selected_ == 1 ? line.a : line.b;
            auto &position = selected_ == 1 ? line.position_a : line.position_b;
            const bool boolean = coretx::detail::boolean_function(line.function);
            index = static_cast<std::uint8_t>(change(index, 0, boolean ? 23 : 31));
            if (boolean && (index >= switch_count))
                position = SwitchPosition::down;
        } else if ((selected_ == 2) || (selected_ == 5)) {
            auto &position = selected_ == 2 ? line.position_a : line.position_b;
            const auto index = selected_ == 2 ? line.a : line.b;
            const bool logical =
                coretx::detail::boolean_function(line.function) && (index >= switch_count);
            position =
                static_cast<SwitchPosition>(logical ? (position == SwitchPosition::down ? -1 : 1)
                                                    : change(static_cast<int>(position), -1, 7));
        } else if (selected_ == 3)
            line.inverted_a = !line.inverted_a;
        else if (selected_ == 6)
            line.inverted_b = !line.inverted_b;
        else if (selected_ == 7)
            line.threshold =
                static_cast<std::int16_t>(change(line.threshold, -resolution, resolution));
        else
            line.inverted = !line.inverted;
    }
    constexpr void render_logical_field(coretx::ui::Framebuffer &display, const std::size_t field,
                                        const int y, const bool ink) const noexcept {
        const auto &line = logical_draft_;
        if (field == 0)
            display.text(64, y,
                         menu_detail::logical_functions[static_cast<std::size_t>(line.function)],
                         ink);
        else if ((field == 1) || (field == 4)) {
            const auto index = field == 1 ? line.a : line.b;
            if (coretx::detail::boolean_function(line.function))
                menu_detail::condition(
                    display, 96, y,
                    {true, index, field == 1 ? line.position_a : line.position_b, false}, ink);
            else if (field == 1)
                coretx::ui::detail::draw_source(
                    display, 96, y,
                    {index < analog_count ? SourceKind::analog : SourceKind::switch_position,
                     static_cast<std::uint8_t>(index < analog_count ? index : index - analog_count),
                     false},
                    ink, &switch_names);
            else
                display.text(111, y, "--", ink);
        } else {
            const int value = field == 2   ? static_cast<int>(line.position_a)
                              : field == 3 ? line.inverted_a
                              : field == 5 ? static_cast<int>(line.position_b)
                              : field == 6 ? line.inverted_b
                              : field == 7 ? line.threshold
                                           : line.inverted;
            display.number(127, y, value, ink);
        }
    }
    [[nodiscard]] static constexpr std::size_t rf_field(const std::size_t row) noexcept {
        return row == 0 ? 0 : features::a7105 ? row : 3;
    }
    [[nodiscard]] constexpr std::size_t row_count() const noexcept {
        return screen_ == MenuScreen::root     ? 3
               : screen_ == MenuScreen::radio  ? menu_detail::external_module_row + 1
               : screen_ == MenuScreen::models ? models::count
               : screen_ == MenuScreen::setup  ? menu_detail::curves_row + 1 + CORETX_MODUL_TANKMIX
               : screen_ == MenuScreen::curves ? curve_count
               : screen_ == MenuScreen::trims  ? board::trim_buttons ? (trim_count * 2) : trim_count
               : screen_ == MenuScreen::multiswitch        ? multiswitch_page_count + 1
               : screen_ == MenuScreen::multiswitch_page   ? 9
               : screen_ == MenuScreen::multiswitch_button ? 5
               : (screen_ == MenuScreen::dashboard_channels) ||
                       (screen_ == MenuScreen::logical_switches)
                   ? 8
               : screen_ == MenuScreen::logical_editor ? menu_detail::logical_fields.size()
               : screen_ == MenuScreen::rf
                   ? 1 + (features::a7105 ? 2U : 0U) + (features::crsf ? 1U : 0U)
               : screen_ == MenuScreen::model_actions ? (can_initialize_ ? 6U : 5U)
                                                      : 2;
    }
    [[nodiscard]] constexpr const char *title() const noexcept {
        namespace tr = coretx::ui::tr;
        switch (screen_) {
        case MenuScreen::root:
            return tr::menu_title;
        case MenuScreen::models:
            return tr::menu_models;
        case MenuScreen::setup:
            return tr::menu_model_setup;
        case MenuScreen::radio:
            return tr::menu_radio_setup;
        case MenuScreen::rf:
            return tr::menu_radio_config;
        case MenuScreen::model_actions:
            return tr::menu_models;
        case MenuScreen::rename:
            return tr::menu_rename;
        case MenuScreen::delete_model:
            return tr::menu_delete;
        case MenuScreen::initialize_models:
            return tr::menu_initialize_title;
        case MenuScreen::editor:
            return editor_.page() == coretx::ui::Page::inputs   ? tr::menu_inputs
                   : editor_.page() == coretx::ui::Page::mixes  ? tr::menu_mixers
                   : editor_.page() == coretx::ui::Page::curves ? tr::menu_curves
#if CORETX_MODUL_TANKMIX
                   : editor_.page() == coretx::ui::Page::tank ? tr::menu_tank
#endif
                                                              : tr::menu_outputs;
        case MenuScreen::curves:
            return tr::menu_curves;
        case MenuScreen::dashboard_channels:
            return tr::menu_dashboard;
        case MenuScreen::logical_switches:
        case MenuScreen::logical_editor:
            return tr::menu_logical;
        case MenuScreen::multiswitch:
        case MenuScreen::multiswitch_page:
        case MenuScreen::multiswitch_button:
            return tr::menu_multiswitch;
        case MenuScreen::trims:
            return tr::menu_trims;
        default:
            return tr::menu_title;
        }
    }
    constexpr void render_editor(const Model &model, const Frame &frame,
                                 coretx::ui::Framebuffer &display) const noexcept {
        namespace tr = coretx::ui::tr;
        using menu_detail::row;
        const auto page = editor_.page();
#if CORETX_MODUL_TANKMIX
        if (page == coretx::ui::Page::tank) {
            const auto selected = editor_.selected_row(model);
            const auto first = selected >= menu_detail::visible_rows
                                   ? selected - menu_detail::visible_rows + 1
                                   : 0;
            for (std::size_t i = first;
                 (i < menu_detail::tank_fields.size()) && (i < first + menu_detail::visible_rows);
                 ++i) {
                const int y =
                    menu_detail::list_y + static_cast<int>(i - first) * menu_detail::row_height;
                const bool ink = i != selected;
                row(display, i, selected, first, menu_detail::tank_fields[i]);
                const int value = i == selected ? editor_.field_value(model)
                                                : coretx::ui::detail::tank_value(model.tank, i);
                if ((i == 0) || (i == 7))
                    display.text(104, y, value ? tr::menu_yes : tr::menu_no, ink);
                else if (i <= 4)
                    coretx::ui::detail::indexed_label(display, 104, y, i <= 2 ? "I" : "CH",
                                                      static_cast<std::size_t>(value), ink);
                else
                    display.number(127, y, value, ink);
                if (editor_.editing() && (i == selected))
                    display.small_text(96, y + 2, "*", ink);
            }
            return;
        }
#else
        (void)frame;
#endif
        if (page == coretx::ui::Page::curves) {
            editor_.render_curve(model, display, menu_detail::list_y, 40);
            return;
        }
        if (editor_.dialog() != coretx::ui::Dialog::none) {
            if (editor_.dialog() == coretx::ui::Dialog::actions) {
                const auto selected = static_cast<std::size_t>(editor_.action());
                const auto first = selected >= menu_detail::visible_rows
                                       ? selected - menu_detail::visible_rows + 1
                                       : 0;
                for (std::size_t i = first;
                     (i < menu_detail::actions.size()) && (i < (first + menu_detail::visible_rows));
                     ++i)
                    row(display, i, selected, first, menu_detail::actions[i]);
            } else {
                row(display, 0, editor_.confirm_delete() ? 1 : 0, 0, tr::menu_no);
                row(display, 1, editor_.confirm_delete() ? 1 : 0, 0, tr::menu_yes);
            }
            if (editor_.notice() != nullptr)
                display.small_text(64, 52, editor_.notice());
            return;
        }
        const auto selected = details_ ? editor_.selected_field() : editor_.selected_row(model);
        const auto count = details_
                               ? static_cast<std::size_t>(coretx::ui::detail::field_count(page))
                           : page == coretx::ui::Page::inputs ? model.inputs.size()
                           : page == coretx::ui::Page::mixes  ? model.mixes.size()
                                                              : channel_count;
        if (count == 0) {
            display.text(0, 16, tr::menu_empty);
            return;
        }
        const auto first =
            selected >= menu_detail::visible_rows ? selected - menu_detail::visible_rows + 1 : 0;
        for (std::size_t i = first; (i < count) && (i < (first + menu_detail::visible_rows)); ++i) {
            const int y =
                menu_detail::list_y + static_cast<int>(i - first) * menu_detail::row_height;
            const bool ink = i != selected;
            if (details_) {
                const auto label = page == coretx::ui::Page::inputs ? menu_detail::input_fields[i]
                                   : page == coretx::ui::Page::mixes
                                       ? menu_detail::mix_fields[i]
                                       : menu_detail::output_fields[i];
                row(display, i, selected, first, label);
                const int value = editor_.field_value(model, static_cast<std::uint8_t>(i));
                if ((page == coretx::ui::Page::outputs) && (i < 3))
                    menu_detail::percent(display, 127, y, value, ink);
                else if ((i == 0) && (page != coretx::ui::Page::outputs)) {
                    const auto source = page == coretx::ui::Page::inputs
                                            ? model.inputs[editor_.selected_row(model)].source
                                            : model.mixes[editor_.selected_row(model)].source;
                    coretx::ui::detail::draw_source(display, 98, y, source, ink, &switch_names);
                } else if ((page == coretx::ui::Page::mixes) && (i == 3))
                    display.text(116, y, value == 0 ? "+=" : value == 1 ? "*=" : ":=", ink);
                else if ((page == coretx::ui::Page::inputs) && (i == 4)) {
                    if (value < 2)
                        display.text(104, y, value == 0 ? "AUTO" : tr::radio_off, ink);
                    else
                        coretx::ui::detail::draw_source(
                            display, 104, y,
                            {SourceKind::analog, static_cast<std::uint8_t>(value - 2)}, ink,
                            &switch_names);
                } else if ((page == coretx::ui::Page::inputs) && (i == 5))
                    display.text(110, y, value == 0 ? "+/-" : value == 1 ? "-" : "+", ink);
                else if (((page == coretx::ui::Page::outputs) && (i == 3)) ||
                         ((page == coretx::ui::Page::inputs) && ((i == 6) || (i == 10))) ||
                         ((page == coretx::ui::Page::mixes) &&
                          ((i == 4) || (i == 5) || (i == 9)))) {
                    const std::string_view answer = value == 0 ? tr::menu_no : tr::menu_yes;
                    display.text(128 - static_cast<int>(answer.size()) * 6, y, answer, ink);
                } else if ((i ==
                            static_cast<std::size_t>(page == coretx::ui::Page::inputs ? 7 : 6)) &&
                           (page != coretx::ui::Page::outputs))
                    coretx::ui::detail::indexed_label(display, 104, y,
                                                      page == coretx::ui::Page::inputs ? "I" : "CH",
                                                      static_cast<std::size_t>(value), ink);
                else if ((i ==
                          static_cast<std::size_t>(page == coretx::ui::Page::inputs ? 8 : 7)) &&
                         (page != coretx::ui::Page::outputs)) {
                    if (value == 0)
                        display.text(110, y, tr::radio_off, ink);
                    else
                        coretx::ui::detail::draw_source(display, 104, y,
                                                        {SourceKind::switch_position,
                                                         static_cast<std::uint8_t>(value - 1),
                                                         false},
                                                        ink, &switch_names);
                } else if ((i ==
                            static_cast<std::size_t>(page == coretx::ui::Page::inputs ? 9 : 8)) &&
                           (page != coretx::ui::Page::outputs))
                    display.text(120, y, value < 0 ? "^" : value > 0 ? "v" : "-", ink);
                else if (((page == coretx::ui::Page::inputs) && (i == 11)) ||
                         ((page == coretx::ui::Page::mixes) && (i == 10))) {
                    if (value == 0)
                        display.text(110, y, tr::radio_off, ink);
                    else
                        coretx::ui::detail::indexed_label(display, 110, y, "C",
                                                          static_cast<std::size_t>(value - 1), ink);
                } else
                    display.number(127, y, value, ink);
                if (editor_.editing() && (i == selected))
                    display.small_text(88, y + 2, "*", ink);
            } else {
                row(display, i, selected, first, "");
                if (page == coretx::ui::Page::outputs) {
                    coretx::ui::detail::indexed_label(display, 0, y, "CH", i, ink);
                    const auto &out = model.outputs[i];
                    menu_detail::percent(display, 72, y, out.minimum_permille.value(), ink);
                    menu_detail::percent(display, 114, y, out.maximum_permille.value(), ink);
                    display.text(116, y, out.reversed ? "<-" : "->", ink);
                } else {
                    const auto draw = [&](const auto &line) constexpr {
                        coretx::ui::detail::indexed_label(
                            display, 0, y, page == coretx::ui::Page::inputs ? "I" : "CH",
                            line.destination, ink);
                        coretx::ui::detail::draw_source(display, 30, y, line.source, ink,
                                                        &switch_names);
                        display.number(92, y, line.weight_percent.value(), ink);
                        display.compact();
                        menu_detail::condition(display, 108, y + 2, line.condition, ink);
                        display.compact(false);
                    };
                    if (page == coretx::ui::Page::inputs) {
                        draw(model.inputs[i]);
                    } else {
                        const auto &line = model.mixes[i];
#if CORETX_MODUL_TANKMIX
                        if (line.role != MixRole::manual) {
                            coretx::ui::detail::indexed_label(display, 0, y, "CH", line.destination,
                                                              ink);
                            display.small_text(30, y + 2, menu_detail::tank_label(line.role), ink);
                            display.small_text(
                                104, y + 2, tank_mix_active(line.role, frame) ? "ON" : "OFF", ink);
                        } else
#endif
                        {
                            draw(line);
                            display.text(96, y,
                                         model.mixes[i].multiplex == Multiplex::add        ? "+="
                                         : model.mixes[i].multiplex == Multiplex::multiply ? "*="
                                                                                           : ":=",
                                         ink);
                        }
                    }
                }
            }
        }
    }
    coretx::ui::Interface editor_{};
    Controls dummy_{};
    models::Name name_{};
    LogicalSwitch logical_draft_{};
    LogicalSwitch logical_saved_field_{};
    std::array<char, 6> multiswitch_label_{};
    MultiSwitchControl multiswitch_control_{};
    std::uint8_t multiswitch_slot_{}, multiswitch_button_{}, multiswitch_cursor_{};
    const char *notice_{};
    std::size_t selected_{}, model_slot_{}, logical_slot_{};
    int draft_value_{};
    MenuScreen screen_{};
    bool details_{}, editing_{};
    bool can_initialize_{};
};

} // namespace coretx::firmware
