// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "platform/stm32f072/platform.hpp"

namespace coretx::firmware {

template <typename Io, unsigned Timer = 2> class Clock {
    // TIM2's 32-bit counter is the timestamp ABI, shared by Mmio and the radio IRQ.
    static_assert(Timer == 2, "The system clock requires the 32-bit TIM2 counter");

  public:
    Clock() = delete;
    [[nodiscard]] static constexpr stm32f072::HardwareError
    initialize(const std::uint32_t attempts = stm32f072::ready_timeout) noexcept {
        using namespace stm32f072;
        using namespace registers;
        Io::write(systick_control, 0);
        Io::write(scb_icsr, SCB_ICSR_PENDSTCLR_Msk);
        const auto error = stm32f072::initialize<Io>(attempts);
        if (error != HardwareError::none) {
            return error;
        }
        return initialize_timer(cpu_hz, attempts);
    }
    [[nodiscard]] static constexpr stm32f072::HardwareError
    initialize_timer(const std::uint32_t source_hz,
                     const std::uint32_t attempts = stm32f072::ready_timeout) noexcept {
        using namespace stm32f072;
        using namespace registers;
        Io::write(rcc_apb1enr, Io::read(rcc_apb1enr) | RCC_APB1ENR_TIM2EN);
        (void)Io::read(rcc_apb1enr);
        Io::write(rcc_apb1rstr, Io::read(rcc_apb1rstr) | RCC_APB1RSTR_TIM2RST);
        Io::write(rcc_apb1rstr, Io::read(rcc_apb1rstr) & ~std::uint32_t{RCC_APB1RSTR_TIM2RST});
        Io::write(rcc_apb2enr, Io::read(rcc_apb2enr) | RCC_APB2ENR_DBGMCUEN);
        (void)Io::read(rcc_apb2enr);
        Io::write(dbg_apb1fz, Io::read(dbg_apb1fz) | DBGMCU_APB1_FZ_DBG_TIM2_STOP);
        Io::write(tim2_cr1, 0);
        Io::write(tim2_dier, 0);
        Io::write(tim2_psc, source_hz / timer_hz - 1U);
        Io::write(tim2_arr, TIM_ARR_ARR);
        Io::write(tim2_egr, TIM_EGR_UG);
        Io::write(tim2_sr, 0);
        Io::write(tim2_cnt, 0);
        Io::write(tim2_cr1, TIM_CR1_CEN);
        const auto first = sample();
        for (std::uint32_t i = 0; i < attempts; ++i) {
            if (sample() != first) {
                return HardwareError::none;
            }
        }
        return HardwareError::timer_stopped;
    }
    // Keep the single register read inline, including measurement builds with -Os.
    [[nodiscard, gnu::always_inline]] static constexpr TimestampUs sample() noexcept {
        return TimestampUs{Io::read(stm32f072::registers::tim2_cnt)};
    }
};

} // namespace coretx::firmware
