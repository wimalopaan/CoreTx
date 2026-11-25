// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "coretx/core/controls.hpp"

#include <array>
#include <string_view>

namespace coretx::ui {

// Display names of the switch sources; the board defines them because the auxiliary inputs differ.
using SwitchNames = std::array<std::string_view, switch_count>;

namespace detail {

// A switch condition is one signed choice of switch, position and negation, e.g. "SA^" or
// "!SA^": 0 is off, inverted conditions are negative. L and M only offer "L1"/"!L1".
inline constexpr int condition_positions =
    static_cast<int>(SwitchPosition::pos6) - static_cast<int>(SwitchPosition::up) + 1;

struct SwitchChoices {
    std::uint32_t switches{};
    std::uint16_t six_position{};
    std::uint32_t middle{};
    std::uint32_t multiswitch{};
    std::uint8_t logical{};
};

[[nodiscard]] constexpr bool binary_condition(const std::size_t index) noexcept {
    return logical_index(index) || multiswitch_index(index);
}

// L and M are active in position down; position up is shown as the negation.
[[nodiscard]] constexpr bool condition_negated(const Condition &condition) noexcept {
    return condition.inverted !=
           (binary_condition(condition.index) && (condition.position == SwitchPosition::up));
}

[[nodiscard]] constexpr int condition_choice(const Condition &condition) noexcept {
    if (!condition.enabled)
        return 0;
    const auto position =
        binary_condition(condition.index) ? SwitchPosition::down : condition.position;
    const int choice = int{condition.index} * condition_positions + static_cast<int>(position) + 2;
    return condition_negated(condition) ? -choice : choice;
}

[[nodiscard]] constexpr Condition select_condition(const int choice) noexcept {
    if (choice == 0)
        return {};
    const int code = (choice < 0 ? -choice : choice) - 1;
    return {true, static_cast<std::uint8_t>(code / condition_positions),
            static_cast<SwitchPosition>(code % condition_positions - 1), choice < 0};
}

[[nodiscard]] constexpr bool condition_available(const Condition &condition,
                                                 const SwitchChoices &choices) noexcept {
    const auto index = condition.index;
    const auto position = condition.position;
    const bool binary = (position == SwitchPosition::up) || (position == SwitchPosition::down);
    if (index < switch_count) {
        if ((choices.switches & (1U << index)) == 0)
            return false;
        if ((choices.six_position & (1U << index)) != 0)
            return (position >= SwitchPosition::pos1) && (position <= SwitchPosition::pos6);
        return binary ||
               ((position == SwitchPosition::middle) && ((choices.middle & (1U << index)) != 0));
    }
    if (logical_index(index))
        return (position == SwitchPosition::down) &&
               ((choices.logical & (1U << (index - switch_count))) != 0);
    return (position == SwitchPosition::down) && multiswitch_index(index) &&
           ((choices.multiswitch & (1U << (index - multiswitch_switch_base))) != 0);
}

// Steps to the next available choice for switch indices below indices; negated choices lie
// below "off" as in EdgeTX.
[[nodiscard]] constexpr int step_condition(const int choice, const int direction,
                                           const SwitchChoices &choices,
                                           const std::size_t indices) noexcept {
    const int maximum = static_cast<int>(indices) * condition_positions;
    int next = choice;
    for (;;) {
        const int candidate = next + direction < -maximum  ? -maximum
                              : next + direction > maximum ? maximum
                                                           : next + direction;
        if (candidate == next)
            return choice;
        next = candidate;
        if ((next == 0) || condition_available(select_condition(next), choices))
            return next;
    }
}

[[nodiscard]] constexpr std::string_view condition_label(const Condition &condition,
                                                         std::array<char, 6> &buffer,
                                                         const SwitchNames &names) noexcept {
    if (!condition.enabled)
        return {};
    std::size_t size{};
    if (condition_negated(condition))
        buffer[size++] = '!';
    if (logical_index(condition.index)) {
        buffer[size++] = 'L';
        buffer[size++] = static_cast<char>('1' + (condition.index - switch_count));
    } else if (multiswitch_index(condition.index)) {
        const auto index = condition.index - multiswitch_switch_base;
        buffer[size++] = 'M';
        buffer[size++] = static_cast<char>('1' + index / multiswitch_function_count);
        buffer[size++] = static_cast<char>('1' + index % multiswitch_function_count);
    } else if ((condition.index < switch_count) && !names[condition.index].empty() &&
               valid_switch_position(condition.position)) {
        for (const auto character : names[condition.index].substr(0, 3))
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

[[nodiscard]] constexpr std::string_view condition_label(const Condition &condition,
                                                         std::array<char, 6> &buffer,
                                                         const SwitchNames *const names) noexcept {
    constexpr SwitchNames unnamed{};
    return condition_label(condition, buffer, names != nullptr ? *names : unnamed);
}

} // namespace detail

class Framebuffer {
  public:
    static constexpr int width = 128;
    static constexpr int height = 64;

    constexpr void clear() noexcept;
    // Limit drawing, including clear(), to one eight-row LCD page.
    constexpr void clip_page(std::uint8_t page) noexcept;
    constexpr void reset_clip() noexcept { clipped_page_ = height / 8; }
    constexpr void compact(const bool enabled = true) noexcept { compact_ = enabled; }
    [[nodiscard]] constexpr int character_width() const noexcept { return compact_ ? 4 : 6; }
    [[nodiscard]] constexpr int character_height() const noexcept { return compact_ ? 6 : 10; }
    constexpr void small_text(int x, int y, const std::string_view &value,
                              bool ink = true) noexcept {
        const bool previous = compact_;
        compact_ = true;
        text(x, y, value, ink);
        compact_ = previous;
    }
    // Centers compact text within the touch zone [left, left + width).
    constexpr void small_text_centered(int left, int width, int y, const std::string_view &value,
                                       bool ink = true) noexcept {
        constexpr int compact_character = 4;
        small_text(left + (width - static_cast<int>(value.size()) * compact_character) / 2, y,
                   value, ink);
    }
    constexpr void pixel(int x, int y, bool ink = true) noexcept;
    [[nodiscard]] constexpr bool pixel_at(int x, int y) const noexcept;
    constexpr void rectangle(int x, int y, int width, int height, bool ink = true) noexcept;
    constexpr void invert(int x, int y, int width, int height) noexcept;
    constexpr void text(int x, int y, const std::string_view &value, bool ink = true) noexcept;
    constexpr void number(int right, int y, std::int32_t value, bool ink = true) noexcept;
    constexpr void unsigned_number(int right, int y, std::uint32_t value, bool ink = true) noexcept;
    [[nodiscard]] constexpr const auto &bytes() const noexcept { return pixels_; }

  private:
    enum class Fill : std::uint8_t { clear, set, invert };
    constexpr void fill(int x, int y, int w, int h, Fill mode) noexcept;
    constexpr void render_number(int right, int y, std::uint32_t magnitude, bool negative,
                                 bool ink) noexcept;
    // Display page order: byte x + (y / 8) * 128, bit y % 8.
    std::array<std::uint8_t, width * height / 8> pixels_{};
    bool compact_{};
    std::uint8_t clipped_page_{height / 8};
};

enum class Page : std::uint8_t {
    controls,
    inputs,
    mixes,
    outputs,
    monitor,
    curves
#if CORETX_MODUL_TANKMIX
    ,
    tank
#endif
};
inline constexpr std::size_t page_count = 6 + CORETX_MODUL_TANKMIX;
enum class Key : std::uint8_t {
    none,
    up,
    down,
    left,
    right,
    enter,
    menu,
    back,
    next_page,
    previous_page
};

enum class PointerKind : std::uint8_t { press, move, release, cancel };
enum class Dialog : std::uint8_t { none, actions, delete_line };
enum class LineAction : std::uint8_t { add, duplicate, erase, move_up, move_down };

struct PointerEvent {
    PointerKind kind{};
    std::int16_t x{-1};
    std::int16_t y{-1};
};

class Interface {
  public:
    constexpr void open(const Page page) noexcept {
        const auto names = switch_names_;
        *this = {};
        switch_names_ = names;
        page_ = page;
    }
    constexpr void set_switch_names(const SwitchNames &names) noexcept { switch_names_ = &names; }
    constexpr void open_curve(const std::uint8_t curve) noexcept {
        open(Page::curves);
        curve_slot_ = curve;
    }
    [[nodiscard]] constexpr std::uint8_t selected_curve() const noexcept { return curve_slot_; }
    [[nodiscard]] constexpr bool managed_mix(const Model &model) const noexcept {
#if CORETX_MODUL_TANKMIX
        return (page_ == Page::mixes) && !model.mixes.empty() &&
               (model.mixes[row(model)].role != MixRole::manual);
#else
        (void)model;
        return false;
#endif
    }
    constexpr void render_curve(const Model &model, Framebuffer &display, int top = 17,
                                int height = 30) const noexcept;
    [[nodiscard]] constexpr std::size_t selected_row(const Model &model) const noexcept {
        return row(model);
    }
    [[nodiscard]] constexpr std::uint8_t selected_field() const noexcept {
        return fields_[static_cast<std::size_t>(page_)];
    }
    [[nodiscard]] constexpr int field_value(const Model &model) const noexcept {
        return value(model);
    }
    [[nodiscard]] constexpr int field_value(const Model &model,
                                            const std::uint8_t field) const noexcept {
        return value(model, field);
    }
    [[nodiscard]] constexpr const char *notice() const noexcept { return notice_; }
    [[nodiscard]] constexpr LineAction action() const noexcept { return action_; }
    [[nodiscard]] constexpr bool confirm_delete() const noexcept { return confirm_delete_; }
    constexpr void set_sources(const std::uint16_t analogs, const std::uint32_t switches,
                               const std::uint16_t six_position,
                               const std::uint32_t multiswitch = 0,
                               const std::uint32_t three_position = ~0U) noexcept {
        analog_sources_ = analogs;
        switch_sources_ = switches;
        six_position_sources_ = six_position;
        multiswitch_sources_ = multiswitch;
        three_position_sources_ = three_position;
    }
    [[nodiscard]] constexpr bool take_changed() noexcept {
        const bool changed = model_changed_;
        model_changed_ = false;
        return changed;
    }
    constexpr void handle(const Key &key, Model &model, Controls &controls,
                          bool coarse = false) noexcept;
    constexpr void handle(const PointerEvent &event, Model &model, Controls &controls,
                          bool coarse = false) noexcept;
    constexpr void render(const Model &model, const Controls &controls,
                          const Evaluation &evaluation, Framebuffer &display) const noexcept;
    constexpr void handle(const Key &key, Model &model, RawControls &raw, ControlSetup &setup,
                          bool coarse = false) noexcept;
    constexpr void handle(const PointerEvent &event, Model &model, RawControls &raw,
                          ControlSetup &setup, bool coarse = false) noexcept;
    constexpr void observe(const RawControls &raw) noexcept;
    constexpr void render(const Model &model, const RawControls &raw,
                          const PreparedControls &prepared, const Evaluation &evaluation,
                          Framebuffer &display) const noexcept;

    [[nodiscard]] constexpr Page page() const noexcept { return page_; }
    [[nodiscard]] constexpr bool editing() const noexcept {
        return calibrating_ ? calibration_editing_ : editing_;
    }
    [[nodiscard]] constexpr Dialog dialog() const noexcept { return dialog_; }
    [[nodiscard]] constexpr bool calibrating() const noexcept { return calibrating_; }
    [[nodiscard]] constexpr PreparationStatus calibration_status() const noexcept {
        return calibration_status_;
    }

  private:
    [[nodiscard]] constexpr std::size_t row_count(const Model &model) const noexcept;
    [[nodiscard]] constexpr std::size_t row(const Model &model) const noexcept;
    [[nodiscard]] constexpr std::size_t first_row(const Model &model) const noexcept;
    constexpr void reveal_row(const Model &model) noexcept;
    [[nodiscard]] constexpr int value(const Model &model) const noexcept {
        return value(model, selected_field());
    }
    [[nodiscard]] constexpr int value(const Model &model, std::uint8_t field) const noexcept;
    constexpr void set_value(Model &model, int value) noexcept;
    constexpr void change(Model &model, int direction, bool coarse) noexcept;
    constexpr void begin_edit(const Model &model) noexcept;
    constexpr void cancel_edit(Model &model) noexcept;
    constexpr void commit_edit(Model &model) noexcept;
    constexpr void perform_action(Model &model) noexcept;
    [[nodiscard]] constexpr bool line_page() const noexcept;
    [[nodiscard]] constexpr detail::SwitchChoices switch_choices(const Model &model) const noexcept;
    [[nodiscard]] constexpr std::size_t control_analog_count() const noexcept {
        return raw_controls_ ? calibration_count : analog_count;
    }
    constexpr void adjust_control(Controls &controls, int direction, bool coarse) noexcept;
    template <typename State, typename... Context>
    constexpr void handle_pointer(const PointerEvent &event, Model &model, State &state,
                                  bool coarse, Context &...context) noexcept;
    constexpr void handle_calibration(const Key &key, RawControls &raw, ControlSetup &setup,
                                      bool coarse) noexcept;
    constexpr void render_calibration(const RawControls &raw, Framebuffer &display) const noexcept;

    Page page_{Page::controls};
    std::array<std::uint8_t, page_count> rows_{};
    std::array<std::uint8_t, page_count> first_rows_{};
    std::array<std::uint8_t, page_count> fields_{0, 0, 0, 0, 2, 1};
    std::uint8_t curve_slot_{};
#if CORETX_MODUL_TANKMIX
    TankMixer tank_draft_{};
#endif
    bool editing_{};
    bool model_changed_{};
    int saved_value_{};
    int pending_destination_{};
    Condition saved_condition_{};
    Dialog dialog_{};
    LineAction action_{};
    bool confirm_delete_{};
    const char *notice_{};
    bool pointer_active_{};
    bool pointer_dragged_{};
    std::int16_t pointer_x_{};
    std::int16_t pointer_y_{};
    std::int16_t scroll_y_{};
    ControlSetup calibration_draft_{};
    PreparationStatus calibration_status_{};
    std::uint8_t calibration_analog_{};
    std::uint8_t calibration_field_{};
    bool calibrating_{};
    bool calibration_editing_{};
    bool capturing_{};
    bool raw_controls_{};
    std::uint16_t analog_sources_{0xFFFF};
    std::uint32_t switch_sources_{(1U << switch_count) - 1U};
    std::uint16_t six_position_sources_{};
    std::uint32_t multiswitch_sources_{};
    std::uint32_t three_position_sources_{~0U};
    const SwitchNames *switch_names_{};
};

[[nodiscard]] constexpr Model simulator_model() noexcept;
[[nodiscard]] constexpr Controls simulator_controls() noexcept;
[[nodiscard]] constexpr ControlSetup simulator_setup() noexcept;
[[nodiscard]] constexpr RawControls simulator_raw_controls() noexcept;

} // namespace coretx::ui

#include "coretx/ui/detail/ui_impl.hpp"
