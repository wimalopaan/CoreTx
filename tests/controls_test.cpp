// SPDX-License-Identifier: GPL-2.0-or-later
#include "coretx/core/controls.hpp"

#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {

void check(const bool condition, const char *const expression, const int line) {
    if (!condition) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
        std::exit(EXIT_FAILURE);
    }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

constexpr coretx::ControlSetup asymmetric_setup() {
    coretx::ControlSetup setup{};
    setup.calibration.fill({100, 1100, 4100});
    return setup;
}

constexpr bool calibrated_values() {
    const auto setup = asymmetric_setup();
    coretx::RawControls raw{};
    constexpr std::array<std::uint16_t, 9> samples{0,    100,   600,  1100, 2600,
                                                   4100, 65535, 1000, 1400};
    constexpr std::array<std::int16_t, 9> expected{-1024, -1024, -512, 0,  512,
                                                   1024,  1024,  -102, 102};
    for (std::size_t i = 0; i < samples.size(); ++i) {
        raw.analogs.fill(samples[i]);
        const auto prepared = coretx::prepare_controls(setup, raw);
        if (!prepared) {
            return false;
        }
        for (std::size_t axis = 0; axis < coretx::analog_count; ++axis) {
            if (prepared.controls.analogs[axis] !=
                (axis < coretx::calibration_count ? expected[i] : 0)) {
                return false;
            }
        }
    }
    return true;
}

constexpr bool rounding_and_narrow_spans() {
    auto setup = asymmetric_setup();
    setup.calibration[0] = {0, 2048, 4096};
    coretx::RawControls raw{};
    for (const auto delta : std::array<std::int16_t, 4>{-3, -1, 1, 3}) {
        raw.analogs[0] = static_cast<std::uint16_t>(2048 + delta);
        const auto prepared = coretx::prepare_controls(setup, raw);
        const auto expected = delta < 0 ? (delta - 1) / 2 : (delta + 1) / 2;
        if (!prepared || (prepared.controls.analogs[0] != expected)) {
            return false;
        }
    }
    setup.calibration[0] = {0, 1, 2};
    for (std::uint16_t sample = 0; sample <= 2; ++sample) {
        raw.analogs[0] = sample;
        const auto prepared = coretx::prepare_controls(setup, raw);
        if (!prepared || (prepared.controls.analogs[0] !=
                          ((static_cast<std::int32_t>(sample) - 1) * coretx::resolution))) {
            return false;
        }
    }
    return true;
}

constexpr bool mapped_pipeline() {
    auto setup = asymmetric_setup();
    setup.calibration[0] = {0, 2000, 4000};
    setup.mapping[0] = {1, true};
    setup.mapping[1] = {0, false};
    coretx::RawControls raw{};
    raw.analogs[0] = 1000;
    raw.analogs[1] = 2600;
    raw.switches = {coretx::SwitchPosition::down, coretx::SwitchPosition::up,
                    coretx::SwitchPosition::middle, coretx::SwitchPosition::down};
    raw.trims = {coretx::TrimSteps::constant<20>(), coretx::TrimSteps::constant<-10>(),
                 coretx::TrimSteps::constant<127>(), coretx::TrimSteps::constant<-127>()};
    const auto prepared = coretx::prepare_controls(setup, raw);
    if (!prepared || (prepared.controls.analogs[0] != -512) ||
        (prepared.controls.analogs[1] != -512) || (prepared.controls.switches != raw.switches)) {
        return false;
    }
    for (std::size_t i = 0; i < coretx::trim_count; ++i) {
        if (prepared.controls.trims[i].value() != raw.trims[i].value()) {
            return false;
        }
    }

    coretx::Model model{};
    model.inputs.resize<2>();
    model.mixes.resize<2>();
    model.inputs[1].destination = 1;
    model.inputs[1].source.index = 1;
    model.mixes[1].destination = 1;
    model.mixes[1].source.index = 1;
    const auto evaluation = coretx::evaluate(model, prepared.controls);
    return evaluation && (evaluation.frame.inputs[0] == -512) &&
           (evaluation.frame.channels[0] == -472) && (evaluation.frame.channels[1] == -532);
}

constexpr bool invalid_configuration() {
    coretx::RawControls raw{};
    if (coretx::validate(coretx::ControlSetup{})) {
        return false;
    }
    for (const auto &invalid :
         std::array<coretx::AnalogCalibration, 4>{{{0, 0, 1}, {0, 1, 1}, {2, 1, 3}, {0, 3, 2}}}) {
        auto setup = asymmetric_setup();
        setup.calibration[2] = invalid;
        const auto prepared = coretx::prepare_controls(setup, raw);
        if (prepared || (prepared.status.code != coretx::PreparationError::calibration) ||
            (prepared.status.index != 2)) {
            return false;
        }
    }
    for (const auto index :
         std::array<std::uint8_t, 2>{static_cast<std::uint8_t>(coretx::analog_count), 255}) {
        auto setup = asymmetric_setup();
        setup.mapping[3].physical_index = index;
        const auto prepared = coretx::prepare_controls(setup, raw);
        if (prepared || (prepared.status.code != coretx::PreparationError::mapping) ||
            (prepared.status.index != 3)) {
            return false;
        }
    }
    for (const auto position : std::array<std::int8_t, 4>{-128, -2, 8, 127}) {
        raw.switches[1] = static_cast<coretx::SwitchPosition>(position);
        const auto prepared = coretx::prepare_controls(asymmetric_setup(), raw);
        if (prepared || (prepared.status.code != coretx::PreparationError::switch_position) ||
            (prepared.status.index != 1)) {
            return false;
        }
    }
    return true;
}

static_assert(calibrated_values());
static_assert(rounding_and_narrow_spans());
static_assert(mapped_pipeline());
static_assert(invalid_configuration());

void monotonic_full_range() {
    auto setup = asymmetric_setup();
    setup.calibration[0] = {0, 1, 65535};
    setup.calibration[1] = {0, 65534, 65535};
    setup.calibration[2] = {700, 29000, 64000};
    setup.mapping[3] = {2, true};
    coretx::RawControls raw{};
    std::array<std::int16_t, 3> previous{-coretx::resolution, -coretx::resolution,
                                         -coretx::resolution};
    for (std::uint32_t sample = 0; sample <= std::numeric_limits<std::uint16_t>::max(); ++sample) {
        raw.analogs.fill(static_cast<std::uint16_t>(sample));
        const auto prepared = coretx::prepare_controls(setup, raw);
        CHECK(prepared);
        for (std::size_t i = 0; i < previous.size(); ++i) {
            const auto value = prepared.controls.analogs[i];
            CHECK(value >= previous[i]);
            CHECK((value >= (-coretx::resolution)) && (value <= coretx::resolution));
            previous[i] = value;
        }
        CHECK(prepared.controls.analogs[3] == (-prepared.controls.analogs[2]));
    }
    for (const auto value : previous) {
        CHECK(value == coretx::resolution);
    }
}

} // namespace

int main() {
    CHECK(calibrated_values());
    CHECK(rounding_and_narrow_spans());
    CHECK(mapped_pipeline());
    CHECK(invalid_configuration());
    monotonic_full_range();
    std::puts("Control preparation tests passed");
}
