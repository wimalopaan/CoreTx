// SPDX-License-Identifier: GPL-2.0-or-later
#include "boards/i6s/workload.hpp"
#include "coretx/core/core.hpp"
#include "edgetx_pipeline.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace {

void check(const bool value, const char *const expression, const int line) {
    if (!value) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
        std::exit(EXIT_FAILURE);
    }
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

constexpr std::int32_t diagnostic_value(const std::int16_t value) noexcept { return value; }

constexpr std::int32_t diagnostic_value(const coretx::MixValue value) noexcept {
    return value.raw();
}

void compare(const coretx::Model &model, const coretx::Controls &controls,
             edgetx_reference::Pipeline &reference, const char *const scenario,
             const std::size_t step) {
    const auto actual = coretx::evaluate(model, controls);
    CHECK(actual);
    const auto expected = reference.evaluate(model, controls);
    auto equal = [&](const auto &left, const auto &right, const char *const stage) {
        for (std::size_t i = 0; i < left.size(); ++i) {
            if (left[i] != right[i]) {
                std::fprintf(stderr, "%s step %zu %s[%zu]: core=%ld EdgeTX=%ld\n", scenario, step,
                             stage, i, static_cast<long>(diagnostic_value(left[i])),
                             static_cast<long>(diagnostic_value(right[i])));
                std::exit(EXIT_FAILURE);
            }
        }
    };
    equal(actual.frame.inputs, expected.inputs, "input");
    equal(actual.frame.mixes, expected.mixes, "mix");
    equal(actual.frame.channels, expected.channels, "output");
}

coretx::Model direct_model() {
    coretx::Model model{};
    model.inputs.resize<1>();
    model.mixes.resize<1>();
    return model;
}

void signed_full_range_pipeline() {
    auto model = direct_model();
    model.inputs[0].expo_percent = coretx::ExpoPercent::constant<-73>();
    model.inputs[0].weight_percent = coretx::InputWeight::constant<-57>();
    model.inputs[0].offset_percent = coretx::InputOffset::constant<17>();
    model.mixes[0].weight_percent = coretx::MixWeight::constant<-33>();
    model.mixes[0].offset_percent = coretx::MixOffset::constant<-17>();
    model.outputs[0] = {coretx::TravelPermille::constant<-700>(),
                        coretx::TravelPermille::constant<1300>(),
                        coretx::SubtrimPermille::constant<123>(), true};
    edgetx_reference::Pipeline reference{};
    coretx::Controls controls{};
    controls.trims[0] = coretx::TrimSteps::constant<37>();
    for (int value = -1024; value <= 1024; ++value) {
        controls.analogs[0] = static_cast<std::int16_t>(value);
        compare(model, controls, reference, "full-range", static_cast<std::size_t>(value + 1024));
    }
    for (const int trim : {-127, 127}) {
        CHECK(controls.trims[0].try_set(trim));
        compare(model, controls, reference, "trim-boundary", static_cast<std::size_t>(trim + 127));
    }
}

void input_selection_and_switch_timeline() {
    auto model = direct_model();
    model.inputs.resize<3>();
    model.inputs[0].condition = {true, 0, coretx::SwitchPosition::down, true};
    model.inputs[0].side = coretx::InputSide::negative;
    model.inputs[0].weight_percent = coretx::InputWeight::constant<-25>();
    model.inputs[0].trim = coretx::TrimSource::stick2;
    model.inputs[1].side = coretx::InputSide::positive;
    model.inputs[1].weight_percent = coretx::InputWeight::constant<50>();
    model.inputs[2].weight_percent = coretx::InputWeight::constant<75>();
    edgetx_reference::Pipeline reference{};
    coretx::Controls controls{};
    controls.trims[0] = coretx::TrimSteps::constant<13>();
    controls.trims[1] = coretx::TrimSteps::constant<-21>();
    std::size_t step{};
    for (const auto position : {coretx::SwitchPosition::up, coretx::SwitchPosition::middle,
                                coretx::SwitchPosition::down}) {
        controls.switches[0] = position;
        for (const int value : {-1024, -1, 0, 1, 1024}) {
            controls.analogs[0] = static_cast<std::int16_t>(value);
            compare(model, controls, reference, "input-selection", step++);
        }
    }
}

void multiplex_and_output_timeline() {
    auto model = direct_model();
    model.mixes.resize<5>();
    model.mixes[0].source = {coretx::SourceKind::analog, 0, false};
    model.mixes[1].source = {coretx::SourceKind::analog, 1, false};
    model.mixes[1].multiplex = coretx::Multiplex::replace;
    model.mixes[1].condition = {true, 0, coretx::SwitchPosition::down, false};
    model.mixes[2].source = {coretx::SourceKind::analog, 2, false};
    model.mixes[2].multiplex = coretx::Multiplex::multiply;
    model.mixes[3].source = {coretx::SourceKind::switch_position, 1, true};
    model.mixes[3].destination = 1;
    model.mixes[4].source = {coretx::SourceKind::maximum, 0, true};
    model.mixes[4].destination = 15;
    model.outputs[0] = {coretx::TravelPermille::constant<-750>(),
                        coretx::TravelPermille::constant<1400>(),
                        coretx::SubtrimPermille::constant<300>(), true};
    model.outputs[1] = {coretx::TravelPermille::constant<-300>(),
                        coretx::TravelPermille::constant<900>(),
                        coretx::SubtrimPermille::constant<-450>(), false};
    edgetx_reference::Pipeline reference{};
    coretx::Controls controls{};
    controls.analogs[0] = 700;
    controls.analogs[1] = -500;
    controls.trims = {coretx::TrimSteps::constant<20>(),
                      coretx::TrimSteps::constant<-40>(),
                      coretx::TrimSteps::constant<10>(),
                      {}};
    std::size_t step{};
    for (const auto position :
         {coretx::SwitchPosition::up, coretx::SwitchPosition::down, coretx::SwitchPosition::up}) {
        controls.switches[0] = position;
        controls.switches[1] = position;
        for (const int factor : {-1024, -513, -1, 0, 1, 513, 1024}) {
            controls.analogs[2] = static_cast<std::int16_t>(factor);
            compare(model, controls, reference, "multiplex", step++);
        }
    }
    model.mixes[0].condition = {true, 0, coretx::SwitchPosition::down, false};
    controls.switches[0] = coretx::SwitchPosition::up;
    compare(model, controls, reference, "inactive-first-line", 0);

    model = direct_model();
    model.mixes.resize<3>();
    for (std::size_t i = 0; i < 3; ++i) {
        model.mixes[i].source = {coretx::SourceKind::maximum, 0, i == 2};
    }
    compare(model, controls, reference, "late-clipping", 0);
    CHECK(coretx::evaluate(model, controls).frame.channels[0] == 1024);
}

void trim_direction_combinations() {
    auto model = direct_model();
    coretx::Controls controls{};
    controls.analogs[0] = 313;
    controls.trims[0] = coretx::TrimSteps::constant<51>();
    controls.trims[1] = coretx::TrimSteps::constant<-17>();
    std::size_t step{};
    for (const auto trim :
         {coretx::TrimSource::automatic, coretx::TrimSource::none, coretx::TrimSource::stick2}) {
        model.inputs[0].trim = trim;
        for (const int weight : {-100, -1, 0, 1, 100}) {
            CHECK(model.inputs[0].weight_percent.try_set(weight));
            for (const bool input_inverted : {false, true}) {
                model.inputs[0].source.inverted = input_inverted;
                for (const bool mix_inverted : {false, true}) {
                    model.mixes[0].source.inverted = mix_inverted;
                    edgetx_reference::Pipeline reference{};
                    compare(model, controls, reference, "trim-direction", step++);
                }
            }
        }
    }
}

struct Generator {
    std::uint32_t state{0x072C0DE};
    int number(const int minimum, const int maximum) {
        state = state * 1664525U + 1013904223U;
        return minimum +
               static_cast<int>(state % static_cast<std::uint32_t>(maximum - minimum + 1));
    }
    bool flag() { return number(0, 1) != 0; }
    coretx::Source source(const bool input_allowed) {
        const auto kind = static_cast<coretx::SourceKind>(number(0, 3));
        if ((kind == coretx::SourceKind::input) && !input_allowed) {
            return {coretx::SourceKind::analog, static_cast<std::uint8_t>(number(0, 7)), flag()};
        }
        const int last = kind == coretx::SourceKind::maximum           ? 0
                         : kind == coretx::SourceKind::switch_position ? 3
                                                                       : 7;
        return {kind, static_cast<std::uint8_t>(number(0, last)), flag()};
    }
};

void generated_multicycle_models() {
    Generator random{};
    for (std::size_t scenario = 0; scenario < 64; ++scenario) {
        coretx::Model model{};
        model.inputs.resize<coretx::max_input_lines>();
        model.mixes.resize<coretx::max_mix_lines>();
        for (std::size_t i = 0; i < coretx::max_input_lines; ++i) {
            auto &line = model.inputs[i];
            line.destination = static_cast<std::uint8_t>(i / 2);
            line.source = random.source(false);
            CHECK(line.weight_percent.try_set(random.number(-100, 100)));
            CHECK(line.offset_percent.try_set(random.number(-10, 10)));
            CHECK(line.expo_percent.try_set(random.number(-100, 100)));
            line.trim = static_cast<coretx::TrimSource>(random.number(0, 5));
            if ((i % 2) == 0) {
                line.side = static_cast<coretx::InputSide>(random.number(0, 2));
                line.condition = {true, static_cast<std::uint8_t>(random.number(0, 3)),
                                  static_cast<coretx::SwitchPosition>(random.number(-1, 1)),
                                  random.flag()};
            }
        }
        for (std::size_t i = 0; i < coretx::max_mix_lines; ++i) {
            auto &line = model.mixes[i];
            line.destination = static_cast<std::uint8_t>(i / 3);
            line.source = random.source(true);
            // Bound native EdgeTX's 32-bit multiplication intermediate, not
            // just its final result, so upstream UB cannot be our test oracle.
            CHECK(line.weight_percent.try_set(random.number(-75, 75)));
            CHECK(line.offset_percent.try_set(random.number(-10, 10)));
            line.multiplex = static_cast<coretx::Multiplex>(random.number(0, 2));
            line.carry_trim = random.flag();
            line.condition = {random.flag(), static_cast<std::uint8_t>(random.number(0, 3)),
                              static_cast<coretx::SwitchPosition>(random.number(-1, 1)),
                              random.flag()};
        }
        for (auto &output : model.outputs) {
            CHECK(output.minimum_permille.try_set(random.number(-1500, 0)));
            CHECK(output.maximum_permille.try_set(random.number(0, 1500)));
            CHECK(output.subtrim_permille.try_set(random.number(-1000, 1000)));
            output.reversed = random.flag();
        }
        edgetx_reference::Pipeline reference{};
        for (std::size_t step = 0; step < 32; ++step) {
            coretx::Controls controls{};
            for (auto &value : controls.analogs) {
                value = static_cast<std::int16_t>(random.number(-1024, 1024));
            }
            for (auto &position : controls.switches) {
                position = static_cast<coretx::SwitchPosition>(random.number(-1, 1));
            }
            for (auto &trim : controls.trims) {
                CHECK(trim.try_set(
                    random.number(coretx::TrimSteps::minimum, coretx::TrimSteps::maximum)));
            }
            compare(model, controls, reference, "generated-model", scenario * 32 + step);
        }
    }
}

void intentional_unmatched_input_difference() {
    auto model = direct_model();
    model.inputs[0].condition = {true, 0, coretx::SwitchPosition::down, false};
    coretx::Controls controls{};
    controls.analogs[0] = 512;
    controls.trims[0] = coretx::TrimSteps::constant<20>();
    controls.switches[0] = coretx::SwitchPosition::down;
    edgetx_reference::Pipeline reference{};
    compare(model, controls, reference, "matched-input", 0);

    controls.switches[0] = coretx::SwitchPosition::up;
    controls.analogs[0] = -100;
    const auto core = coretx::evaluate(model, controls);
    const auto edge = reference.evaluate(model, controls);
    CHECK(core);
    CHECK(core.frame.inputs[0] == 0);
    CHECK(core.frame.channels[0] == 0);
    CHECK(edge.inputs[0] == 512);
    CHECK(edge.channels[0] == 552);

    model.inputs[0].condition = {};
    model.inputs[0].side = coretx::InputSide::positive;
    controls.analogs[0] = 512;
    compare(model, controls, reference, "positive-half", 0);
    controls.analogs[0] = -512;
    const auto negative_core = coretx::evaluate(model, controls);
    const auto negative_edge = reference.evaluate(model, controls);
    CHECK(negative_core);
    CHECK(negative_core.frame.channels[0] == 0);
    CHECK(negative_edge.inputs[0] == 0);
    CHECK(negative_edge.channels[0] == 40);
}

void firmware_workload_pipeline() {
    const auto model = coretx::firmware::maximum_model();
    edgetx_reference::Pipeline reference{};
    for (std::uint32_t phase = 0; phase < 2049U; ++phase) {
        compare(model, coretx::firmware::synthetic_controls(phase), reference, "firmware-workload",
                phase);
    }
    for (const std::uint32_t phase : {0xFFFFFFFEU, 0xFFFFFFFFU, 0U}) {
        compare(model, coretx::firmware::synthetic_controls(phase), reference,
                "firmware-phase-wrap", phase);
    }
}

} // namespace

int main() {
    signed_full_range_pipeline();
    input_selection_and_switch_timeline();
    multiplex_and_output_timeline();
    trim_direction_combinations();
    generated_multicycle_models();
    intentional_unmatched_input_difference();
    firmware_workload_pipeline();
    std::puts("CoreTX: upstream pipeline comparisons passed");
}
