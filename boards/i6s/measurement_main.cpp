// SPDX-License-Identifier: GPL-2.0-or-later
#include "boards/common/board.hpp"
#include "boards/i6s/power.hpp"
#include "boards/i6s/workload.hpp"
#include "platform/stm32f072/measurements.hpp"
#include "platform/stm32f072/mmio.hpp"
#include "platform/stm32f072/watchdog.hpp"

extern "C" {
constinit coretx::Model coretx_model = coretx::firmware::maximum_model();
constinit volatile std::uint32_t coretx_frame_count{};
// Exception number (IPSR) saved by startup's Default_Handler before halting.
constinit volatile std::uint32_t coretx_fault{};
constinit volatile coretx::ErrorCode coretx_error_code{coretx::ErrorCode::none};
constinit volatile coretx::Stage coretx_error_stage{coretx::Stage::model};
constinit volatile std::uint32_t coretx_error_index{};
constinit volatile std::int16_t coretx_channels[coretx::channel_count]{};
constinit volatile coretx::firmware::i6s::HardwareError coretx_hardware_error{};
constinit volatile bool coretx_hardware_ready{};
constinit volatile std::uint32_t coretx_cpu_hz{};
constinit volatile std::uint32_t coretx_timer_hz{};
constinit volatile std::uint32_t coretx_eval_us_last{};
constinit volatile std::uint32_t coretx_eval_us_max{};
constinit volatile std::uint32_t coretx_stack_used_bytes{};
constinit volatile bool coretx_stack_guard_ok{};
extern std::uint32_t __stack_limit;
extern std::uint32_t __stack_top;
}

namespace {

struct Mmio : coretx::firmware::stm32f072::Mmio {
    Mmio() = delete;

    [[nodiscard]] static std::uint32_t read(const std::uintptr_t address) noexcept {
        // Keep evaluation work between the timer reads, even when optimized.
        asm volatile("" ::: "memory");
        const auto value = coretx::firmware::stm32f072::Mmio::read(address);
        asm volatile("" ::: "memory");
        return value;
    }
};

using Watchdog = coretx::firmware::Watchdog<Mmio>;
using Hardware = coretx::firmware::i6s::Hardware;
using Board = coretx::firmware::SenderBoard<Mmio, Hardware>;
using Clock = coretx::firmware::Clock<Mmio, Hardware::clock_timer>;

// Keep a generic runtime entry point instead of specializing for the fixed demo model.
[[gnu::noinline, gnu::noclone]] constexpr coretx::Evaluation
evaluate_runtime(const coretx::Model &model, const coretx::Controls &controls) noexcept {
    return coretx::evaluate(model, controls);
}

[[noreturn]] void halt() noexcept {
    asm volatile("cpsid i");
    for (;;) {
        asm volatile("wfi");
    }
}

} // namespace

extern "C" [[noreturn]] int main() noexcept {
    Board::prepare_power();
    coretx::firmware::external_module_off<Mmio, Hardware>();
    coretx::firmware::internal_front_end_off<Mmio, Hardware::radio>();
    const auto boot_ready = Board::bootloader_startup();
    if (Watchdog::initialize() != coretx::firmware::WatchdogError::none) {
        coretx_hardware_error = coretx::firmware::i6s::HardwareError::watchdog;
        halt();
    }
    if (!boot_ready) {
        coretx_hardware_error = coretx::firmware::i6s::HardwareError::bootloader;
        Watchdog::record_failure(0x80);
        halt();
    }
    coretx_hardware_error = Clock::initialize();
    if (coretx_hardware_error != coretx::firmware::i6s::HardwareError::none) {
        halt();
    }
    coretx_cpu_hz = coretx::firmware::i6s::cpu_hz;
    coretx_timer_hz = coretx::firmware::i6s::timer_hz;
    coretx_hardware_ready = true;
    const auto stack_begin = reinterpret_cast<std::uintptr_t>(&__stack_limit);
    const auto stack_end = reinterpret_cast<std::uintptr_t>(&__stack_top);
    const auto stack_words = static_cast<std::uint32_t>((stack_end - stack_begin) / 4U);
    if (Watchdog::start_runtime(Clock::sample()) != coretx::firmware::WatchdogError::none) {
        coretx_hardware_error = coretx::firmware::i6s::HardwareError::watchdog;
        halt();
    }
    for (;;) {
        const auto controls = coretx::firmware::synthetic_controls(coretx_frame_count);
        const auto start = Clock::sample();
        const auto result = evaluate_runtime(coretx_model, controls);
        const auto elapsed = Clock::sample() - start;
        coretx_eval_us_last = elapsed.count();
        if (elapsed.count() > coretx_eval_us_max) {
            coretx_eval_us_max = elapsed.count();
        }
        const auto stack = coretx::firmware::inspect_stack(
            reinterpret_cast<const volatile std::uint32_t *>(stack_begin), stack_words);
        coretx_stack_guard_ok = stack.guard_intact;
        if (stack.used_bytes > coretx_stack_used_bytes) {
            coretx_stack_used_bytes = stack.used_bytes;
        }
        if (!stack.guard_intact) {
            coretx_hardware_error = coretx::firmware::i6s::HardwareError::stack_guard;
            coretx_hardware_ready = false;
            halt();
        }
        if (!result) {
            coretx_error_stage = result.status.stage;
            coretx_error_index = result.status.index;
            coretx_error_code = result.status.code;
            halt();
        }
        for (std::size_t i = 0; i < coretx::channel_count; ++i) {
            coretx_channels[i] = result.frame.channels[i];
        }
        coretx_frame_count = coretx_frame_count + 1U;
        if (Watchdog::service(Clock::sample()) != coretx::firmware::WatchdogError::none) {
            coretx_hardware_error = coretx::firmware::i6s::HardwareError::watchdog;
            coretx_hardware_ready = false;
            halt();
        }
    }
}
