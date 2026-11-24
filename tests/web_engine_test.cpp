// SPDX-License-Identifier: GPL-2.0-or-later
#include "boards/common/model_storage.hpp"
#include "platform/web/simulator.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>

namespace {
void require(bool condition, const char *description) {
    if (!condition) {
        std::fprintf(stderr, "%s (status %d: %s)\n", description, sim_status(), sim_error());
        std::exit(1);
    }
}
void settle() {
    for (int i = 0; i < 50; ++i)
        sim_step(100);
}
} // namespace

int main() {
    namespace fw = coretx::firmware;
    auto memory = std::span{sim_eeprom(), fw::models::memory_size};
    std::fill(memory.begin(), memory.end(), std::uint8_t{0xFF});
    sim_init();
    require((sim_status() & 4) == 0, "No valid channels before storage preflight");
    settle();
    require((sim_status() & 7) == 7, "Factory model, calibration and inputs load");
    require(!sim_busy(), "Native journals reach idle");
    require(sim_output() == 0, "New sender starts RF OFF");
    require(std::any_of(sim_lcd(), sim_lcd() + 8192, [](auto v) { return v != 0; }),
            "Real application renders LCD");
    sim_set_analog(0, 0);
    sim_step(100);
    require(sim_channels()[0] == 1024, "Physical right stick uses calibration and reversal");
    sim_set_analog(0, 4095);
    sim_step(100);
    require(sim_channels()[0] == -1024, "Channels follow physical controls while RF OFF");
    sim_set_analog(0, 4096);
    sim_step(100);
    require(sim_channels()[0] == -1024, "Invalid ADC values cannot corrupt input");

    sim_set_analog(0, 2048);
    sim_trim(0, 1);
    sim_step(100);
    sim_trim(0, 0);
    settle();
    require(!sim_busy(), "Trim save reaches idle");
    const auto revision = sim_revision();
    std::array<std::uint8_t, fw::models::memory_size> snapshot{};
    std::copy(memory.begin(), memory.end(), snapshot.begin());
    sim_init();
    settle();
    require(std::equal(memory.begin(), memory.end(), snapshot.begin()),
            "Restart preserves exact native EEPROM image");
    require(sim_revision() == revision, "Restart does not rewrite current records");

    fw::models::Record record{};
    fw::models::Data data{};
    fw::models::Info info{};
    std::copy_n(memory.begin() + fw::models::base, record.size(), record.begin());
    require(fw::models::decode(record, 0, {0x12345678}, data, info) == 1,
            "Browser image uses native model records");
    data.model.mixes.resize<7>();
    data.model.mixes[4].destination = 4;
    data.model.mixes[4].source = {coretx::SourceKind::switch_position, 0, false};
    data.model.mixes[5].destination = 5;
    data.model.mixes[5].source = {coretx::SourceKind::analog, 4, false};
    data.model.outputs[5].reversed = true;
    data.model.mixes[6].destination = 6;
    data.model.mixes[6].source = {coretx::SourceKind::switch_position, 16, false};
    require(fw::models::encode(data, 0, true, 100, record), "Encode imported model");
    std::copy(record.begin(), record.end(), memory.begin() + fw::models::base);
    sim_init();
    settle();
    sim_set_switch(0, 0);
    sim_set_analog(4, 0);
    sim_step(100);
    require(sim_channels()[4] == -1024, "Stock switch passes through real input decoding");
    require(sim_channels()[5] == 1024, "Pot passes through output reversal");
    sim_set_switch(0, 2);
    sim_set_analog(4, 4095);
    sim_step(100);
    require(sim_channels()[4] == 1024, "Switch release and positions reach channel bars");
    require(sim_channels()[5] == -1024, "Output bars represent final channels, not mixer sums");
    if constexpr (fw::features::i6s) {
        sim_set_switch(14, 2);
        sim_step(100);
        require(sim_channels()[6] == 1024, "i6S rear switch uses real SE source");
        sim_set_switch(14, 0);
        sim_step(100);
        require(sim_channels()[6] == -1024, "i6S rear switch releases correctly");
        std::array<std::uint8_t, 8192> before{};
        std::copy_n(sim_lcd(), before.size(), before.begin());
        sim_touch(64, 40, 1);
        for (int i = 0; i < 10; ++i)
            sim_step(100);
        sim_touch(64, 40, 0);
        sim_step(100);
        require(!std::equal(before.begin(), before.end(), sim_lcd()),
                "Touch polling preserves long press to open real sender menu");
        sim_key(8, 1);
        sim_key(8, 0);
        sim_step(100);
    }

    // Unknown model versions must enter the real recovery dialog without touching the image.
    for (std::size_t bank = 0; bank < 2; ++bank) {
        const auto start = fw::models::base + bank * fw::models::record_size;
        if (memory[start] != 'M')
            continue;
        memory[start + 4] = fw::models::format_version + 1;
        const auto checksum = coretx::storage::crc32(
            std::span<const std::uint8_t>{memory.subspan(start, fw::models::record_size - 4)});
        for (std::size_t i = 0; i < 4; ++i)
            memory[start + fw::models::record_size - 4 + i] =
                static_cast<std::uint8_t>(checksum >> (8 * i));
    }
    std::copy(memory.begin(), memory.end(), snapshot.begin());
    sim_init();
    settle();
    require((sim_status() & 16) != 0, "Unsupported model invokes firmware recovery");
    require((sim_status() & 4) == 0, "Recovery invalidates channel outputs");
    require(std::equal(memory.begin(), memory.end(), snapshot.begin()),
            "Unsupported import is never silently overwritten");
    if constexpr (fw::features::i6s) {
        sim_touch(40, 35, 1);
        sim_step(100);
        sim_touch(40, 35, -1);
        sim_step(100);
        require((sim_status() & 32) == 0, "Cancelled touch cannot activate a recovery action");
        sim_touch(40, 35, 1);
        sim_touch(40, 35, 0);
        sim_step(100);
        require((sim_status() & 32) != 0, "A subsequent intentional recovery tap still works");
    }
    std::puts("Browser simulator engine tests passed");
}
