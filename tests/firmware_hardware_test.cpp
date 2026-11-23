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
    static constexpr std::array addresses{reg::rcc_apb2enr,
                                          reg::rcc_cr,
                                          reg::rcc_cfgr,
                                          reg::rcc_apb1rstr,
                                          reg::rcc_ahbenr,
                                          reg::rcc_apb1enr,
                                          reg::rcc_cfgr2,
                                          reg::flash_acr,
                                          reg::gpiob_moder,
                                          reg::gpiob_otyper,
                                          reg::gpiob_bsrr,
                                          reg::tim2_cr1,
                                          reg::tim2_sr,
                                          reg::tim2_egr,
                                          reg::tim2_cnt,
                                          reg::tim2_psc,
                                          reg::tim2_arr,
                                          reg::dbg_apb1fz,
                                          reg::tim2_dier,
                                          reg::systick_control,
                                          reg::scb_icsr,
                                          GPIOB_BASE + offsetof(GPIO_TypeDef, PUPDR),
                                          GPIOB_BASE + offsetof(GPIO_TypeDef, IDR),
                                          GPIOD_BASE + offsetof(GPIO_TypeDef, BSRR)};
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
struct PowerPins {};
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

constexpr bool power_button_sequence() {
    for (const std::uint32_t base : {0U, 0xFFD80000U}) {
        hw::PowerButton button{};
        if (button.update(coretx::TimestampUs{base}, true) ||
            button.update(coretx::TimestampUs{base + 2'000'000U}, true))
            return false;
        (void)button.update(coretx::TimestampUs{base + 2'000'001U}, false);
        (void)button.update(coretx::TimestampUs{base + 2'000'002U}, true);
        (void)button.update(coretx::TimestampUs{base + 2'000'003U}, false);
        (void)button.update(coretx::TimestampUs{base + 2'030'003U}, false);
        auto pressed_at = coretx::TimestampUs{base + 2'030'004U};
        if (button.update(pressed_at, true) ||
            button.update(pressed_at + coretx::DurationUs{1'499'999}, true))
            return false;
        if (button.update(pressed_at + coretx::DurationUs{1'500'000}, false))
            return false;
        pressed_at = pressed_at + coretx::DurationUs{1'500'001};
        if (button.update(pressed_at, true) ||
            !button.update(pressed_at + hw::PowerButton::hold_duration, true) ||
            !button.requested() ||
            button.update(pressed_at + hw::PowerButton::hold_duration, true) ||
            button.update(pressed_at + hw::PowerButton::hold_duration, false) ||
            !button.requested())
            return false;
    }
    return true;
}

bool power_pins() {
    using Test = Fixture<PowerPins>;
    Test::reset_registers();
    auto &io = Test::registers;
    hw::hold_power<Test::Io>();
    hw::initialize_power_button<Test::Io>();
    const auto mode = io.cell(reg::gpiob_moder);
    if (((mode >> 28) & 15U) != 4U)
        return false;
    if (((io.cell(GPIOB_BASE + offsetof(GPIO_TypeDef, PUPDR)) >> 28) & 3U) != 1U)
        return false;
    io.cell(GPIOB_BASE + offsetof(GPIO_TypeDef, IDR)) = 1U << 14;
    if (hw::power_button_pressed<Test::Io>())
        return false;
    io.cell(GPIOB_BASE + offsetof(GPIO_TypeDef, IDR)) = 0;
    if (!hw::power_button_pressed<Test::Io>())
        return false;
    hw::release_power<Test::Io>();
    return io.writes[1] == std::pair{reg::gpiob_bsrr, std::uint32_t{GPIO_BSRR_BS_15}} &&
           io.writes[io.write_count - 1] ==
               std::pair{reg::gpiob_bsrr, std::uint32_t{GPIO_BSRR_BR_15}};
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

struct BootRegisters {
    static constexpr std::array addresses{reg::rcc_cr,
                                          reg::rcc_cfgr,
                                          reg::rcc_cfgr2,
                                          reg::rcc_ahbenr,
                                          reg::rcc_apb1enr,
                                          reg::rcc_apb2enr,
                                          reg::flash_acr,
                                          reg::systick_control,
                                          reg::scb_icsr,
                                          RCC_BASE + offsetof(RCC_TypeDef, CSR),
                                          RCC_BASE + offsetof(RCC_TypeDef, CIR),
                                          RCC_BASE + offsetof(RCC_TypeDef, AHBRSTR),
                                          SysTick_BASE + offsetof(SysTick_Type, LOAD),
                                          SysTick_BASE + offsetof(SysTick_Type, VAL),
                                          GPIOA_BASE + offsetof(GPIO_TypeDef, MODER),
                                          GPIOA_BASE + offsetof(GPIO_TypeDef, PUPDR),
                                          GPIOB_BASE + offsetof(GPIO_TypeDef, MODER),
                                          GPIOB_BASE + offsetof(GPIO_TypeDef, OTYPER),
                                          GPIOB_BASE + offsetof(GPIO_TypeDef, ODR),
                                          GPIOC_BASE + offsetof(GPIO_TypeDef, MODER),
                                          GPIOC_BASE + offsetof(GPIO_TypeDef, OTYPER),
                                          GPIOC_BASE + offsetof(GPIO_TypeDef, OSPEEDR),
                                          GPIOC_BASE + offsetof(GPIO_TypeDef, PUPDR),
                                          GPIOC_BASE + offsetof(GPIO_TypeDef, ODR),
                                          GPIOD_BASE + offsetof(GPIO_TypeDef, MODER),
                                          GPIOD_BASE + offsetof(GPIO_TypeDef, OTYPER),
                                          GPIOD_BASE + offsetof(GPIO_TypeDef, OSPEEDR),
                                          GPIOD_BASE + offsetof(GPIO_TypeDef, PUPDR),
                                          GPIOD_BASE + offsetof(GPIO_TypeDef, ODR),
                                          NVIC_BASE + offsetof(NVIC_Type, ICER),
                                          NVIC_BASE + offsetof(NVIC_Type, ICPR),
                                          SYSCFG_BASE + offsetof(SYSCFG_TypeDef, CFGR1),
                                          std::uintptr_t{0x1FFFC800},
                                          std::uintptr_t{0x1FFFC804}};
    std::array<std::uint32_t, addresses.size()> values{};
    std::array<std::pair<std::uintptr_t, std::uint32_t>, 256> writes{};
    std::size_t write_count{};
    std::uint16_t keys{};
    std::uint32_t milliseconds{}, press_from{}, release_at{100};
    hw::HardwareError failure{};
    bool tick_stopped{}, barrier_called{};

    constexpr BootRegisters() {
        cell(reg::rcc_cr) = RCC_CR_HSION | RCC_CR_HSIRDY;
        cell(RCC_BASE + offsetof(RCC_TypeDef, CSR)) = RCC_CSR_PORRSTF;
        cell(0x1FFFC800) = 0x20004000;
        cell(0x1FFFC804) = 0x1FFFC901;
    }
    constexpr std::uint32_t &cell(const std::uintptr_t address) {
        for (std::size_t i = 0; i < addresses.size(); ++i)
            if (addresses[i] == address)
                return values[i];
        std::abort();
    }
    std::uint32_t read(const std::uintptr_t address) {
        const auto pressed =
            (milliseconds >= press_from) && (milliseconds < release_at) ? keys : 0U;
        if (address == GPIOA_BASE + offsetof(GPIO_TypeDef, IDR))
            return 0xFFFFU & ~(pressed << 9);
        if (address == GPIOD_BASE + offsetof(GPIO_TypeDef, IDR)) {
            std::uint32_t rows{};
            for (unsigned column = 0; column < 3; ++column)
                if ((cell(GPIOC_BASE + offsetof(GPIO_TypeDef, ODR)) & (1U << (6U + column))) == 0)
                    rows |= (pressed >> (column * 4U)) & 0xFU;
            return 0xFFFFU & ~(rows << 12);
        }
        if ((address == reg::systick_control) && ((cell(address) & SysTick_CTRL_ENABLE_Msk) != 0) &&
            !tick_stopped) {
            ++milliseconds;
            return cell(address) | SysTick_CTRL_COUNTFLAG_Msk;
        }
        return cell(address);
    }
    void write(const std::uintptr_t address, const std::uint32_t value) {
        if (write_count == writes.size())
            std::abort();
        writes[write_count++] = {address, value};
        for (const auto port : {GPIOB_BASE, GPIOC_BASE, GPIOD_BASE})
            if (address == port + offsetof(GPIO_TypeDef, BSRR)) {
                auto &output = cell(port + offsetof(GPIO_TypeDef, ODR));
                output = (output | (value & 0xFFFFU)) & ~(value >> 16);
                return;
            }
        cell(address) = value;
        if (address == reg::rcc_cr) {
            auto &cr = cell(address);
            cr &= ~std::uint32_t{RCC_CR_HSIRDY | RCC_CR_HSERDY | RCC_CR_PLLRDY};
            if (((value & RCC_CR_HSION) != 0) && (failure != hw::HardwareError::hsi_ready))
                cr |= RCC_CR_HSIRDY;
            if (failure == hw::HardwareError::oscillators_off)
                cr |= RCC_CR_PLLRDY;
        } else if (address == reg::rcc_cfgr) {
            cell(address) &= ~std::uint32_t{RCC_CFGR_SWS};
            if (failure == hw::HardwareError::hsi_switch)
                cell(address) |= RCC_CFGR_SWS_PLL;
        } else if ((address == RCC_BASE + offsetof(RCC_TypeDef, AHBRSTR)) &&
                   ((value & RCC_AHBRSTR_GPIOARST) != 0)) {
            cell(GPIOA_BASE + offsetof(GPIO_TypeDef, MODER)) = 0x28000000U;
            cell(GPIOA_BASE + offsetof(GPIO_TypeDef, PUPDR)) = 0x24000000U;
        }
    }
    void barrier() { barrier_called = true; }
};

struct BootKeys {};
struct BootPreparation {};

bool bootloader_keys() {
    using Test = coretx::test::RegisterFixture<BootKeys, BootRegisters>;
    using I6X = coretx::firmware::SenderBoard<Test::Io, coretx::firmware::i6x::Hardware>;
    using I6S = coretx::firmware::SenderBoard<Test::Io, coretx::firmware::i6s::Hardware>;
    using Request = coretx::firmware::stm32f072::BootloaderRequest;
    for (const auto keys : std::array<std::uint16_t, 7>{0, 0x2, 0x40, 0x81, 0x24, 0x66, 0x42}) {
        Test::reset_registers();
        auto &io = Test::registers;
        io.keys = keys;
        if (I6X::bootloader_request() != (keys == 0x42 ? Request::requested : Request::none))
            return false;
        if ((io.milliseconds != 30) || (io.cell(reg::systick_control) != 0) ||
            ((io.cell(GPIOC_BASE + offsetof(GPIO_TypeDef, ODR)) & 0x1C0U) != 0x1C0U))
            return false;
    }
    for (const auto keys : std::array<std::uint16_t, 4>{0, 1, 2, 3}) {
        Test::reset_registers();
        Test::registers.keys = keys;
        if (I6S::bootloader_request() != (keys == 3 ? Request::requested : Request::none))
            return false;
    }
    for (const bool touch : {false, true}) {
        const auto request = [touch] {
            return touch ? I6S::bootloader_request() : I6X::bootloader_request();
        };
        for (const bool late_press : {false, true}) {
            Test::reset_registers();
            auto &io = Test::registers;
            io.keys = touch ? 3 : 0x42;
            io.press_from = late_press ? 15 : 0;
            io.release_at = late_press ? 100 : 27;
            if (request() != Request::none)
                return false;
        }
        Test::reset_registers();
        Test::registers.tick_stopped = true;
        if ((request() != Request::error) || (Test::registers.cell(reg::systick_control) != 0))
            return false;
        for (const auto flags :
             {std::uint32_t{RCC_CSR_PINRSTF}, std::uint32_t{RCC_CSR_PORRSTF | RCC_CSR_IWDGRSTF},
              std::uint32_t{RCC_CSR_PORRSTF | RCC_CSR_WWDGRSTF},
              std::uint32_t{RCC_CSR_PORRSTF | RCC_CSR_SFTRSTF}}) {
            Test::reset_registers();
            auto &io = Test::registers;
            io.keys = touch ? 3 : 0x42;
            io.cell(RCC_BASE + offsetof(RCC_TypeDef, CSR)) = flags;
            if ((request() != Request::none) || (io.write_count != 0))
                return false;
        }
    }
    return true;
}

bool bootloader_preparation() {
    using Test = coretx::test::RegisterFixture<BootPreparation, BootRegisters>;
    using Board = coretx::firmware::SenderBoard<Test::Io, coretx::firmware::i6s::Hardware>;
    Test::reset_registers();
    auto &io = Test::registers;
    Board::prepare_power();
    io.cell(reg::rcc_cr) |= RCC_CR_HSEON | RCC_CR_PLLON | RCC_CR_CSSON | RCC_CR_HSEBYP;
    io.cell(reg::rcc_cfgr) = RCC_CFGR_SW_PLL | RCC_CFGR_SWS_PLL;
    io.cell(reg::rcc_apb1enr) = RCC_APB1ENR_TIM2EN;
    io.cell(reg::flash_acr) = FLASH_ACR_LATENCY | FLASH_ACR_PRFTBE;
    if (!Board::Bootloader::prepare() || !io.barrier_called ||
        (io.cell(reg::systick_control) != 0) || (io.cell(reg::rcc_cfgr) != 0) ||
        (io.cell(reg::rcc_apb1enr) != 0) || (io.cell(reg::rcc_apb2enr) != RCC_APB2ENR_SYSCFGEN) ||
        ((io.cell(SYSCFG_BASE + offsetof(SYSCFG_TypeDef, CFGR1)) & SYSCFG_CFGR1_MEM_MODE) != 1) ||
        (io.cell(NVIC_BASE + offsetof(NVIC_Type, ICER)) != 0xFFFFFFFFU) ||
        (io.cell(NVIC_BASE + offsetof(NVIC_Type, ICPR)) != 0xFFFFFFFFU) ||
        ((io.cell(GPIOB_BASE + offsetof(GPIO_TypeDef, ODR)) & GPIO_ODR_15) == 0) ||
        ((io.cell(GPIOB_BASE + offsetof(GPIO_TypeDef, MODER)) & GPIO_MODER_MODER15) !=
         GPIO_MODER_MODER15_0) ||
        ((io.cell(reg::rcc_ahbenr) & RCC_AHBENR_GPIOBEN) == 0))
        return false;
    for (const auto &[address, value] : io.writes)
        if ((address == RCC_BASE + offsetof(RCC_TypeDef, AHBRSTR)) &&
            ((value & RCC_AHBRSTR_GPIOBRST) != 0))
            return false;
    for (const auto failure : {hw::HardwareError::hsi_ready, hw::HardwareError::hsi_switch,
                               hw::HardwareError::oscillators_off}) {
        Test::reset_registers();
        io.failure = failure;
        if (Board::Bootloader::prepare() || io.barrier_called)
            return false;
    }
    for (const auto &[address, value] :
         {std::pair{0x1FFFC800U, 0x20004008U}, std::pair{0x1FFFC800U, 0x20000000U},
          std::pair{0x1FFFC800U, 0x20003FFCU}, std::pair{0x1FFFC804U, 0x1FFFC900U},
          std::pair{0x1FFFC804U, 0x08000101U}}) {
        Test::reset_registers();
        io.cell(address) = value;
        if (Board::Bootloader::prepare() || (io.write_count != 0))
            return false;
    }
    return true;
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
    if (!bootloader_keys() || !bootloader_preparation()) {
        std::fputs("FAIL: bootloader keys/handoff checks\n", stderr);
        return EXIT_FAILURE;
    }
    if (!board_initialization() || !hardware_failures() || !sender_power_preparation() ||
        !power_button_sequence() || !power_pins() || !shared_timebase() || !stack_watermark()) {
        std::fputs("FAIL: firmware board/measurement checks\n", stderr);
        return EXIT_FAILURE;
    }
    std::puts("Firmware board/measurement checks passed");
}
