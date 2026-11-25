// SPDX-License-Identifier: GPL-2.0-or-later
#include "coretx/core/core.hpp"
#include "edgetx_reference.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <type_traits>

namespace {

void check(const bool condition, const char *const expression, const int line) {
    if (!condition) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
        std::exit(EXIT_FAILURE);
    }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

constexpr coretx::Model direct_model() {
    coretx::Model model{};
    model.inputs.resize<1>();
    model.mixes.resize<1>();
    return model;
}

constexpr bool constant_pipeline() {
    auto model = direct_model();
    coretx::Controls controls{};
    controls.analogs[0] = 512;
    controls.trims[0] = coretx::TrimSteps::constant<20>();
    model.inputs[0].weight_percent = coretx::InputWeight::constant<-50>();
    const auto weighted = coretx::evaluate(model, controls);
    if (!weighted || (weighted.frame.inputs[0] != -256) || (weighted.frame.channels[0] != -296)) {
        return false;
    }
    model.inputs[0].condition = {true, 0, coretx::SwitchPosition::up, false};
    controls.switches[0] = coretx::SwitchPosition::down;
    if (coretx::evaluate(model, controls).frame.channels[0] != 0) {
        return false;
    }
    model = direct_model();
    model.mixes.resize<2>();
    model.mixes[0].source.kind = coretx::SourceKind::maximum;
    model.mixes[0].weight_percent = coretx::MixWeight::constant<50>();
    model.mixes[1] = model.mixes[0];
    model.mixes[1].multiplex = coretx::Multiplex::multiply;
    model.outputs[0].reversed = true;
    if (coretx::evaluate(model, controls).frame.channels[0] != -256) {
        return false;
    }
    model.outputs[0].minimum_permille = coretx::TravelPermille::constant<1001>();
    const auto invalid = coretx::evaluate(model, controls);
    return !coretx::validate(model) && !invalid &&
           (invalid.status.code == coretx::ErrorCode::output_settings) &&
           (invalid.status.stage == coretx::Stage::outputs);
}

static_assert(constant_pipeline());

constexpr bool curve_interpolation() {
    using namespace coretx;
    constexpr Curve identity{};
    for (int input = -1100; input <= 1100; ++input)
        if (evaluate_curve(identity, input) != std::clamp(input, -1024, 1024))
            return false;
    constexpr Curve curve{{-75, -25, 60}, {80, -60, 30, -90, 50}};
    constexpr std::array knots{-1024, -768, -256, 614, 1024};
    constexpr std::array values{819, -614, 307, -922, 512};
    for (std::size_t i = 0; i < knots.size(); ++i)
        if (evaluate_curve(curve, knots[i]) != values[i])
            return false;
    return (evaluate_curve(curve, -896) == 103) && (evaluate_curve(curve, -512) == -154) &&
           (evaluate_curve(curve, 179) == -308) && (evaluate_curve(curve, 819) == -205) &&
           (evaluate_curve(curve, std::numeric_limits<std::int32_t>::min()) == 819) &&
           (evaluate_curve(curve, std::numeric_limits<std::int32_t>::max()) == 512) &&
           (evaluate_curve(Curve{{-33, 1, 99}, {-100, -1, 1, 99, 100}}, -338) == -10);
}
static_assert(curve_interpolation());

constexpr bool curve_pipeline() {
    using namespace coretx;
    auto model = direct_model();
    Controls controls{};
    controls.analogs[0] = 512;
    for (std::uint8_t selected = 1; selected <= curve_count; ++selected) {
        model.inputs[0].curve = model.mixes[0].curve = selected;
        const auto identity = evaluate(model, controls);
        if (!identity || (identity.frame.inputs[0] != 512) ||
            (identity.frame.mixes[0].raw() != 512 * MixValue::scale))
            return false;
    }
    model.curves[3].y = {-100, 100, 0, -100, 100};
    model.inputs[0].curve = model.mixes[0].curve = 4;
    model.inputs[0].expo_percent = ExpoPercent::constant<100>();
    model.inputs[0].weight_percent = InputWeight::constant<50>();
    model.inputs[0].offset_percent = InputOffset::constant<25>();
    model.mixes[0].weight_percent = MixWeight::constant<50>();
    model.mixes[0].offset_percent = MixOffset::constant<25>();
    controls.trims[0] = TrimSteps::constant<20>();
    const auto transformed = evaluate(model, controls);
    if (!transformed || (transformed.frame.inputs[0] != 128) ||
        (transformed.frame.mixes[0].raw() != 88 * MixValue::scale))
        return false;
    model.mixes[0].carry_trim = false;
    if (evaluate(model, controls).frame.mixes[0].raw() != 128 * MixValue::scale)
        return false;
    model = direct_model();
    controls.analogs[0] = resolution;
    controls.trims[0] = TrimSteps::constant<127>();
    model.inputs[0].offset_percent = InputOffset::constant<100>();
    if (evaluate(model, controls).frame.mixes[0].raw() != 2302 * MixValue::scale)
        return false; // Disabling a curve must not introduce a clamp.
    model.mixes[0].curve = 1;
    if (evaluate(model, controls).frame.mixes[0].raw() != 1024 * MixValue::scale)
        return false;
    model.mixes[0].source = {SourceKind::analog, 0, true};
    model.curves[0].y.front() = 25;
    return evaluate(model, controls).frame.mixes[0].raw() == 256 * MixValue::scale;
}
static_assert(curve_pipeline());

constexpr bool curve_validation() {
    using namespace coretx;
    constexpr std::array invalid{Curve{{-50, -50, 50}},
                                 Curve{{50, 0, -50}},
                                 Curve{{-100, 0, 50}},
                                 Curve{{-50, 0, 100}},
                                 Curve{{-50, 0, 50}, {-101, 0, 0, 0, 100}},
                                 Curve{{-50, 0, 50}, {-100, 0, 0, 0, 101}}};
    for (const auto &curve : invalid) {
        auto model = direct_model();
        model.curves[2] = curve;
        const auto status = validate(model);
        if (status || (status.code != ErrorCode::curve_settings) ||
            (status.stage != Stage::model) || (status.index != 2) || evaluate(model, Controls{}))
            return false;
    }
    auto model = direct_model();
    model.inputs[0].curve = 5;
    auto status = validate(model);
    if (status || (status.code != ErrorCode::input_settings) || (status.stage != Stage::inputs))
        return false;
    model.inputs[0].curve = 0;
    model.mixes[0].curve = 255;
    status = validate(model);
    return !status && (status.code == ErrorCode::mix_settings) && (status.stage == Stage::mixes);
}
static_assert(curve_validation());

constexpr bool assignable_input_trims() {
    using namespace coretx;
    auto model = direct_model();
    Controls controls{};
    controls.analogs[0] = 512;
    controls.trims[3] = TrimSteps::constant<10>();
    model.inputs[0].trim = TrimSource::stick4;
    if (evaluate(model, controls).frame.channels[0] != 532)
        return false;
    model.mixes[0].source.inverted = true;
    if (evaluate(model, controls).frame.channels[0] != -532)
        return false;
    model.mixes[0].source.inverted = false;
    model.mixes[0].carry_trim = false;
    model.inputs[0].condition = {true, physical_switch_count, SwitchPosition::down};
    controls.switches[physical_switch_count] = SwitchPosition::down;
    if (evaluate(model, controls).frame.channels[0] != 512)
        return false;
    controls.switches[physical_switch_count] = SwitchPosition::middle;
    if (evaluate(model, controls).frame.channels[0] != 0)
        return false;
    model.inputs[0].condition.position = SwitchPosition::up;
    controls.switches[physical_switch_count] = SwitchPosition::up;
    if (evaluate(model, controls).frame.channels[0] != 512)
        return false;
    controls.active_switches &= ~(1U << physical_switch_count);
    model.inputs[0].condition.inverted = true;
    return evaluate(model, controls).frame.channels[0] == 0;
}
static_assert(assignable_input_trims());

constexpr bool logical_predicates() {
    using namespace coretx;
    auto model = direct_model();
    Controls controls{};
    controls.analogs[0] = 512;
    auto &comparison = model.logical_switches[7];
    comparison.a = 0;
    comparison.threshold = 512;
    constexpr std::array functions{LogicalFunction::greater,       LogicalFunction::less,
                                   LogicalFunction::equal,         LogicalFunction::not_equal,
                                   LogicalFunction::greater_equal, LogicalFunction::less_equal};
    constexpr std::array expected{false, false, true, false, true, true};
    for (std::size_t i = 0; i < functions.size(); ++i) {
        comparison.function = functions[i];
        const auto frame = evaluate(model, controls);
        if (!frame || (((frame.frame.logical.active & 0x80U) != 0) != expected[i]))
            return false;
    }
    comparison.function = LogicalFunction::less;
    comparison.inverted_a = true;
    comparison.threshold = 0;
    auto &gate = model.logical_switches[0];
    gate.function = LogicalFunction::and_;
    gate.a = switch_count + 7;
    gate.position_a = SwitchPosition::down;
    gate.b = 0;
    gate.position_b = SwitchPosition::up;
    controls.switches[0] = SwitchPosition::up;
    model.inputs[0].condition = {true, switch_count, SwitchPosition::down, false};
    model.mixes[0].condition = {true, switch_count + 7, SwitchPosition::down, false};
    if (evaluate(model, controls).frame.channels[0] != 512)
        return false;
    for (const auto function :
         {LogicalFunction::and_, LogicalFunction::or_, LogicalFunction::xor_}) {
        gate.function = function;
        for (const bool b : {false, true}) {
            controls.switches[0] = b ? SwitchPosition::up : SwitchPosition::down;
            for (const bool inverted : {false, true}) {
                gate.inverted = inverted;
                const bool value = function == LogicalFunction::and_  ? b
                                   : function == LogicalFunction::or_ ? true
                                                                      : !b;
                const auto result = evaluate(model, controls);
                if (!result || (((result.frame.logical.active & 1U) != 0) != (value != inverted)))
                    return false;
            }
        }
    }
    gate.inverted = false;
    gate.inverted_a = true;
    gate.inverted_b = true;
    controls.switches[0] = SwitchPosition::down;
    gate.function = LogicalFunction::xor_;
    model.inputs[0].condition.inverted = true;
    if ((evaluate(model, controls).frame.logical.active & 1U) == 0)
        return false;
    gate.function = LogicalFunction::and_;
    if (evaluate(model, controls).frame.channels[0] != 512)
        return false;
    controls.active_analogs = 0;
    auto result = evaluate(model, controls);
    if (!result || (result.frame.logical.available != 0) || (result.frame.channels[0] != 0))
        return false;
    controls.active_analogs = 1;
    comparison.a = 16;
    comparison.inverted_a = false;
    comparison.function = LogicalFunction::equal;
    comparison.threshold = -resolution;
    controls.switches[0] = SwitchPosition::up;
    if ((evaluate(model, controls).frame.logical.active & 0x80U) == 0)
        return false;
    controls.active_switches = 0;
    return evaluate(model, controls).frame.logical.available == 0;
}

constexpr bool logical_validation_and_latches() {
    using namespace coretx;
    auto model = direct_model();
    auto &line = model.logical_switches[0];
    line.function = LogicalFunction::srff;
    line.a = 0;
    line.b = 1;
    line.position_a = line.position_b = SwitchPosition::down;
    model.mixes[0].condition = {true, switch_count, SwitchPosition::down, false};
    Controls controls{};
    controls.analogs[0] = resolution;
    LogicalState state{};
    const auto run = [&]() constexpr { return evaluate(model, controls, state); };
    if (!run() || (state.latches != 0))
        return false;
    controls.switches[0] = SwitchPosition::down;
    if ((run().frame.channels[0] != resolution) || (state.latches != 1))
        return false;
    controls.switches[0] = SwitchPosition::up;
    if ((run().frame.channels[0] != resolution) || (state.latches != 1))
        return false;
    line.function = LogicalFunction::disabled;
    model.mixes[0].condition.enabled = false;
    if (!run() || (state.latches != 1))
        return false;
    line.function = LogicalFunction::srff;
    model.mixes[0].condition.enabled = true;
    if ((run().frame.channels[0] != resolution) || (state.latches != 1))
        return false;
    controls.active_switches = 1;
    line.inverted = true;
    if ((run().frame.logical.available != 0) || (state.latches != 1))
        return false;
    controls.active_switches = 3;
    line.inverted = false;
    controls.switches[0] = controls.switches[1] = SwitchPosition::down;
    if ((run().frame.channels[0] != 0) || (state.latches != 0))
        return false; // Reset dominates Set.
    controls.switches[1] = SwitchPosition::up;
    model.mixes.resize<24>();
    for (auto &mix : model.mixes) {
        mix = {};
        mix.source = {SourceKind::maximum, 0, false};
        mix.weight_percent = MixWeight::constant<500>();
        mix.multiplex = Multiplex::multiply;
    }
    const auto failed = run();
    if (failed || (failed.status.code != ErrorCode::arithmetic_overflow) || (state.latches != 0) ||
        (failed.frame.logical.active != 0))
        return false;
    model = direct_model();
    model.logical_switches[0].function = LogicalFunction::and_;
    model.logical_switches[0].a = switch_count;
    model.logical_switches[0].position_a = SwitchPosition::down;
    if (validate(model).code != ErrorCode::logical_cycle)
        return false;
    model.logical_switches[0].a = switch_count + 1;
    if (validate(model).code != ErrorCode::logical_settings)
        return false; // Disabled references are rejected.
    model.logical_switches[1] = model.logical_switches[0];
    model.logical_switches[1].a = switch_count;
    if (validate(model).code != ErrorCode::logical_cycle)
        return false;
    model.logical_switches[0].a = switch_count + logical_switch_count;
    if (validate(model).code != ErrorCode::logical_settings)
        return false;
    model.logical_switches = {};
    model.logical_switches[0].function = LogicalFunction::greater;
    model.logical_switches[0].a = analog_count + switch_count;
    if (validate(model).code != ErrorCode::logical_settings)
        return false;
    model.logical_switches[0].a = 0;
    model.logical_switches[0].threshold = 1025;
    if (validate(model).code != ErrorCode::logical_settings)
        return false;
    model.logical_switches = {};
    model.inputs[0].condition = {true, switch_count, SwitchPosition::down, false};
    if (validate(model).code != ErrorCode::condition)
        return false;
    model.inputs[0].condition = {};
    model.dashboard_channels[0] = 17;
    return validate(model).code == ErrorCode::dashboard_settings;
}

static_assert(logical_predicates());
static_assert(logical_validation_and_latches());

constexpr bool bitmap_input_selection() {
    coretx::Model model{};
    model.inputs.resize<3>();
    model.mixes.resize<1>();
    constexpr auto last = static_cast<std::uint8_t>(coretx::input_count - 1);
    for (auto &line : model.inputs) {
        line.destination = last;
    }
    model.inputs[0].condition = {true, 0, coretx::SwitchPosition::down, false};
    model.inputs[1].weight_percent = coretx::InputWeight::constant<-100>();
    model.inputs[2].source.index = 1;
    model.mixes[0].source.index = last;
    coretx::Controls controls{};
    controls.analogs[0] = 128;
    controls.analogs[1] = 700;
    controls.trims[0] = coretx::TrimSteps::constant<11>();
    auto result = coretx::evaluate(model, controls);
    if (!result || (result.frame.inputs[last] != -128) || (result.frame.channels[0] != -150)) {
        return false;
    }
    controls.switches[0] = coretx::SwitchPosition::down;
    result = coretx::evaluate(model, controls);
    if (!result || (result.frame.inputs[last] != 128) || (result.frame.channels[0] != 150)) {
        return false;
    }
    model.mixes[0].source.inverted = true;
    result = coretx::evaluate(model, controls);
    if (!result || (result.frame.channels[0] != -150)) {
        return false;
    }
    model.mixes[0].source.index = 32;
    result = coretx::evaluate(model, controls);
    return !result && (result.status.code == coretx::ErrorCode::source);
}

static_assert(bitmap_input_selection());

template <typename Value, std::int32_t Number>
concept HasBoundedConstant = requires { Value::template constant<Number>(); };

static_assert(!HasBoundedConstant<coretx::InputWeight, 101>);
static_assert(!HasBoundedConstant<coretx::TrimSteps, -128>);
static_assert(!HasBoundedConstant<coretx::TrimSteps, 128>);
static_assert(!std::is_constructible_v<coretx::InputWeight, std::int32_t>);
static_assert(!std::is_convertible_v<coretx::InputWeight, std::int32_t>);
static_assert(!std::is_assignable_v<coretx::InputWeight &, coretx::InputOffset>);
static_assert(!std::is_assignable_v<coretx::MixWeight &, coretx::MixOffset>);
static_assert(!std::is_assignable_v<coretx::InputWeight &, coretx::MixWeight>);

template <typename Value> constexpr bool bounded_limits() {
    Value value{};
    if (!value.try_set(Value::minimum) || (value.value() != Value::minimum) ||
        value.try_set(Value::minimum - 1) || (value.value() != Value::minimum)) {
        return false;
    }
    if (!value.try_set(Value::maximum) || (value.value() != Value::maximum) ||
        value.try_set(Value::maximum + 1) || (value.value() != Value::maximum)) {
        return false;
    }
    return !value.try_set(std::numeric_limits<std::int32_t>::min()) &&
           !value.try_set(std::numeric_limits<std::int32_t>::max()) &&
           (value.value() == Value::maximum);
}

constexpr bool bounded_values() {
    return bounded_limits<coretx::InputWeight>() && bounded_limits<coretx::MixWeight>() &&
           bounded_limits<coretx::InputOffset>() && bounded_limits<coretx::MixOffset>() &&
           bounded_limits<coretx::ExpoPercent>() && bounded_limits<coretx::TravelPermille>() &&
           bounded_limits<coretx::SubtrimPermille>() && bounded_limits<coretx::TrimSteps>();
}

static_assert(bounded_values());

static_assert(!std::is_convertible_v<std::int32_t, coretx::MixValue>);
static_assert(!std::is_convertible_v<coretx::MixValue, std::int32_t>);
static_assert(!std::is_constructible_v<coretx::MixValue, std::int32_t>);

constexpr bool fractional_mix_arithmetic() {
    auto value = coretx::MixValue::from_raw(128);
    if (!coretx::detail::combine_mix(value, coretx::MixValue::from_raw(-1),
                                     coretx::Multiplex::add) ||
        (value.raw() != 127)) {
        return false;
    }
    value = coretx::MixValue::from_raw(262272);
    if (!coretx::detail::combine_mix(value, coretx::MixValue::from_raw(-1),
                                     coretx::Multiplex::multiply) ||
        (value.raw() != -257)) {
        return false;
    }
    value = coretx::MixValue::from_raw(262272);
    return coretx::detail::combine_mix(value, coretx::MixValue::from_raw(255),
                                       coretx::Multiplex::multiply) &&
           (value == coretx::MixValue{});
}

constexpr bool checked_mix_arithmetic() {
    for (const std::int32_t limit :
         {std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()}) {
        const auto original = coretx::MixValue::from_raw(limit);
        auto value = original;
        if (coretx::detail::combine_mix(value, coretx::MixValue::from_raw(limit < 0 ? -1 : 1),
                                        coretx::Multiplex::add) ||
            (value != original)) {
            return false;
        }
        if (coretx::detail::combine_mix(value, coretx::MixValue::from_channel_units(2048),
                                        coretx::Multiplex::multiply) ||
            (value != original)) {
            return false;
        }
        if (!coretx::detail::combine_mix(value, coretx::MixValue::from_channel_units(1024),
                                         coretx::Multiplex::multiply) ||
            (value != original)) {
            return false;
        }
    }
    return true;
}

static_assert(fractional_mix_arithmetic());
static_assert(checked_mix_arithmetic());

void passthrough_and_input_priority() {
    auto model = direct_model();
    coretx::Controls controls{};
    for (int value = -1024; value <= 1024; ++value) {
        controls.analogs[0] = static_cast<std::int16_t>(value);
        const auto result = coretx::evaluate(model, controls);
        CHECK(result);
        CHECK(result.frame.inputs[0] == value);
        CHECK(result.frame.mixes[0] ==
              coretx::MixValue::from_channel_units(static_cast<std::int16_t>(value)));
        CHECK(result.frame.channels[0] == value);
    }

    model.inputs.resize<3>();
    model.inputs[0].condition = {true, 0, coretx::SwitchPosition::down, false};
    model.inputs[0].weight_percent = coretx::InputWeight::constant<25>();
    model.inputs[1].side = coretx::InputSide::negative;
    model.inputs[1].weight_percent = coretx::InputWeight::constant<50>();
    model.inputs[2].weight_percent = coretx::InputWeight::constant<75>();
    controls.analogs[0] = 512;
    controls.switches[0] = coretx::SwitchPosition::up;
    CHECK(coretx::evaluate(model, controls).frame.inputs[0] == 384);
    controls.analogs[0] = -512;
    CHECK(coretx::evaluate(model, controls).frame.inputs[0] == -256);
    controls.switches[0] = coretx::SwitchPosition::down;
    CHECK(coretx::evaluate(model, controls).frame.inputs[0] == -128);

    model.inputs[0].condition.inverted = true;
    CHECK(coretx::evaluate(model, controls).frame.inputs[0] == -256);
    model.inputs[1].condition = {true, 0, coretx::SwitchPosition::middle, false};
    model.inputs[2].condition = model.inputs[1].condition;
    const auto missing = coretx::evaluate(model, controls);
    CHECK(missing);
    CHECK(missing.frame.inputs[0] == 0);
    CHECK(missing.frame.channels[0] == 0);
}

void input_reference_sweep() {
    auto model = direct_model();
    coretx::Controls controls{};
    for (const int amount : {-100, -73, -30, 0, 30, 73, 100}) {
        CHECK(model.inputs[0].expo_percent.try_set(amount));
        for (const int weight : {-100, -33, 0, 33, 100}) {
            CHECK(model.inputs[0].weight_percent.try_set(weight));
            for (const int offset : {-100, -17, 0, 17, 100}) {
                CHECK(model.inputs[0].offset_percent.try_set(offset));
                for (int value = -1024; value <= 1024; ++value) {
                    controls.analogs[0] = static_cast<std::int16_t>(value);
                    const auto result = coretx::evaluate(model, controls);
                    CHECK(result);
                    const auto expected = edgetx_reference::input(value, amount, weight, offset);
                    CHECK(result.frame.inputs[0] == expected);
                    CHECK(result.frame.channels[0] ==
                          edgetx_reference::output(expected * 256, -1000, 1000, 0, false));
                }
            }
        }
    }
}

void trims_and_source_inversion() {
    auto model = direct_model();
    coretx::Controls controls{};
    controls.analogs[0] = 300;
    controls.trims[0] = coretx::TrimSteps::constant<20>();
    CHECK(coretx::evaluate(model, controls).frame.inputs[0] == 300);
    CHECK(coretx::evaluate(model, controls).frame.channels[0] == 340);
    model.inputs[0].weight_percent = coretx::InputWeight::constant<-50>();
    CHECK(coretx::evaluate(model, controls).frame.channels[0] == -190);
    model.mixes[0].source.inverted = true;
    CHECK(coretx::evaluate(model, controls).frame.channels[0] == 190);
    model.mixes[0].carry_trim = false;
    CHECK(coretx::evaluate(model, controls).frame.channels[0] == 150);
    model.mixes[0].carry_trim = true;
    model.inputs[0].trim = coretx::TrimSource::none;
    CHECK(coretx::evaluate(model, controls).frame.channels[0] == 150);
    model.inputs[0].trim = coretx::TrimSource::stick2;
    controls.trims[1] = coretx::TrimSteps::constant<-15>();
    CHECK(coretx::evaluate(model, controls).frame.channels[0] == 120);

    model.mixes[0].source = {coretx::SourceKind::analog, 0, true};
    CHECK(coretx::evaluate(model, controls).frame.channels[0] == -340);
    model.mixes[0].source = {coretx::SourceKind::maximum, 0, false};
    CHECK(coretx::evaluate(model, controls).frame.channels[0] == 1024);
    model.mixes[0].source = {coretx::SourceKind::switch_position, 0, false};
    controls.switches[0] = coretx::SwitchPosition::up;
    CHECK(coretx::evaluate(model, controls).frame.channels[0] == -1024);
}

void reduced_trim_range() {
    const auto model = direct_model();
    coretx::Controls controls{};
    CHECK(controls.trims[0].try_set(127));
    CHECK(!controls.trims[0].try_set(128));
    CHECK(coretx::evaluate(model, controls).frame.channels[0] == 254);
    CHECK(controls.trims[0].try_set(-127));
    CHECK(!controls.trims[0].try_set(-128));
    CHECK(coretx::evaluate(model, controls).frame.channels[0] == -254);
}

void multiplex_order_and_unclipped_accumulators() {
    auto model = direct_model();
    model.mixes.resize<3>();
    model.mixes[0].source = {coretx::SourceKind::analog, 0, false};
    model.mixes[1].source = {coretx::SourceKind::analog, 1, false};
    model.mixes[2].source = {coretx::SourceKind::analog, 2, false};
    coretx::Controls controls{};
    controls.analogs[0] = 1024;
    controls.analogs[1] = 1024;
    controls.analogs[2] = -1024;
    CHECK(coretx::evaluate(model, controls).frame.channels[0] == 1024);
    model.mixes[2].weight_percent = coretx::MixWeight::constant<50>();
    const auto result = coretx::evaluate(model, controls);
    CHECK(result);
    CHECK(result.frame.mixes[0] == coretx::MixValue::from_channel_units(1536));
    CHECK(result.frame.channels[0] == 1024);

    controls.analogs[0] = 300;
    controls.analogs[1] = 500;
    controls.analogs[2] = 512;
    model.mixes[1].multiplex = coretx::Multiplex::replace;
    model.mixes[2].multiplex = coretx::Multiplex::multiply;
    model.mixes[2].weight_percent = coretx::MixWeight::constant<100>();
    CHECK(coretx::evaluate(model, controls).frame.channels[0] == 250);
    model.mixes[0].condition = {true, 0, coretx::SwitchPosition::down, false};
    model.mixes[1].condition = model.mixes[0].condition;
    CHECK(coretx::evaluate(model, controls).frame.channels[0] == 0);
    // Only the first configured line ignores its multiplex selector.
    model.mixes.resize<1>();
    model.mixes[0].condition = {};
    model.mixes[0].multiplex = coretx::Multiplex::multiply;
    CHECK(coretx::evaluate(model, controls).frame.channels[0] == 300);
}

void mixer_reference_sweep() {
    auto model = direct_model();
    model.mixes[0].source = {coretx::SourceKind::analog, 0, false};
    model.mixes[0].carry_trim = false;
    model.mixes.resize<2>();
    model.mixes[1].source = {coretx::SourceKind::analog, 1, false};
    model.mixes[1].carry_trim = false;
    model.mixes[1].multiplex = coretx::Multiplex::multiply;
    coretx::Controls controls{};
    controls.analogs[1] = -517;
    for (const int weight : {-500, -101, -33, 0, 33, 101, 500}) {
        CHECK(model.mixes[0].weight_percent.try_set(weight));
        for (const int offset : {-500, -17, 0, 17, 500}) {
            CHECK(model.mixes[0].offset_percent.try_set(offset));
            for (int value = -1024; value <= 1024; ++value) {
                controls.analogs[0] = static_cast<std::int16_t>(value);
                const auto result = coretx::evaluate(model, controls);
                CHECK(result);
                const auto first = edgetx_reference::contribution(value, weight, offset);
                const auto second = edgetx_reference::contribution(-517, 100, 0);
                const auto expected = edgetx_reference::multiply(first, second);
                CHECK(result.frame.mixes[0].raw() == expected);
                CHECK(result.frame.channels[0] ==
                      edgetx_reference::output(expected, -1000, 1000, 0, false));
            }
        }
    }
}

void output_reference_sweep() {
    auto model = direct_model();
    model.mixes[0].weight_percent = coretx::MixWeight::constant<175>();
    coretx::Controls controls{};
    using Travel = coretx::TravelPermille;
    using Subtrim = coretx::SubtrimPermille;
    constexpr std::array<coretx::Output, 7> outputs{
        coretx::Output{},
        coretx::Output{Travel::constant<-700>(), Travel::constant<1300>(), Subtrim::constant<123>(),
                       false},
        coretx::Output{Travel::constant<-1500>(), Travel::constant<1500>(),
                       Subtrim::constant<-900>(), true},
        coretx::Output{Travel::constant<-100>(), Travel::constant<200>(), Subtrim::constant<900>(),
                       false},
        coretx::Output{Travel::constant<-200>(), Travel::constant<100>(), Subtrim::constant<-900>(),
                       true},
        coretx::Output{Travel::constant<100>(), Travel::constant<900>(), {}, false},
        coretx::Output{Travel::constant<-500>(), Travel::constant<-500>(), {}, true}};
    for (const auto output : outputs) {
        model.outputs[0] = output;
        for (int value = -1024; value <= 1024; ++value) {
            controls.analogs[0] = static_cast<std::int16_t>(value);
            const auto result = coretx::evaluate(model, controls);
            CHECK(result);
            const auto mixed = edgetx_reference::contribution(value, 175, 0);
            CHECK(result.frame.channels[0] ==
                  edgetx_reference::output(mixed, output.minimum_permille.value(),
                                           output.maximum_permille.value(),
                                           output.subtrim_permille.value(), output.reversed));
        }
    }
    // Exercise half-step rounding independently of the default integral output path.
    model.mixes[0].weight_percent = coretx::MixWeight::constant<50>();
    model.outputs[0] = {};
    controls.analogs[0] = 1;
    CHECK(coretx::evaluate(model, controls).frame.channels[0] == 1);
    controls.analogs[0] = -1;
    CHECK(coretx::evaluate(model, controls).frame.channels[0] == -1);
}

void independent_channels_and_capacity() {
    coretx::Model model{};
    model.inputs.resize<coretx::max_input_lines>();
    model.mixes.resize<coretx::max_mix_lines>();
    coretx::Controls controls{};
    for (std::size_t i = 0; i < coretx::analog_count; ++i) {
        controls.analogs[i] = static_cast<std::int16_t>((i % coretx::input_count) * 100);
    }
    for (std::size_t i = 0; i < coretx::max_input_lines; ++i) {
        model.inputs[i].destination = static_cast<std::uint8_t>(i / 2);
        model.inputs[i].source.index = static_cast<std::uint8_t>(i / 2);
    }
    for (std::size_t i = 0; i < coretx::max_mix_lines; ++i) {
        model.mixes[i].destination =
            static_cast<std::uint8_t>(i * coretx::channel_count / coretx::max_mix_lines);
        model.mixes[i].source.index = model.mixes[i].destination % coretx::input_count;
        model.mixes[i].multiplex = coretx::Multiplex::replace;
    }
    const auto result = coretx::evaluate(model, controls);
    CHECK(result);
    for (std::size_t i = 0; i < coretx::channel_count; ++i) {
        CHECK(result.frame.channels[i] == controls.analogs[i % coretx::input_count]);
    }

    model = direct_model();
    controls.analogs[0] = 512;
    CHECK(coretx::evaluate(model, controls).frame.channels[0] == 512);
    model.mixes.resize<0>();
    model.inputs.resize<0>();
    const auto empty = coretx::evaluate(model, controls);
    CHECK(empty);
    CHECK(empty.frame.inputs[0] == 0);
    CHECK(empty.frame.mixes[0] == coretx::MixValue{});
    CHECK(empty.frame.channels[0] == 0);
}

void validation_and_overflow() {
    auto model = direct_model();
    coretx::Controls controls{};
    auto expect = [&](const coretx::ErrorCode &code, const coretx::Stage &stage,
                      const std::size_t index) {
        const auto result = coretx::evaluate(model, controls);
        CHECK(!result);
        CHECK(result.status.code == code);
        CHECK(result.status.stage == stage);
        CHECK(result.status.index == index);
    };
    CHECK(!model.inputs.try_resize(coretx::max_input_lines + 1));
    CHECK(coretx::validate(model));
    model = direct_model();
    model.inputs[0].source.kind = coretx::SourceKind::input;
    expect(coretx::ErrorCode::source, coretx::Stage::inputs, 0);
    model = direct_model();
    model.inputs[0].side = static_cast<coretx::InputSide>(3);
    expect(coretx::ErrorCode::input_settings, coretx::Stage::inputs, 0);
    model = direct_model();
    model.mixes.resize<2>();
    model.mixes[0].destination = 1;
    expect(coretx::ErrorCode::line_order, coretx::Stage::mixes, 1);
    model = direct_model();
    model.mixes[0].source.index = 8;
    expect(coretx::ErrorCode::source, coretx::Stage::mixes, 0);
    model = direct_model();
    model.mixes[0].condition = {true, static_cast<std::uint8_t>(coretx::switch_count),
                                coretx::SwitchPosition::up, false};
    expect(coretx::ErrorCode::condition, coretx::Stage::mixes, 0);
    model = direct_model();
    model.outputs[0].minimum_permille = coretx::TravelPermille::constant<1001>();
    expect(coretx::ErrorCode::output_settings, coretx::Stage::outputs, 0);
    model = direct_model();
    controls.analogs[0] = 1025;
    expect(coretx::ErrorCode::control_value, coretx::Stage::controls, 0);
    controls = {};
    model.mixes.resize<8>();
    for (auto &line : model.mixes) {
        line.source = {coretx::SourceKind::maximum, 0, false};
        line.weight_percent = coretx::MixWeight::constant<500>();
        line.multiplex = coretx::Multiplex::multiply;
    }
    expect(coretx::ErrorCode::arithmetic_overflow, coretx::Stage::mixes, 5);
}

#if CORETX_MODUL_TANKMIX
constexpr bool tank_pipeline() {
    using namespace coretx;
    Model model{};
    model.inputs.resize<3>();
    for (std::size_t i = 0; i < 3; ++i) {
        model.inputs[i].destination = static_cast<std::uint8_t>(i);
        model.inputs[i].source.index = static_cast<std::uint8_t>(i);
    }
    model.mixes.resize<1>();
    model.mixes[0].destination = 5;
    model.mixes[0].source.index = 2;
    model.outputs[5].reversed = true;
    for (auto &logical : model.logical_switches) {
        logical.function = LogicalFunction::greater;
        logical.a = 2;
    }
    const TankMixer tank{true, 0, 1, 3, 2, 50, 3};
    if (!configure_tank(model, tank) || (model.mixes.size() != 7) || !validate(model) ||
        !model.outputs[5].reversed || (model.inputs[2].source.index != 2))
        return false;
    Controls controls{};
    controls.analogs[0] = 400;
    controls.analogs[1] = 200;
    controls.analogs[2] = 123;
    LogicalState state{};
    auto result = evaluate(model, controls, state);
    if (!result || result.frame.tank_reverse || (result.frame.channels[3] != 500) ||
        (result.frame.channels[2] != 300) || (result.frame.channels[5] != -123) ||
        (result.frame.logical.available != 0xFF) || (result.frame.logical.active != 0xFF))
        return false;
    controls.analogs[0] = -400;
    result = evaluate(model, controls, state);
    if (!result || !result.frame.tank_reverse || (result.frame.channels[3] != -500) ||
        (result.frame.channels[2] != -300) || (state.latches != 0))
        return false;
    // Direction follows processed drive input, not separately carried trim or steering.
    controls.trims[0] = TrimSteps::constant<127>();
    for (const auto drive : {-31, -30, 0, 30, 31}) {
        controls.analogs[0] = static_cast<std::int16_t>(drive);
        if (!evaluate(model, controls, state).frame.tank_reverse)
            return false;
    }
    controls.analogs[0] = 32;
    result = evaluate(model, controls, state);
    if (!result || result.frame.tank_reverse ||
        !tank_mix_active(MixRole::tank_forward, result.frame) ||
        tank_mix_active(MixRole::tank_reverse, result.frame))
        return false;
    for (const auto drive : {31, 0, -31}) {
        controls.analogs[0] = static_cast<std::int16_t>(drive);
        if (evaluate(model, controls, state).frame.tank_reverse)
            return false;
    }
    controls.analogs[0] = -32;
    if (!evaluate(model, controls, state).frame.tank_reverse)
        return false;
    controls.analogs[0] = 0;
    LogicalState startup{};
    if (evaluate(model, controls, startup).frame.tank_reverse)
        return false;
    // A failed evaluation must not commit direction or logical latch updates.
    controls.analogs[0] = 1025;
    if (evaluate(model, controls, state) || !state.tank_reverse)
        return false;
    const auto normal = model;
    auto overflow = model.mixes[6];
    overflow.source = {SourceKind::maximum, 0, false};
    overflow.weight_percent = MixWeight::constant<500>();
    overflow.multiplex = Multiplex::multiply;
    model.mixes[6] = overflow;
    for (unsigned i = 0; i < 7; ++i)
        if (!model.mixes.try_push_back(overflow))
            return false;
    controls.analogs[0] = 400;
    if (evaluate(model, controls, state) || !state.tank_reverse)
        return false;
    model = normal;
    controls.analogs[0] = 0;
    auto disabled = tank;
    disabled.enabled = false;
    if (!configure_tank(model, disabled) || (model.tank != disabled) || (model.mixes.size() != 1) ||
        (model.mixes[0].destination != 5) || !evaluate(model, controls, state) ||
        state.tank_reverse)
        return false;
    model.mixes[0].destination = 2;
    if (!validate(model))
        return false;
    model = normal;
    model.inputs[0].weight_percent = InputWeight::constant<-100>();
    controls.analogs[0] = 400;
    state = {};
    if (!evaluate(model, controls, state).frame.tank_reverse)
        return false;
    model.inputs[0].offset_percent = InputOffset::constant<100>();
    return !evaluate(model, controls, state).frame.tank_reverse;
}
static_assert(tank_pipeline());

constexpr bool tank_reversal_option() {
    using namespace coretx;
    Model model{};
    model.inputs.resize<2>();
    model.inputs[1].destination = 1;
    model.inputs[1].source.index = 1;
    auto tank = model.tank;
    tank.enabled = true;
    tank.steering_percent = 50;
    Controls controls{};
    controls.analogs[0] = -400;
    controls.analogs[1] = 200;
    LogicalState state{};
    if (!tank.reverse_steering || !configure_tank(model, tank))
        return false;
    auto result = evaluate(model, controls, state);
    if (!result || !result.frame.tank_reverse || (result.frame.channels[0] != -500) ||
        (result.frame.channels[1] != -300))
        return false;
    tank.reverse_steering = false;
    if (!configure_tank(model, tank))
        return false;
    result = evaluate(model, controls, state);
    if (!result || result.frame.tank_reverse || state.tank_reverse ||
        (result.frame.channels[0] != -300) || (result.frame.channels[1] != -500))
        return false;
    controls.analogs[0] = 0;
    tank.reverse_steering = true;
    if (!configure_tank(model, tank))
        return false;
    result = evaluate(model, controls, state);
    if (!result || result.frame.tank_reverse || (result.frame.channels[0] != 100) ||
        (result.frame.channels[1] != -100))
        return false;
    controls.analogs[0] = -400;
    return evaluate(model, controls, state).frame.tank_reverse;
}
static_assert(tank_reversal_option());

constexpr bool tank_third_motor() {
    using namespace coretx;
    Model model{};
    model.inputs.resize<2>();
    model.inputs[0].trim = TrimSource::none;
    model.inputs[1].destination = 1;
    model.inputs[1].source.index = 1;
    model.curves[0].y[0] = -90; // Preserve edited but unreferenced curves.
    model.inputs[1].curve = 2;
    TankMixer tank{};
    tank.enabled = true;
    tank.third_motor = true;
    tank.third_channel = 15;
    if (!configure_tank(model, tank) || (model.mixes.size() != 7) ||
        (model.tank.third_curve != 3) ||
        (model.curves[2] != Curve{{-50, 0, 25}, {0, 0, 0, 0, 100}}))
        return false;
    Controls controls{};
    LogicalState state{};
    controls.analogs[1] = 300;
    constexpr std::array drives{-1024, -512, 0, 256, 640, 1024};
    constexpr std::array expected{0, 0, 0, 0, 512, 1024};
    for (std::size_t i = 0; i < drives.size(); ++i) {
        controls.analogs[0] = static_cast<std::int16_t>(drives[i]);
        const auto result = evaluate(model, controls, state);
        if (!result || (result.frame.channels[15] != expected[i]) ||
            !tank_mix_active(MixRole::tank_third, result.frame))
            return false;
    }
    // The third output follows the processed drive input and normal output settings.
    model.inputs[0].weight_percent = InputWeight::constant<-100>();
    model.outputs[15].reversed = true;
    controls.analogs[0] = -640;
    if (evaluate(model, controls).frame.channels[15] != -512)
        return false;
    model.curves[2].y.back() = 50;
    tank.steering_percent = 50;
    if (!configure_tank(model, tank) || (model.tank.third_curve != 3) ||
        (evaluate(model, controls).frame.channels[15] != -256))
        return false;
    for (const unsigned mutation : {0U, 1U, 2U, 3U, 4U, 5U}) {
        auto bad = model;
        switch (mutation) {
        case 0:
            bad.mixes[6].role = MixRole::tank_drive;
            break;
        case 1:
            bad.mixes[0].role = MixRole::tank_third;
            break;
        case 2:
            bad.mixes[6].curve = 1;
            break;
        case 3:
            bad.tank.third_curve = 0;
            break;
        case 4:
            (void)bad.mixes.try_erase(6);
            break;
        case 5:
            bad.tank.third_channel = bad.tank.left_channel;
            break;
        }
        if (validate(bad))
            return false;
    }
    tank.third_motor = false;
    if (!configure_tank(model, tank) || (model.mixes.size() != 6))
        return false;
    tank.third_motor = true;
    if (!configure_tank(model, tank) || (model.curves[2].y.back() != 50))
        return false;
    tank.enabled = false;
    if (!configure_tank(model, tank) || !model.mixes.empty() || (model.tank.third_curve != 3))
        return false;
    Model unavailable{};
    unavailable.curves.fill(Curve{{-50, 0, 50}, {0, 0, 0, 0, 0}});
    tank.enabled = true;
    if ((configure_tank(unavailable, tank).code != ErrorCode::curve_capacity) ||
        !unavailable.mixes.empty() || unavailable.tank.third_motor ||
        (unavailable.curves[0].y.back() != 0))
        return false;
    Model conflict{};
    conflict.mixes.resize<1>();
    conflict.mixes[0].destination = 15;
    return (configure_tank(conflict, tank).code == ErrorCode::tank_conflict) &&
           (conflict.mixes.size() == 1) && (conflict.curves[0] == Curve{});
}
static_assert(tank_third_motor());

constexpr bool tank_configuration_validation() {
    using namespace coretx;
    Model model{};
    const TankMixer tank{true, 0, 1, 0, 1, 100, 3};
    if (!configure_tank(model, tank))
        return false;
    const auto canonical = model;
    for (const unsigned mutation : {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U}) {
        model = canonical;
        auto &line = model.mixes[0];
        switch (mutation) {
        case 0:
            line.role = static_cast<MixRole>(4);
            break;
        case 1:
            line.source.index = 2;
            break;
        case 2:
            line.carry_trim = false;
            break;
        case 3:
            line.offset_percent = MixOffset::constant<1>();
            break;
        case 4:
            line.curve = 1;
            break;
        case 5:
            line.multiplex = Multiplex::replace;
            break;
        case 6:
            line.role = MixRole::manual;
            break;
        case 7:
            line.role = MixRole::tank_forward;
            break;
        case 8:
            model.tank.enabled = false;
            break;
        case 9:
            (void)model.mixes.try_erase(0);
            break;
        }
        if (validate(model))
            return false;
    }
    for (const unsigned mutation : {0U, 1U, 2U, 3U, 4U, 5U, 6U}) {
        auto bad = tank;
        switch (mutation) {
        case 0:
            bad.drive_input = 8;
            break;
        case 1:
            bad.steering_input = 8;
            break;
        case 2:
            bad.left_channel = 16;
            break;
        case 3:
            bad.right_channel = bad.left_channel;
            break;
        case 4:
            bad.steering_percent = 101;
            break;
        case 5:
            bad.hysteresis_percent = 0;
            break;
        case 6:
            bad.hysteresis_percent = 21;
            break;
        }
        model = canonical;
        if ((configure_tank(model, bad).code != ErrorCode::tank_settings) ||
            (model.tank != canonical.tank) || !validate(model))
            return false;
    }
    model = canonical;
    auto moved = tank;
    moved.left_channel = 15;
    moved.right_channel = 14;
    moved.drive_input = moved.steering_input = 7;
    moved.steering_percent = 0;
    moved.hysteresis_percent = 20;
    return configure_tank(model, moved) && validate(model) && (model.mixes[0].destination == 14) &&
           (model.mixes[5].destination == 15);
}
static_assert(tank_configuration_validation());
#else
constexpr bool tank_unavailable() {
    coretx::Model model{};
    coretx::Controls controls{};
    model.tank.enabled = true;
    if (coretx::evaluate(model, controls).status.code != coretx::ErrorCode::tank_unavailable)
        return false;
    model.tank.enabled = false;
    model.mixes.resize<1>();
    model.mixes[0].role = coretx::MixRole::tank_drive;
    return coretx::evaluate(model, controls).status.code == coretx::ErrorCode::tank_unavailable;
}
static_assert(tank_unavailable());
#endif

} // namespace

int main() {
#if CORETX_MODUL_TANKMIX
    CHECK(tank_pipeline());
    CHECK(tank_reversal_option());
    CHECK(tank_third_motor());
    CHECK(tank_configuration_validation());
#else
    CHECK(tank_unavailable());
#endif
    CHECK(curve_interpolation());
    CHECK(curve_pipeline());
    CHECK(curve_validation());
    CHECK(logical_predicates());
    CHECK(logical_validation_and_latches());
    CHECK(bitmap_input_selection());
    CHECK(bounded_values());
    CHECK(fractional_mix_arithmetic());
    CHECK(checked_mix_arithmetic());
    passthrough_and_input_priority();
    input_reference_sweep();
    trims_and_source_inversion();
    reduced_trim_range();
    multiplex_order_and_unclipped_accumulators();
    mixer_reference_sweep();
    output_reference_sweep();
    independent_channels_and_capacity();
    validation_and_overflow();
    std::puts("CoreTX: all checks passed");
}
