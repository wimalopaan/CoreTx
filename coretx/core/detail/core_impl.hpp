// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) EdgeTX
// Based on OpenTX, th9x, er9x and gruvin9x.
// Modified 2026-11-19 for a standalone, bounded integer-only calculation core.
// Fixed-point arithmetic adapted from EdgeTX revision 90e8f0c55:
// radio/src/mixer.cpp, edgetx.h and edgetx_helpers.h (GPL-2.0-or-later).
#pragma once

#include "coretx/core/core.hpp"
#include "coretx/utility/fixed_bitset.hpp"

#include <algorithm>
#include <limits>

namespace coretx {

namespace detail {

constexpr std::int32_t round_divide(const std::int32_t value, const std::int32_t divisor) noexcept {
    return (value < 0 ? value - divisor / 2 : value + divisor / 2) / divisor;
}

constexpr std::int64_t floor_divide(const std::int64_t value, const std::int32_t divisor) noexcept {
    const auto quotient = value / divisor;
    return quotient - ((value % divisor) < 0 ? 1 : 0);
}

constexpr bool in_range(const std::int32_t value, const std::int32_t lower,
                        const std::int32_t upper) noexcept {
    return (value >= lower) && (value <= upper);
}

constexpr bool valid_tank(const TankMixer &tank) noexcept {
    return (tank.drive_input < input_count) && (tank.steering_input < input_count) &&
           (tank.left_channel < channel_count) && (tank.right_channel < channel_count) &&
           (tank.left_channel != tank.right_channel) && (tank.steering_percent <= 100) &&
           in_range(tank.hysteresis_percent, 1, 20) && (tank.third_channel < channel_count) &&
           (tank.third_curve <= curve_count) &&
           (!tank.third_motor || ((tank.third_channel != tank.left_channel) &&
                                  (tank.third_channel != tank.right_channel))) &&
           (!tank.enabled || !tank.third_motor || (tank.third_curve != 0));
}

#if CORETX_MODUL_TANKMIX
constexpr bool tank_channel(const TankMixer &tank, const std::size_t channel) noexcept {
    return (channel == tank.left_channel) || (channel == tank.right_channel) ||
           (tank.third_motor && (channel == tank.third_channel));
}

constexpr MixLine tank_line(const TankMixer &tank, const std::uint8_t channel,
                            const MixRole role) noexcept {
    MixLine line{};
    line.destination = channel;
    line.role = role;
    line.source.index = (role == MixRole::tank_drive) || (role == MixRole::tank_third)
                            ? tank.drive_input
                            : tank.steering_input;
    if (role == MixRole::tank_third)
        line.curve = tank.third_curve;
    else if (role != MixRole::tank_drive) {
        const bool positive = (channel == tank.left_channel) == (role == MixRole::tank_forward);
        (void)line.weight_percent.try_set(positive ? tank.steering_percent
                                                   : -tank.steering_percent);
    }
    return line;
}

constexpr bool canonical_tank_line(const MixLine &line, const TankMixer &tank) noexcept {
    const auto expected = tank_line(tank, line.destination, line.role);
    return (line.source.kind == expected.source.kind) &&
           (line.source.index == expected.source.index) && !line.source.inverted &&
           !line.condition.enabled && (line.condition.index == 0) &&
           (line.condition.position == SwitchPosition::up) && !line.condition.inverted &&
           (line.multiplex == Multiplex::add) && line.carry_trim &&
           (line.weight_percent == expected.weight_percent) && (line.offset_percent.value() == 0) &&
           (line.curve == expected.curve);
}
#endif

constexpr bool valid_source(const Source &source, const bool allow_input) noexcept {
    switch (source.kind) {
    case SourceKind::analog:
        return source.index < analog_count;
    case SourceKind::input:
        return allow_input && (source.index < input_count);
    case SourceKind::switch_position:
        return (source.index < switch_count) || multiswitch_index(source.index);
    case SourceKind::maximum:
        return source.index == 0;
    }
    return false;
}

constexpr bool valid_condition(const Condition &condition) noexcept {
    return (
        ((condition.index < switch_count) && valid_switch_position(condition.position)) ||
        ((condition.index < condition_count) && ((condition.position == SwitchPosition::up) ||
                                                 (condition.position == SwitchPosition::down))));
}

constexpr bool multiswitch_active(const std::uint8_t index, const Controls &controls) noexcept {
    return (controls.multiswitch & (1U << (index - multiswitch_switch_base))) != 0;
}

constexpr bool multiswitch_available(const std::uint8_t index, const Controls &controls) noexcept {
    return (controls.multiswitch_available & (1U << (index - multiswitch_switch_base))) != 0;
}

constexpr bool matches(const Condition &condition, const Controls &controls,
                       const LogicalFrame &logical) noexcept {
    if (!condition.enabled) {
        return true;
    }
    if (multiswitch_index(condition.index)) {
        if (!multiswitch_available(condition.index, controls))
            return false;
        return (multiswitch_active(condition.index, controls) ==
                (condition.position == SwitchPosition::down)) != condition.inverted;
    }
    if (condition.index >= switch_count) {
        const auto bit = 1U << (condition.index - switch_count);
        if ((logical.available & bit) == 0)
            return false;
        return (((logical.active & bit) != 0) == (condition.position == SwitchPosition::down)) !=
               condition.inverted;
    }
    if ((controls.active_switches & (1U << condition.index)) == 0)
        return false;
    return (controls.switches[condition.index] == condition.position) != condition.inverted;
}

constexpr bool boolean_function(const LogicalFunction function) noexcept {
    return (function >= LogicalFunction::and_) && (function <= LogicalFunction::srff);
}

constexpr bool valid_logical_operand(const Model &model, const std::uint8_t index,
                                     const SwitchPosition position) noexcept {
    if (index < switch_count)
        return valid_switch_position(position);
    if ((position != SwitchPosition::up) && (position != SwitchPosition::down))
        return false;
    if (multiswitch_index(index))
        return true;
    return logical_index(index) &&
           (model.logical_switches[index - switch_count].function != LogicalFunction::disabled);
}

constexpr std::uint8_t dependencies(const LogicalSwitch &line) noexcept {
    if (!boolean_function(line.function))
        return 0;
    const auto bit = [](const std::uint8_t index) constexpr -> unsigned {
        return logical_index(index) ? (1U << (index - switch_count)) : 0U;
    };
    return static_cast<std::uint8_t>(bit(line.a) | bit(line.b));
}

constexpr Status validate_logical(const Model &model) noexcept {
    for (std::size_t i = 0; i < logical_switch_count; ++i) {
        const auto &line = model.logical_switches[i];
        const auto operands =
            boolean_function(line.function) ? condition_count : analog_count + switch_count;
        if ((line.function > LogicalFunction::srff) || (line.a >= operands) ||
            (line.b >= operands) || !valid_switch_position(line.position_a) ||
            !valid_switch_position(line.position_b) ||
            !in_range(line.threshold, -resolution, resolution))
            return {ErrorCode::logical_settings, Stage::model, i};
        if (line.function == LogicalFunction::disabled)
            continue;
        if (boolean_function(line.function)) {
            if (!valid_logical_operand(model, line.a, line.position_a) ||
                !valid_logical_operand(model, line.b, line.position_b))
                return {ErrorCode::logical_settings, Stage::model, i};
        }
    }
    std::uint8_t resolved{};
    for (std::size_t pass = 0; pass < logical_switch_count; ++pass) {
        for (std::size_t i = 0; i < logical_switch_count; ++i) {
            if ((dependencies(model.logical_switches[i]) & ~resolved) == 0)
                resolved |= static_cast<std::uint8_t>(1U << i);
        }
    }
    if (resolved != 0xFF)
        return {ErrorCode::logical_cycle, Stage::model, 0};
    // MultiSwitch states feed back one cycle late; button conditions must not depend on them.
    std::uint8_t multiswitch_dependent{};
    for (std::size_t pass = 0; pass < logical_switch_count; ++pass)
        for (std::size_t i = 0; i < logical_switch_count; ++i) {
            const auto &line = model.logical_switches[i];
            if (boolean_function(line.function) &&
                (multiswitch_index(line.a) || multiswitch_index(line.b) ||
                 ((dependencies(line) & multiswitch_dependent) != 0)))
                multiswitch_dependent |= static_cast<std::uint8_t>(1U << i);
        }
    for (std::size_t page = 0; page < model.multiswitch.size(); ++page)
        for (std::size_t button = 0; button < multiswitch_function_count; ++button) {
            const auto condition = model.multiswitch[page].controls[button].condition();
            if (condition.enabled && logical_index(condition.index) &&
                ((multiswitch_dependent & (1U << (condition.index - switch_count))) != 0))
                return {ErrorCode::logical_cycle, Stage::model,
                        page * multiswitch_function_count + button};
        }
    for (const auto channel : model.dashboard_channels) {
        if (channel > channel_count)
            return {ErrorCode::dashboard_settings, Stage::model, channel};
    }
    for (std::size_t i = 0; i < model.multiswitch.size(); ++i) {
        const auto &page = model.multiswitch[i];
        if ((page.address < -1) || (page.address > 255))
            return {ErrorCode::multiswitch_settings, Stage::model, i};
        for (const auto &label : page.labels)
            for (const char character : label)
                if ((character != 0) && ((character < ' ') || (character > '~')))
                    return {ErrorCode::multiswitch_settings, Stage::model, i};
    }
    return {};
}

constexpr LogicalFrame evaluate_logical(const Model &model, const Controls &controls,
                                        LogicalState &state) noexcept {
    LogicalFrame result{};
    std::uint8_t resolved{};
    for (std::size_t pass = 0; pass < logical_switch_count; ++pass) {
        for (std::size_t i = 0; i < logical_switch_count; ++i) {
            const auto bit = static_cast<std::uint8_t>(1U << i);
            const auto &line = model.logical_switches[i];
            if (((resolved & bit) != 0) || ((dependencies(line) & ~resolved) != 0))
                continue;
            resolved |= bit;
            if (line.function == LogicalFunction::disabled)
                continue;
            bool value{};
            if (boolean_function(line.function)) {
                const auto available = [&](const std::uint8_t index) constexpr {
                    return index < switch_count ? ((controls.active_switches & (1U << index)) != 0)
                           : multiswitch_index(index)
                               ? multiswitch_available(index, controls)
                               : ((result.available & (1U << (index - switch_count))) != 0);
                };
                if (!available(line.a) || !available(line.b))
                    continue;
                const bool a =
                    matches({true, line.a, line.position_a, line.inverted_a}, controls, result);
                const bool b =
                    matches({true, line.b, line.position_b, line.inverted_b}, controls, result);
                switch (line.function) {
                case LogicalFunction::and_:
                    value = a && b;
                    break;
                case LogicalFunction::or_:
                    value = a || b;
                    break;
                case LogicalFunction::xor_:
                    value = a != b;
                    break;
                case LogicalFunction::srff:
                    value = !b && (a || ((state.latches & bit) != 0));
                    if (value)
                        state.latches |= bit;
                    else
                        state.latches &= static_cast<std::uint8_t>(~bit);
                    break;
                default:
                    break;
                }
            } else {
                std::int16_t source{};
                if (line.a < analog_count) {
                    if ((controls.active_analogs & (1U << line.a)) == 0)
                        continue;
                    source = controls.analogs[line.a];
                } else {
                    const auto index = line.a - analog_count;
                    if ((controls.active_switches & (1U << index)) == 0)
                        continue;
                    source = switch_value(controls.switches[index]);
                }
                if (line.inverted_a)
                    source = static_cast<std::int16_t>(-source);
                switch (line.function) {
                case LogicalFunction::greater:
                    value = source > line.threshold;
                    break;
                case LogicalFunction::less:
                    value = source < line.threshold;
                    break;
                case LogicalFunction::equal:
                    value = source == line.threshold;
                    break;
                case LogicalFunction::not_equal:
                    value = source != line.threshold;
                    break;
                case LogicalFunction::greater_equal:
                    value = source >= line.threshold;
                    break;
                case LogicalFunction::less_equal:
                    value = source <= line.threshold;
                    break;
                default:
                    break;
                }
            }
            result.available |= bit;
            if (value != line.inverted)
                result.active |= bit;
        }
    }
    return result;
}

constexpr std::int32_t source_value(const Source &source, const Controls &controls,
                                    const Frame &frame) noexcept {
    std::int32_t value{};
    switch (source.kind) {
    case SourceKind::analog:
        value = (controls.active_analogs & (1U << source.index)) != 0
                    ? controls.analogs[source.index]
                    : 0;
        break;
    case SourceKind::input:
        value = frame.inputs[source.index];
        break;
    case SourceKind::switch_position:
        if (multiswitch_index(source.index))
            value = !multiswitch_available(source.index, controls) ? 0
                    : multiswitch_active(source.index, controls)   ? resolution
                                                                   : -resolution;
        else
            value = (controls.active_switches & (1U << source.index)) != 0
                        ? switch_value(controls.switches[source.index])
                        : 0;
        break;
    case SourceKind::maximum:
        value = resolution;
        break;
    }
    return source.inverted ? -value : value;
}

constexpr std::int32_t positive_expo(const std::uint32_t value,
                                     const std::uint32_t amount) noexcept {
    const auto weight =
        static_cast<std::uint32_t>(round_divide(static_cast<std::int32_t>(amount) * 256, 100));
    auto cubic = value * value;
    cubic *= weight;
    cubic >>= 8;
    cubic *= value;
    cubic >>= 12;
    return static_cast<std::int32_t>((cubic + (256 - weight) * value + 128) >> 8);
}

constexpr std::int32_t expo(const std::int32_t value, const ExpoPercent setting) noexcept {
    const auto amount = setting.value();
    if (amount == 0) {
        return value;
    }
    const auto magnitude = static_cast<std::uint32_t>(value < 0 ? -value : value);
    const auto transformed = amount < 0
                                 ? resolution - positive_expo(resolution - magnitude,
                                                              static_cast<std::uint32_t>(-amount))
                                 : positive_expo(magnitude, static_cast<std::uint32_t>(amount));
    return value < 0 ? -transformed : transformed;
}

constexpr std::int8_t trim_origin(const InputLine &line) noexcept {
    if (line.trim == TrimSource::none) {
        return -1;
    }
    if (line.trim != TrimSource::automatic) {
        return static_cast<std::int8_t>(static_cast<std::uint8_t>(line.trim) -
                                        static_cast<std::uint8_t>(TrimSource::stick1));
    }
    if ((line.source.kind == SourceKind::analog) && (line.source.index < trim_count)) {
        return static_cast<std::int8_t>(line.source.index);
    }
    return -1;
}

constexpr std::int32_t mix_trim(const MixLine &line, const Controls &controls,
                                const std::array<std::int8_t, input_count> &origins,
                                const FixedBitset<input_count> reversed) noexcept {
    if (!line.carry_trim) {
        return 0;
    }
    std::int8_t origin{-1};
    bool reverse{};
    if ((line.source.kind == SourceKind::analog) && (line.source.index < trim_count)) {
        origin = static_cast<std::int8_t>(line.source.index);
    } else if (line.source.kind == SourceKind::input) {
        origin = origins[line.source.index];
        reverse = reversed[line.source.index];
    }
    if (origin < 0) {
        return 0;
    }
    const std::int32_t value = controls.trims[static_cast<std::size_t>(origin)].value() * 2;
    return reverse != line.source.inverted ? -value : value;
}

constexpr MixValue mix_contribution(const std::int32_t value, const MixLine &line) noexcept {
    const auto weight = round_divide(line.weight_percent.value() * 10 * MixValue::scale, 100);
    auto contribution = round_divide(value * weight, 10);
    contribution +=
        round_divide(round_divide(line.offset_percent.value() * 10 * resolution, 100), 10) *
        MixValue::scale;
    return MixValue::from_raw(contribution);
}

[[nodiscard]] constexpr bool combine_mix(MixValue &accumulated, const MixValue contribution,
                                         const Multiplex &multiplex) noexcept {
    std::int64_t combined = contribution.raw();
    switch (multiplex) {
    case Multiplex::add:
        combined = static_cast<std::int64_t>(accumulated.raw()) + contribution.raw();
        break;
    case Multiplex::multiply:
        // EdgeTX discards the contribution's fractional bits before multiplying.
        combined = floor_divide(
            floor_divide(contribution.raw(), MixValue::scale) * accumulated.raw(), resolution);
        break;
    case Multiplex::replace:
        break;
    }
    if ((combined < std::numeric_limits<std::int32_t>::min()) ||
        (combined > std::numeric_limits<std::int32_t>::max())) {
        return false;
    }
    accumulated = MixValue::from_raw(static_cast<std::int32_t>(combined));
    return true;
}

constexpr std::int16_t apply_output(const MixValue value, const Output &output) noexcept {
    constexpr auto full_scale = resolution * MixValue::scale;
    constexpr auto half_scale = full_scale / 2;
    const auto minimum = round_divide(output.minimum_permille.value() * resolution, 1000);
    const auto maximum = round_divide(output.maximum_permille.value() * resolution, 1000);
    auto center = std::clamp(round_divide(output.subtrim_permille.value() * resolution, 1000),
                             minimum, maximum);
    const auto limited = std::clamp(value.raw(), -full_scale, full_scale);
    const auto travel = limited > 0 ? maximum - center : center - minimum;
    const auto scaled = limited * travel;
    // Arithmetic floor division preserves EdgeTX rounding for negative values.
    const auto delta =
        floor_divide(scaled + (scaled < 0 ? half_scale - 1 : half_scale), full_scale);
    center += static_cast<std::int32_t>(delta);
    center = std::clamp(center, minimum, maximum);
    return static_cast<std::int16_t>(output.reversed ? -center : center);
}

} // namespace detail

constexpr std::int32_t evaluate_curve(const Curve &curve, const std::int32_t input) noexcept {
    const auto units = [](const std::int8_t percent) constexpr {
        return detail::round_divide(percent * resolution, 100);
    };
    if (input <= -resolution)
        return units(curve.y.front());
    if (input >= resolution)
        return units(curve.y.back());
    std::int32_t left_x = -resolution;
    auto left_y = units(curve.y.front());
    for (std::size_t i = 0; i < curve.y.size() - 1; ++i) {
        const auto right_x = i < curve.x.size() ? units(curve.x[i]) : resolution;
        const auto right_y = units(curve.y[i + 1]);
        if (input <= right_x)
            return detail::round_divide(left_y * (right_x - input) + right_y * (input - left_x),
                                        right_x - left_x);
        left_x = right_x;
        left_y = right_y;
    }
    return units(curve.y.back());
}

constexpr Status validate(const Model &model) noexcept {
    if (!detail::valid_tank(model.tank))
        return {ErrorCode::tank_settings, Stage::model, 0};
    if ((model.trim_switches >> trim_count) != 0)
        return {ErrorCode::input_settings, Stage::model, 0};
    if (const auto status = detail::validate_logical(model); !status)
        return status;
    for (std::size_t i = 0; i < model.curves.size(); ++i) {
        const auto &curve = model.curves[i];
        if ((curve.x.front() <= -100) || (curve.x.back() >= 100) || (curve.x[0] >= curve.x[1]) ||
            (curve.x[1] >= curve.x[2]))
            return {ErrorCode::curve_settings, Stage::model, i};
        for (const auto y : curve.y)
            if (!detail::in_range(y, -100, 100))
                return {ErrorCode::curve_settings, Stage::model, i};
    }
    const auto valid_condition = [&](const Condition &condition) constexpr {
        return detail::valid_condition(condition) &&
               (!condition.enabled || !logical_index(condition.index) ||
                (model.logical_switches[condition.index - switch_count].function !=
                 LogicalFunction::disabled));
    };
    std::array<std::uint16_t, multiswitch_page_count * multiswitch_function_count> destinations{};
    std::size_t destination_count{};
    for (std::size_t page_index = 0; page_index < model.multiswitch.size(); ++page_index) {
        const auto &page = model.multiswitch[page_index];
        for (std::size_t i = 0; i < page.controls.size(); ++i) {
            const auto &control = page.controls[i];
            // Button conditions cannot reference MultiSwitch states.
            if (((control.flags() & ~multiswitch_control::valid_mask) != 0) ||
                !valid_condition(control.condition()) ||
                (control.condition().index >= multiswitch_switch_base))
                return {ErrorCode::multiswitch_settings, Stage::model,
                        page_index * multiswitch_function_count + i};
            if (page.address < 0)
                continue;
            const auto destination = static_cast<std::uint16_t>(
                (static_cast<unsigned>(control.target_address(page.address))
                 << multiswitch_control::output_bits) |
                control.output());
            for (std::size_t previous = 0; previous < destination_count; ++previous)
                if (destinations[previous] == destination)
                    return {ErrorCode::multiswitch_duplicate, Stage::model,
                            page_index * multiswitch_function_count + i};
            destinations[destination_count++] = destination;
        }
    }
    for (std::size_t i = 0; i < model.inputs.size(); ++i) {
        const auto &line = model.inputs[i];
        if (line.destination >= input_count) {
            return {ErrorCode::destination, Stage::inputs, i};
        }
        if ((i > 0) && (line.destination < model.inputs[i - 1].destination)) {
            return {ErrorCode::line_order, Stage::inputs, i};
        }
        if (!detail::valid_source(line.source, false)) {
            return {ErrorCode::source, Stage::inputs, i};
        }
        if (!valid_condition(line.condition)) {
            return {ErrorCode::condition, Stage::inputs, i};
        }
        if ((static_cast<std::uint8_t>(line.side) >
             static_cast<std::uint8_t>(InputSide::positive)) ||
            (static_cast<std::uint8_t>(line.trim) >
             static_cast<std::uint8_t>(TrimSource::stick4)) ||
            (line.curve > curve_count)) {
            return {ErrorCode::input_settings, Stage::inputs, i};
        }
    }
#if CORETX_MODUL_TANKMIX
    std::array<std::uint8_t, 3> tank_roles{};
#else
    if (model.tank.enabled)
        return {ErrorCode::tank_unavailable, Stage::model, 0};
#endif
    for (std::size_t i = 0; i < model.mixes.size(); ++i) {
        const auto &line = model.mixes[i];
#if CORETX_MODUL_TANKMIX
        if (line.role > MixRole::tank_third)
            return {ErrorCode::tank_settings, Stage::mixes, i};
        const bool reserved = detail::tank_channel(model.tank, line.destination);
        if (line.role == MixRole::manual) {
            if (model.tank.enabled && reserved)
                return {ErrorCode::tank_conflict, Stage::mixes, i};
        } else {
            if (!model.tank.enabled || !reserved || !detail::canonical_tank_line(line, model.tank))
                return {ErrorCode::tank_settings, Stage::mixes, i};
            const bool third =
                model.tank.third_motor && (line.destination == model.tank.third_channel);
            if (third != (line.role == MixRole::tank_third))
                return {ErrorCode::tank_settings, Stage::mixes, i};
            auto &roles = tank_roles[third                                         ? 2
                                     : line.destination == model.tank.left_channel ? 0
                                                                                   : 1];
            const auto bit =
                static_cast<std::uint8_t>(1U << (static_cast<unsigned>(line.role) - 1));
            if ((roles & bit) != 0)
                return {ErrorCode::tank_settings, Stage::mixes, i};
            roles |= bit;
        }
#else
        if (line.role != MixRole::manual)
            return {ErrorCode::tank_unavailable, Stage::mixes, i};
#endif
        if (line.destination >= channel_count) {
            return {ErrorCode::destination, Stage::mixes, i};
        }
        if ((i > 0) && (line.destination < model.mixes[i - 1].destination)) {
            return {ErrorCode::line_order, Stage::mixes, i};
        }
        if (!detail::valid_source(line.source, true)) {
            return {ErrorCode::source, Stage::mixes, i};
        }
        if (!valid_condition(line.condition)) {
            return {ErrorCode::condition, Stage::mixes, i};
        }
        if ((static_cast<std::uint8_t>(line.multiplex) >
             static_cast<std::uint8_t>(Multiplex::replace)) ||
            (line.curve > curve_count)) {
            return {ErrorCode::mix_settings, Stage::mixes, i};
        }
    }
#if CORETX_MODUL_TANKMIX
    if (model.tank.enabled && ((tank_roles[0] != 7) || (tank_roles[1] != 7) ||
                               (tank_roles[2] != (model.tank.third_motor ? 8 : 0))))
        return {ErrorCode::tank_settings, Stage::model, 0};
#endif
    for (std::size_t i = 0; i < channel_count; ++i) {
        const auto &output = model.outputs[i];
        if (output.minimum_permille > output.maximum_permille) {
            return {ErrorCode::output_settings, Stage::outputs, i};
        }
    }
    return {};
}

#if CORETX_MODUL_TANKMIX
constexpr Status configure_tank(Model &model, const TankMixer &settings) noexcept {
    auto tank = settings;
    if (tank.third_curve == 0)
        tank.third_curve = model.tank.third_curve;
    bool initialize_curve{};
    if (tank.third_motor && (tank.third_curve == 0)) {
        for (std::size_t i = 0; i < curve_count; ++i) {
            if (model.curves[i] != Curve{})
                continue;
            bool used = model.tank.third_curve == i + 1;
            for (const auto &line : model.inputs)
                used |= line.curve == i + 1;
            for (const auto &line : model.mixes)
                used |= line.curve == i + 1;
            if (!used) {
                tank.third_curve = static_cast<std::uint8_t>(i + 1);
                initialize_curve = true;
                break;
            }
        }
        if (tank.third_curve == 0)
            return {ErrorCode::curve_capacity, Stage::model, curve_count};
    }
    if (!detail::valid_tank(tank))
        return {ErrorCode::tank_settings, Stage::model, 0};
    if (const auto status = validate(model); !status)
        return status;
    std::size_t manual_count{};
    for (std::size_t i = 0; i < model.mixes.size(); ++i) {
        const auto &line = model.mixes[i];
        if (line.role != MixRole::manual)
            continue;
        ++manual_count;
        if (tank.enabled && detail::tank_channel(tank, line.destination))
            return {ErrorCode::tank_conflict, Stage::mixes, i};
    }
    if (manual_count + (tank.enabled ? (tank.third_motor ? 7U : 6U) : 0U) > max_mix_lines)
        return {ErrorCode::capacity, Stage::mixes, manual_count};
    for (auto i = model.mixes.size(); i > 0; --i)
        if (model.mixes[i - 1].role != MixRole::manual)
            (void)model.mixes.try_erase(i - 1);
    // Preflight above guarantees all insertions succeed; no model-sized stack copy is needed.
    for (std::size_t channel = 0; channel < channel_count; ++channel) {
        if (tank.enabled && detail::tank_channel(tank, channel)) {
            std::size_t index{};
            while ((index < model.mixes.size()) && (model.mixes[index].destination < channel))
                ++index;
            if (tank.third_motor && (channel == tank.third_channel))
                (void)model.mixes.try_insert(
                    index, detail::tank_line(tank, static_cast<std::uint8_t>(channel),
                                             MixRole::tank_third));
            else {
                for (const auto role :
                     {MixRole::tank_drive, MixRole::tank_forward, MixRole::tank_reverse})
                    (void)model.mixes.try_insert(
                        index++, detail::tank_line(tank, static_cast<std::uint8_t>(channel), role));
            }
        }
    }
    if (initialize_curve)
        model.curves[tank.third_curve - 1] = {{-50, 0, 25}, {0, 0, 0, 0, 100}};
    model.tank = tank;
    return {};
}

constexpr bool tank_mix_active(const MixRole role, const Frame &frame) noexcept {
    return (role == MixRole::manual) || (role == MixRole::tank_drive) ||
           (role == MixRole::tank_third) ||
           ((role == MixRole::tank_forward) && !frame.tank_reverse) ||
           ((role == MixRole::tank_reverse) && frame.tank_reverse);
}
#endif

constexpr Evaluation evaluate(const Model &model, const Controls &controls,
                              LogicalState &state) noexcept {
    if (const auto status = validate(model); !status) {
        return {status, {}};
    }
    for (std::size_t i = 0; i < analog_count; ++i) {
        if (!detail::in_range(controls.analogs[i], -resolution, resolution)) {
            return {{ErrorCode::control_value, Stage::controls, i}, {}};
        }
    }
    for (std::size_t i = 0; i < switch_count; ++i) {
        if (!valid_switch_position(controls.switches[i])) {
            return {{ErrorCode::control_value, Stage::controls, analog_count + i}, {}};
        }
    }
    Evaluation result{};
    auto &frame = result.frame;
    auto next = state;
    frame.logical = detail::evaluate_logical(model, controls, next);
    std::array<std::int8_t, input_count> origins{};
    origins.fill(-1);
    FixedBitset<input_count> reversed{};
    FixedBitset<input_count> selected{};
    for (const auto &line : model.inputs) {
        const auto destination = line.destination;
        if (selected[destination] || !detail::matches(line.condition, controls, frame.logical)) {
            continue;
        }
        auto value = std::clamp(detail::source_value(line.source, controls, frame),
                                -static_cast<std::int32_t>(resolution),
                                static_cast<std::int32_t>(resolution));
        if (((line.side == InputSide::negative) && (value >= 0)) ||
            ((line.side == InputSide::positive) && (value < 0))) {
            continue;
        }
        value = detail::expo(value, line.expo_percent);
        if (line.curve != 0)
            value = evaluate_curve(model.curves[line.curve - 1], value);
        value = detail::round_divide(value * line.weight_percent.value() * 10, 1000);
        value += detail::round_divide(
            detail::round_divide(line.offset_percent.value() * 10 * resolution, 100), 10);
        frame.inputs[destination] = static_cast<std::int16_t>(value);
        origins[destination] = detail::trim_origin(line);
        if (line.weight_percent.value() < 0) {
            reversed.set(destination);
        }
        selected.set(destination);
    }
#if CORETX_MODUL_TANKMIX
    if (model.tank.enabled && model.tank.reverse_steering) {
        // Direction follows the processed drive input, before independently carried mixer trim.
        const auto drive = frame.inputs[model.tank.drive_input];
        const auto threshold =
            detail::round_divide(model.tank.hysteresis_percent * resolution, 100);
        if (drive > threshold)
            next.tank_reverse = false;
        else if (drive < -threshold)
            next.tank_reverse = true;
    } else {
        next.tank_reverse = false;
    }
    frame.tank_reverse = next.tank_reverse;
#endif
    for (std::size_t i = 0; i < model.mixes.size(); ++i) {
        const auto &line = model.mixes[i];
#if CORETX_MODUL_TANKMIX
        if (!tank_mix_active(line.role, frame))
            continue;
#endif
        if (!detail::matches(line.condition, controls, frame.logical)) {
            continue;
        }
        auto value = detail::source_value(line.source, controls, frame) +
                     detail::mix_trim(line, controls, origins, reversed);
        if (line.curve != 0)
            value = evaluate_curve(model.curves[line.curve - 1], value);
        const auto contribution = detail::mix_contribution(value, line);
        auto &accumulated = frame.mixes[line.destination];
        if ((i > 0) && (model.mixes[i - 1].destination == line.destination)) {
            if (!detail::combine_mix(accumulated, contribution, line.multiplex)) {
                return {{ErrorCode::arithmetic_overflow, Stage::mixes, i}, {}};
            }
        } else {
            accumulated = contribution;
        }
    }
    for (std::size_t i = 0; i < channel_count; ++i) {
        frame.channels[i] = detail::apply_output(frame.mixes[i], model.outputs[i]);
    }
    state = next;
    return result;
}

constexpr Evaluation evaluate(const Model &model, const Controls &controls) noexcept {
    LogicalState state{};
    return evaluate(model, controls, state);
}

constexpr const char *error_message(const ErrorCode &code) noexcept {
    switch (code) {
    case ErrorCode::none:
        return "success";
    case ErrorCode::line_count:
        return "too many input or mixer lines";
    case ErrorCode::destination:
        return "destination index out of range";
    case ErrorCode::line_order:
        return "lines must be grouped in ascending destination order";
    case ErrorCode::source:
        return "invalid or unsupported source";
    case ErrorCode::condition:
        return "invalid switch condition";
    case ErrorCode::input_settings:
        return "invalid input settings";
    case ErrorCode::mix_settings:
        return "invalid mixer settings";
    case ErrorCode::output_settings:
        return "invalid output settings";
    case ErrorCode::control_value:
        return "control value out of range";
    case ErrorCode::arithmetic_overflow:
        return "mixer accumulator overflow";
    case ErrorCode::logical_settings:
        return "invalid logical switch";
    case ErrorCode::logical_cycle:
        return "logical switch cycle";
    case ErrorCode::dashboard_settings:
        return "invalid dashboard channel";
    case ErrorCode::multiswitch_settings:
        return "invalid MultiSwitch page";
    case ErrorCode::multiswitch_duplicate:
        return "duplicate MultiSwitch target";
    case ErrorCode::curve_settings:
        return "invalid curve points";
    case ErrorCode::tank_settings:
        return "invalid tank mixer settings or managed lines";
    case ErrorCode::tank_conflict:
        return "tank channel contains manual mixer lines";
    case ErrorCode::capacity:
        return "not enough mixer line capacity";
    case ErrorCode::curve_capacity:
        return "no unused default model curve";
    case ErrorCode::tank_unavailable:
        return "tank mixer module not compiled in";
    }
    return "unknown error";
}

constexpr const char *stage_name(const Stage &stage) noexcept {
    switch (stage) {
    case Stage::model:
        return "model";
    case Stage::inputs:
        return "inputs";
    case Stage::mixes:
        return "mixes";
    case Stage::outputs:
        return "outputs";
    case Stage::controls:
        return "controls";
    }
    return "unknown stage";
}

} // namespace coretx
