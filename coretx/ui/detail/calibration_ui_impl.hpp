// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace coretx::ui {
namespace detail {

inline constexpr std::array<std::string_view, 6> calibration_fields{"RAW",    "CENTER",  "RANGE",
                                                                    "SOURCE", "REVERSE", "APPLY"};

constexpr void adjust_raw(std::uint16_t &sample, const int direction, const bool coarse) noexcept {
    sample = static_cast<std::uint16_t>(
        std::clamp(static_cast<int>(sample) + direction * (coarse ? 256 : 32), 0, 4095));
}

} // namespace detail

constexpr void Interface::observe(const RawControls &raw) noexcept {
    if (calibrating_ && capturing_) {
        const auto physical = calibration_draft_.mapping[calibration_analog_].physical_index;
        auto &calibration = calibration_draft_.calibration[physical];
        calibration.minimum = std::min(calibration.minimum, raw.analogs[physical]);
        calibration.maximum = std::max(calibration.maximum, raw.analogs[physical]);
    }
}

constexpr void Interface::handle_calibration(const Key &key, RawControls &raw, ControlSetup &setup,
                                             const bool coarse) noexcept {
    if (key == Key::none) {
        return;
    }
    pointer_active_ = false;
    if ((key == Key::back) || (key == Key::next_page) || (key == Key::previous_page)) {
        calibrating_ = false;
        calibration_editing_ = false;
        capturing_ = false;
        return;
    }
    auto &mapping = calibration_draft_.mapping[calibration_analog_];
    auto &calibration = calibration_draft_.calibration[mapping.physical_index];
    if (calibration_editing_) {
        if (key == Key::enter) {
            observe(raw);
            calibration_editing_ = false;
            capturing_ = false;
        } else if ((key == Key::left) || (key == Key::right)) {
            const int direction = key == Key::right ? 1 : -1;
            if (calibration_field_ == 3) {
                mapping.physical_index = static_cast<std::uint8_t>(
                    std::clamp(static_cast<int>(mapping.physical_index) + direction, 0,
                               static_cast<int>(calibration_count) - 1));
            } else {
                detail::adjust_raw(raw.analogs[mapping.physical_index], direction, coarse);
                observe(raw);
            }
        }
        return;
    }
    calibration_status_ = {};
    if ((key == Key::up) && (calibration_analog_ > 0)) {
        --calibration_analog_;
    } else if ((key == Key::down) && ((calibration_analog_ + 1U) < calibration_count)) {
        ++calibration_analog_;
    } else if ((key == Key::left) || (key == Key::right)) {
        calibration_field_ = static_cast<std::uint8_t>(
            (static_cast<int>(calibration_field_) + (key == Key::right ? 1 : 5)) % 6);
    } else if (key == Key::enter) {
        switch (calibration_field_) {
        case 0:
        case 3:
            calibration_editing_ = true;
            break;
        case 1:
            calibration.center = raw.analogs[mapping.physical_index];
            break;
        case 2:
            calibration.minimum = raw.analogs[mapping.physical_index];
            calibration.maximum = calibration.minimum;
            capturing_ = true;
            calibration_editing_ = true;
            break;
        case 4:
            mapping.reversed = !mapping.reversed;
            break;
        case 5:
            calibration_status_ = validate(calibration_draft_);
            if (calibration_status_) {
                setup = calibration_draft_;
                calibrating_ = false;
            }
            break;
        }
    }
}

constexpr void Interface::handle(const Key &key, Model &model, RawControls &raw,
                                 ControlSetup &setup, const bool coarse) noexcept {
    raw_controls_ = true;
    analog_sources_ &= static_cast<std::uint16_t>((1U << calibration_count) - 1U);
    if (calibrating_) {
        handle_calibration(key, raw, setup, coarse);
        if ((key == Key::next_page) || (key == Key::previous_page)) {
            handle(key, model, raw, setup, coarse);
        }
        return;
    }
    if ((key == Key::menu) && (page_ == Page::controls)) {
        pointer_active_ = false;
        calibration_draft_ = setup;
        calibration_status_ = validate(setup);
        // A bad external mapping must not become an unchecked array index in the editor.
        for (std::size_t i = 0; i < calibration_count; ++i) {
            if (setup.mapping[i].physical_index >= calibration_count) {
                calibration_status_ = {PreparationError::mapping, i};
                return;
            }
        }
        calibration_analog_ = 0;
        calibration_field_ = 0;
        calibration_editing_ = false;
        capturing_ = false;
        calibrating_ = true;
        return;
    }
    if ((key != Key::none) && (page_ == Page::controls) && (row(model) < calibration_count) &&
        ((key == Key::left) || (key == Key::right) || (key == Key::enter))) {
        pointer_active_ = false;
        const auto selected = row(model);
        if (key == Key::enter) {
            raw.analogs[selected] = selected == 2 ? 0 : 2048;
        } else {
            detail::adjust_raw(raw.analogs[selected], key == Key::right ? 1 : -1, coarse);
        }
        return;
    }
    Controls controls{};
    controls.switches = raw.switches;
    controls.trims = raw.trims;
    handle(key, model, controls, coarse);
    raw.switches = controls.switches;
    raw.trims = controls.trims;
}

constexpr void Interface::handle(const PointerEvent &event, Model &model, RawControls &raw,
                                 ControlSetup &setup, const bool coarse) noexcept {
    raw_controls_ = true;
    analog_sources_ &= static_cast<std::uint16_t>((1U << calibration_count) - 1U);
    if (!calibrating_) {
        handle_pointer(event, model, raw, coarse, setup);
        return;
    }
    if ((event.kind == PointerKind::cancel) || (event.x < 0) || (event.y < 0) ||
        (event.x >= Framebuffer::width) || (event.y >= Framebuffer::height)) {
        pointer_active_ = false;
        return;
    }
    if (event.kind == PointerKind::press) {
        if (!pointer_active_) {
            pointer_active_ = true;
            pointer_dragged_ = false;
            pointer_x_ = event.x;
            pointer_y_ = event.y;
        }
        return;
    }
    if (!pointer_active_) {
        return;
    }
    const auto zone = [](const int x, const int y) constexpr {
        return y < 16    ? x < 56 ? 0 : 1
               : y >= 48 ? 2 + x / 32
               : y < 25  ? 6
               : y < 33  ? 7
               : y < 41  ? x < 64 ? 8 : 9
                         : 10;
    };
    const int dx = event.x - pointer_x_;
    const int dy = event.y - pointer_y_;
    if ((dx <= -4) || (dx >= 4) || (dy <= -4) || (dy >= 4) ||
        (zone(pointer_x_, pointer_y_) != zone(event.x, event.y))) {
        pointer_dragged_ = true;
    }
    if (event.kind != PointerKind::release) {
        return;
    }
    pointer_active_ = false;
    if (pointer_dragged_) {
        return;
    }
    const auto selected = zone(event.x, event.y);
    if (selected < 2) {
        if (!calibration_editing_) {
            if (selected == 0) {
                calibration_analog_ =
                    static_cast<std::uint8_t>((calibration_analog_ + 1) % calibration_count);
            } else {
                handle_calibration(Key::right, raw, setup, coarse);
            }
        }
    } else if (selected < 6) {
        constexpr std::array keys{Key::back, Key::left, Key::right, Key::enter};
        handle_calibration(keys[static_cast<std::size_t>(selected - 2)], raw, setup, coarse);
    } else if (!calibration_editing_) {
        constexpr std::array<std::uint8_t, 5> fields{0, 2, 1, 4, 5};
        calibration_field_ = fields[static_cast<std::size_t>(selected - 6)];
        handle_calibration(Key::enter, raw, setup, coarse);
    }
}

constexpr void Interface::render_calibration(const RawControls &raw,
                                             Framebuffer &display) const noexcept {
    display.clear();
    display.rectangle(0, 0, 128, detail::header_height);
    display.text(1, 4, "CAL", false);
    display.text(25, 4, detail::analog_names[calibration_analog_], false);
    display.text(58, 4, detail::calibration_fields[calibration_field_], false);
    if (calibration_editing_) {
        display.rectangle(120, 4, 5, 9, false);
    }
    display.compact();
    const auto &mapping = calibration_draft_.mapping[calibration_analog_];
    const auto &calibration = calibration_draft_.calibration[mapping.physical_index];
    detail::indexed_label(display, 1, 17, "P", mapping.physical_index, true);
    display.number(62, 17, raw.analogs[mapping.physical_index]);
    display.text(70, 17, "N");
    const auto preview = prepare_controls(calibration_draft_, raw);
    if (preview) {
        display.number(126, 17, preview.controls.analogs[calibration_analog_]);
    } else {
        display.text(104, 17, "---");
    }
    display.text(1, 25, "LO");
    display.number(62, 25, calibration.minimum);
    display.text(70, 25, "HI");
    display.number(126, 25, calibration.maximum);
    display.text(1, 33, "MID");
    display.number(62, 33, calibration.center);
    display.text(90, 33, mapping.reversed ? "REV" : "FWD");
    if (capturing_) {
        display.text(1, 41, "MOVE OK TO STOP");
    } else if (!calibration_status_) {
        display.text(1, 41, "BAD CAL P");
        display.number(66, 41, static_cast<std::int32_t>(calibration_status_.index + 1));
    } else {
        display.text(1, 41, "APPLY DRAFT");
    }
    display.compact(false);
    display.rectangle(0, 48, 128, 1);
    display.text(4, 53, "BACK");
    display.text(45, 53, "-");
    display.text(77, 53, "+");
    display.text(100, 53, calibration_field_ == 5 ? "SAVE" : "OK");
    for (int x = 32; x < 128; x += 32) {
        display.rectangle(x, 48, 1, 16);
    }
}

constexpr void Interface::render(const Model &model, const RawControls &raw,
                                 const PreparedControls &prepared, const Evaluation &evaluation,
                                 Framebuffer &display) const noexcept {
    if (calibrating_) {
        render_calibration(raw, display);
        return;
    }
    if (!prepared) {
        display.clear();
        display.text(1, 4, "CONTROL ERROR");
        display.text(1, 18, preparation_error_message(prepared.status.code));
        display.number(126, 34, static_cast<std::int32_t>(prepared.status.index));
        return;
    }
    auto view = *this;
    view.raw_controls_ = true;
    view.render(model, prepared.controls, evaluation, display);
    if (page_ != Page::controls) {
        return;
    }
    display.rectangle(56, 0, 62, 16);
    display.text(66, 4, "CAL", false);
    if (!evaluation) {
        return;
    }
    const auto first = view.first_row(model);
    for (std::size_t line = first;
         (line < calibration_count) && (line < (first + detail::visible_rows)); ++line) {
        const int y =
            detail::header_height + 1 + static_cast<int>(line - first) * detail::row_height;
        const bool ink = line != view.row(model);
        display.rectangle(0, y - 1, 128, detail::row_height, !ink);
        detail::indexed_label(display, 1, y, "P", line, ink);
        detail::bar(display, y, static_cast<int>(raw.analogs[line]) * 2 - 4095, 4095, ink);
        display.number(126, y, raw.analogs[line], ink);
    }
}

} // namespace coretx::ui
