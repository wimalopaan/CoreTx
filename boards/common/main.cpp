// SPDX-License-Identifier: GPL-2.0-or-later
#include "boards/common/application.hpp"
#include "boards/common/board.hpp"
#include "platform/stm32f072/mmio.hpp"

#include <optional>

extern "C" {
// Exception number (IPSR) saved by startup's Default_Handler before halting.
constinit volatile std::uint32_t coretx_fault{};
}

namespace {

namespace hw = coretx::firmware;
using Board = hw::SenderBoard<hw::stm32f072::Mmio>;
using Sender = hw::i6x::Application<Board, Board::Input>;

// Construct in place in main so zero-filled framebuffers do not occupy flash.
constinit std::optional<Sender> application_storage{};

[[noreturn]] void halt(const std::uint8_t failure = 0x80) noexcept {
    Board::Watchdog::record_failure(failure);
    Board::power_off();
    asm volatile("cpsid i");
    for (;;) {
        asm volatile("wfi");
    }
}

} // namespace

#if CORETX_A7105
static_assert(Board::Hardware::clock_timer == 2);
static_assert(hw::stm32f072::ExtiTraits<Board::Hardware::radio.status>::irq == EXTI2_3_IRQn);
extern "C" void TIM2_IRQHandler() noexcept { Board::RadioTimer::interrupt(); }
extern "C" void EXTI2_3_IRQHandler() noexcept { Board::RadioTimer::gio2_interrupt(); }
#endif

extern "C" [[noreturn]] int main() noexcept {
    Board::prepare_power();
    Board::power_off();
    if (Board::Watchdog::initialize() != hw::WatchdogError::none) {
        halt();
    }
    auto &application = application_storage.emplace();
    if (application.initialize() == hw::i6x::ApplicationState::fatal) {
        halt();
    }
    if (Board::Watchdog::start_runtime(Board::now_us()) != hw::WatchdogError::none) {
        halt();
    }
    for (;;) {
        if (application.service(Board::now_us()) == hw::i6x::ApplicationState::fatal) {
            halt(static_cast<std::uint8_t>(0x80U | static_cast<unsigned>(application.failure())));
        }
        if (!Board::stack_intact())
            halt(static_cast<std::uint8_t>(
                0x80U | static_cast<unsigned>(hw::i6x::ApplicationFailure::stack)));
        if (Board::Watchdog::service(Board::now_us()) != hw::WatchdogError::none) {
            halt();
        }
        Board::Watchdog::mark(0);
    }
}
