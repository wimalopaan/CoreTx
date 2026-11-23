// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "platform/stm32f072/platform.hpp"

extern "C" [[noreturn]] void coretx_enter_rom_bootloader(std::uintptr_t vectors) noexcept;

namespace coretx::firmware::stm32f072 {

enum class BootloaderRequest : std::uint8_t { none, requested, error };

// Only used at cold startup, before the PLL, application peripherals and IWDG.
template <typename Io> struct Bootloader {
    Bootloader() = delete;
    inline static constexpr std::uintptr_t address = 0x1FFFC800;
    inline static constexpr auto systick_reload = SysTick_BASE + offsetof(SysTick_Type, LOAD);
    inline static constexpr auto systick_current = SysTick_BASE + offsetof(SysTick_Type, VAL);

    [[nodiscard]] static bool cold_start() noexcept {
        const auto flags = Io::read(RCC_BASE + offsetof(RCC_TypeDef, CSR));
        return ((flags & RCC_CSR_PORRSTF) != 0) &&
               ((flags & (RCC_CSR_IWDGRSTF | RCC_CSR_WWDGRSTF | RCC_CSR_SFTRSTF)) == 0);
    }
    static void start_sampling() noexcept {
        Io::write(registers::systick_control, 0);
        Io::write(systick_reload, 8'000U - 1U);
        Io::write(systick_current, 0);
        Io::write(registers::systick_control, SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_ENABLE_Msk);
    }
    [[nodiscard]] static bool wait_tick() noexcept {
        return wait_for<Io>(registers::systick_control, SysTick_CTRL_COUNTFLAG_Msk,
                            SysTick_CTRL_COUNTFLAG_Msk, ready_timeout);
    }
    static void stop_sampling() noexcept {
        Io::write(registers::systick_control, 0);
        Io::write(systick_current, 0);
        Io::write(registers::scb_icsr, SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk);
    }
    // Interrupts must be masked by the caller until the assembly handoff.
    [[nodiscard]] static bool prepare() noexcept {
        const auto stack = Io::read(address);
        const auto entry = Io::read(address + 4U);
        if ((stack <= SRAM_BASE) || (stack > (SRAM_BASE + 16'384U)) || ((stack & 7U) != 0) ||
            ((entry & 1U) == 0) || (entry < address) || (entry >= 0x1FFFF800U))
            return false;
        stop_sampling();
        Io::write(registers::rcc_cr, Io::read(registers::rcc_cr) | RCC_CR_HSION);
        if (!wait_for<Io>(registers::rcc_cr, RCC_CR_HSIRDY, RCC_CR_HSIRDY, ready_timeout))
            return false;
        Io::write(registers::rcc_cfgr, 0);
        if (!wait_for<Io>(registers::rcc_cfgr, RCC_CFGR_SWS, RCC_CFGR_SWS_HSI, ready_timeout))
            return false;
        Io::write(registers::rcc_cr,
                  Io::read(registers::rcc_cr) &
                      ~std::uint32_t{RCC_CR_HSEON | RCC_CR_PLLON | RCC_CR_CSSON | RCC_CR_HSEBYP});
        if (!wait_for<Io>(registers::rcc_cr, RCC_CR_HSERDY | RCC_CR_PLLRDY, 0, ready_timeout))
            return false;
        Io::write(registers::flash_acr, 0);
        Io::write(registers::rcc_cfgr2, 0);
        Io::write(RCC_BASE + offsetof(RCC_TypeDef, CIR), 0);
        Io::write(registers::rcc_apb1enr, 0);
        Io::write(registers::rcc_apb2enr, RCC_APB2ENR_SYSCFGEN);
        (void)Io::read(registers::rcc_apb2enr);
        // Reset USB pins without resetting GPIOB/PB15 or the RF power-off outputs.
        constexpr auto reset = RCC_BASE + offsetof(RCC_TypeDef, AHBRSTR);
        Io::write(reset, RCC_AHBRSTR_GPIOARST);
        Io::write(reset, 0);
        Io::write(NVIC_BASE + offsetof(NVIC_Type, ICER), 0xFFFFFFFFU);
        Io::write(NVIC_BASE + offsetof(NVIC_Type, ICPR), 0xFFFFFFFFU);
        constexpr auto remap = SYSCFG_BASE + offsetof(SYSCFG_TypeDef, CFGR1);
        Io::write(remap, (Io::read(remap) & ~std::uint32_t{SYSCFG_CFGR1_MEM_MODE}) |
                             SYSCFG_CFGR1_MEM_MODE_0);
        Io::barrier();
        return true;
    }
};

} // namespace coretx::firmware::stm32f072
