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

} // namespace

int main() {
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
