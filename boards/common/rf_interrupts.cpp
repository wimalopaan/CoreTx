// SPDX-License-Identifier: GPL-2.0-or-later
#include "coretx/features.hpp"

// Without A7105, startup's weak IRQ aliases retain the Default_Handler fault path.
#if CORETX_A7105
#include "boards/common/board.hpp"
#include "platform/stm32f072/mmio.hpp"

namespace {
using Board = coretx::firmware::SenderBoard<coretx::firmware::stm32f072::Mmio>;
static_assert(Board::Hardware::clock_timer == 2);
static_assert(coretx::firmware::stm32f072::ExtiTraits<Board::Hardware::radio.status>::irq ==
              EXTI2_3_IRQn);
} // namespace

extern "C" void TIM2_IRQHandler() noexcept { Board::RadioTimer::interrupt(); }
extern "C" void EXTI2_3_IRQHandler() noexcept { Board::RadioTimer::gio2_interrupt(); }
#endif
