// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "coretx/core/core.hpp"

namespace coretx::firmware {

[[nodiscard]] constexpr Model maximum_model() noexcept {
    Model model{};
    model.inputs.resize<max_input_lines>();
    model.mixes.resize<max_mix_lines>();
    for (std::size_t i = 0; i < max_input_lines; ++i) {
        auto &line = model.inputs[i];
        line.destination = static_cast<std::uint8_t>(i / 2);
        line.source = {SourceKind::analog, line.destination, false};
        line.side = (i % 2) == 0 ? InputSide::negative : InputSide::positive;
        line.expo_percent =
            (i % 2) == 0 ? ExpoPercent::constant<30>() : ExpoPercent::constant<-30>();
        line.weight_percent =
            (i % 2) == 0 ? InputWeight::constant<100>() : InputWeight::constant<-75>();
        line.offset_percent = InputOffset::constant<10>();
    }
    constexpr std::array modes{Multiplex::add, Multiplex::multiply, Multiplex::replace};
    for (std::size_t i = 0; i < max_mix_lines; ++i) {
        auto &line = model.mixes[i];
        line.destination = static_cast<std::uint8_t>(i * channel_count / max_mix_lines);
        line.source = {SourceKind::input, static_cast<std::uint8_t>(i % input_count), false};
        line.weight_percent = (i % 2) == 0 ? MixWeight::constant<150>() : MixWeight::constant<50>();
        line.offset_percent = MixOffset::constant<-10>();
        line.multiplex = modes[i % modes.size()];
    }
    for (std::size_t i = 0; i < channel_count; ++i) {
        model.outputs[i] = {TravelPermille::constant<-900>(), TravelPermille::constant<1200>(),
                            SubtrimPermille::constant<75>(), (i % 2) != 0};
    }
    return model;
}

[[nodiscard]] constexpr Controls synthetic_controls(const std::uint32_t phase) noexcept {
    Controls controls{};
    for (std::size_t i = 0; i < analog_count; ++i) {
        const auto offset = static_cast<std::uint32_t>(i) * 257U;
        controls.analogs[i] = static_cast<std::int16_t>(
            static_cast<std::int32_t>((phase % 2049U + offset) % 2049U) - resolution);
    }
    for (std::size_t i = 0; i < switch_count; ++i) {
        controls.switches[i] = static_cast<SwitchPosition>(
            static_cast<int>((phase / 32U + static_cast<std::uint32_t>(i)) % 3U) - 1);
    }
    for (std::size_t i = 0; i < trim_count; ++i) {
        controls.trims[i] =
            ((phase >> i) & 1U) != 0 ? TrimSteps::constant<127>() : TrimSteps::constant<-127>();
    }
    return controls;
}

static_assert(validate(maximum_model()));

} // namespace coretx::firmware
