// SPDX-License-Identifier: GPL-2.0-or-later
#include "coretx/ui/ui.hpp"

#include <cstdio>
#include <cstdlib>

namespace {

using coretx::ui::Framebuffer;
using coretx::ui::Interface;
using coretx::ui::Key;
using coretx::ui::PointerKind;

void check(const bool passed, const char *const expression, const int line) {
    if (!passed) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
        std::exit(EXIT_FAILURE);
    }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

constexpr bool same_setup(const coretx::ControlSetup &a, const coretx::ControlSetup &b) {
    for (std::size_t i = 0; i < coretx::calibration_count; ++i) {
        if ((a.calibration[i].minimum != b.calibration[i].minimum) ||
            (a.calibration[i].center != b.calibration[i].center) ||
            (a.calibration[i].maximum != b.calibration[i].maximum) ||
            (a.mapping[i].physical_index != b.mapping[i].physical_index) ||
            (a.mapping[i].reversed != b.mapping[i].reversed)) {
            return false;
        }
    }
    return true;
}

constexpr void press(Interface &ui, coretx::Model &model, coretx::RawControls &raw,
                     coretx::ControlSetup &setup, const Key &key, const int times = 1,
                     const bool coarse = false) {
    for (int i = 0; i < times; ++i) {
        ui.handle(key, model, raw, setup, coarse);
    }
}

constexpr void point(Interface &ui, coretx::Model &model, coretx::RawControls &raw,
                     coretx::ControlSetup &setup, const PointerKind &kind, const int x = -1,
                     const int y = -1, const bool coarse = false) {
    ui.handle({kind, static_cast<std::int16_t>(x), static_cast<std::int16_t>(y)}, model, raw, setup,
              coarse);
}

constexpr void tap(Interface &ui, coretx::Model &model, coretx::RawControls &raw,
                   coretx::ControlSetup &setup, const int x, const int y,
                   const bool coarse = false) {
    point(ui, model, raw, setup, PointerKind::press, x, y, coarse);
    point(ui, model, raw, setup, PointerKind::release, x, y, coarse);
}

constexpr bool raw_controls() {
    auto model = coretx::ui::simulator_model();
    auto setup = coretx::ui::simulator_setup();
    auto raw = coretx::ui::simulator_raw_controls();
    Interface ui{};
    const auto initial = coretx::prepare_controls(setup, raw);
    if (!initial || (initial.controls.analogs[2] != -1024)) {
        return false;
    }
    press(ui, model, raw, setup, Key::right);
    const auto moved = coretx::prepare_controls(setup, raw);
    if (!moved || (raw.analogs[0] != 2080) || (moved.controls.analogs[0] != 16)) {
        return false;
    }
    press(ui, model, raw, setup, Key::enter);
    press(ui, model, raw, setup, Key::down, 2);
    press(ui, model, raw, setup, Key::right, 30, true);
    if (raw.analogs[2] != 4095) {
        return false;
    }
    press(ui, model, raw, setup, Key::enter);
    press(ui, model, raw, setup, Key::down, static_cast<int>(coretx::calibration_count) - 2);
    press(ui, model, raw, setup, Key::right);
    press(ui, model, raw, setup, Key::down, static_cast<int>(coretx::switch_count));
    press(ui, model, raw, setup, Key::left, 1, true);
    const auto prepared = coretx::prepare_controls(setup, raw);
    return prepared && (prepared.controls.analogs[0] == 0) &&
           (prepared.controls.analogs[2] == -1024) &&
           (prepared.controls.switches[0] == coretx::SwitchPosition::down) &&
           (prepared.controls.trims[0].value() == -8);
}

constexpr bool mapping_transaction() {
    auto model = coretx::ui::simulator_model();
    model.inputs[0].expo_percent = {};
    auto setup = coretx::ui::simulator_setup();
    const auto original = setup;
    auto raw = coretx::ui::simulator_raw_controls();
    raw.analogs[3] = 3072;
    Interface ui{};
    press(ui, model, raw, setup, Key::menu);
    press(ui, model, raw, setup, Key::right, 3);
    press(ui, model, raw, setup, Key::enter);
    press(ui, model, raw, setup, Key::right, 3);
    press(ui, model, raw, setup, Key::enter);
    press(ui, model, raw, setup, Key::right);
    press(ui, model, raw, setup, Key::enter);
    if (!ui.calibrating() || !same_setup(setup, original) ||
        (coretx::prepare_controls(setup, raw).controls.analogs[0] != 0)) {
        return false;
    }
    Framebuffer display{};
    const auto active = coretx::prepare_controls(setup, raw);
    ui.render(model, raw, active, coretx::evaluate(model, active.controls), display);
    if (display.bytes()[0] == 0) {
        return false;
    }
    press(ui, model, raw, setup, Key::right);
    press(ui, model, raw, setup, Key::enter);
    const auto prepared = coretx::prepare_controls(setup, raw);
    if (ui.calibrating() || !prepared || (setup.mapping[0].physical_index != 3) ||
        !setup.mapping[0].reversed || (prepared.controls.analogs[0] != -512) ||
        (coretx::evaluate(model, prepared.controls).frame.channels[0] != -512)) {
        return false;
    }
    press(ui, model, raw, setup, Key::menu);
    press(ui, model, raw, setup, Key::right, 4);
    press(ui, model, raw, setup, Key::enter);
    press(ui, model, raw, setup, Key::back);
    return !ui.calibrating() && setup.mapping[0].reversed;
}

constexpr bool capture_and_validation() {
    auto model = coretx::ui::simulator_model();
    auto setup = coretx::ui::simulator_setup();
    const auto original = setup;
    auto raw = coretx::ui::simulator_raw_controls();
    raw.analogs[0] = 1920;
    Interface ui{};
    press(ui, model, raw, setup, Key::menu);
    press(ui, model, raw, setup, Key::right);
    press(ui, model, raw, setup, Key::enter); // Capture center.
    press(ui, model, raw, setup, Key::right);
    press(ui, model, raw, setup, Key::enter); // Capture range without travel.
    press(ui, model, raw, setup, Key::enter);
    press(ui, model, raw, setup, Key::right, 3);
    press(ui, model, raw, setup, Key::enter);
    if (!ui.calibrating() || ui.calibration_status() ||
        (ui.calibration_status().code != coretx::PreparationError::calibration) ||
        (ui.calibration_status().index != 0) || !same_setup(setup, original)) {
        return false;
    }
    press(ui, model, raw, setup, Key::right, 3); // Return to RANGE.
    press(ui, model, raw, setup, Key::enter);
    press(ui, model, raw, setup, Key::left, 4, true);
    press(ui, model, raw, setup, Key::right, 8, true);
    if (!same_setup(setup, original)) {
        return false;
    }
    press(ui, model, raw, setup, Key::enter);
    press(ui, model, raw, setup, Key::right, 3);
    press(ui, model, raw, setup, Key::enter);
    const auto prepared = coretx::prepare_controls(setup, raw);
    return !ui.calibrating() && prepared && (setup.calibration[0].minimum == 896) &&
           (setup.calibration[0].center == 1920) && (setup.calibration[0].maximum == 2944) &&
           (prepared.controls.analogs[0] == 1024) &&
           (coretx::evaluate(model, prepared.controls).frame.channels[0] == 1024);
}

constexpr bool touch_and_cancel() {
    auto model = coretx::ui::simulator_model();
    auto setup = coretx::ui::simulator_setup();
    const auto original = setup;
    auto raw = coretx::ui::simulator_raw_controls();
    Interface ui{};
    tap(ui, model, raw, setup, 80, 8);
    tap(ui, model, raw, setup, 20, 20);
    point(ui, model, raw, setup, PointerKind::press, 80, 56);
    point(ui, model, raw, setup, PointerKind::cancel);
    point(ui, model, raw, setup, PointerKind::release, 80, 56);
    point(ui, model, raw, setup, PointerKind::press, 80, 56);
    point(ui, model, raw, setup, PointerKind::move, 85, 56);
    point(ui, model, raw, setup, PointerKind::release, 80, 56);
    if (raw.analogs[0] != 2048) {
        return false;
    }
    tap(ui, model, raw, setup, 110, 56);
    tap(ui, model, raw, setup, 20, 28); // RANGE.
    for (int i = 0; i < 4; ++i) {
        tap(ui, model, raw, setup, 48, 56, true);
    }
    for (int i = 0; i < 8; ++i) {
        tap(ui, model, raw, setup, 80, 56, true);
    }
    tap(ui, model, raw, setup, 110, 56);
    tap(ui, model, raw, setup, 110, 36); // Reverse the draft.
    if (!same_setup(setup, original)) {
        return false;
    }
    tap(ui, model, raw, setup, 20, 44);
    const auto prepared = coretx::prepare_controls(setup, raw);
    if (ui.calibrating() || !prepared || (prepared.controls.analogs[0] != -1024) ||
        !setup.mapping[0].reversed || (setup.calibration[0].minimum != 1024) ||
        (setup.calibration[0].maximum != 3072)) {
        return false;
    }
    const auto committed = setup;
    tap(ui, model, raw, setup, 80, 8);
    tap(ui, model, raw, setup, 20, 28);
    press(ui, model, raw, setup, Key::next_page);
    return !ui.calibrating() && !ui.editing() && (ui.page() == coretx::ui::Page::inputs) &&
           same_setup(setup, committed);
}

constexpr bool observed_samples_and_bad_mapping() {
    auto model = coretx::ui::simulator_model();
    auto setup = coretx::ui::simulator_setup();
    auto raw = coretx::ui::simulator_raw_controls();
    Interface ui{};
    press(ui, model, raw, setup, Key::menu);
    press(ui, model, raw, setup, Key::right, 2);
    press(ui, model, raw, setup, Key::enter);
    raw.analogs[0] = 500;
    ui.observe(raw);
    raw.analogs[0] = 3800;
    ui.observe(raw);
    press(ui, model, raw, setup, Key::enter);
    press(ui, model, raw, setup, Key::right, 3);
    press(ui, model, raw, setup, Key::enter);
    if (ui.calibrating() || (setup.calibration[0].minimum != 500) ||
        (setup.calibration[0].maximum != 3800)) {
        return false;
    }
    setup.calibration[0] = {};
    setup.mapping[3].physical_index = 255;
    press(ui, model, raw, setup, Key::menu);
    const auto prepared = coretx::prepare_controls(setup, raw);
    Framebuffer display{};
    ui.render(model, raw, prepared, {}, display);
    return !ui.calibrating() && !prepared &&
           (ui.calibration_status().code == coretx::PreparationError::mapping) &&
           (ui.calibration_status().index == 3);
}

static_assert(raw_controls());
static_assert(mapping_transaction());
static_assert(capture_and_validation());
static_assert(touch_and_cancel());
static_assert(observed_samples_and_bad_mapping());

} // namespace

int main() {
    CHECK(raw_controls());
    CHECK(mapping_transaction());
    CHECK(capture_and_validation());
    CHECK(touch_and_cancel());
    CHECK(observed_samples_and_bad_mapping());
    std::puts("Calibration UI tests passed");
}
