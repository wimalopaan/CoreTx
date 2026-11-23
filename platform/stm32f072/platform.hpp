// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "coretx/utility/time.hpp"

#include <stm32f072xb.h>

#include <cstddef>
#include <cstdint>

namespace coretx::firmware {
using literals::operator""_us;
using literals::operator""_ms;
} // namespace coretx::firmware

namespace coretx::firmware::stm32f072 {

inline constexpr std::uint32_t cpu_hz = 48'000'000;
inline constexpr std::uint32_t timer_hz = 1'000'000;
inline constexpr std::uint32_t ready_timeout = 100'000;

enum class HardwareError : std::uint8_t {
    none,
    hsi_ready,
    hsi_switch,
    oscillators_off,
    hse_ready,
    flash_latency,
    pll_ready,
    pll_switch,
    timer_stopped,
    stack_guard,
    watchdog,
    bootloader
};

namespace registers {
inline constexpr std::uintptr_t rcc_cr = RCC_BASE + offsetof(RCC_TypeDef, CR);
inline constexpr std::uintptr_t rcc_cfgr = RCC_BASE + offsetof(RCC_TypeDef, CFGR);
inline constexpr std::uintptr_t rcc_apb1rstr = RCC_BASE + offsetof(RCC_TypeDef, APB1RSTR);
inline constexpr std::uintptr_t rcc_ahbenr = RCC_BASE + offsetof(RCC_TypeDef, AHBENR);
inline constexpr std::uintptr_t rcc_apb2enr = RCC_BASE + offsetof(RCC_TypeDef, APB2ENR);
inline constexpr std::uintptr_t rcc_apb1enr = RCC_BASE + offsetof(RCC_TypeDef, APB1ENR);
inline constexpr std::uintptr_t rcc_cfgr2 = RCC_BASE + offsetof(RCC_TypeDef, CFGR2);
inline constexpr std::uintptr_t flash_acr = FLASH_R_BASE + offsetof(FLASH_TypeDef, ACR);
inline constexpr std::uintptr_t gpiob_moder = GPIOB_BASE + offsetof(GPIO_TypeDef, MODER);
inline constexpr std::uintptr_t gpiob_otyper = GPIOB_BASE + offsetof(GPIO_TypeDef, OTYPER);
inline constexpr std::uintptr_t gpiob_bsrr = GPIOB_BASE + offsetof(GPIO_TypeDef, BSRR);
inline constexpr std::uintptr_t tim2_cr1 = TIM2_BASE + offsetof(TIM_TypeDef, CR1);
inline constexpr std::uintptr_t tim2_dier = TIM2_BASE + offsetof(TIM_TypeDef, DIER);
inline constexpr std::uintptr_t tim2_sr = TIM2_BASE + offsetof(TIM_TypeDef, SR);
inline constexpr std::uintptr_t tim2_egr = TIM2_BASE + offsetof(TIM_TypeDef, EGR);
inline constexpr std::uintptr_t tim2_cnt = TIM2_BASE + offsetof(TIM_TypeDef, CNT);
inline constexpr std::uintptr_t tim2_psc = TIM2_BASE + offsetof(TIM_TypeDef, PSC);
inline constexpr std::uintptr_t tim2_arr = TIM2_BASE + offsetof(TIM_TypeDef, ARR);
inline constexpr std::uintptr_t dbg_apb1fz = DBGMCU_BASE + offsetof(DBGMCU_TypeDef, APB1FZ);
inline constexpr std::uintptr_t systick_control = SysTick_BASE + offsetof(SysTick_Type, CTRL);
inline constexpr std::uintptr_t scb_icsr = SCB_BASE + offsetof(SCB_Type, ICSR);
} // namespace registers

template <typename Io>
[[nodiscard]] constexpr bool wait_for(const std::uintptr_t address, const std::uint32_t mask,
                                      const std::uint32_t expected,
                                      const std::uint32_t attempts) noexcept {
    for (std::uint32_t i = 0; i < attempts; ++i) {
        if ((Io::read(address) & mask) == expected) {
            return true;
        }
    }
    return false;
}

template <typename Io>
[[nodiscard]] constexpr HardwareError
initialize(const std::uint32_t attempts = ready_timeout) noexcept {
    using namespace registers;
    Io::write(rcc_cr, Io::read(rcc_cr) | RCC_CR_HSION);
    if (!wait_for<Io>(rcc_cr, RCC_CR_HSIRDY, RCC_CR_HSIRDY, attempts)) {
        return HardwareError::hsi_ready;
    }
    Io::write(rcc_cfgr, (Io::read(rcc_cfgr) & ~std::uint32_t{RCC_CFGR_SW}) | RCC_CFGR_SW_HSI);
    if (!wait_for<Io>(rcc_cfgr, RCC_CFGR_SWS, RCC_CFGR_SWS_HSI, attempts)) {
        return HardwareError::hsi_switch;
    }
    Io::write(rcc_cr,
              Io::read(rcc_cr) & ~std::uint32_t{RCC_CR_PLLON | RCC_CR_HSEON | RCC_CR_CSSON});
    if (!wait_for<Io>(rcc_cr, RCC_CR_PLLRDY | RCC_CR_HSERDY, 0, attempts)) {
        return HardwareError::oscillators_off;
    }
    Io::write(rcc_cr, (Io::read(rcc_cr) & ~std::uint32_t{RCC_CR_HSEBYP}) | RCC_CR_HSEON);
    if (!wait_for<Io>(rcc_cr, RCC_CR_HSERDY, RCC_CR_HSERDY, attempts)) {
        return HardwareError::hse_ready;
    }
    Io::write(flash_acr, FLASH_ACR_LATENCY | FLASH_ACR_PRFTBE);
    if ((Io::read(flash_acr) & FLASH_ACR_LATENCY) == 0) {
        return HardwareError::flash_latency;
    }
    Io::write(rcc_cfgr2,
              (Io::read(rcc_cfgr2) & ~std::uint32_t{RCC_CFGR2_PREDIV}) | RCC_CFGR2_PREDIV_DIV1);
    // Both reference boards use 8 MHz HSE / 1 * 6, with undivided AHB/APB.
    Io::write(rcc_cfgr, (Io::read(rcc_cfgr) &
                         ~std::uint32_t{RCC_CFGR_PLLSRC | RCC_CFGR_PLLXTPRE | RCC_CFGR_PLLMUL |
                                        RCC_CFGR_HPRE | RCC_CFGR_PPRE | RCC_CFGR_SW}) |
                            RCC_CFGR_PLLSRC_HSE_PREDIV | RCC_CFGR_PLLXTPRE_HSE_PREDIV_DIV1 |
                            RCC_CFGR_PLLMUL6 | RCC_CFGR_HPRE_DIV1 | RCC_CFGR_PPRE_DIV1);
    Io::write(rcc_cr, Io::read(rcc_cr) | RCC_CR_PLLON);
    if (!wait_for<Io>(rcc_cr, RCC_CR_PLLRDY, RCC_CR_PLLRDY, attempts)) {
        return HardwareError::pll_ready;
    }
    Io::write(rcc_cfgr, (Io::read(rcc_cfgr) & ~std::uint32_t{RCC_CFGR_SW}) | RCC_CFGR_SW_PLL);
    if (!wait_for<Io>(rcc_cfgr, RCC_CFGR_SWS, RCC_CFGR_SWS_PLL, attempts)) {
        return HardwareError::pll_switch;
    }
    return HardwareError::none;
}

} // namespace coretx::firmware::stm32f072
