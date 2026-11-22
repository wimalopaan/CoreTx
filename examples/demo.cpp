// SPDX-License-Identifier: GPL-2.0-or-later
#include "coretx/core/core.hpp"

#include <charconv>
#include <cstdio>
#include <string_view>

int main(const int argc, char **const argv) {
    if ((argc != 1) && (argc != 5)) {
        std::fprintf(stderr, "Usage: %s [roll pitch throttle yaw], each -1024..1024\n", argv[0]);
        return 1;
    }
    coretx::Controls controls{};
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument{argv[i]};
        int value{};
        const auto [end, error] =
            std::from_chars(argument.data(), argument.data() + argument.size(), value);
        if ((error != std::errc{}) || (end != (argument.data() + argument.size())) ||
            (value < (-coretx::resolution)) || (value > coretx::resolution)) {
            std::fprintf(stderr, "Invalid control value: %s (expected -1024..1024)\n", argv[i]);
            return 1;
        }
        controls.analogs[static_cast<std::size_t>(i - 1)] = static_cast<std::int16_t>(value);
    }

    coretx::Model model{};
    model.inputs.resize<4>();
    model.mixes.resize<4>();
    for (std::uint8_t i = 0; i < 4; ++i) {
        model.inputs[i].destination = i;
        model.inputs[i].source.index = i;
        model.mixes[i].destination = i;
        model.mixes[i].source.index = i;
    }
    model.inputs[0].expo_percent = coretx::ExpoPercent::constant<30>();
    model.inputs[1].expo_percent = coretx::ExpoPercent::constant<30>();

    const auto evaluation = coretx::evaluate(model, controls);
    if (!evaluation) {
        std::fprintf(stderr, "%s[%zu]: %s\n", coretx::stage_name(evaluation.status.stage),
                     evaluation.status.index, coretx::error_message(evaluation.status.code));
        return 1;
    }

    std::puts("Channel   Analog    Input    Mix/256   Output");
    for (std::size_t i = 0; i < 4; ++i) {
        std::printf("CH%-6zu %7d %8d %10ld %8d\n", i + 1, static_cast<int>(controls.analogs[i]),
                    static_cast<int>(evaluation.frame.inputs[i]),
                    static_cast<long>(evaluation.frame.mixes[i].raw() / coretx::MixValue::scale),
                    static_cast<int>(evaluation.frame.channels[i]));
    }
}
