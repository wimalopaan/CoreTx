// SPDX-License-Identifier: GPL-2.0-or-later
#include "coretx/core/controls.hpp"

#include <charconv>
#include <cstdio>
#include <limits>
#include <string_view>

int main(const int argc, char **const argv) {
    if ((argc != 1) && (argc != 5)) {
        std::fprintf(stderr, "Usage: %s [raw0 raw1 raw2 raw3], each 0..65535\n", argv[0]);
        return 1;
    }
    coretx::ControlSetup setup{};
    setup.calibration.fill({100, 2000, 4000});
    setup.mapping[0] = {3, true};
    setup.mapping[3] = {0, false};
    coretx::RawControls raw{};
    raw.analogs.fill(2000);
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument{argv[i]};
        std::uint32_t value{};
        const auto [end, error] =
            std::from_chars(argument.data(), argument.data() + argument.size(), value);
        if ((error != std::errc{}) || (end != (argument.data() + argument.size())) ||
            (value > std::numeric_limits<std::uint16_t>::max())) {
            std::fprintf(stderr, "Invalid raw value: %s (expected 0..65535)\n", argv[i]);
            return 1;
        }
        raw.analogs[static_cast<std::size_t>(i - 1)] = static_cast<std::uint16_t>(value);
    }
    const auto prepared = coretx::prepare_controls(setup, raw);
    if (!prepared) {
        std::fprintf(stderr, "Control preparation[%zu]: %s\n", prepared.status.index,
                     coretx::preparation_error_message(prepared.status.code));
        return 1;
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
    const auto evaluation = coretx::evaluate(model, prepared.controls);
    if (!evaluation) {
        std::fprintf(stderr, "%s[%zu]: %s\n", coretx::stage_name(evaluation.status.stage),
                     evaluation.status.index, coretx::error_message(evaluation.status.code));
        return 1;
    }
    std::puts("Channel   Physical      Raw   Analog    Input    Mix/256   Output");
    for (std::size_t i = 0; i < 4; ++i) {
        const auto physical = setup.mapping[i].physical_index;
        std::printf("CH%-6zu %8u %8u %8d %8d %10ld %8d\n", i + 1, static_cast<unsigned>(physical),
                    static_cast<unsigned>(raw.analogs[physical]),
                    static_cast<int>(prepared.controls.analogs[i]),
                    static_cast<int>(evaluation.frame.inputs[i]),
                    static_cast<long>(evaluation.frame.mixes[i].raw() / coretx::MixValue::scale),
                    static_cast<int>(evaluation.frame.channels[i]));
    }
}
