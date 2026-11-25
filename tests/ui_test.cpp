// SPDX-License-Identifier: GPL-2.0-or-later
#include "coretx/ui/ui.hpp"

#include <cstdio>
#include <limits>

namespace {

using coretx::ui::Dialog;
using coretx::ui::Framebuffer;
using coretx::ui::Interface;
using coretx::ui::Key;
using coretx::ui::LineAction;
using coretx::ui::Page;
using coretx::ui::PointerKind;
int failures{};

void check(const bool passed, const char *const description) {
    if (!passed) {
        std::fprintf(stderr, "FAILED: %s\n", description);
        ++failures;
    }
}

constexpr void press(Interface &ui, coretx::Model &model, coretx::Controls &controls,
                     const Key &key, const int times = 1, const bool coarse = false) {
    for (int i = 0; i < times; ++i) {
        ui.handle(key, model, controls, coarse);
    }
}

constexpr void go(Interface &ui, coretx::Model &model, coretx::Controls &controls,
                  const Page &page) {
    for (std::size_t i = 0; (ui.page() != page) && (i < coretx::ui::page_count); ++i) {
        ui.handle(Key::next_page, model, controls);
    }
}

constexpr void edit(Interface &ui, coretx::Model &model, coretx::Controls &controls,
                    const Key &direction, const int times = 1, const bool coarse = false) {
    press(ui, model, controls, Key::enter);
    press(ui, model, controls, direction, times, coarse);
    press(ui, model, controls, Key::enter);
}

constexpr void point(Interface &ui, coretx::Model &model, coretx::Controls &controls,
                     const PointerKind &kind, const int x = -1, const int y = -1) {
    ui.handle({kind, static_cast<std::int16_t>(x), static_cast<std::int16_t>(y)}, model, controls);
}

constexpr void tap(Interface &ui, coretx::Model &model, coretx::Controls &controls, const int x,
                   const int y) {
    point(ui, model, controls, PointerKind::press, x, y);
    point(ui, model, controls, PointerKind::release, x, y);
}

constexpr void swipe(Interface &ui, coretx::Model &model, coretx::Controls &controls,
                     const int from, const int to) {
    point(ui, model, controls, PointerKind::press, 20, from);
    point(ui, model, controls, PointerKind::move, 20, to);
    point(ui, model, controls, PointerKind::release, 20, to);
}

constexpr bool constant_editor() {
    auto model = coretx::ui::simulator_model();
    auto controls = coretx::ui::simulator_controls();
    Interface ui{};
    tap(ui, model, controls, 80, 56);
    go(ui, model, controls, Page::inputs);
    press(ui, model, controls, Key::right);
    edit(ui, model, controls, Key::left);
    press(ui, model, controls, Key::menu);
    press(ui, model, controls, Key::down);
    press(ui, model, controls, Key::enter);
    press(ui, model, controls, Key::right, 6);
    edit(ui, model, controls, Key::right);
    if ((model.inputs.size() != 9) || (model.inputs[0].weight_percent.value() != 99) ||
        (model.inputs[2].destination != 1) || (model.inputs[2].source.index != 0) ||
        !coretx::validate(model)) {
        return false;
    }
    Framebuffer display{};
    const auto evaluation = coretx::evaluate(model, controls);
    ui.render(model, controls, evaluation, display);
    if (!evaluation || (display.bytes()[0] == 0)) {
        return false;
    }
    press(ui, model, controls, Key::menu);
    press(ui, model, controls, Key::down, 2);
    press(ui, model, controls, Key::enter);
    tap(ui, model, controls, 80, 56);
    tap(ui, model, controls, 110, 56);
    return (model.inputs.size() == 8) && (ui.dialog() == Dialog::none) &&
           static_cast<bool>(coretx::validate(model));
}

static_assert(constant_editor());

void framebuffer() {
    Framebuffer display{};
    display.pixel(0, 7);
    display.pixel(0, 8);
    display.pixel(127, 63);
    display.pixel(-1, 0);
    display.pixel(128, 0);
    display.pixel(0, 64);
    check((display.bytes()[0] == 128) && (display.bytes()[128] == 1) &&
              (display.bytes()[1023] == 128),
          "LCD page layout and clipped pixels");
    display.rectangle(-2, -2, 4, 4);
    display.rectangle(127, 63, 8, 8, false);
    check(display.pixel_at(1, 1) && !display.pixel_at(2, 2) && !display.pixel_at(127, 63),
          "clipped rectangle drawing and erasing");
    display.clear();
    display.number(126, 8, std::numeric_limits<std::int32_t>::min());
    Framebuffer expected{};
    expected.text(60, 8, "-2147483648");
    check(display.bytes() == expected.bytes(), "minimum signed integer formatting");
    display.clear();
    expected.clear();
    display.unsigned_number(126, 8, std::numeric_limits<std::uint32_t>::max());
    expected.text(66, 8, "4294967295");
    check(display.bytes() == expected.bytes(), "maximum unsigned watchdog counter formatting");
}

bool text_matches_pixel_rendering() {
    constexpr std::array<std::array<int, 2>, 12> positions{{{0, 0},
                                                            {7, 7},
                                                            {4, 8},
                                                            {-3, 5},
                                                            {124, 9},
                                                            {127, 63},
                                                            {3, -6},
                                                            {3, -1},
                                                            {3, 58},
                                                            {3, 64},
                                                            {128, 0},
                                                            {3, -7}}};
    for (const bool ink : {false, true}) {
        Framebuffer background{};
        background.pixel(10, 8);
        if (!ink)
            background.rectangle(1, 1, 126, 62);
        for (int code = 0; code < 256; ++code) {
            const std::array<char, 2> text{static_cast<char>(code), 'A'};
            for (const auto &position : positions) {
                auto actual = background;
                auto expected = background;
                actual.text(position[0], position[1], {text.data(), text.size()}, ink);
                auto x = position[0];
                for (const char ch : text) {
                    if (x >= Framebuffer::width)
                        break;
                    const auto rows = coretx::ui::detail::glyph(ch);
                    for (int row = 0; row < 9; ++row)
                        for (int column = 0; column < 5; ++column)
                            if ((rows[static_cast<std::size_t>(row)] & (1U << (4 - column))) != 0)
                                expected.pixel(x + column, position[1] + row, ink);
                    x += 6;
                }
                if (actual.bytes() != expected.bytes())
                    return false;
            }
        }
    }
    return true;
}

bool compact_text_matches_pixel_rendering() {
    for (const bool ink : {false, true}) {
        Framebuffer background{};
        background.rectangle(0, 0, 128, 64, !ink);
        for (int code = 0; code < 256; ++code) {
            const std::array<char, 2> text{static_cast<char>(code), 'A'};
            for (const int x : {-3, 0, 7, 125, 127, 128}) {
                for (int y = -7; y <= 64; ++y) {
                    auto actual = background;
                    auto expected = background;
                    actual.small_text(x, y, {text.data(), text.size()}, ink);
                    auto column = x;
                    for (const char ch : text) {
                        if (column >= Framebuffer::width)
                            break;
                        const auto glyph = coretx::ui::detail::micro_glyph(ch);
                        for (int col = 0; col < 3; ++col)
                            for (int row = 0; row < 6; ++row)
                                if ((glyph[static_cast<std::size_t>(col)] & (1U << row)) != 0)
                                    expected.pixel(column + col, y + row, ink);
                        column += 4;
                    }
                    if (actual.bytes() != expected.bytes())
                        return false;
                }
            }
        }
    }
    return true;
}

bool rectangles_match_pixel_rendering() {
    Framebuffer background{};
    for (int y = 0; y < Framebuffer::height; ++y)
        for (int x = 0; x < Framebuffer::width; ++x)
            background.pixel(x, y, ((x + y) % 3) == 0);
    for (const bool ink : {false, true})
        for (const int x : {-2, 0, 1, 127, 128})
            for (const int w : {-1, 0, 1, 2, 128, 130})
                for (const int y : {-10, -1, 0, 1, 7, 8, 9, 15, 55, 56, 57, 63, 64})
                    for (const int h : {-1, 0, 1, 2, 7, 8, 9, 16, 64, 80}) {
                        auto actual = background;
                        auto expected = background;
                        actual.rectangle(x, y, w, h, ink);
                        for (int row = std::max(y, 0); row < std::min(y + h, 64); ++row)
                            for (int column = std::max(x, 0); column < std::min(x + w, 128);
                                 ++column)
                                expected.pixel(column, row, ink);
                        if (actual.bytes() != expected.bytes())
                            return false;
                    }
    return true;
}

bool clipped_frames_match_full_rendering() {
    const auto draw = [](Framebuffer &display) {
        display.clear();
        display.rectangle(-2, -3, 7, 20);
        display.rectangle(8, 5, 110, 49);
        display.rectangle(9, 7, 108, 45, false);
        display.text(-3, -1, "ABCDEFGHIJKLMN");
        display.text(3, 7, "Page crossings", false);
        display.text(110, 57, "Clipped");
        display.small_text(2, 15, "Compact page crossings");
        display.small_text(2, 23, "Erasing compact text", false);
        display.number(127, 33, -123456);
        display.small_text(1, 61, "Bottom clip");
        display.pixel(127, 63);
    };
    Framebuffer full{}, staged{};
    draw(full);
    staged.rectangle(0, 0, 128, 64);
    for (std::uint8_t page = 0; page < 8; ++page) {
        const auto previous = staged;
        staged.clip_page(page);
        draw(staged);
        for (std::size_t i = 0; i < staged.bytes().size(); ++i) {
            const auto expected = (i / 128) == page ? full.bytes()[i] : previous.bytes()[i];
            if (staged.bytes()[i] != expected)
                return false;
        }
        staged.reset_clip();
    }
    if (staged.bytes() != full.bytes())
        return false;
    staged.clear();
    for (const auto byte : staged.bytes())
        if (byte != 0)
            return false;
    return true;
}

void controls() {
    auto model = coretx::ui::simulator_model();
    auto state = coretx::ui::simulator_controls();
    Interface ui{};
    press(ui, model, state, Key::right, 20, true);
    check(state.analogs[0] == 1024, "analog upper clamp");
    press(ui, model, state, Key::left, 40, true);
    check(state.analogs[0] == -1024, "analog lower clamp");
    press(ui, model, state, Key::enter);
    check(state.analogs[0] == 0, "analog reset");
    press(ui, model, state, Key::down, 2);
    state.analogs[2] = 123;
    press(ui, model, state, Key::enter);
    check(state.analogs[2] == -1024, "throttle reset is low");
    press(ui, model, state, Key::down, static_cast<int>(coretx::analog_count) - 2);
    press(ui, model, state, Key::right, 4);
    check(state.switches[0] == coretx::SwitchPosition::down, "switch upper clamp after scrolling");
    press(ui, model, state, Key::left, 4);
    check(state.switches[0] == coretx::SwitchPosition::up, "switch lower clamp");
    press(ui, model, state, Key::enter);
    press(ui, model, state, Key::down, static_cast<int>(coretx::switch_count));
    press(ui, model, state, Key::right, 32, true);
    check(state.trims[0].value() == 127, "trim upper clamp");
    press(ui, model, state, Key::left, 64, true);
    check(state.trims[0].value() == -127, "trim lower clamp");
    press(ui, model, state, Key::enter);
    check((state.trims[0].value() == 0) && (state.switches[0] == coretx::SwitchPosition::middle),
          "trim and switch reset");
}

void inputs() {
    auto model = coretx::ui::simulator_model();
    auto state = coretx::ui::simulator_controls();
    state.analogs[0] = 1024;
    Interface ui{};
    go(ui, model, state, Page::inputs);
    press(ui, model, state, Key::right);
    press(ui, model, state, Key::enter);
    press(ui, model, state, Key::left, 10, true);
    check(coretx::evaluate(model, state).frame.channels[0] == 0,
          "input edit affects live pipeline");
    press(ui, model, state, Key::back);
    check((model.inputs[0].weight_percent.value() == 100) && !ui.editing(),
          "cancel restores value and leaves edit mode");
    edit(ui, model, state, Key::left, 5, true);
    check(coretx::evaluate(model, state).frame.channels[0] == 512, "committed input weight");
    press(ui, model, state, Key::right);
    press(ui, model, state, Key::enter);
    press(ui, model, state, Key::right);
    press(ui, model, state, Key::next_page);
    check((model.inputs[0].expo_percent.value() == 30) && !ui.editing(),
          "page change cancels pending edit");
    press(ui, model, state, Key::previous_page);
    press(ui, model, state, Key::left, 2);
    edit(ui, model, state, Key::right,
         static_cast<int>(coretx::analog_count + coretx::switch_count + coretx::input_count));
    check(model.inputs[0].source.kind == coretx::SourceKind::maximum,
          "input source selector excludes virtual inputs");
    edit(ui, model, state, Key::left);
    check((model.inputs[0].source.kind == coretx::SourceKind::switch_position) &&
              (model.inputs[0].source.index == (coretx::switch_count - 1)),
          "input switch source selection");
}

void mixers() {
    auto model = coretx::ui::simulator_model();
    auto state = coretx::ui::simulator_controls();
    Interface ui{};
    go(ui, model, state, Page::mixes);
    edit(ui, model, state, Key::right, 30);
    check((model.mixes[0].source.kind == coretx::SourceKind::input) &&
              (model.mixes[0].source.index == 7),
          "mixer source reaches last virtual input");
    edit(ui, model, state, Key::left,
         static_cast<int>(coretx::analog_count + coretx::switch_count + coretx::input_count));
    check((model.mixes[0].source.kind == coretx::SourceKind::analog) &&
              (model.mixes[0].source.index == 0),
          "mixer source lower bound");
    press(ui, model, state, Key::right);
    edit(ui, model, state, Key::right, 100, true);
    check(model.mixes[0].weight_percent.value() == 500, "mixer extended weight clamp");
    press(ui, model, state, Key::right, 2);
    edit(ui, model, state, Key::right, 5);
    check(model.mixes[0].multiplex == coretx::Multiplex::replace, "multiplex selector");
    press(ui, model, state, Key::right);
    edit(ui, model, state, Key::left);
    press(ui, model, state, Key::right);
    edit(ui, model, state, Key::right);
    state.analogs[0] = 1024;
    state.trims[0] = coretx::TrimSteps::constant<127>();
    check(!model.mixes[0].carry_trim && model.mixes[0].source.inverted &&
              (coretx::evaluate(model, state).frame.channels[0] == -1024),
          "mixer flags applied to live output");
}

void outputs() {
    auto model = coretx::ui::simulator_model();
    auto state = coretx::ui::simulator_controls();
    Interface ui{};
    go(ui, model, state, Page::outputs);
    press(ui, model, state, Key::enter);
    press(ui, model, state, Key::right, 40, true);
    check(model.outputs[0].minimum_permille.value() == model.outputs[0].maximum_permille.value(),
          "minimum does not cross maximum");
    press(ui, model, state, Key::back);
    edit(ui, model, state, Key::right, 10, true);
    press(ui, model, state, Key::right);
    edit(ui, model, state, Key::left, 40, true);
    check((model.outputs[0].maximum_permille.value() == 0) &&
              static_cast<bool>(coretx::validate(model)),
          "maximum does not cross minimum");

    Interface scrolling{};
    go(scrolling, model, state, Page::outputs);
    press(scrolling, model, state, Key::down, 30);
    edit(scrolling, model, state, Key::right);
    check((model.outputs[15].minimum_permille.value() == -990) &&
              (model.outputs[14].minimum_permille.value() == -1000),
          "scrolling and row bound reach last output");
}

void touch_editing() {
    auto model = coretx::ui::simulator_model();
    auto state = coretx::ui::simulator_controls();
    Interface ui{};
    tap(ui, model, state, 20, 28);
    tap(ui, model, state, 80, 56);
    check((state.analogs[1] == 16) && (state.analogs[0] == 0),
          "touch selects a control without resetting or changing it");
    tap(ui, model, state, 110, 56);
    check(state.analogs[1] == 0, "touch reset button");
    state.analogs[0] = 1024;
    tap(ui, model, state, 20, 8);
    tap(ui, model, state, 80, 8);
    tap(ui, model, state, 110, 20);
    point(ui, model, state, PointerKind::press, 48, 56);
    check(model.inputs[0].weight_percent.value() == 100, "button activates on release, not press");
    point(ui, model, state, PointerKind::release, 48, 56);
    check(ui.editing() && (model.inputs[0].weight_percent.value() == 99) &&
              (coretx::evaluate(model, state).frame.channels[0] < 1024),
          "touch field edit changes live calculation");
    tap(ui, model, state, 110, 28);
    tap(ui, model, state, 80, 8);
    swipe(ui, model, state, 44, 20);
    tap(ui, model, state, 48, 56);
    check((model.inputs[0].weight_percent.value() == 98) &&
              (model.inputs[1].weight_percent.value() == 100),
          "pending edit locks row, field and scrolling");
    tap(ui, model, state, 16, 56);
    check(!ui.editing() && (ui.page() == Page::inputs) &&
              (model.inputs[0].weight_percent.value() == 100),
          "touch back cancels instead of changing page while editing");
    tap(ui, model, state, 110, 20);
    tap(ui, model, state, 48, 56);
    tap(ui, model, state, 110, 56);
    check(!ui.editing() && (model.inputs[0].weight_percent.value() == 99), "touch edit commit");
    tap(ui, model, state, 110, 28);
    tap(ui, model, state, 48, 56);
    tap(ui, model, state, 20, 8);
    check((ui.page() == Page::mixes) && !ui.editing() &&
              (model.inputs[1].weight_percent.value() == 100),
          "touch page change restores uncommitted value");
    tap(ui, model, state, 16, 56);
    check(ui.page() == Page::inputs, "touch back changes page outside editing");
}

void touch_scrolling() {
    auto model = coretx::ui::simulator_model();
    auto state = coretx::ui::simulator_controls();
    Interface ui{};
    go(ui, model, state, Page::outputs);
    for (int i = 0; i < 10; ++i) {
        swipe(ui, model, state, 44, 20);
    }
    tap(ui, model, state, 110, 44);
    tap(ui, model, state, 80, 56);
    tap(ui, model, state, 110, 56);
    check((model.outputs[15].minimum_permille.value() == -990) &&
              (model.outputs[14].minimum_permille.value() == -1000),
          "swipes scroll and clamp viewport to last output");
    press(ui, model, state, Key::up);
    edit(ui, model, state, Key::right);
    check(model.outputs[14].minimum_permille.value() == -990,
          "keyboard navigation after touch scrolling");
    for (int i = 0; i < 10; ++i) {
        swipe(ui, model, state, 20, 44);
    }
    tap(ui, model, state, 110, 20);
    tap(ui, model, state, 80, 56);
    tap(ui, model, state, 110, 56);
    check((model.outputs[0].minimum_permille.value() == -990) &&
              (model.outputs[1].minimum_permille.value() == -1000),
          "reverse swipes return to first output without accidental edits");
    go(ui, model, state, Page::monitor);
    tap(ui, model, state, 110, 20);
    check(!ui.editing(), "monitor values remain read-only by touch");
    tap(ui, model, state, 110, 56);
    check(ui.page() == Page::curves, "monitor touch page button opens curves");
    press(ui, model, state, Key::next_page);

    model.inputs.resize<0>();
    go(ui, model, state, Page::inputs);
    swipe(ui, model, state, 44, 20);
    tap(ui, model, state, 110, 20);
    tap(ui, model, state, 110, 56);
    check(!ui.editing() && model.inputs.empty(), "touch on empty list is safe");
}

void touch_cancellation() {
    auto model = coretx::ui::simulator_model();
    auto state = coretx::ui::simulator_controls();
    Interface ui{};
    point(ui, model, state, PointerKind::release, 80, 56);
    point(ui, model, state, PointerKind::press, 80, 56);
    point(ui, model, state, PointerKind::cancel);
    point(ui, model, state, PointerKind::release, 80, 56);
    point(ui, model, state, PointerKind::press, 80, 56);
    point(ui, model, state, PointerKind::move, 128, 56);
    point(ui, model, state, PointerKind::release, 80, 56);
    point(ui, model, state, PointerKind::press, 80, 56);
    point(ui, model, state, PointerKind::move, 80, 44);
    point(ui, model, state, PointerKind::move, 80, 56);
    point(ui, model, state, PointerKind::release, 80, 56);
    point(ui, model, state, PointerKind::press, 63, 56);
    point(ui, model, state, PointerKind::release, 64, 56);
    point(ui, model, state, PointerKind::press, 63, 56);
    point(ui, model, state, PointerKind::move, 64, 56);
    point(ui, model, state, PointerKind::release, 63, 56);
    check(state.analogs[0] == 0, "unmatched release, cancel, leaving screen/button and crossing "
                                 "button boundaries do not click");
    point(ui, model, state, PointerKind::press, 80, 56);
    point(ui, model, state, PointerKind::move, 82, 57);
    ui.handle(Key::none, model, state);
    point(ui, model, state, PointerKind::release, 81, 57);
    check(state.analogs[0] == 16, "small touch jitter and unrelated keys preserve click");
    point(ui, model, state, PointerKind::press, 80, 56);
    press(ui, model, state, Key::next_page);
    point(ui, model, state, PointerKind::release, 80, 56);
    tap(ui, model, state, 110, 20);
    tap(ui, model, state, 48, 56);
    tap(ui, model, state, 110, 56);
    check((model.inputs[0].source.kind == coretx::SourceKind::analog) &&
              (model.inputs[0].source.index == 0) &&
              (model.inputs[0].weight_percent.value() == 100),
          "keyboard action cancels a stale pointer release");
}

bool same_region(const Framebuffer &a, const Framebuffer &b, const int x, const int y, const int w,
                 const int h) {
    for (int row = y; row < (y + h); ++row) {
        for (int column = x; column < (x + w); ++column) {
            if (a.pixel_at(column, row) != b.pixel_at(column, row)) {
                return false;
            }
        }
    }
    return true;
}

void action(Interface &ui, coretx::Model &model, coretx::Controls &state,
            const LineAction &selected) {
    if (ui.dialog() != Dialog::none) {
        press(ui, model, state, Key::menu);
    }
    press(ui, model, state, Key::menu);
    press(ui, model, state, Key::down, static_cast<int>(selected));
    press(ui, model, state, Key::enter);
    check(static_cast<bool>(coretx::validate(model)), "line action preserves valid model");
}

bool shows_notice(Interface &ui, const coretx::Model &model, const coretx::Controls &state,
                  const char *const message) {
    Framebuffer display{};
    ui.render(model, state, coretx::evaluate(model, state), display);
    Framebuffer expected{};
    expected.text(1, 40, message);
    return same_region(display, expected, 0, 40, 128, 7);
}

template <typename Line, std::size_t Capacity>
void line_lifecycle(coretx::Model &model, coretx::FixedVector<Line, Capacity> &lines,
                    const Page &page) {
    auto state = coretx::ui::simulator_controls();
    Interface ui{};
    go(ui, model, state, page);
    action(ui, model, state, LineAction::duplicate);
    check(lines.empty() && shows_notice(ui, model, state, "NO LINE"),
          "duplicating empty list reports an error");
    action(ui, model, state, LineAction::add);
    check((lines.size() == 1) && (lines[0].weight_percent.value() == 0),
          "empty list supports adding a zero-weight line");
    check(lines[0].weight_percent.try_set(37), "set valid line weight");
    lines[0].source.inverted = true;
    lines[0].condition = {true, 3, coretx::SwitchPosition::middle, true};
    action(ui, model, state, LineAction::duplicate);
    check((lines.size() == 2) && (lines[1].weight_percent.value() == 37) &&
              lines[1].source.inverted && lines[1].condition.enabled &&
              (lines[1].condition.index == 3) &&
              (lines[1].condition.position == coretx::SwitchPosition::middle) &&
              lines[1].condition.inverted,
          "duplicate copies settings and condition next to original");
    action(ui, model, state, LineAction::erase);
    check((lines.size() == 2) && (ui.dialog() == Dialog::delete_line),
          "delete requires confirmation");
    press(ui, model, state, Key::enter);
    check((lines.size() == 2) && (ui.dialog() == Dialog::actions), "delete defaults to NO");
    action(ui, model, state, LineAction::erase);
    press(ui, model, state, Key::right);
    press(ui, model, state, Key::back);
    check(lines.size() == 2, "back cancels a selected YES");
    action(ui, model, state, LineAction::erase);
    press(ui, model, state, Key::right);
    press(ui, model, state, Key::enter);
    check((lines.size() == 1) && (ui.dialog() == Dialog::none), "confirmed deletion");
    for (std::size_t i = 1; i < Capacity; ++i) {
        action(ui, model, state, LineAction::duplicate);
    }
    action(ui, model, state, LineAction::add);
    check((lines.size() == Capacity) && shows_notice(ui, model, state, "LIST FULL"),
          "adding beyond capacity reports full list");
    action(ui, model, state, LineAction::duplicate);
    check((lines.size() == Capacity) && shows_notice(ui, model, state, "LIST FULL"),
          "duplicating beyond capacity does not overwrite lines");
    for (std::size_t i = 0; i < Capacity; ++i) {
        action(ui, model, state, LineAction::erase);
        press(ui, model, state, Key::right);
        press(ui, model, state, Key::enter);
        check(static_cast<bool>(coretx::validate(model)), "deletion maintains valid model");
    }
    check(lines.empty() && (ui.dialog() == Dialog::none), "last line can be deleted");
    action(ui, model, state, LineAction::add);
    check((lines.size() == 1) && (lines[0].weight_percent.value() == 0) &&
              !lines[0].condition.enabled,
          "adding after deleting all lines starts with clean defaults");
}

void line_actions() {
    coretx::Model model{};
    model.mixes.resize<1>();
    auto neutral_controls = coretx::ui::simulator_controls();
    neutral_controls.trims[0] = coretx::TrimSteps::constant<127>();
    Interface neutral_ui{};
    go(neutral_ui, model, neutral_controls, Page::inputs);
    action(neutral_ui, model, neutral_controls, LineAction::add);
    check((coretx::evaluate(model, neutral_controls).frame.channels[0] == 0) &&
              (model.inputs[0].trim == coretx::TrimSource::none),
          "new zero-weight input does not inject independent stick trim");
    model = {};
    line_lifecycle(model, model.inputs, Page::inputs);
    line_lifecycle(model, model.mixes, Page::mixes);

    model = {};
    model.inputs.resize<3>();
    model.inputs[0].source.kind = coretx::SourceKind::maximum;
    model.inputs[1].source.kind = coretx::SourceKind::maximum;
    model.inputs[1].weight_percent = coretx::InputWeight::constant<50>();
    model.inputs[2].destination = 1;
    auto state = coretx::ui::simulator_controls();
    Interface inputs_ui{};
    go(inputs_ui, model, state, Page::inputs);
    press(inputs_ui, model, state, Key::down);
    action(inputs_ui, model, state, LineAction::move_up);
    check(coretx::evaluate(model, state).frame.inputs[0] == 512,
          "moving input line changes first-matching priority");
    action(inputs_ui, model, state, LineAction::move_up);
    check(shows_notice(inputs_ui, model, state, "FIRST IN TARGET"),
          "cannot move input above target group");
    action(inputs_ui, model, state, LineAction::move_down);
    check(coretx::evaluate(model, state).frame.inputs[0] == 1024,
          "moving input back restores priority");
    action(inputs_ui, model, state, LineAction::move_down);
    check(shows_notice(inputs_ui, model, state, "LAST IN TARGET") &&
              (model.inputs[2].destination == 1),
          "cannot move into another target group");

    model = {};
    model.mixes.resize<3>();
    model.mixes[0].source.kind = coretx::SourceKind::maximum;
    model.mixes[0].weight_percent = coretx::MixWeight::constant<50>();
    model.mixes[1] = model.mixes[0];
    model.mixes[1].multiplex = coretx::Multiplex::multiply;
    model.mixes[2].destination = 1;
    Interface mixes_ui{};
    go(mixes_ui, model, state, Page::mixes);
    check(coretx::evaluate(model, state).frame.channels[0] == 256, "ordered Add/Multiply setup");
    press(mixes_ui, model, state, Key::down);
    action(mixes_ui, model, state, LineAction::move_up);
    check(coretx::evaluate(model, state).frame.channels[0] == 1024,
          "moving Multiply first uses first-configured-line semantics");
    action(mixes_ui, model, state, LineAction::move_down);
    check(coretx::evaluate(model, state).frame.channels[0] == 256,
          "moving Multiply back restores output");
}

template <typename Line, std::size_t Capacity>
void target_edits(coretx::Model &model, coretx::FixedVector<Line, Capacity> &lines,
                  const Page &page) {
    lines.template resize<4>();
    constexpr std::array<std::uint8_t, 4> destinations{0, 0, 1, 3};
    constexpr std::array<std::int16_t, 4> weights{11, 12, 21, 31};
    for (std::size_t i = 0; i < 4; ++i) {
        lines[i].destination = destinations[i];
        check(lines[i].weight_percent.try_set(weights[i]), "set valid target-edit weight");
    }
    auto state = coretx::ui::simulator_controls();
    Interface ui{};
    go(ui, model, state, page);
    press(ui, model, state, Key::right, page == Page::inputs ? 7 : 6);
    press(ui, model, state, Key::enter);
    for (int i = 0; i < 3; ++i) {
        press(ui, model, state, Key::right);
        check((lines[0].destination == 0) && static_cast<bool>(coretx::validate(model)),
              "pending target is not published as unsorted model");
    }
    press(ui, model, state, Key::back);
    check((lines[0].weight_percent.value() == 11) && (lines[1].weight_percent.value() == 12) &&
              (lines[0].destination == 0),
          "target cancellation preserves exact priority");
    edit(ui, model, state, Key::right);
    check((lines[0].weight_percent.value() == 12) && (lines[1].weight_percent.value() == 21) &&
              (lines[2].weight_percent.value() == 11) && (lines[2].destination == 1),
          "target commit appends selected line to destination group");
    edit(ui, model, state, Key::left);
    check((lines[1].weight_percent.value() == 11) && (lines[1].destination == 0),
          "selection follows relocated line in both directions");
    press(ui, model, state, Key::enter, 2);
    check((lines[0].weight_percent.value() == 12) && (lines[1].weight_percent.value() == 11),
          "committing unchanged target does not alter priority");
    edit(ui, model, state, Key::right, 30);
    check((lines[3].weight_percent.value() == 11) &&
              (lines[3].destination == (page == Page::inputs ? 7 : 15)) &&
              static_cast<bool>(coretx::validate(model)),
          "target upper bound and move past last group");
}

void targets_and_conditions() {
    {
        coretx::Model model{};
        target_edits(model, model.inputs, Page::inputs);
        target_edits(model, model.mixes, Page::mixes);
    }
    for (const auto page : {Page::inputs, Page::mixes}) {
        coretx::Model model{};
        model.inputs.resize<1>();
        model.mixes.resize<1>();
        model.inputs[0].source.kind = coretx::SourceKind::maximum;
        auto &condition =
            page == Page::inputs ? model.inputs[0].condition : model.mixes[0].condition;
        condition = {false, 3, coretx::SwitchPosition::middle, true};
        auto state = coretx::ui::simulator_controls();
        Interface ui{};
        go(ui, model, state, page);
        press(ui, model, state, Key::right, page == Page::inputs ? 8 : 7);
        press(ui, model, state, Key::enter);
        press(ui, model, state, Key::right, 10);
        press(ui, model, state, Key::back);
        check(!condition.enabled && (condition.index == 3) && condition.inverted &&
                  (condition.position == coretx::SwitchPosition::middle),
              "cancelling switch selection restores disabled condition details");
        condition = {};
        edit(ui, model, state, Key::right);
        state.switches[0] = coretx::SwitchPosition::up;
        check(coretx::evaluate(model, state).frame.channels[0] == 1024,
              "switch-enabled input or mix passes matching controls");
        state.switches[0] = coretx::SwitchPosition::down;
        check(coretx::evaluate(model, state).frame.channels[0] == 0,
              "switch condition suppresses nonmatching line");
        press(ui, model, state, Key::right);
        edit(ui, model, state, Key::right);
        state.switches[0] = coretx::SwitchPosition::middle;
        check(coretx::evaluate(model, state).frame.channels[0] == 1024,
              "switch position editor selects middle");
        press(ui, model, state, Key::right);
        edit(ui, model, state, Key::right);
        check(coretx::evaluate(model, state).frame.channels[0] == 0, "condition inversion editor");
        press(ui, model, state, Key::left, 2);
        edit(ui, model, state, Key::left);
        check(!condition.enabled && (coretx::evaluate(model, state).frame.channels[0] == 1024),
              "OFF condition is unconditional");
        press(ui, model, state, Key::left);
        edit(ui, model, state, Key::right, 30);
        check(static_cast<bool>(coretx::validate(model)),
              "single-line target edit keeps model valid");
    }
}

void touch_actions() {
    coretx::Model model{};
    auto state = coretx::ui::simulator_controls();
    Interface ui{};
    go(ui, model, state, Page::inputs);
    tap(ui, model, state, 110, 56);
    check(ui.dialog() == Dialog::actions, "touch MENU available on empty page");
    tap(ui, model, state, 110, 56);
    check((model.inputs.size() == 1) && (ui.dialog() == Dialog::none), "touch adds first line");
    tap(ui, model, state, 110, 56);
    tap(ui, model, state, 80, 56);
    tap(ui, model, state, 110, 56);
    check(model.inputs.size() == 2, "touch duplicates selected line");
    tap(ui, model, state, 110, 56);
    tap(ui, model, state, 80, 56);
    tap(ui, model, state, 80, 56);
    tap(ui, model, state, 110, 56);
    check((model.inputs.size() == 2) && (ui.dialog() == Dialog::delete_line),
          "touch delete opens confirmation");
    tap(ui, model, state, 110, 56);
    check((model.inputs.size() == 2) && (ui.dialog() == Dialog::actions),
          "touch confirmation defaults to keeping line");
    tap(ui, model, state, 110, 56);
    tap(ui, model, state, 80, 56);
    tap(ui, model, state, 110, 56);
    check((model.inputs.size() == 1) && (ui.dialog() == Dialog::none), "touch YES then OK deletes");
    tap(ui, model, state, 110, 56);
    press(ui, model, state, Key::next_page);
    check((ui.page() == Page::mixes) && (ui.dialog() == Dialog::none),
          "page change closes modal without stale actions");
    model.mixes.resize<1>();
    model.mixes[0].destination = static_cast<std::uint8_t>(coretx::channel_count);
    press(ui, model, state, Key::right, 6);
    press(ui, model, state, Key::enter);
    check(!ui.editing() && (ui.dialog() == Dialog::actions) &&
              shows_notice(ui, model, state, "INVALID MODEL"),
          "invalid external model cannot enter target editing");
}

void rendering() {
    auto model = coretx::ui::simulator_model();
    auto state = coretx::ui::simulator_controls();
    Interface ui{};
    Framebuffer display{};
    for (std::size_t page = 0; page < coretx::ui::page_count; ++page) {
        const int fields =
            ui.page() == Page::controls ? 1 : coretx::ui::detail::field_count(ui.page());
        for (int field = 0; field < fields; ++field) {
            for (int row = 0; row < 25; ++row) {
                ui.render(model, state, coretx::evaluate(model, state), display);
                press(ui, model, state, Key::down);
            }
            if (ui.page() != Page::controls) {
                press(ui, model, state, Key::right);
            }
        }
        press(ui, model, state, Key::next_page);
    }
    go(ui, model, state, Page::monitor);
    press(ui, model, state, Key::enter);
    check(!ui.editing(), "monitor is read-only");
    model.inputs[0].source.index = 255;
    const auto invalid = coretx::evaluate(model, state);
    check(!invalid, "invalid source detected");
    for (std::size_t page = 0; page < coretx::ui::page_count; ++page) {
        ui.render(model, state, invalid, display);
        Framebuffer expected{};
        expected.text(1, 18, "CALCULATION ERROR");
        check(same_region(display, expected, 0, 18, 128, 7),
              "failed calculation renders error instead of channels");
        press(ui, model, state, Key::next_page);
    }
    model = {};
    for (const auto page : {Page::inputs, Page::mixes}) {
        go(ui, model, state, page);
        press(ui, model, state, Key::enter);
        press(ui, model, state, Key::down);
        ui.render(model, state, coretx::evaluate(model, state), display);
        Framebuffer expected{};
        expected.text(1, 20, "NO LINES");
        check(!ui.editing() && same_region(display, expected, 0, 20, 128, 7),
              "empty pages render safely without editing inactive lines");
    }
}

void curves() {
    auto model = coretx::ui::simulator_model();
    auto state = coretx::ui::simulator_controls();
    Interface ui{};
    ui.open_curve(2);
    check(ui.page() == Page::curves && ui.selected_curve() == 2,
          "curve editor opens selected model curve");
    press(ui, model, state, Key::down);
    press(ui, model, state, Key::left);
    edit(ui, model, state, Key::right, 20, true);
    check(model.curves[2].x[0] == -1 && static_cast<bool>(coretx::validate(model)),
          "inner X cannot reach its next neighbor");
    edit(ui, model, state, Key::left, 20, true);
    check(model.curves[2].x[0] == -99, "inner X cannot reach fixed endpoint");
    press(ui, model, state, Key::enter);
    press(ui, model, state, Key::right, 2, true);
    press(ui, model, state, Key::back);
    check(model.curves[2].x[0] == -99 && !ui.editing(), "cancel restores curve point");
    press(ui, model, state, Key::up);
    press(ui, model, state, Key::enter);
    check(!ui.editing() && ui.field_value(model) == -100, "outer X stays read-only");
    press(ui, model, state, Key::down, 4);
    press(ui, model, state, Key::enter);
    check(!ui.editing() && ui.field_value(model) == 100, "positive outer X stays read-only");
    press(ui, model, state, Key::right);
    edit(ui, model, state, Key::left, 20, true);
    check(model.curves[2].y[4] == -100 && ui.take_changed(), "endpoint Y is editable and bounded");
    press(ui, model, state, Key::menu);
    check(ui.selected_curve() == 3 && !ui.take_changed(), "curve selection is not a model edit");
    press(ui, model, state, Key::menu);
    check(ui.selected_curve() == 0, "curve selection wraps across four curves");

    Framebuffer before{}, after{};
    ui.render(model, state, coretx::evaluate(model, state), before);
    tap(ui, model, state, 52, 29);
    check(ui.editing() && ui.selected_row(model) == 2 && ui.selected_field() == 1,
          "touch selects point Y in the compact list");
    press(ui, model, state, Key::right, 5, true);
    press(ui, model, state, Key::enter);
    ui.render(model, state, coretx::evaluate(model, state), after);
    check(model.curves[0].y[2] == 50 && !same_region(before, after, 75, 17, 53, 30),
          "curve preview follows edited point values");
    tap(ui, model, state, 110, 29);
    check(!ui.editing() && model.curves[0].y[2] == 50, "curve graph is read-only");
    swipe(ui, model, state, 40, 20);
    check(!ui.editing(), "dragging curve points does not enter editing");
    tap(ui, model, state, 66, 7);
    check(ui.selected_curve() == 1, "touch curve header selects next curve");

    ui.open(Page::inputs);
    press(ui, model, state, Key::right, 11);
    edit(ui, model, state, Key::right, 4);
    check(model.inputs[0].curve == 4 && model.inputs[0].destination == 0 &&
              !model.inputs[0].condition.enabled,
          "input curve selector leaves target and condition untouched");
    ui.open(Page::mixes);
    press(ui, model, state, Key::right, 10);
    edit(ui, model, state, Key::right, 4);
    check(model.mixes[0].curve == 4 && model.mixes[0].destination == 0 &&
              !model.mixes[0].condition.enabled,
          "mixer can reuse the same model curve");
    edit(ui, model, state, Key::left, 4);
    check(model.mixes[0].curve == 0, "curve selection can be disabled");
}

#if CORETX_MODUL_TANKMIX
void tank_mixer() {
    auto model = coretx::ui::simulator_model();
    auto controls = coretx::ui::simulator_controls();
    Interface ui{};
    ui.open(Page::tank);
    press(ui, model, controls, Key::enter);
    press(ui, model, controls, Key::right);
    press(ui, model, controls, Key::enter);
    check(!model.tank.enabled && ui.editing() && ui.notice() != nullptr &&
              (model.mixes.size() == 8),
          "tank activation reports occupied channels without changing model");
    press(ui, model, controls, Key::back);
    press(ui, model, controls, Key::down, 3);
    edit(ui, model, controls, Key::right, 8);
    press(ui, model, controls, Key::down);
    edit(ui, model, controls, Key::right, 8);
    press(ui, model, controls, Key::up, 4);
    edit(ui, model, controls, Key::right);
    check(model.tank.enabled && (model.mixes.size() == 14) && ui.take_changed(),
          "tank activation adds six module-owned lines on free channels");
    press(ui, model, controls, Key::down, 5);
    edit(ui, model, controls, Key::left, 4, true);
    check(model.tank.steering_percent == 60 && (model.mixes[9].weight_percent.value() == 60) &&
              (model.mixes[10].weight_percent.value() == -60),
          "tank steering percentage manages direction-dependent weights");
    press(ui, model, controls, Key::down);
    edit(ui, model, controls, Key::left, 20, true);
    check(model.tank.hysteresis_percent == 1, "hysteresis cannot be disabled accidentally");
    edit(ui, model, controls, Key::right, 20, true);
    check(model.tank.hysteresis_percent == 20, "hysteresis upper editor bound");
    press(ui, model, controls, Key::down);
    check(ui.field_value(model) == 1, "steering reversal defaults to ON");
    press(ui, model, controls, Key::enter);
    press(ui, model, controls, Key::left);
    press(ui, model, controls, Key::back);
    check(model.tank.reverse_steering, "cancel preserves reversal option");
    edit(ui, model, controls, Key::left);
    check(!model.tank.reverse_steering && (model.mixes.size() == 14),
          "steering reversal can be disabled without removing managed lines");
    edit(ui, model, controls, Key::right);
    ui.open(Page::mixes);
    press(ui, model, controls, Key::down, 8);
    for (int field = 0; field < coretx::ui::detail::field_count(Page::mixes); ++field) {
        press(ui, model, controls, Key::enter);
        check(!ui.editing() && ui.notice() != nullptr, "all managed mix fields are read-only");
        press(ui, model, controls, Key::right);
    }
    for (const auto action :
         {LineAction::duplicate, LineAction::erase, LineAction::move_up, LineAction::move_down}) {
        const auto result = coretx::ui::detail::edit_lines(model.mixes, 8, action);
        check(result.error != nullptr && (model.mixes.size() == 14),
              "line mutation helper rejects managed rows");
    }
    const auto added = coretx::ui::detail::edit_lines(model.mixes, 8, LineAction::add);
    check(added.error == nullptr && (model.mixes.size() == 15) &&
              (model.mixes[added.selected].role == coretx::MixRole::manual) &&
              (model.mixes[added.selected].destination == 0) &&
              static_cast<bool>(coretx::validate(model)),
          "adding from a managed row chooses an unreserved channel");
    check(coretx::ui::detail::edit_lines(model.mixes, added.selected, LineAction::erase).error ==
              nullptr,
          "new manual row remains independently removable");
    const auto retarget = coretx::ui::detail::retarget(model.mixes, 8, 15);
    check(retarget.error != nullptr && (model.mixes[8].destination == 8),
          "retarget helper rejects managed row");
    const auto reserved = coretx::ui::detail::retarget(model.mixes, 0, 8);
    check(reserved.error != nullptr && (model.mixes[0].destination == 0),
          "manual rows cannot enter reserved tank channels");
    ui.open(Page::mixes);
    press(ui, model, controls, Key::right, 6);
    press(ui, model, controls, Key::enter);
    press(ui, model, controls, Key::right, 8);
    press(ui, model, controls, Key::enter);
    check(ui.editing() && (model.mixes[0].destination == 0) && ui.notice() != nullptr,
          "target editor reports channel reservation without mutating list");
    press(ui, model, controls, Key::back);
    press(ui, model, controls, Key::down, 9);
    Framebuffer forward{}, reverse{};
    coretx::LogicalState state{};
    controls.analogs[0] = 512;
    auto evaluation = coretx::evaluate(model, controls, state);
    ui.render(model, controls, evaluation, forward);
    controls.analogs[0] = -512;
    evaluation = coretx::evaluate(model, controls, state);
    ui.render(model, controls, evaluation, reverse);
    check(!same_region(forward, reverse, 100, 17, 28, 30),
          "managed row indicators display module direction conditions and live activation");
    ui.open(Page::outputs);
    press(ui, model, controls, Key::down, 8);
    edit(ui, model, controls, Key::right);
    check(model.outputs[8].minimum_permille.value() == -990,
          "tank outputs remain normally editable");
    ui.open(Page::inputs);
    press(ui, model, controls, Key::right);
    edit(ui, model, controls, Key::left);
    check(model.inputs[0].weight_percent.value() == 99, "tank inputs remain normally editable");
    ui.open(Page::tank);
    edit(ui, model, controls, Key::left);
    check(!model.tank.enabled && (model.mixes.size() == 8) && (model.tank.left_channel == 8) &&
              (model.tank.steering_percent == 60),
          "disabling removes owned lines and retains module configuration");
    // First choose a free third output, then activate the extra motor and module.
    press(ui, model, controls, Key::down, 9);
    edit(ui, model, controls, Key::right, 8);
    press(ui, model, controls, Key::up);
    edit(ui, model, controls, Key::right);
    const auto curve = model.tank.third_curve;
    check(model.tank.third_motor && (curve != 0) && (model.tank.third_channel == 10),
          "third motor allocates its curve and remembers selected output while tank is disabled");
    press(ui, model, controls, Key::up, 8);
    edit(ui, model, controls, Key::right);
    check(model.tank.enabled && (model.mixes.size() == 15) &&
              (model.mixes[14].role == coretx::MixRole::tank_third) &&
              (model.mixes[14].curve == curve),
          "third motor adds a managed throttle-to-curve mixer");
    press(ui, model, controls, Key::down, 10);
    press(ui, model, controls, Key::enter);
    check((ui.page() == Page::curves) && (ui.selected_curve() == curve - 1),
          "third motor curve row opens its assigned curve editor");
    press(ui, model, controls, Key::down, 4);
    edit(ui, model, controls, Key::left, 2, true);
    check(model.curves[curve - 1].y.back() == 80, "third motor curve remains normally editable");
    ui.open(Page::tank);
    press(ui, model, controls, Key::down, 8);
    edit(ui, model, controls, Key::left);
    check(!model.tank.third_motor && (model.mixes.size() == 14),
          "turning off third motor removes only its mixer");
    edit(ui, model, controls, Key::right);
    check(model.tank.third_motor && (model.tank.third_curve == curve) &&
              (model.curves[curve - 1].y.back() == 80) && coretx::validate(model),
          "reactivating third motor preserves its customized curve");
}

#endif
} // namespace

int main() {
    framebuffer();
    Framebuffer standard{}, lower{};
    standard.text(0, 0, "A");
    lower.text(0, 0, "a");
    check(standard.pixel_at(0, 8) && standard.pixel_at(4, 8) && !standard.pixel_at(0, 9) &&
              (standard.bytes() != lower.bytes()),
          "standard font uses nine ink rows and distinct lowercase");
    standard.small_text(12, 2, "a");
    check((standard.character_width() == 6) && (standard.character_height() == 10),
          "small annotation restores the standard font");
    check(text_matches_pixel_rendering(),
          "column text preserves glyphs, clipping, page crossings and erasing");
    check(compact_text_matches_pixel_rendering(),
          "compact columns preserve glyphs, clipping, page crossings and erasing");
    check(rectangles_match_pixel_rendering(),
          "page masks preserve rectangles, clipped edges and background pixels");
    check(clipped_frames_match_full_rendering(),
          "clipped frame bands preserve full rendering and leave other bands untouched");
    controls();
    inputs();
    mixers();
    outputs();
    touch_editing();
    touch_scrolling();
    touch_cancellation();
    line_actions();
    targets_and_conditions();
    touch_actions();
    rendering();
    curves();
#if CORETX_MODUL_TANKMIX
    tank_mixer();
#endif
    return failures == 0 ? 0 : 1;
}
