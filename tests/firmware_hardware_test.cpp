// SPDX-License-Identifier: GPL-2.0-or-later
#include "boards/common/board.hpp"
#include "boards/i6s/power.hpp"
#include "platform/stm32f072/measurements.hpp"
#include "static_fixture.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace {

namespace hw = coretx::firmware::i6s;
namespace reg = hw::registers;

struct FakeRegisters {
    hw::HardwareError failure{};
    static constexpr std::array addresses{
        reg::rcc_apb2enr, reg::rcc_cr,    reg::rcc_cfgr,   reg::rcc_apb1rstr, reg::rcc_ahbenr,
        reg::rcc_apb1enr, reg::rcc_cfgr2, reg::flash_acr,  reg::gpiob_moder,  reg::gpiob_otyper,
        reg::gpiob_bsrr,  reg::tim2_cr1,  reg::tim2_sr,    reg::tim2_egr,     reg::tim2_cnt,
        reg::tim2_psc,    reg::tim2_arr,  reg::dbg_apb1fz, reg::tim2_dier,    reg::systick_control,
        reg::scb_icsr};
    std::array<std::uint32_t, addresses.size()> values{};
    std::array<std::pair<std::uintptr_t, std::uint32_t>, 32> writes{};
    std::size_t write_count{};

    constexpr FakeRegisters() {
        cell(reg::rcc_cr) = RCC_CR_HSION | RCC_CR_HSIRDY | RCC_CR_HSEON | RCC_CR_HSERDY |
                            RCC_CR_PLLON | RCC_CR_PLLRDY;
        cell(reg::rcc_cfgr) = RCC_CFGR_SW_PLL | RCC_CFGR_SWS_PLL | RCC_CFGR_HPRE | RCC_CFGR_PPRE;
        cell(reg::gpiob_moder) = 0x2AAAAAAAU;
        cell(reg::gpiob_otyper) = 0xFFFFFFFFU;
        cell(reg::rcc_ahbenr) = RCC_AHBENR_GPIOAEN;
        cell(reg::rcc_apb1enr) = RCC_APB1ENR_TIM6EN;
        cell(reg::rcc_apb2enr) = RCC_APB2ENR_SYSCFGEN;
        cell(reg::systick_control) = SysTick_CTRL_ENABLE_Msk | SysTick_CTRL_TICKINT_Msk;
        cell(reg::tim2_dier) = TIM_DIER_UIE;
    }

    constexpr std::uint32_t &cell(const std::uintptr_t address) {
        for (std::size_t i = 0; i < addresses.size(); ++i) {
            if (addresses[i] == address) {
                return values[i];
            }
        }
        std::abort();
    }

    constexpr std::uint32_t read(const std::uintptr_t address) {
        if ((address == reg::tim2_cnt) && ((cell(reg::tim2_cr1) & TIM_CR1_CEN) != 0) &&
            (failure != hw::HardwareError::timer_stopped)) {
            ++cell(address);
        }
        return cell(address);
    }

    constexpr void write(const std::uintptr_t address, const std::uint32_t value) {
        if (write_count == writes.size()) {
            std::abort();
        }
        writes[write_count++] = {address, value};
        if ((address == reg::dbg_apb1fz) &&
            ((cell(reg::rcc_apb2enr) & RCC_APB2ENR_DBGMCUEN) == 0)) {
            return;
        }
        const auto previous = cell(address);
        cell(address) = value;
        if (address == reg::rcc_cr) {
            constexpr std::uint32_t ready_bits = RCC_CR_HSIRDY | RCC_CR_HSERDY | RCC_CR_PLLRDY;
            auto &cr = cell(address);
            cr &= ~ready_bits;
            if (((value & RCC_CR_HSION) != 0) && (failure != hw::HardwareError::hsi_ready)) {
                cr |= RCC_CR_HSIRDY;
            }
            if (((value & RCC_CR_HSEON) != 0) && (failure != hw::HardwareError::hse_ready)) {
                cr |= RCC_CR_HSERDY;
            }
            if (((value & RCC_CR_PLLON) != 0) && (failure != hw::HardwareError::pll_ready)) {
                cr |= RCC_CR_PLLRDY;
            }
            if (failure == hw::HardwareError::oscillators_off) {
                cr |= RCC_CR_PLLRDY;
            }
        } else if (address == reg::rcc_cfgr) {
            const auto source = value & std::uint32_t{RCC_CFGR_SW};
            const bool blocked =
                ((source == RCC_CFGR_SW_HSI) && (failure == hw::HardwareError::hsi_switch)) ||
                ((source == RCC_CFGR_SW_PLL) && (failure == hw::HardwareError::pll_switch));
            cell(address) =
                (value & ~std::uint32_t{RCC_CFGR_SWS}) |
                (blocked ? previous & std::uint32_t{RCC_CFGR_SWS} : source << RCC_CFGR_SWS_Pos);
        } else if ((address == reg::flash_acr) && (failure == hw::HardwareError::flash_latency)) {
            cell(address) &= ~std::uint32_t{FLASH_ACR_LATENCY};
        }
    }
};

struct BoardInitialization {};
struct HardwareFailures {};
struct PowerPreparation {};
struct FirstTimebase {};
struct SecondTimebase {};
template <typename Scenario> using Fixture = coretx::test::RegisterFixture<Scenario, FakeRegisters>;

bool board_initialization() {
    using Test = Fixture<BoardInitialization>;
    Test::reset_registers();
    auto &io = Test::registers;
    if ((hw::initialize<Test::Io>(4) != hw::HardwareError::none) ||
        (io.cell(reg::gpiob_moder) != 0x6AAAAAAAU) || (io.cell(reg::gpiob_otyper) != 0xFFFF7FFFU) ||
        (io.cell(reg::rcc_ahbenr) != (RCC_AHBENR_GPIOAEN | RCC_AHBENR_GPIOBEN)) ||
        (io.cell(reg::rcc_apb1enr) != (RCC_APB1ENR_TIM6EN | RCC_APB1ENR_TIM2EN)) ||
        ((io.cell(reg::rcc_cfgr) & (RCC_CFGR_PLLSRC | RCC_CFGR_PLLXTPRE | RCC_CFGR_PLLMUL |
                                    RCC_CFGR_HPRE | RCC_CFGR_PPRE | RCC_CFGR_SW | RCC_CFGR_SWS)) !=
         (RCC_CFGR_PLLSRC_HSE_PREDIV | RCC_CFGR_PLLMUL6 | RCC_CFGR_SW_PLL | RCC_CFGR_SWS_PLL)) ||
        (io.cell(reg::rcc_cfgr2) != 0) || (io.cell(reg::tim2_psc) != 47) ||
        (io.cell(reg::tim2_arr) != TIM_ARR_ARR) || (io.cell(reg::tim2_cr1) != TIM_CR1_CEN) ||
        (io.cell(reg::tim2_dier) != 0) || (io.cell(reg::systick_control) != 0) ||
        (io.cell(reg::scb_icsr) != SCB_ICSR_PENDSTCLR_Msk) ||
        (io.cell(reg::rcc_apb2enr) != (RCC_APB2ENR_SYSCFGEN | RCC_APB2ENR_DBGMCUEN)) ||
        ((io.cell(reg::dbg_apb1fz) & DBGMCU_APB1_FZ_DBG_TIM2_STOP) == 0)) {
        return false;
    }
    std::size_t latch_high = io.write_count;
    std::size_t latch_output = io.write_count;
    std::size_t latency = io.write_count;
    std::size_t pll_switch = io.write_count;
    for (std::size_t i = 0; i < io.write_count; ++i) {
        const auto &[address, value] = io.writes[i];
        if ((address == reg::gpiob_bsrr) && (value == GPIO_BSRR_BS_15)) {
            latch_high = i;
        } else if (address == reg::gpiob_moder) {
            latch_output = i;
        } else if ((address == reg::flash_acr) && ((value & FLASH_ACR_LATENCY) != 0)) {
            latency = i;
        } else if ((address == reg::rcc_cfgr) && ((value & RCC_CFGR_SW) == RCC_CFGR_SW_PLL)) {
            pll_switch = i;
        }
    }
    return (latch_high < latch_output) && (latency < pll_switch);
}

bool hardware_failures() {
    using Test = Fixture<HardwareFailures>;
    for (const auto failure : {hw::HardwareError::hsi_ready, hw::HardwareError::hsi_switch,
                               hw::HardwareError::oscillators_off, hw::HardwareError::hse_ready,
                               hw::HardwareError::flash_latency, hw::HardwareError::pll_ready,
                               hw::HardwareError::pll_switch, hw::HardwareError::timer_stopped}) {
        Test::reset_registers();
        auto &io = Test::registers;
        io.failure = failure;
        if (hw::initialize<Test::Io>(4) != failure) {
            return false;
        }
    }
    return true;
}

bool sender_power_preparation() {
    using Test = Fixture<PowerPreparation>;
    Test::reset_registers();
    auto &io = Test::registers;
    coretx::firmware::SenderBoard<Test::Io>::prepare_power();
    if constexpr (!coretx::firmware::features::i6s) {
        return io.write_count == 0;
    }
    return (io.write_count == 4) && (io.writes[0].first == reg::rcc_ahbenr) &&
           (io.writes[1] == std::pair{reg::gpiob_bsrr, std::uint32_t{GPIO_BSRR_BS_15}}) &&
           (io.writes[2].first == reg::gpiob_otyper) && (io.writes[3].first == reg::gpiob_moder) &&
           (io.cell(reg::gpiob_moder) == 0x6AAAAAAAU) &&
           (io.cell(reg::gpiob_otyper) == 0xFFFF7FFFU);
}

bool shared_timebase() {
    using First = Fixture<FirstTimebase>;
    using Second = Fixture<SecondTimebase>;
    using Clock = coretx::firmware::Clock<First::Io>;
    First::reset_registers();
    Second::reset_registers();
    auto &io = First::registers;
    if (Clock::initialize(4) != hw::HardwareError::none) {
        return false;
    }
    const auto writes = io.write_count;
    io.cell(reg::tim2_cnt) = 0xFFFF'FFFEU;
    const auto before_wrap = Clock::sample();
    const auto after_wrap = Clock::sample();
    auto &other = Second::registers;
    other.cell(reg::tim2_cnt) = 123U;
    return (before_wrap == coretx::TimestampUs{0xFFFF'FFFFU}) &&
           (after_wrap == coretx::TimestampUs{}) &&
           ((after_wrap - before_wrap) == coretx::DurationUs{1U}) &&
           (coretx::firmware::Clock<Second::Io>::sample() == coretx::TimestampUs{123U}) &&
           (Clock::sample() == coretx::TimestampUs{1U}) && (io.write_count == writes) &&
           (other.write_count == 0);
}

constexpr bool stack_watermark() {
    std::array<std::uint32_t, 8> stack{};
    stack.fill(RC_STACK_PATTERN);
    auto usage = coretx::firmware::inspect_stack(stack.data(), 8);
    if ((usage.used_bytes != 0) || !usage.guard_intact) {
        return false;
    }
    stack[6] = 0;
    usage = coretx::firmware::inspect_stack(stack.data(), 8);
    if ((usage.used_bytes != 8) || !usage.guard_intact) {
        return false;
    }
    stack[3] = 0;
    usage = coretx::firmware::inspect_stack(stack.data(), 8);
    if ((usage.used_bytes != 20) || usage.guard_intact) {
        return false;
    }
    stack[0] = 0;
    usage = coretx::firmware::inspect_stack(stack.data(), 8);
    return (usage.used_bytes == 32) && !usage.guard_intact &&
           !coretx::firmware::inspect_stack(stack.data(), 0).guard_intact;
}

static_assert(stack_watermark());

} // namespace

int main() {
    if (!board_initialization() || !hardware_failures() || !sender_power_preparation() ||
        !shared_timebase() || !stack_watermark()) {
        std::fputs("FAIL: firmware board/measurement checks\n", stderr);
        return EXIT_FAILURE;
    }
    std::puts("Firmware board/measurement checks passed");
}
