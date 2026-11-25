// SPDX-License-Identifier: GPL-2.0-or-later
#define CORETX_I6S_TOUCH_TRACE
#include "boards/common/board.hpp"
#include "boards/common/calibration_storage.hpp"
#include "boards/common/dashboard.hpp"
#include "boards/i6x/keys.hpp"
#include "coretx/ui/diagnostics.hpp"
#include "platform/stm32f072/adc.hpp"
#include "platform/stm32f072/clock.hpp"
#include "platform/stm32f072/lcd.hpp"
#include "static_fixture.hpp"

#include <cstdio>
#include <cstdlib>

namespace {

namespace hw = coretx::firmware;
namespace chip = coretx::firmware::stm32f072;
using adc_reg = chip::AdcTraits<hw::Hardware::adc>;
struct adc_dma_registers : chip::DmaTraits<hw::Hardware::adc_dma> {
    static constexpr auto configuration = hw::adc_dma_configuration;
};
namespace reg = chip::registers;
struct MetadataIo;
using CoherentAdc = hw::Adc<MetadataIo, coretx::firmware::Hardware, hw::gimbal_inputs, true>;
using StockAdc = hw::Adc<MetadataIo, coretx::firmware::Hardware, hw::stock_inputs>;
using LcdError = coretx::firmware::LcdError;
using StorageProbe = hw::StorageProbe<MetadataIo, coretx::firmware::Hardware>;
using Eeprom = hw::Eeprom<MetadataIo, coretx::firmware::Hardware>;

template <typename Test> struct TestMemory {
    TestMemory() = delete;
    inline static constexpr auto page_size = Eeprom::page_size;
    inline static constinit hw::detail::EepromState<typename Test::Io, coretx::firmware::Hardware>
        driver{};

    static void reset() noexcept { driver = {}; }
    static void select(const std::uint8_t address) noexcept { driver.select(address); }
    [[nodiscard]] static bool read(const std::uint16_t address,
                                   const std::span<std::uint8_t> bytes) noexcept {
        return driver.read(address, bytes);
    }
    [[nodiscard]] static bool write(const std::uint16_t address,
                                    const std::span<const std::uint8_t> bytes) noexcept {
        return driver.write(address, bytes);
    }
    static void service(const coretx::TimestampUs now) noexcept { driver.service(now); }
    [[nodiscard]] static bool busy() noexcept { return driver.busy(); }
    [[nodiscard]] static hw::I2cError error() noexcept { return driver.error(); }
};

struct FakeRegisters {
    struct Cell {
        std::uintptr_t address{};
        std::uint32_t value{};
    };
    struct Byte {
        std::uint8_t value{};
        bool data{};
        std::uint32_t at{};
    };
    std::array<Cell, 96> cells{};
    std::size_t cell_count{};
    std::array<Byte, 2200> lcd_bytes{};
    std::size_t lcd_count{};
    std::array<std::uint16_t, 16> input{};
    std::size_t converted{};
    std::uint16_t *dma_destination{};
    std::size_t cpu_dr_reads{};
    std::size_t copy_advances{};
    std::size_t dma_length{};
    std::size_t sample_reads{};
    std::size_t mutate_after_read{};
    std::size_t advance_on_each_read{};
    std::uint32_t copy_delay{};
    bool copy_error{};
    hw::AdcError failure{};
    bool paused{};
    bool timer_stopped{};
    bool barrier_called{};
    bool calibration_while_enabled{};
    bool lcd_timing_error{};
    bool lcd_has_fallen{};
    std::uint32_t lcd_data_changed{};
    std::uint32_t lcd_rise{};
    std::uint32_t lcd_fall{};
    std::uint16_t matrix_keys{};

    constexpr FakeRegisters() {
        (void)cell(reg::tim2_cnt);
        cell(reg::rcc_cr) = RCC_CR_HSION | RCC_CR_HSIRDY;
        cell(GPIOA_BASE + offsetof(GPIO_TypeDef, MODER)) = 0xAAAAAAAAU;
        cell(GPIOB_BASE + offsetof(GPIO_TypeDef, MODER)) = 0xAAAAAAAAU;
        input[0] = 0;
        input[1] = 1000;
        input[2] = 2048;
        input[3] = 4095;
        input[4] = 73;
    }

    constexpr std::uint32_t &cell(const std::uintptr_t address) {
        for (std::size_t i = 0; i < cell_count; ++i) {
            if (cells[i].address == address) {
                return cells[i].value;
            }
        }
        if (cell_count == cells.size()) {
            std::abort();
        }
        auto &entry = cells[cell_count++];
        entry.address = address;
        return entry.value;
    }

    constexpr std::uint32_t read(const std::uintptr_t address) {
        if (address == (GPIOD_BASE + offsetof(GPIO_TypeDef, IDR))) {
            std::uint32_t pressed{};
            const auto columns = cell(GPIOC_BASE + offsetof(GPIO_TypeDef, ODR));
            for (std::uint32_t i = 0; i < 3; ++i) {
                if ((columns & (std::uint32_t{1} << (6U + i))) == 0) {
                    pressed |= (std::uint32_t{matrix_keys} >> (i * 4U)) & 0xFU;
                }
            }
            return (~pressed & 0xFU) << 12;
        }
        if ((address == reg::tim2_cnt) && !timer_stopped) {
            ++cell(address);
        } else if ((address == adc_reg::isr) && !paused &&
                   ((cell(adc_reg::cr) & ADC_CR_ADSTART) != 0)) {
            advance_dma();
        } else if (address == adc_reg::dr) {
            ++cpu_dr_reads;
            cell(adc_reg::isr) &= ~std::uint32_t{ADC_ISR_EOC};
            const auto value = input[converted++];
            if (converted == static_cast<std::size_t>(std::popcount(cell(adc_reg::chselr)))) {
                cell(adc_reg::cr) &= ~std::uint32_t{ADC_CR_ADSTART};
            }
            return value;
        }
        return cell(address);
    }

    [[nodiscard]] constexpr std::uint32_t now_us() { return read(reg::tim2_cnt); }
    constexpr void barrier() { barrier_called = true; }

    constexpr void write(const std::uintptr_t address, const std::uint32_t value) {
        if (address == adc_dma_registers::ifcr) {
            cell(adc_dma_registers::isr) &=
                (value & DMA_IFCR_CGIF1) != 0
                    ? ~std::uint32_t{DMA_ISR_GIF1 | DMA_ISR_TCIF1 | DMA_ISR_HTIF1 | DMA_ISR_TEIF1}
                    : ~value;
            return;
        }
        if (address == adc_reg::isr) {
            cell(address) &= ~value;
            return;
        }

        if ((address == adc_reg::reset) && ((value & RCC_APB2RSTR_ADCRST) != 0)) {
            cell(adc_reg::cr) = 0;
            cell(adc_reg::isr) = 0;
        }
        if (address == adc_reg::cr) {
            if ((value & ADC_CR_ADCAL) != 0) {
                calibration_while_enabled |= (cell(address) & ADC_CR_ADEN) != 0;
            }
            if (((value & ADC_CR_ADSTART) != 0) && ((cell(address) & ADC_CR_ADSTART) == 0)) {
                converted = 0;
            }
        }
        cell(address) = value;
        if (address == adc_dma_registers::cndtr) {
            dma_length = value;
        }
        if (address == reg::rcc_cr) {
            if ((value & RCC_CR_HSION) != 0)
                cell(address) |= RCC_CR_HSIRDY;
            if ((value & RCC_CR_HSEON) != 0)
                cell(address) |= RCC_CR_HSERDY;
            if ((value & RCC_CR_PLLON) != 0)
                cell(address) |= RCC_CR_PLLRDY;
            if ((value & RCC_CR_PLLON) == 0)
                cell(address) &= ~std::uint32_t{RCC_CR_PLLRDY};
            if ((value & RCC_CR_HSEON) == 0)
                cell(address) &= ~std::uint32_t{RCC_CR_HSERDY};
        } else if (address == reg::rcc_cfgr) {
            cell(address) = (value & ~std::uint32_t{RCC_CFGR_SWS}) |
                            ((value & std::uint32_t{RCC_CFGR_SW}) << RCC_CFGR_SWS_Pos);
        } else if (address == adc_reg::cr) {
            if (((value & ADC_CR_ADCAL) != 0) && (failure != hw::AdcError::calibration)) {
                cell(address) &= ~std::uint32_t{ADC_CR_ADCAL};
            }
            if (((value & ADC_CR_ADEN) != 0) && (failure != hw::AdcError::ready)) {
                cell(adc_reg::isr) |= ADC_ISR_ADRDY;
            }
            if ((value & ADC_CR_ADSTP) != 0) {
                cell(address) &= ~std::uint32_t{ADC_CR_ADSTART | ADC_CR_ADSTP};
            }
        }
        for (const auto port :
             {GPIOA_BASE, GPIOB_BASE, GPIOC_BASE, GPIOD_BASE, GPIOE_BASE, GPIOF_BASE}) {
            if (address != (port + offsetof(GPIO_TypeDef, BSRR))) {
                continue;
            }
            auto &odr = cell(port + offsetof(GPIO_TypeDef, ODR));
            const auto previous = odr;
            odr = (odr & ~(value >> coretx::firmware::bsrr_reset_shift)) |
                  (value & (GPIO_BSRR_BR_0 - 1U));
            const auto time = cell(reg::tim2_cnt);
            if (((port == GPIOE_BASE) && (((previous ^ odr) & 0xFFU) != 0)) ||
                ((port == GPIOB_BASE) && (((previous ^ odr) & GPIO_ODR_3) != 0))) {
                if (((cell(GPIOD_BASE + offsetof(GPIO_TypeDef, ODR)) & GPIO_ODR_7) != 0) ||
                    (lcd_has_fallen && ((time - lcd_fall) < 1U))) {
                    lcd_timing_error = true;
                }
                lcd_data_changed = time;
            }
            if ((port == GPIOD_BASE) && ((previous & GPIO_ODR_7) == 0) &&
                ((odr & GPIO_ODR_7) != 0)) {
                if (((time - lcd_data_changed) < 1U) ||
                    (lcd_has_fallen && ((time - lcd_fall) < 1U))) {
                    lcd_timing_error = true;
                }
                lcd_rise = time;
            }
            if ((port == GPIOD_BASE) && ((previous & GPIO_ODR_7) != 0) &&
                ((odr & GPIO_ODR_7) == 0)) {
                if ((time - lcd_rise) < 1U) {
                    lcd_timing_error = true;
                }
                lcd_has_fallen = true;
                lcd_fall = time;
                if (lcd_count == lcd_bytes.size()) {
                    std::abort();
                }
                lcd_bytes[lcd_count++] = {
                    static_cast<std::uint8_t>(cell(GPIOE_BASE + offsetof(GPIO_TypeDef, ODR))),
                    (cell(GPIOB_BASE + offsetof(GPIO_TypeDef, ODR)) & GPIO_ODR_3) != 0, time};
            }
        }
    }
    constexpr void write_address(const std::uintptr_t address, std::uint16_t *const destination) {
        if (address != adc_dma_registers::cmar) {
            std::abort();
        }
        dma_destination = destination;
    }

    constexpr void advance_dma() {
        if (((cell(adc_dma_registers::ccr) & DMA_CCR_EN) == 0) ||
            ((cell(adc_reg::cr) & ADC_CR_ADSTART) == 0)) {
            return;
        }
        const auto count = static_cast<std::size_t>(std::popcount(cell(adc_reg::chselr)));
        const auto remaining = cell(adc_dma_registers::cndtr);
        if ((remaining == 0) || (remaining > dma_length)) {
            return;
        }
        dma_destination[dma_length - remaining] = input[converted++ % count];
        if (--cell(adc_dma_registers::cndtr) == (dma_length / 2)) {
            cell(adc_dma_registers::isr) |= DMA_ISR_HTIF1;
            cell(adc_reg::isr) |= ADC_ISR_EOSEQ;
        }
        if (cell(adc_dma_registers::cndtr) == 0) {
            cell(adc_dma_registers::isr) |= DMA_ISR_TCIF1;
            cell(adc_reg::isr) |= ADC_ISR_EOSEQ;
            cell(adc_dma_registers::cndtr) = static_cast<std::uint32_t>(dma_length);
        }
    }

    [[nodiscard]] constexpr std::uint16_t read16(const std::uint16_t *const source) {
        if (++sample_reads == mutate_after_read) {
            input[0] = 123;
            input[1] = 234;
            input[2] = 345;
            input[3] = 456;
        }
        for (std::size_t i = 0; i < advance_on_each_read; ++i) {
            advance_dma();
        }
        for (std::size_t i = 0; i < copy_advances; ++i) {
            advance_dma();
        }
        copy_advances = 0;
        cell(reg::tim2_cnt) += copy_delay;
        copy_delay = 0;
        if (copy_error) {
            cell(adc_dma_registers::isr) |= DMA_ISR_TEIF1;
        }
        return *source;
    }
};

template <typename Adc, typename Registers> hw::AdcError initialize_adc(Registers &io) {
    const auto error = Adc::initialize(4);
    if (error != hw::AdcError::none) {
        return error;
    }
    const auto now = io.now_us();
    (void)Adc::service(coretx::TimestampUs{now + 2U});
    if (Adc::initializing()) {
        (void)Adc::service(coretx::TimestampUs{now + Adc::scan_timeout_us.count()});
    }
    return Adc::error();
}

bool adc_scans() {
    struct TestScenario {};
    using Test = coretx::test::RegisterFixture<TestScenario, FakeRegisters>;
    using Adc = hw::Adc<Test::Io, coretx::firmware::Hardware, hw::gimbal_inputs, true>;

    Test::reset_registers();
    auto &io = Test::registers;

    // Reinitializing an enabled ADC must reset it before calibration.
    io.cell(adc_reg::cr) = ADC_CR_ADEN | ADC_CR_ADSTART;
    if ((initialize_adc<Adc>(io) != hw::AdcError::none) || io.calibration_while_enabled ||
        ((io.cell(GPIOA_BASE + offsetof(GPIO_TypeDef, MODER)) & 0xFFFFFF00U) != 0xAAAAAA00U) ||
        (io.cell(adc_reg::cfgr1) != (ADC_CFGR1_DMAEN | ADC_CFGR1_DMACFG | ADC_CFGR1_CONT)) ||
        (io.cell(adc_dma_registers::cpar) != adc_reg::dr) ||
        ((io.cell(reg::rcc_ahbenr) & RCC_AHBENR_DMA1EN) == 0) ||
        (io.cell(adc_dma_registers::ccr) != adc_dma_registers::configuration) || Adc::valid()) {
        return false;
    }
    constexpr std::uint32_t start = 0xFFFFFFF0U;
    if (Adc::service(coretx::TimestampUs{start}) != hw::ScanEvent::pending)
        return false;
    for (std::uint32_t i = 0; i < 4; ++i) {
        const auto event = Adc::service(coretx::TimestampUs{start + 10U * (i + 1U)});
        if ((event != (i == 3 ? hw::ScanEvent::complete : hw::ScanEvent::pending)) ||
            (Adc::valid() != (i == 3))) {
            return false;
        }
    }
    const CoherentAdc::Samples first{0, 1000, 2048, 4095};
    if ((Adc::samples() != first) || (io.cpu_dr_reads != 0) ||
        ((io.cell(adc_dma_registers::ccr) & DMA_CCR_EN) == 0))
        return false;
    if ((Adc::service(coretx::TimestampUs{start + 100U}) != hw::ScanEvent::pending) ||
        ((io.cell(adc_reg::cr) & ADC_CR_ADSTART) == 0)) {
        return false;
    }
    if (Adc::samples() != first)
        return false; // No partial scan is published.
    io.cell(adc_reg::isr) |= ADC_ISR_OVR;
    return (Adc::service(coretx::TimestampUs{start + CoherentAdc::scan_period_us.count() + 20U}) ==
            hw::ScanEvent::failed) &&
           (Adc::error() == hw::AdcError::overrun) && !Adc::valid() &&
           ((io.cell(adc_reg::cr) & ADC_CR_ADSTART) == 0);
}

bool isolated_adc_contexts() {
    struct FirstTestScenario {};
    using FirstTest = coretx::test::RegisterFixture<FirstTestScenario, FakeRegisters>;
    struct SecondTestScenario {};
    using SecondTest = coretx::test::RegisterFixture<SecondTestScenario, FakeRegisters>;
    using First = hw::Adc<FirstTest::Io, coretx::firmware::Hardware, hw::gimbal_inputs, true>;
    using Second = hw::Adc<SecondTest::Io, coretx::firmware::Hardware, hw::gimbal_inputs, true>;
    FirstTest::reset_registers();
    SecondTest::reset_registers();
    auto &first_io = FirstTest::registers;
    auto &second_io = SecondTest::registers;
    second_io.input[0] = 321;
    second_io.input[3] = 3000;
    if ((initialize_adc<First>(first_io) != hw::AdcError::none) ||
        (initialize_adc<Second>(second_io) != hw::AdcError::none)) {
        return false;
    }
    (void)First::service(coretx::TimestampUs{0});
    (void)Second::service(coretx::TimestampUs{0});
    for (std::uint32_t i = 0; i < 4; ++i) {
        (void)First::service(coretx::TimestampUs{20U + i});
        (void)Second::service(coretx::TimestampUs{20U + i});
    }
    if (!First::valid() || !Second::valid() || (First::samples()[0] != 0) ||
        (Second::samples()[0] != 321) || (First::samples()[3] != 4095) ||
        (Second::samples()[3] != 3000)) {
        return false;
    }
    first_io.cell(adc_dma_registers::isr) |= DMA_ISR_TEIF1;
    return (First::service(coretx::TimestampUs{100}) == hw::ScanEvent::failed) &&
           (Second::service(coretx::TimestampUs{100}) == hw::ScanEvent::pending) &&
           (Second::error() == hw::AdcError::none) && Second::valid();
}

template <bool Coherent> struct AdcFailureScenario {};
template <bool Coherent> struct UninitializedAdcScenario {};
template <bool Coherent> struct DelayedAdcScenario {};

template <bool Coherent = true> bool adc_failures() {
    using Test = coretx::test::RegisterFixture<AdcFailureScenario<Coherent>, FakeRegisters>;
    using Adc = hw::Adc<typename Test::Io, coretx::firmware::Hardware, hw::gimbal_inputs, Coherent>;

    using UninitializedTest =
        coretx::test::RegisterFixture<UninitializedAdcScenario<Coherent>, FakeRegisters>;
    using Uninitialized = hw::Adc<typename UninitializedTest::Io, coretx::firmware::Hardware,
                                  hw::gimbal_inputs, Coherent>;
    UninitializedTest::reset_registers();

    if ((Uninitialized::error() != hw::AdcError::not_initialized) ||
        (Uninitialized::service(coretx::TimestampUs{0}) != hw::ScanEvent::failed)) {
        return false;
    }
    for (const auto failure : {hw::AdcError::calibration, hw::AdcError::ready}) {
        Test::reset_registers();
        auto &io = Test::registers;
        io.failure = failure;

        if ((initialize_adc<Adc>(io) != failure) || Adc::valid() ||
            (Adc::service(coretx::TimestampUs{0}) != hw::ScanEvent::failed)) {
            return false;
        }
    }
    for (const auto failure : {hw::AdcError::timeout, hw::AdcError::sequence, hw::AdcError::dma}) {
        Test::reset_registers();
        auto &io = Test::registers;

        if (initialize_adc<Adc>(io) != hw::AdcError::none)
            return false;
        (void)Adc::service(coretx::TimestampUs{100});
        if (failure == hw::AdcError::timeout) {
            io.paused = true;
        } else if (failure == hw::AdcError::dma) {
            io.cell(adc_dma_registers::isr) |= DMA_ISR_TEIF1;
        } else {
            io.paused = true;
            io.cell(adc_dma_registers::cndtr) = 9;
        }
        if ((Adc::service(coretx::TimestampUs{failure == hw::AdcError::timeout
                                                  ? 100 + CoherentAdc::scan_timeout_us.count()
                                                  : 110}) != hw::ScanEvent::failed) ||
            (Adc::error() != failure) || Adc::valid() ||
            ((io.cell(adc_dma_registers::ccr) & DMA_CCR_EN) != 0)) {
            return false;
        }
    }
    return true;
}

bool single_buffer_scans() {
    struct TestScenario {};
    using Test = coretx::test::RegisterFixture<TestScenario, FakeRegisters>;
    using Adc = hw::Adc<Test::Io, coretx::firmware::Hardware, hw::gimbal_inputs, false>;

    Test::reset_registers();
    auto &io = Test::registers;

    if (initialize_adc<Adc>(io) != hw::AdcError::none) {
        return false;
    }
    constexpr std::uint32_t start = 0xFFFFFF00U;
    (void)Adc::service(coretx::TimestampUs{start});
    io.paused = true;
    if (io.dma_length != 4) {
        return false;
    }
    for (std::size_t i = 0; i < 3; ++i) {
        io.advance_dma();
        if ((Adc::service(coretx::TimestampUs{start + 10U}) != hw::ScanEvent::pending) ||
            Adc::valid()) {
            return false;
        }
    }
    io.advance_dma();
    if ((Adc::service(coretx::TimestampUs{start + 100U}) != hw::ScanEvent::complete) ||
        (Adc::samples() != CoherentAdc::Samples{0, 1000, 2048, 4095})) {
        return false;
    }
    const auto first = Adc::samples();
    io.sample_reads = 0;
    io.mutate_after_read = 3;
    io.advance_on_each_read = 4;
    io.copy_delay = CoherentAdc::half_period_us.count() * 3;
    for (std::size_t i = 0; i < 4; ++i) {
        io.advance_dma();
    }
    if ((Adc::service(coretx::TimestampUs{start + 5'099U}) != hw::ScanEvent::pending) ||
        (Adc::samples() != first) ||
        (Adc::service(coretx::TimestampUs{start + 5'100U}) != hw::ScanEvent::complete) ||
        (Adc::samples() != CoherentAdc::Samples{0, 1000, 345, 456}) || (Adc::discarded() != 0) ||
        !Adc::valid() || (io.cpu_dr_reads != 0) ||
        ((io.cell(adc_dma_registers::ccr) & DMA_CCR_EN) == 0)) {
        return false;
    }
    io.advance_on_each_read = 0;
    if ((Adc::service(coretx::TimestampUs{start + 10'100U}) != hw::ScanEvent::complete) ||
        (Adc::samples() != CoherentAdc::Samples{123, 234, 345, 456})) {
        return false;
    }
    return (Adc::service(coretx::TimestampUs{start + 20'100U}) == hw::ScanEvent::failed) &&
           (Adc::error() == hw::AdcError::timeout) && !Adc::valid();
}

template <bool Coherent> bool delayed_adc_service() {
    using Test = coretx::test::RegisterFixture<DelayedAdcScenario<Coherent>, FakeRegisters>;
    using Adc = hw::Adc<typename Test::Io, coretx::firmware::Hardware, hw::stock_inputs, Coherent>;

    Test::reset_registers();
    auto &io = Test::registers;

    if (initialize_adc<Adc>(io) != hw::AdcError::none) {
        return false;
    }
    constexpr std::uint32_t start = 0xFFFFF000U;
    (void)Adc::service(coretx::TimestampUs{start});
    io.paused = true;
    for (std::uint32_t scan = 1; scan <= 2; ++scan) {
        io.input[0] = static_cast<std::uint16_t>(scan * 123U);
        for (std::size_t i = 0; i < hw::stock_inputs.size(); ++i) {
            io.advance_dma();
        }
        const auto now = start + scan * Adc::scan_timeout_us.count() * 3U;
        if ((Adc::service(coretx::TimestampUs{now}) != hw::ScanEvent::complete) || !Adc::valid() ||
            (Adc::error() != hw::AdcError::none) || (Adc::samples()[0] != io.input[0])) {
            return false;
        }
    }
    // A delayed poll must not disable a healthy ADC; missing DMA progress still must.
    const auto stopped = start + Adc::scan_timeout_us.count() * 6U;
    return (Adc::service(coretx::TimestampUs{stopped + Adc::scan_timeout_us.count() - 1U}) ==
            hw::ScanEvent::pending) &&
           (Adc::service(coretx::TimestampUs{stopped + Adc::scan_timeout_us.count()}) ==
            hw::ScanEvent::failed) &&
           (Adc::error() == hw::AdcError::timeout) && !Adc::valid() &&
           ((io.cell(adc_dma_registers::ccr) & DMA_CCR_EN) == 0);
}

bool dma_autonomous_scans() {
    struct TestScenario {};
    using Test = coretx::test::RegisterFixture<TestScenario, FakeRegisters>;
    using Adc = hw::Adc<Test::Io, coretx::firmware::Hardware, hw::gimbal_inputs, true>;

    Test::reset_registers();
    auto &io = Test::registers;

    if (initialize_adc<Adc>(io) != hw::AdcError::none) {
        return false;
    }
    constexpr std::uint32_t start = 0xFFFFFF00U;
    (void)Adc::service(coretx::TimestampUs{start});
    io.paused = true;
    for (std::uint32_t scan = 0; scan < 3; ++scan) {
        const auto now = start + 100U + scan * CoherentAdc::scan_period_us.count();
        io.input[0] = static_cast<std::uint16_t>(100U + scan);
        // Many full scans may be superseded; only the latest finished half is wanted.
        for (std::size_t i = 0; i < 12; ++i) {
            io.advance_dma();
        }
        if ((Adc::service(coretx::TimestampUs{now}) != hw::ScanEvent::complete) ||
            (Adc::samples()[0] != io.input[0]) || (Adc::samples()[3] != 4095) ||
            (io.cell(adc_dma_registers::isr) != 0) || (io.cpu_dr_reads != 0)) {
            return false;
        }
        io.input[0] = 999;
        io.advance_dma();
        if (Adc::samples()[0] == 999) {
            return false;
        }
    }
    // Reinitialization must disable the old transfer before rebinding its buffer.
    if ((initialize_adc<Adc>(io) != hw::AdcError::none) || Adc::valid() ||
        ((io.cell(adc_dma_registers::ccr) & DMA_CCR_EN) != 0)) {
        return false;
    }
    return true;
}

bool dma_incomplete_sequence() {
    struct TestScenario {};
    using Test = coretx::test::RegisterFixture<TestScenario, FakeRegisters>;
    using Adc = hw::Adc<Test::Io, coretx::firmware::Hardware, hw::gimbal_inputs, true>;

    for (std::size_t kind = 0; kind < 4; ++kind) {
        Test::reset_registers();
        auto &io = Test::registers;

        if (initialize_adc<Adc>(io) != hw::AdcError::none) {
            return false;
        }
        (void)Adc::service(coretx::TimestampUs{0});
        io.paused = true;
        for (std::size_t i = 0; i < 4; ++i) {
            io.advance_dma();
        }
        io.copy_advances = kind == 0 ? 4U : kind == 1 ? 8U : 0U;
        io.copy_delay = (kind == 1) || (kind == 2) ? CoherentAdc::half_period_us.count() * 2 : 0U;
        io.copy_error = kind == 3;
        const auto result = Adc::service(coretx::TimestampUs{100});
        if (kind == 3) {
            if ((result != hw::ScanEvent::failed) || (Adc::error() != hw::AdcError::dma)) {
                return false;
            }
        } else if ((result != hw::ScanEvent::pending) || Adc::valid() || (Adc::discarded() != 1)) {
            return false;
        } else {
            for (std::size_t i = 0; i < hw::gimbal_inputs.size(); ++i) {
                io.advance_dma();
            }
            io.copy_delay = CoherentAdc::half_period_us.count() * 2;
            if ((Adc::service(coretx::TimestampUs{CoherentAdc::scan_timeout_us.count()}) !=
                 hw::ScanEvent::failed) ||
                (Adc::error() != hw::AdcError::timeout) || Adc::valid()) {
                return false;
            }
        }
    }
    return true;
}

bool dma_latest_half_and_recovery() {
    struct TestScenario {};
    using Test = coretx::test::RegisterFixture<TestScenario, FakeRegisters>;
    using Adc = hw::Adc<Test::Io, coretx::firmware::Hardware, hw::gimbal_inputs, true>;

    Test::reset_registers();
    auto &io = Test::registers;

    if (initialize_adc<Adc>(io) != hw::AdcError::none) {
        return false;
    }
    (void)Adc::service(coretx::TimestampUs{0});
    io.paused = true;
    io.input[0] = 100;
    for (std::size_t i = 0; i < 4; ++i) {
        io.advance_dma();
    }
    if ((Adc::service(coretx::TimestampUs{100}) != hw::ScanEvent::complete) ||
        (Adc::samples()[0] != 100)) {
        return false;
    }
    io.input[0] = 200;
    for (std::size_t i = 0; i < 4; ++i) {
        io.advance_dma();
    }
    io.input[0] = 300;
    io.advance_dma(); // First half is now incomplete; second half is the latest full scan.
    if ((Adc::service(coretx::TimestampUs{5'099}) != hw::ScanEvent::pending) ||
        (Adc::service(coretx::TimestampUs{5'100}) != hw::ScanEvent::complete) ||
        (Adc::samples()[0] != 200)) {
        return false;
    }
    for (std::size_t i = 0; i < 3; ++i) {
        io.advance_dma();
    }
    io.copy_advances = 4;
    if ((Adc::service(coretx::TimestampUs{10'100}) != hw::ScanEvent::pending) ||
        (Adc::samples()[0] != 200) || !Adc::valid() || (Adc::discarded() != 1)) {
        return false;
    }
    if ((Adc::service(coretx::TimestampUs{10'101}) != hw::ScanEvent::complete) ||
        (Adc::samples()[0] != 300)) {
        return false;
    }
    return (Adc::service(coretx::TimestampUs{20'101}) == hw::ScanEvent::failed) &&
           (Adc::error() == hw::AdcError::timeout) && !Adc::valid();
}

inline constexpr auto extended_inputs = [] {
    std::array<hw::AnalogInput, 5> inputs{};
    for (std::size_t i = 0; i < hw::gimbal_inputs.size(); ++i) {
        inputs[i] = hw::gimbal_inputs[i];
    }
    inputs.back() = {{GPIOA_BASE, 4}, ADC_CHSELR_CHSEL4, "PA4"};
    return inputs;
}();

bool expandable_scan() {
    struct TestScenario {};
    using Test = coretx::test::RegisterFixture<TestScenario, FakeRegisters>;
    using Adc = hw::Adc<Test::Io, coretx::firmware::Hardware, extended_inputs, true>;

    Test::reset_registers();
    auto &io = Test::registers;

    if (initialize_adc<Adc>(io) != hw::AdcError::none)
        return false;
    (void)Adc::service(coretx::TimestampUs{0});
    for (std::uint32_t i = 0; i < 5; ++i) {
        const auto event = Adc::service(coretx::TimestampUs{10 + i});
        if (event != (i == 4 ? hw::ScanEvent::complete : hw::ScanEvent::pending))
            return false;
    }
    return Adc::valid() && (Adc::samples().back() == 73);
}

bool stock_scan() {
    struct TestScenario {};
    using Test = coretx::test::RegisterFixture<TestScenario, FakeRegisters>;
    using Adc =
        hw::Adc<Test::Io, coretx::firmware::Hardware, hw::stock_inputs, hw::coherent_adc_snapshots>;

    Test::reset_registers();
    auto &io = Test::registers;

    for (std::size_t i = 0; i < hw::stock_inputs.size(); ++i) {
        io.input[i] = static_cast<std::uint16_t>(100U + i * 300U);
    }
    if ((initialize_adc<Adc>(io) != hw::AdcError::none) || (io.cell(adc_reg::chselr) != 0x3FFU) ||
        ((io.cell(GPIOA_BASE + offsetof(GPIO_TypeDef, MODER)) & 0xFFFFU) != 0xFFFFU) ||
        ((io.cell(GPIOB_BASE + offsetof(GPIO_TypeDef, MODER)) & 0xFU) != 0xFU) ||
        ((io.cell(GPIOB_BASE + offsetof(GPIO_TypeDef, MODER)) & GPIO_MODER_MODER15) !=
         (0xAAAAAAAAU & GPIO_MODER_MODER15))) {
        return false;
    }
    (void)Adc::service(coretx::TimestampUs{0});
    for (std::uint32_t i = 0; i < hw::stock_inputs.size(); ++i) {
        const auto event = Adc::service(coretx::TimestampUs{10U + i});
        if ((event != (i == 9 ? hw::ScanEvent::complete : hw::ScanEvent::pending)) ||
            (Adc::valid() != (i == 9))) {
            return false;
        }
    }
    for (std::size_t i = 0; i < Adc::samples().size(); ++i) {
        if (Adc::samples()[i] != io.input[i]) {
            return false;
        }
    }
    return true;
}

bool sender_battery_scan() {
    struct TestScenario {};
    using Test = coretx::test::RegisterFixture<TestScenario, FakeRegisters>;
    using Adc = hw::Adc<Test::Io, coretx::firmware::Hardware, hw::sender_inputs,
                        hw::coherent_adc_snapshots>;

    Test::reset_registers();
    auto &io = Test::registers;

    for (std::size_t i = 0; i < hw::sender_inputs.size(); ++i)
        io.input[i] = static_cast<std::uint16_t>(100U + i * 300U);
    io.input[hw::battery_index] = 2526;
    if ((initialize_adc<Adc>(io) != hw::AdcError::none) || (io.cell(adc_reg::chselr) != 0x7FFU) ||
        ((io.cell(GPIOC_BASE + offsetof(GPIO_TypeDef, MODER)) & 3U) != 3U))
        return false;
    (void)Adc::service(coretx::TimestampUs{0});
    for (std::uint32_t i = 0; i <= hw::battery_index; ++i)
        if (Adc::service(coretx::TimestampUs{10U + i}) !=
            (i == hw::battery_index ? hw::ScanEvent::complete : hw::ScanEvent::pending))
            return false;
    hw::Diagnostics diagnostics{};
    diagnostics.observe(std::span{Adc::samples()}.first<hw::stock_inputs.size()>(),
                        coretx::TimestampUs{21});
    for (std::size_t i = 0; i < hw::analog_indices.size(); ++i)
        if (diagnostics.raw().analogs[i] != io.input[hw::analog_indices[i]])
            return false;
    if ((hw::battery_millivolts(Adc::samples()[hw::battery_index]) != 6200) ||
        (hw::battery_millivolts(421) != 1200))
        return false;
    for (std::size_t i = 11; i < Adc::samples().size(); ++i)
        if (Adc::samples()[i] != 0)
            return false;
    if ((Adc::set_extended(true) != hw::AdcError::none) || Adc::valid())
        return false;
    const auto expected_mask = hw::extra_adc_count == 0          ? 0x7FFU
                               : coretx::firmware::features::i6s ? 0xFFFFU
                                                                 : 0xF7FFU;
    if (io.cell(adc_reg::chselr) != expected_mask)
        return false;
    (void)Adc::service(coretx::TimestampUs{100});
    (void)Adc::service(coretx::TimestampUs{102});
    for (std::uint32_t i = 0; i < hw::sender_inputs.size(); ++i)
        if (Adc::service(coretx::TimestampUs{110U + i}) != ((i + 1) == hw::sender_inputs.size()
                                                                ? hw::ScanEvent::complete
                                                                : hw::ScanEvent::pending))
            return false;
    for (std::size_t i = 0; i < Adc::samples().size(); ++i)
        if (Adc::samples()[i] != io.input[i])
            return false;
    return (Adc::set_extended(false) == hw::AdcError::none) &&
           (io.cell(adc_reg::chselr) == 0x7FFU) && !Adc::valid();
}

constexpr StockAdc::Samples stock_samples(const std::uint16_t analog = 2000) {
    StockAdc::Samples samples{};
    for (const auto index : hw::analog_indices) {
        samples[index] = analog;
    }
    samples[4] = 500;
    samples[5] = 3500;
    samples[8] = 2000;
    samples[9] = 500;
    return samples;
}

constexpr void calibrate(hw::Diagnostics &state) {
    state.observe(stock_samples(), coretx::TimestampUs{0});
    state.begin_calibration();
    state.handle(hw::Key::ok);
    state.observe(stock_samples(400), coretx::TimestampUs{5'000});
    state.observe(stock_samples(3600), coretx::TimestampUs{10'000});
    state.handle(hw::Key::ok);
}

constexpr bool calibration_and_pipeline() {
    hw::Diagnostics state{};
    state.begin_calibration();
    state.handle(hw::Key::ok);
    if ((state.calibration_error() != hw::CalibrationError::no_sample) || state.output_valid()) {
        return false;
    }
    state.handle(hw::Key::cancel);
    calibrate(state);
    if (!state.calibrated() || !state.output_valid() ||
        (state.phase() != hw::CalibrationPhase::idle) || !state.prepared() || !state.evaluation()) {
        return false;
    }
    for (std::size_t i = 0; i < hw::analog_indices.size(); ++i) {
        const auto expected = i < 2 ? -coretx::resolution : coretx::resolution;
        if ((state.prepared().controls.analogs[i] != expected) ||
            (state.evaluation().frame.inputs[i] != expected) ||
            (state.evaluation().frame.channels[i] != expected)) {
            return false;
        }
    }
    constexpr std::array<std::int16_t, 4> switch_outputs{-1024, 1024, 0, -1024};
    for (std::size_t i = 0; i < switch_outputs.size(); ++i) {
        if (state.evaluation().frame.channels[6 + i] != switch_outputs[i]) {
            return false;
        }
    }
    for (std::size_t i = 10; i < coretx::channel_count; ++i) {
        if (state.evaluation().frame.channels[i] != 0) {
            return false;
        }
    }
    state.observe(stock_samples(), coretx::TimestampUs{15'000});
    for (std::size_t i = 0; i < hw::analog_indices.size(); ++i) {
        if (state.evaluation().frame.channels[i] != 0) {
            return false;
        }
    }
    auto moved = stock_samples();
    moved[6] = 4095;
    moved[7] = 0;
    state.observe(moved, coretx::TimestampUs{20'000});
    if ((state.evaluation().frame.channels[4] != 1024) ||
        (state.evaluation().frame.channels[5] != -1024) ||
        (state.prepared().controls.analogs[6] != 0) ||
        (state.prepared().controls.analogs[7] != 0)) {
        return false;
    }
    state.invalidate();
    return !state.valid() && !state.output_valid();
}

constexpr bool calibration_deferred_pipeline() {
    hw::Diagnostics state{};
    state.observe(stock_samples(), coretx::TimestampUs{0});
    state.begin_calibration();
    state.handle(hw::Key::ok);
    state.observe(stock_samples(400), coretx::TimestampUs{5'000});
    state.observe(stock_samples(3600), coretx::TimestampUs{10'000});
    state.defer_refresh();
    state.handle(hw::Key::ok);
    if (!state.calibrated() || state.output_valid() ||
        (state.phase() != hw::CalibrationPhase::idle))
        return false;
    state.flush_refresh();
    if (!state.output_valid())
        return false;
    state.begin_calibration();
    state.defer_refresh();
    state.handle(hw::Key::cancel);
    if (state.output_valid())
        return false;
    state.flush_refresh();
    return state.output_valid();
}

static_assert(calibration_deferred_pipeline());

constexpr bool editing_deferred_pipeline() {
    auto model = hw::diagnostic_model;
    hw::Diagnostics state{};
    calibrate(state);
    state.observe(stock_samples(), coretx::TimestampUs{20'000});
    state.use_model(model);
    state.defer_refresh();
    model.trims[0] = coretx::TrimSteps::constant<10>();
    state.refresh_model();
    if (state.output_valid() || (state.evaluation().frame.channels[0] != 0))
        return false;
    state.flush_refresh();
    if (!state.output_valid() || (state.evaluation().frame.channels[0] != 20))
        return false;
    auto samples = stock_samples();
    samples[0] = 400;
    state.observe(samples, coretx::TimestampUs{30'000});
    state.defer_refresh();
    model.inputs[0].weight_percent = coretx::InputWeight::constant<50>();
    state.refresh_model();
    if (state.output_valid() || (state.evaluation().frame.channels[0] != 1024))
        return false;
    state.flush_refresh();
    if (!state.output_valid() || (state.evaluation().frame.channels[0] != 532))
        return false;
    if constexpr (coretx::firmware::features::i6s)
        return true;
    model.trim_switches = 3;
    auto &latch = model.logical_switches[0];
    latch.function = coretx::LogicalFunction::srff;
    latch.a = coretx::physical_switch_count;
    latch.b = coretx::physical_switch_count + 1;
    latch.position_a = latch.position_b = coretx::SwitchPosition::down;
    state.refresh_model();
    state.defer_refresh();
    state.observe_trims(1);
    state.refresh_model();
    state.flush_refresh();
    if (!state.output_valid() || ((state.evaluation().frame.logical.active & 1U) == 0))
        return false;
    state.observe_trims(0);
    return state.output_valid() && ((state.evaluation().frame.logical.active & 1U) != 0);
}

constexpr bool calibration_transaction() {
    hw::Diagnostics state{};
    calibrate(state);
    const auto before = state.setup();
    state.observe(stock_samples(), coretx::TimestampUs{20'000});
    state.begin_calibration();
    state.handle(hw::Key::ok);
    auto moved = stock_samples(400);
    moved[7] = 2000;
    state.observe(moved, coretx::TimestampUs{25'000});
    moved = stock_samples(3600);
    moved[7] = 2000;
    state.observe(moved, coretx::TimestampUs{30'000});
    state.handle(hw::Key::ok);
    if (state.output_valid() || (state.calibration_error() != hw::CalibrationError::range) ||
        (state.bad_axis() != 5) || (state.phase() != hw::CalibrationPhase::range)) {
        return false;
    }
    for (std::size_t i = 0; i < coretx::calibration_count; ++i) {
        if ((state.setup().calibration[i].minimum != before.calibration[i].minimum) ||
            (state.setup().calibration[i].center != before.calibration[i].center) ||
            (state.setup().calibration[i].maximum != before.calibration[i].maximum)) {
            return false;
        }
    }
    state.handle(hw::Key::cancel);
    if (!state.output_valid()) {
        return false;
    }
    // Pot centers come from endpoints, not their position at the center-capture step.
    moved = stock_samples();
    moved[6] = 400;
    state.observe(moved, coretx::TimestampUs{35'000});
    state.begin_calibration();
    state.handle(hw::Key::ok);
    state.observe(stock_samples(400), coretx::TimestampUs{40'000});
    state.observe(stock_samples(3600), coretx::TimestampUs{45'000});
    state.handle(hw::Key::ok);
    return state.output_valid() && (state.setup().calibration[4].center == 2000);
}

bool calibration_pages_include_configured_pots() {
    hw::Diagnostics state{};
    coretx::InputSetup setup{};
    setup.extra_analog = true;
    setup.modes.fill(coretx::InputMode::pot);
    if (!state.set_input_setup(setup))
        return false;
    std::array<std::uint16_t, hw::sender_inputs.size()> samples{};
    samples.fill(2000);
    state.observe(samples, coretx::TimestampUs{0});
    state.begin_calibration();
    state.handle(hw::Key::ok);
    constexpr auto target = 6 + hw::extra_adc_count - 1;
    const auto target_sample =
        hw::extra_adc_count == 0 ? hw::analog_indices.back() : samples.size() - 1;
    samples.fill(400);
    samples[target_sample] = 2000;
    state.observe(samples, coretx::TimestampUs{5'000});
    samples.fill(3600);
    samples[target_sample] = 2000;
    state.observe(samples, coretx::TimestampUs{10'000});
    state.handle(hw::Key::up);
    coretx::ui::Framebuffer display{};
    hw::render_dashboard(state, nullptr, display);
    const auto expected_first =
        ((4 + target) / hw::Diagnostics::calibration_rows) * hw::Diagnostics::calibration_rows;
    if (state.calibration_first() != expected_first)
        return false;
    const int y =
        hw::CalibrationLayout::range_first_y +
        static_cast<int>(4 + target - expected_first) * hw::CalibrationLayout::range_row_spacing;
    coretx::ui::Framebuffer expected{};
    expected.compact();
    expected.text(hw::CalibrationLayout::range_label_x, y, coretx::auxiliary_names[target]);
    expected.number(hw::CalibrationLayout::sample_right, y, 2000);
    expected.number(hw::CalibrationLayout::minimum_right, y, 2000);
    expected.number(hw::CalibrationLayout::maximum_right, y, 2000);
    for (int row = y; row < (y + hw::OverviewLayout::character_height); ++row)
        for (int x = 0; x < coretx::ui::Framebuffer::width; ++x)
            if (display.pixel_at(x, row) != expected.pixel_at(x, row))
                return false;
    state.handle(hw::Key::down);
    if (state.calibration_first() != 0)
        return false;
    state.handle(hw::Key::ok);
    if ((state.phase() != hw::CalibrationPhase::range) ||
        (state.calibration_error() != hw::CalibrationError::range) ||
        (state.bad_axis_name() != coretx::auxiliary_names[target]) ||
        (state.calibration_first() != expected_first))
        return false;
    samples.fill(400);
    state.observe(samples, coretx::TimestampUs{15'000});
    samples.fill(3600);
    state.observe(samples, coretx::TimestampUs{20'000});
    state.handle(hw::Key::ok);
    const auto &axis = state.input_setup().calibration[target];
    return state.calibrated() && (state.phase() == hw::CalibrationPhase::idle) &&
           (axis.minimum == 400) && (axis.center == 2000) && (axis.maximum == 3600);
}

constexpr bool calibration_rejects_jitter() {
    hw::Diagnostics state{};
    state.observe(stock_samples(), coretx::TimestampUs{0});
    state.begin_calibration();
    state.handle(hw::Key::ok);
    state.observe(stock_samples(1950), coretx::TimestampUs{5'000});
    state.observe(stock_samples(2050), coretx::TimestampUs{10'000});
    state.handle(hw::Key::ok);
    if (state.calibrated() || state.output_valid() ||
        (state.calibration_error() != hw::CalibrationError::range) || (state.bad_axis() != 0)) {
        return false;
    }
    state.observe(stock_samples(400), coretx::TimestampUs{15'000});
    state.observe(stock_samples(3600), coretx::TimestampUs{20'000});
    state.handle(hw::Key::ok);
    return state.output_valid() && (state.calibration_error() == hw::CalibrationError::none);
}

constexpr bool stock_switches() {
    if ((hw::decode_switch(2047, false) != coretx::SwitchPosition::up) ||
        (hw::decode_switch(2048, false) != coretx::SwitchPosition::down) ||
        (hw::decode_switch(1364, true) != coretx::SwitchPosition::up) ||
        (hw::decode_switch(1365, true) != coretx::SwitchPosition::middle) ||
        (hw::decode_switch(2730, true) != coretx::SwitchPosition::down)) {
        return false;
    }
    hw::Diagnostics state{};
    constexpr std::uint32_t start = 0xFFFFF000U;
    auto samples = stock_samples();
    state.observe(samples, coretx::TimestampUs{start});
    samples[4] = 3500;
    samples[8] = 500;
    state.observe(samples, coretx::TimestampUs{start + 5'000U});
    samples[4] = 500; // Contact bounce must restart SA's debounce interval.
    state.observe(samples, coretx::TimestampUs{start + 10'000U});
    samples[4] = 3500;
    state.observe(samples, coretx::TimestampUs{start + 15'000U});
    state.observe(samples, coretx::TimestampUs{start + 24'999U});
    if ((state.raw().switches[0] != coretx::SwitchPosition::up) ||
        (state.raw().switches[2] != coretx::SwitchPosition::middle)) {
        return false;
    }
    state.observe(samples, coretx::TimestampUs{start + 25'000U});
    if (state.raw().switches[2] != coretx::SwitchPosition::up) {
        return false;
    }
    state.observe(samples, coretx::TimestampUs{start + 35'000U});
    return state.raw().switches[0] == coretx::SwitchPosition::down;
}

bool stock_keys() {
    struct KeyScenario {};
    using Test = coretx::test::RegisterFixture<KeyScenario, FakeRegisters>;
    using Keys = hw::i6x::Keys<Test::Io>;
    Test::reset_registers();
    auto &io = Test::registers;
    io.cell(reg::tim2_cnt) = 0xFFFFF000U;
    Keys::initialize();
    if (((io.cell(GPIOC_BASE + offsetof(GPIO_TypeDef, OTYPER)) & 0x1C0U) != 0x1C0U) ||
        ((io.cell(GPIOD_BASE + offsetof(GPIO_TypeDef, PUPDR)) & 0xFF000000U) != 0x55000000U) ||
        ((io.cell(GPIOC_BASE + offsetof(GPIO_TypeDef, MODER)) & GPIO_MODER_MODER9) != 0)) {
        return false;
    }
    auto now = io.cell(reg::tim2_cnt);
    // A short OK pulse must not advance the calibration wizard.
    for (std::size_t i = 0; i < 15; ++i) {
        io.matrix_keys = i < 6 ? std::uint16_t{1} << 10 : 0;
        now += 1'000;
        if (Keys::service(coretx::TimestampUs{now}) != hw::Key::none) {
            return false;
        }
    }
    const auto scan = [&](const std::uint16_t pressed, const hw::Key expected) {
        io.matrix_keys = pressed;
        std::size_t events{};
        for (std::size_t i = 0; i < 60; ++i) {
            now += 1'000;
            const auto event = Keys::service(coretx::TimestampUs{now});
            if (event != hw::Key::none) {
                if ((event != expected) || (i < 20)) {
                    return false;
                }
                ++events;
            }
        }
        return events == (expected == hw::Key::none ? 0U : 1U);
    };
    for (std::size_t i = 0; i < 4; ++i) {
        constexpr std::array expected{hw::Key::down, hw::Key::up, hw::Key::ok, hw::Key::cancel};
        if (!scan(static_cast<std::uint16_t>(std::uint16_t{1} << (8U + i)),
                  i < 2 ? expected[i] : hw::Key::none) ||
            !scan(static_cast<std::uint16_t>(std::uint16_t{1} << (8U + i)), hw::Key::none) ||
            !scan(0, i < 2 ? hw::Key::none : expected[i])) {
            return false;
        }
    }
    if (!scan((std::uint16_t{1} << 10) | (std::uint16_t{1} << 11), hw::Key::none) ||
        !scan(0, hw::Key::none))
        return false;
    for (const auto [bit, expected] :
         std::array{std::pair{10U, hw::Key::ok_long}, std::pair{11U, hw::Key::cancel_long}}) {
        io.matrix_keys = static_cast<std::uint16_t>(1U << bit);
        std::size_t events{};
        for (std::size_t i = 0; i < 1000; ++i) {
            now += 1000;
            const auto key = Keys::service(coretx::TimestampUs{now});
            if (key != hw::Key::none) {
                if ((key != expected) || (i < 800))
                    return false;
                ++events;
            }
        }
        if ((events != 1) || !scan(0, hw::Key::none))
            return false;
    }
    // A ghost combination must not turn a held OK into a short OK action.
    if (!scan(std::uint16_t{1} << 10, hw::Key::none) || !Keys::ok_held() ||
        !scan((std::uint16_t{1} << 10) | (std::uint16_t{1} << 11), hw::Key::none) ||
        Keys::ok_held() || !scan(0, hw::Key::none) ||
        !scan(std::uint16_t{1} << 10, hw::Key::none) || !Keys::ok_held() || !scan(0, hw::Key::ok) ||
        Keys::ok_held())
        return false;
    if (!scan(0x11, hw::Key::none) || (Keys::trim_buttons() != 0x11) ||
        !scan(0x411, hw::Key::none) || !Keys::ok_held() || (Keys::trim_buttons() != 0x11) ||
        !scan(0x11, hw::Key::ok) || !scan(0x33, hw::Key::none) || (Keys::trim_buttons() != 0) ||
        !scan(0, hw::Key::none))
        return false;
    io.matrix_keys = std::uint16_t{1} << 9;
    unsigned repeats{};
    for (unsigned i = 0; i < 700; ++i) {
        now += 1000;
        const auto key = Keys::service(coretx::TimestampUs{now});
        if (key == hw::Key::up)
            ++repeats;
        else if (key != hw::Key::none)
            return false;
    }
    return (repeats >= 3) && scan(0, hw::Key::none);
}

constexpr bool extended_physical_footer() {
    hw::Diagnostics state{coretx::firmware::features::i6s};
    auto calibration = state.setup();
    calibration.calibration.fill({0, 2048, 4095});
    if (!state.restore_calibration(calibration))
        return false;
    auto setup = state.input_setup();
    setup.modes.fill(coretx::InputMode::six_position);
    setup.extra_analog = true;
    setup.external_module = false;
    for (std::size_t i = 0; i < (coretx::firmware::features::i6s ? 5U : 2U); ++i)
        setup.set_digital_mode(i, coretx::DigitalInputMode::two_position);
    if (!state.set_input_setup(setup))
        return false;
    std::array<std::uint16_t, 16> samples{};
    state.observe(samples, coretx::TimestampUs{100'000});
    state.observe_digital(0x15, coretx::TimestampUs{100'000});
    const auto expected_mask = !coretx::firmware::features::extended_inputs ? std::uint16_t{0x3F}
                               : coretx::firmware::features::i6s            ? std::uint16_t{0xFFFF}
                                                                            : std::uint16_t{0x1BFF};
    if (!state.output_valid() || (state.switch_mask() != expected_mask) ||
        (state.prepared().controls.active_switches != expected_mask))
        return false;
    coretx::ui::Framebuffer display{};
    const auto row_matches = [&](const std::string_view text) constexpr {
        hw::render_dashboard(state, nullptr, display);
        coretx::ui::Framebuffer expected{};
        expected.compact();
        expected.text(0, 52, text);
        for (int y = 52; y < 58; ++y)
            for (int x = 0; x < 128; ++x)
                if (display.pixel_at(x, y) != expected.pixel_at(x, y))
                    return false;
        return true;
    };
    const std::string_view all = !coretx::firmware::features::extended_inputs ? "A1B1C1D1a1b1"
                                 : coretx::firmware::features::i6s
                                     ? "A1B1C1D1a1b1I1J1K1L1M1GvH^NvO^Pv"
                                     : "A1B1C1D1a1b1c1d1e1f1EvF^";
    if (!row_matches(all))
        return false;
    const auto unchanged = display;
    display.compact();
    hw::render_physical_switches(display, state.prepared().controls.switches, expected_mask, 0);
    for (int y = 0; y < 64; ++y) {
        if ((y >= 52) && (y < 58))
            continue;
        for (int x = 0; x < 128; ++x)
            if (display.pixel_at(x, y) != unchanged.pixel_at(x, y))
                return false;
    }
    for (std::uint16_t position = 0; position < 6; ++position) {
        samples[4] = static_cast<std::uint16_t>(position * 683U);
        const auto now = 200'000U + (static_cast<std::uint32_t>(position) * 40'000U);
        state.observe(samples, coretx::TimestampUs{now});
        state.observe(samples, coretx::TimestampUs{now + 20'000U});
        hw::render_dashboard(state, nullptr, display);
        coretx::ui::Framebuffer expected{};
        expected.compact();
        const std::array<char, 2> label{'A', static_cast<char>('1' + position)};
        expected.text(0, 52, {label.data(), label.size()});
        for (int y = 52; y < 58; ++y)
            for (int x = 0; x < 8; ++x)
                if (display.pixel_at(x, y) != expected.pixel_at(x, y))
                    return false;
    }
    setup.modes[0] = coretx::InputMode::off;
    setup.modes[5] = coretx::InputMode::pot;
    setup.extra_analog = false;
    setup.digital_modes = 0;
    if (!state.set_input_setup(setup) || !row_matches("B?C?D?a?"))
        return false;
    state.observe(samples, coretx::TimestampUs{500'000});
    return (state.prepared().controls.active_switches == 0x1E) && row_matches("B1C1D1a1");
}

constexpr bool stock_dashboard_empty() {
    hw::Diagnostics state{};
    coretx::ui::Framebuffer display{};
    hw::render_dashboard(state, nullptr, display);
    const auto empty = display.bytes();
    state.observe(stock_samples(), coretx::TimestampUs{0});
    hw::render_dashboard(state, nullptr, display);
    if (display.bytes() == empty) {
        return false;
    }
    calibrate(state);
    hw::render_dashboard(state, nullptr, display);
    return display.bytes() != empty;
}

constexpr bool overview_and_latch_lifecycle() {
    using coretx::LogicalFunction;
    hw::Diagnostics state{};
    state.handle(hw::Key::ok);
    if ((state.page() != 0) || (state.phase() != hw::CalibrationPhase::idle))
        return false;
    calibrate(state);
    coretx::Model model{};
    model.mixes.resize<2>();
    model.mixes[0].destination = 8;
    model.mixes[0].source = {coretx::SourceKind::maximum, 0, false};
    model.mixes[0].weight_percent = coretx::MixWeight::constant<-50>();
    model.mixes[1].destination = 15;
    model.mixes[1].source = {coretx::SourceKind::maximum, 0, false};
    model.mixes[1].weight_percent = coretx::MixWeight::constant<150>();
    model.outputs[15].maximum_permille = coretx::TravelPermille::constant<1500>();
    model.dashboard_channels = {16, 0, 9, 1, 0, 2, 0, 0};
    auto &latch = model.logical_switches[0];
    latch.function = LogicalFunction::srff;
    latch.a = 0;
    latch.b = 3;
    latch.position_a = latch.position_b = coretx::SwitchPosition::down;
    model.logical_switches[7].function = LogicalFunction::greater;
    model.logical_switches[7].a = 15;
    model.logical_switches[7].inverted = true;
    state.use_model(model);
    if (state.evaluation().frame.channels[15] != 1536)
        return false;
    coretx::telemetry::Pool pool{};
    pool.publish(coretx::telemetry::transmitter_battery, 6200, coretx::TimestampUs{100});
    pool.publish(
        {coretx::telemetry::Source::afhds2a, coretx::telemetry::Quantity::receiver_voltage, 4},
        5000, coretx::TimestampUs{100});
    pool.publish({coretx::telemetry::Source::afhds2a, coretx::telemetry::Quantity::receiver_error},
                 88, coretx::TimestampUs{100});
    pool.publish({coretx::telemetry::Source::crsf, coretx::telemetry::Quantity::rssi, 1}, -75,
                 coretx::TimestampUs{100});
    coretx::ui::Framebuffer display{};
    const auto render = [&](const coretx::firmware::RadioOutput output,
                            const coretx::TimestampUs now) constexpr {
        hw::render_dashboard(state, nullptr, display, {}, pool, output, nullptr, now, "Glider");
    };
    render(coretx::firmware::RadioOutput::internal, coretx::TimestampUs{100});
    coretx::ui::Framebuffer staged{};
    hw::render_overview(state, staged, pool, coretx::firmware::RadioOutput::internal,
                        coretx::TimestampUs{100}, "Glider", nullptr, hw::OverviewSection::header);
    for (std::size_t slot = 0; slot < 8; ++slot)
        hw::render_overview(state, staged, pool, coretx::firmware::RadioOutput::internal,
                            coretx::TimestampUs{100}, "Glider", nullptr,
                            hw::OverviewSection::channel, slot);
    hw::render_overview(state, staged, pool, coretx::firmware::RadioOutput::internal,
                        coretx::TimestampUs{100}, "Glider", nullptr, hw::OverviewSection::footer);
    if (staged.bytes() != display.bytes())
        return false;
    using Layout = hw::OverviewLayout;
    coretx::ui::Framebuffer heading{};
    heading.compact();
    heading.text(Layout::left, Layout::channel_heading_y, coretx::ui::tr::output_channels);
    heading.text(104, Layout::model_y, "Glider");
    for (int y = Layout::channel_heading_y;
         y < (Layout::channel_heading_y + Layout::character_height); ++y)
        for (int x = 0; x < coretx::ui::Framebuffer::width; ++x)
            if (display.pixel_at(x, y) != heading.pixel_at(x, y))
                return false;
    if (!display.pixel_at(Layout::fill_left, Layout::zero_y - Layout::maximum_fill_height) ||
        display.pixel_at(Layout::fill_left, Layout::negative_fill_y + 2) ||
        !display.pixel_at(2 * Layout::channel_spacing + Layout::fill_left,
                          Layout::negative_fill_y + 2) ||
        display.pixel_at(2 * Layout::channel_spacing + Layout::fill_left, Layout::zero_y - 5) ||
        display.pixel_at(Layout::channel_spacing + Layout::fill_left, Layout::zero_y))
        return false;
    const auto header_matches = [&](const std::string_view tx, const std::string_view rx,
                                    const std::string_view label,
                                    const std::string_view signal) constexpr {
        coretx::ui::Framebuffer expected{};
        expected.compact();
        expected.text(Layout::left, Layout::voltage_y, "TX");
        expected.text(Layout::tx_voltage_right - static_cast<int>(tx.size()) * 4, Layout::voltage_y,
                      tx);
        expected.text(Layout::rx_label_x, Layout::voltage_y, "RX");
        expected.text(Layout::rx_voltage_right - static_cast<int>(rx.size()) * 4, Layout::voltage_y,
                      rx);
        expected.text(Layout::signal_label_x, Layout::voltage_y, label);
        expected.text(Layout::right - static_cast<int>(signal.size()) * 4, Layout::voltage_y,
                      signal);
        for (int y = 0; y < Layout::character_height; ++y)
            for (int x = 0; x < 128; ++x)
                if (display.pixel_at(x, y) != expected.pixel_at(x, y))
                    return false;
        return true;
    };
    if (!header_matches("6.20V", "5.00V", "LQ", "12%"))
        return false;
    for (const auto error : {0, 100}) {
        pool.publish(
            {coretx::telemetry::Source::afhds2a, coretx::telemetry::Quantity::receiver_error},
            error, coretx::TimestampUs{100});
        render(coretx::firmware::RadioOutput::internal, coretx::TimestampUs{100});
        if (!header_matches("6.20V", "5.00V", "LQ", error == 0 ? "100%" : "0%"))
            return false;
    }
    pool.invalidate(
        {coretx::telemetry::Source::afhds2a, coretx::telemetry::Quantity::receiver_error});
    render(coretx::firmware::RadioOutput::internal, coretx::TimestampUs{100});
    if (!header_matches("6.20V", "5.00V", "LQ", "--"))
        return false;
    render(coretx::firmware::RadioOutput::external, coretx::TimestampUs{500'100});
    if (!header_matches("--", "--", "RSSI", "-75"))
        return false;
    pool.publish(
        {coretx::telemetry::Source::crsf, coretx::telemetry::Quantity::flight_battery_voltage, 4},
        5200, coretx::TimestampUs{100});
    render(coretx::firmware::RadioOutput::external, coretx::TimestampUs{500'100});
    if (!header_matches("--", "5.20V", "RSSI", "-75"))
        return false;
    render(coretx::firmware::RadioOutput::external, coretx::TimestampUs{2'000'100});
    if (!header_matches("--", "--", "RSSI", "--"))
        return false;
    pool.publish({coretx::telemetry::Source::afhds2a, coretx::telemetry::Quantity::receiver_error},
                 88, coretx::TimestampUs{100});
    render(coretx::firmware::RadioOutput::internal, coretx::TimestampUs{2'000'100});
    if (!header_matches("--", "--", "LQ", "--"))
        return false;
    render(coretx::firmware::RadioOutput::off, coretx::TimestampUs{100});
    if (!header_matches("6.20V", "--", "RSSI", "--"))
        return false;
    coretx::ui::Framebuffer before_second{};
    hw::render_dashboard(state, nullptr, before_second, {}, pool,
                         coretx::firmware::RadioOutput::off, nullptr, coretx::TimestampUs{999'999},
                         "Glider");
    render(coretx::firmware::RadioOutput::off, coretx::TimestampUs{1'000'000});
    if (display.bytes() != before_second.bytes())
        return false;
    render(coretx::firmware::RadioOutput::off, coretx::TimestampUs{1'999'999});
    if (display.bytes() != before_second.bytes())
        return false;
    render(coretx::firmware::RadioOutput::off, coretx::TimestampUs{2'000'000});
    if (!header_matches("--", "--", "RSSI", "--"))
        return false;
    auto samples = stock_samples();
    samples[4] = 3500;
    state.observe(samples, coretx::TimestampUs{30'000});
    state.observe(samples, coretx::TimestampUs{50'000});
    if ((state.evaluation().frame.logical.active & 1U) == 0)
        return false;
    render(coretx::firmware::RadioOutput::off, coretx::TimestampUs{50'000});
    coretx::ui::Framebuffer footer{};
    footer.compact();
    footer.text(0, 52, "AvBv");
    footer.text(0, 58, "L11");
    footer.text(112, 58, "L8?");
    for (int y = 52; y < 64; ++y)
        for (int x = 0; x < 16; ++x)
            if (display.pixel_at(x, y) != footer.pixel_at(x, y))
                return false;
    for (int y = 58; y < 64; ++y)
        for (int x = 112; x < 128; ++x)
            if (display.pixel_at(x, y) != footer.pixel_at(x, y))
                return false;
    samples[4] = 500;
    state.observe(samples, coretx::TimestampUs{70'000});
    state.observe(samples, coretx::TimestampUs{90'000});
    state.refresh_model();
    if ((state.evaluation().frame.logical.active & 1U) == 0)
        return false;
    // Preview evaluations must never commit a Reset to the live latch.
    state.allow_logical_commit(false);
    samples[9] = 3500;
    state.observe(samples, coretx::TimestampUs{110'000});
    state.observe(samples, coretx::TimestampUs{130'000});
    samples[9] = 500;
    state.observe(samples, coretx::TimestampUs{150'000});
    state.observe(samples, coretx::TimestampUs{170'000});
    state.allow_logical_commit(true);
    state.refresh_model();
    if ((state.evaluation().frame.logical.active & 1U) == 0)
        return false;
    state.use_model(model);
    if ((state.evaluation().frame.logical.active & 1U) != 0)
        return false;
    model.dashboard_channels[0] = 17;
    state.refresh_model();
    return !state.output_valid() &&
           (state.evaluation().status.code == coretx::ErrorCode::dashboard_settings);
}
constexpr bool stock_dashboard() {
    if (!stock_dashboard_empty())
        return false;
    hw::Diagnostics state{};
    coretx::ui::Framebuffer display{};
    calibrate(state);
    hw::render_dashboard(state, nullptr, display);
    const auto overview = display.bytes();
    state.handle(hw::Key::down);
    if (state.page() != hw::Diagnostics::system_page) {
        return false;
    }
    state.handle(hw::Key::ok);
    if (state.phase() != hw::CalibrationPhase::idle) {
        return false;
    }
    hw::render_dashboard(state, nullptr, display);
    if (display.bytes() == overview)
        return false;
    state.handle(hw::Key::down);
    if (state.page() != 0) {
        return false;
    }
    state.handle(hw::Key::up);
    if (state.page() != hw::Diagnostics::system_page) {
        return false;
    }
    state.handle(hw::Key::up);
    state.handle(hw::Key::ok);
    if (state.phase() != hw::CalibrationPhase::idle)
        return false;
    state.begin_calibration();
    hw::render_dashboard(state, nullptr, display);
    const auto center = display.bytes();
    state.handle(hw::Key::ok);
    hw::render_dashboard(state, nullptr, display);
    if (display.bytes() == center) {
        return false;
    }
    hw::render_dashboard(state, coretx::ui::tr::overrun, display);
    coretx::ui::Framebuffer expected{};
    expected.text(3, 3, coretx::ui::tr::adc_error);
    expected.text(3, 20, coretx::ui::tr::overrun);
    return display.bytes() == expected.bytes();
}

constexpr bool dashboard_values() {
    constexpr std::array<std::string_view, 4> labels{"RH PA0", "RV PA1", "LV PA2", "LH PA3"};
    constexpr std::array<std::uint16_t, 4> samples{0, 1000, 2048, 4095};
    coretx::ui::Framebuffer display{};
    coretx::ui::render_adc_dashboard(labels, samples, true, nullptr, display);
    coretx::ui::Framebuffer numbers{};
    for (std::size_t i = 0; i < 4; ++i) {
        numbers.number(123, 18 + static_cast<int>(i) * 10, samples[i]);
    }
    for (int y = 18; y < 56; ++y) {
        for (int x = 90; x < 128; ++x) {
            if (display.pixel_at(x, y) != numbers.pixel_at(x, y))
                return false;
        }
    }
    const auto valid = display.bytes();
    coretx::ui::render_adc_dashboard(labels, samples, false, nullptr, display);
    if (display.bytes() == valid)
        return false;
    coretx::ui::render_adc_dashboard(labels, samples, false, "OVERRUN", display);
    for (int y = 44; y < 64; ++y) {
        for (int x = 0; x < 128; ++x) {
            if (display.pixel_at(x, y))
                return false;
        }
    }
    return true;
}

constexpr bool system_info_rendering() {
    hw::Diagnostics state{};
    for (std::size_t page = 0; page < hw::Diagnostics::system_page; ++page) {
        state.handle(hw::Key::down);
    }
    for (const auto probe_state : {hw::StorageProbeState::found, hw::StorageProbeState::not_found,
                                   hw::StorageProbeState::failed}) {
        coretx::ui::Framebuffer display{};
        hw::render_dashboard(state, coretx::ui::tr::overrun, display,
                             {probe_state, hw::I2cError::timeout, 1, coretx::ui::tr::ram_cal, 17,
                              coretx::ui::tr::inputs_invalid, "I6S"});
        coretx::ui::Framebuffer expected{};
        expected.compact();
        expected.text(0, 0, coretx::ui::tr::system_info);
        expected.text(96, 0, "FW");
        expected.unsigned_number(127, 0, coretx::firmware_version);
        expected.text(0, 8, coretx::ui::tr::sender);
        expected.text(96, 8, "I6S");
        expected.text(0, 39, coretx::ui::tr::calibration_status);
        expected.text(48, 39, coretx::ui::tr::ram_cal);
        expected.text(0, 47, coretx::ui::tr::input_status);
        expected.text(32, 47, coretx::ui::tr::inputs_invalid);
        expected.text(0, 55, coretx::ui::tr::watchdog_resets);
        expected.unsigned_number(127, 55, 17);
        for (int y = 0; y < 64; ++y) {
            if (!((y < 6) || ((y >= 8) && (y < 14)) || (y >= 39)))
                continue;
            for (int x = 0; x < 128; ++x) {
                if (display.pixel_at(x, y) != expected.pixel_at(x, y)) {
                    return false;
                }
            }
        }
    }
    return true;
}

bool adc_deferred_enable() {
    struct TestScenario {};
    using Test = coretx::test::RegisterFixture<TestScenario, FakeRegisters>;
    using Adc =
        hw::Adc<Test::Io, coretx::firmware::Hardware, hw::stock_inputs, hw::coherent_adc_snapshots>;

    Test::reset_registers();
    auto &io = Test::registers;

    io.timer_stopped = true;
    if ((Adc::initialize(4) != hw::AdcError::none) || !Adc::initializing() ||
        ((io.cell(adc_reg::cr) & ADC_CR_ADEN) != 0)) {
        return false;
    }
    if ((Adc::service(coretx::TimestampUs{0}) != hw::ScanEvent::pending) ||
        (Adc::service(coretx::TimestampUs{1}) != hw::ScanEvent::pending) ||
        ((io.cell(adc_reg::cr) & ADC_CR_ADEN) != 0) ||
        (Adc::service(coretx::TimestampUs{2}) != hw::ScanEvent::pending) || Adc::initializing() ||
        ((io.cell(adc_reg::cr) & ADC_CR_ADEN) == 0)) {
        return false;
    }
    return true;
}

bool bootloader_message() {
    struct Scenario {};
    using Test = coretx::test::RegisterFixture<Scenario, FakeRegisters>;
    using Board = hw::SenderBoard<Test::Io>;
    Test::reset_registers();
    auto &io = Test::registers;
    if (!Board::bootloader_message() || Board::Display::busy() || io.lcd_timing_error ||
        (io.cell(reg::tim2_psc) != 7) ||
        ((io.cell(reg::rcc_cr) & (RCC_CR_HSEON | RCC_CR_PLLON)) != 0))
        return false;
    coretx::ui::Framebuffer expected{};
    expected.text(22, 28, "DFU-Bootloader");
    std::size_t pixel{};
    for (std::size_t i = 0; i < io.lcd_count; ++i)
        if (io.lcd_bytes[i].data) {
            if ((pixel >= expected.bytes().size()) ||
                (io.lcd_bytes[i].value != expected.bytes()[pixel++]))
                return false;
        }
    if ((pixel != expected.bytes().size()) || (io.lcd_bytes[io.lcd_count - 1].value != 0xAF))
        return false;
    io.cell(Board::Bootloader::address) = 0x20004000;
    io.cell(Board::Bootloader::address + 4U) = 0x1FFFC901;
    if (!Board::Bootloader::prepare() || !io.barrier_called ||
        ((io.cell(GPIOF_BASE + offsetof(GPIO_TypeDef, ODR)) & GPIO_ODR_3) == 0) ||
        ((io.cell(GPIOC_BASE + offsetof(GPIO_TypeDef, ODR)) & GPIO_ODR_9) == 0) ||
        ((io.cell(GPIOB_BASE + offsetof(GPIO_TypeDef, ODR)) & GPIO_ODR_4) == 0) ||
        (io.cell(reg::rcc_apb1enr) != 0))
        return false;
    Test::reset_registers();
    io.timer_stopped = true;
    return !Board::bootloader_message() && (io.lcd_count == 0);
}

bool lcd_irregular_timing() {
    struct TestScenario {};
    using Test = coretx::test::RegisterFixture<TestScenario, FakeRegisters>;
    Test::reset_registers();
    auto &io = Test::registers;
    io.timer_stopped = true;
    io.cell(reg::tim2_cnt) = 0xFFFFF000U;
    using Display = coretx::firmware::Lcd<Test::Io, coretx::firmware::Hardware>;
    coretx::ui::Framebuffer display{};
    if ((Display::service(display) != LcdError::not_initialized) ||
        (Display::initialize() != LcdError::none)) {
        return false;
    }
    Display::begin_frame(); // May be queued while reset/initialization is still in progress.
    const auto start = io.cell(reg::tim2_cnt);
    io.cell(reg::tim2_cnt) = start + 1'000U;
    (void)Display::service(display);
    if ((io.cell(GPIOB_BASE + offsetof(GPIO_TypeDef, ODR)) & GPIO_ODR_4) != 0) {
        return false;
    }
    io.cell(reg::tim2_cnt) = start + 1'001U;
    (void)Display::service(display);
    if ((io.cell(GPIOB_BASE + offsetof(GPIO_TypeDef, ODR)) & GPIO_ODR_4) == 0) {
        return false;
    }
    constexpr std::array<std::uint32_t, 7> steps{0, 1, 0, 2, 17, 100, 1'000};
    for (std::size_t i = 0; Display::busy() && (i < 50'000); ++i) {
        io.cell(reg::tim2_cnt) += steps[i % steps.size()];
        const auto before = io.cell(reg::tim2_cnt);
        if ((Display::service(display) != LcdError::none) || (io.cell(reg::tim2_cnt) != before)) {
            return false;
        }
    }
    if (Display::busy() || io.lcd_timing_error || (io.lcd_count != (12 + (8 * (3 + 128)) + 1))) {
        return false;
    }
    if ((io.lcd_bytes[io.lcd_count - 1].value != 0xAF) || ((io.lcd_bytes[0].at - start) < 2'002U) ||
        ((io.lcd_bytes[1].at - io.lcd_bytes[0].at) < 10'001U) ||
        ((io.lcd_bytes[12].at - io.lcd_bytes[11].at) < 20'001U)) {
        return false;
    }
    Display::begin_frame();
    Display::begin_frame();
    return Display::error() == LcdError::frame_busy;
}

bool lcd_bounded_transfer() {
    struct Scenario {};
    using Test = coretx::test::RegisterFixture<Scenario, FakeRegisters>;
    using Display = coretx::firmware::Lcd<Test::Io, coretx::firmware::Hardware>;
    Test::reset_registers();
    auto &io = Test::registers;
    io.timer_stopped = true;
    coretx::ui::Framebuffer display{};
    display.rectangle(0, 0, 128, 64);
    if (Display::initialize() != LcdError::none)
        return false;
    for (unsigned i = 0; Display::busy() && (i < 500); ++i) {
        io.cell(reg::tim2_cnt) += 100;
        (void)Display::service(display);
    }
    if (Display::busy())
        return false;
    const auto initial_bytes = io.lcd_count;
    Display::begin_frame();
    const auto stopped_at = io.cell(reg::tim2_cnt);
    if ((Display::service(display) != LcdError::none) || (io.cell(reg::tim2_cnt) != stopped_at) ||
        (io.lcd_count != initial_bytes))
        return false;
    io.cell(reg::tim2_cnt) = 0xFFFFFF00U;
    io.timer_stopped = false;
    for (unsigned calls = 0; Display::busy() && (calls < 150); ++calls) {
        const auto before = Test::Io::now_us();
        if ((Display::service(display) != LcdError::none) ||
            ((Test::Io::now_us() - before) > coretx::DurationUs{110}))
            return false;
    }
    if (Display::busy() || io.lcd_timing_error ||
        ((io.lcd_count - initial_bytes) != ((8 * (3 + 128)) + 1))) {
        std::fprintf(stderr, "LCD bounded frame: busy=%d, bytes=%zu, timing_error=%d\n",
                     Display::busy(), io.lcd_count - initial_bytes, io.lcd_timing_error);
        return false;
    }
    return true;
}

bool lcd_bus_and_board() {
    struct ClockScenario {};
    using Test = coretx::test::RegisterFixture<ClockScenario, FakeRegisters>;
    Test::reset_registers();
    auto &io = Test::registers;
    if ((coretx::firmware::Clock<Test::Io>::initialize(4) != chip::HardwareError::none) ||
        ((io.cell(reg::rcc_apb1enr) & RCC_APB1ENR_TIM2EN) == 0) ||
        (io.cell(GPIOB_BASE + offsetof(GPIO_TypeDef, MODER)) != 0xAAAAAAAAU)) {
        return false;
    }
    using Display = coretx::firmware::Lcd<Test::Io, coretx::firmware::Hardware>;
    if ((Display::initialize() != LcdError::none) ||
        ((io.cell(GPIOB_BASE + offsetof(GPIO_TypeDef, MODER)) & GPIO_MODER_MODER15) !=
         (0xAAAAAAAAU & GPIO_MODER_MODER15)) ||
        (io.lcd_count != 0) || !Display::busy()) {
        return false;
    }
    coretx::ui::Framebuffer display{};
    display.pixel(0, 0);
    display.pixel(127, 63);
    for (std::size_t i = 0; Display::busy() && (i < 100'000); ++i) {
        if (Display::service(display) != LcdError::none) {
            return false;
        }
    }
    if (Display::busy() || (io.lcd_count != 12) || (io.lcd_bytes[0].value != 0xE2) ||
        (io.lcd_bytes[1].value != 0xAE)) {
        return false;
    }
    const auto first = io.lcd_count;
    Display::begin_frame();
    for (std::size_t i = 0; Display::busy() && (i < 100'000); ++i) {
        if (Display::service(display) != LcdError::none)
            return false;
    }
    if (Display::busy()) {
        return false;
    }
    std::size_t cursor = first;
    for (std::size_t page = 0; page < 8; ++page) {
        if (io.lcd_bytes[cursor].data || (io.lcd_bytes[cursor++].value != (0xB0 | page)) ||
            io.lcd_bytes[cursor].data || (io.lcd_bytes[cursor++].value != 0x04) ||
            io.lcd_bytes[cursor].data || (io.lcd_bytes[cursor++].value != 0x10)) {
            return false;
        }
        for (std::size_t x = 0; x < 128; ++x) {
            if (!io.lcd_bytes[cursor].data ||
                (io.lcd_bytes[cursor++].value != display.bytes()[page * 128 + x])) {
                return false;
            }
        }
    }
    if (io.lcd_bytes[cursor].data || (io.lcd_bytes[cursor++].value != 0xAF) ||
        (cursor != io.lcd_count)) {
        return false;
    }
    Display::begin_frame();
    const auto before = io.lcd_count;
    for (std::size_t i = 0; Display::busy() && (i < 100'000); ++i) {
        if (Display::service(display) != LcdError::none)
            return false;
    }
    if (Display::busy() || ((io.lcd_count - before) != (8 * (3 + 128))) || io.lcd_timing_error)
        return false;
    // With no elapsed time, repeated service calls must not advance a pulse.
    io.timer_stopped = true;
    Display::begin_frame();
    const auto stopped = io.lcd_count;
    for (std::size_t i = 0; i < 10; ++i) {
        if ((Display::service(display) != LcdError::none) || (io.lcd_count != stopped)) {
            return false;
        }
    }
    return (io.cell(GPIOD_BASE + offsetof(GPIO_TypeDef, ODR)) & GPIO_ODR_7) == 0;
}

bool cooperative_adc_lcd() {
    struct TestScenario {};
    using Test = coretx::test::RegisterFixture<TestScenario, FakeRegisters>;
    Test::reset_registers();
    auto &io = Test::registers;
    using Adc = hw::Adc<Test::Io, coretx::firmware::Hardware, hw::stock_inputs>;
    using Display = coretx::firmware::Lcd<Test::Io, coretx::firmware::Hardware>;
    coretx::ui::Framebuffer display{};
    if ((Display::initialize() != LcdError::none) || (Adc::initialize(4) != hw::AdcError::none)) {
        return false;
    }
    Display::begin_frame();
    std::size_t scans{};
    for (std::size_t i = 0; Display::busy() && (i < 100'000); ++i) {
        const auto event = Adc::service(coretx::TimestampUs{io.now_us()});
        if ((event == hw::ScanEvent::failed) || (Display::service(display) != LcdError::none)) {
            return false;
        }
        if (event == hw::ScanEvent::complete) {
            ++scans;
        }
    }
    return !Display::busy() && (scans > 0) && Adc::valid() && (Adc::samples()[3] == 4095) &&
           !io.lcd_timing_error;
}

struct I2cRegisters : FakeRegisters {
    std::uint8_t responders{};
    std::array<std::uint8_t, 8> addresses{};
    std::size_t probes{};
    bool stalled{};
    bool invalid_transfer{};

    constexpr void write(const std::uintptr_t address, const std::uint32_t value) {
        using i2c = chip::I2cTraits<hw::Hardware::storage.peripheral>;
        if (address == i2c::icr) {
            cell(i2c::isr) &= ~value;
            return;
        }
        if ((address == reg::rcc_apb1rstr) && ((value & RCC_APB1RSTR_I2C2RST) != 0)) {
            cell(i2c::isr) = 0;
            cell(i2c::cr2) = 0;
        }
        FakeRegisters::write(address, value);
        if ((address == i2c::cr2) && ((value & I2C_CR2_START) != 0)) {
            const auto target = static_cast<std::uint8_t>((value & I2C_CR2_SADD) >> 1U);
            invalid_transfer |=
                ((value & (I2C_CR2_NBYTES | I2C_CR2_RD_WRN | I2C_CR2_ADD10)) != 0) ||
                ((value & I2C_CR2_AUTOEND) == 0);
            if ((probes == addresses.size()) || (target < 0x50) || (target > 0x57)) {
                std::abort();
            }
            addresses[probes++] = target;
            cell(i2c::isr) =
                stalled ? I2C_ISR_BUSY
                        : I2C_ISR_STOPF |
                              ((responders & (1U << (target - 0x50))) == 0 ? I2C_ISR_NACKF : 0U);
        }
    }
};

bool storage_detection() {
    struct Scenario {};
    using Test = coretx::test::RegisterFixture<Scenario, I2cRegisters>;
    using Probe = hw::StorageProbe<Test::Io, coretx::firmware::Hardware>;

    using i2c = chip::I2cTraits<hw::Hardware::storage.peripheral>;
    for (const auto responders : std::array<std::uint8_t, 4>{0, 1, 0x80, 0xA5}) {
        Test::reset_registers();
        auto &io = Test::registers;

        io.responders = responders;
        const auto initial_mode = io.cell(GPIOB_BASE + offsetof(GPIO_TypeDef, MODER));
        Probe::initialize();
        if ((Probe::state() != hw::StorageProbeState::scanning) ||
            (io.cell(i2c::cr1) != I2C_CR1_PE) || (io.cell(i2c::timingr) != StorageProbe::timing) ||
            (io.probes != 0)) {
            return false;
        }
        constexpr auto pin_mask = (3U << 20) | (3U << 22);
        if ((io.cell(GPIOB_BASE + offsetof(GPIO_TypeDef, MODER)) !=
             ((initial_mode & (~pin_mask)) | (2U << 20) | (2U << 22))) ||
            ((io.cell(GPIOB_BASE + offsetof(GPIO_TypeDef, OTYPER)) & (3U << 10)) != (3U << 10)) ||
            ((io.cell(GPIOB_BASE + offsetof(GPIO_TypeDef, PUPDR)) & pin_mask) !=
             ((1U << 20) | (1U << 22))) ||
            ((io.cell(GPIOB_BASE + offsetof(GPIO_TypeDef, AFR) + 4U) & 0xFF00U) != 0x1100U)) {
            return false;
        }
        const auto start = io.now_us();
        for (std::uint32_t step = 0; step < 16; ++step) {
            Probe::service(coretx::TimestampUs{start + step});
        }
        if ((Probe::state() !=
             (responders == 0 ? hw::StorageProbeState::not_found : hw::StorageProbeState::found)) ||
            (Probe::acknowledged() != responders) || (Probe::error() != hw::I2cError::none) ||
            io.invalid_transfer || (io.probes != 8)) {
            return false;
        }
        for (std::size_t i = 0; i < io.addresses.size(); ++i) {
            if (io.addresses[i] != (0x50U + i)) {
                return false;
            }
        }
        Probe::service(coretx::TimestampUs{start + 100'000});
        if (io.probes != 8) {
            return false;
        }
        hw::Diagnostics state{};
        for (std::size_t page = 0; page < hw::Diagnostics::system_page; ++page) {
            state.handle(hw::Key::down);
        }
        coretx::ui::Framebuffer display{};
        hw::render_dashboard(state, coretx::ui::tr::overrun, display,
                             {Probe::state(), Probe::error(), Probe::acknowledged()});
        coretx::ui::Framebuffer expected{};
        expected.compact();
        expected.text(0, 0, coretx::ui::tr::system_info);
        expected.text(96, 0, "FW");
        expected.unsigned_number(127, 0, coretx::firmware_version);
        expected.text(0, 8, coretx::ui::tr::sender);
        expected.text(96, 8, "I6X");
        expected.text(0, 16, "EEPROM");
        expected.text(0, 39, coretx::ui::tr::calibration_status);
        expected.text(48, 39, coretx::ui::tr::need_cal);
        expected.text(0, 47, coretx::ui::tr::input_status);
        expected.text(32, 47, coretx::ui::tr::status_ok);
        expected.text(0, 55, coretx::ui::tr::watchdog_resets);
        expected.unsigned_number(127, 55, 0);
        if (responders == 0) {
            expected.text(32, 16, coretx::ui::tr::not_found);
        } else {
            expected.text(32, 16, coretx::ui::tr::eeprom_found);
            for (std::uint8_t i = 0; i < 8; ++i) {
                if ((responders & (1U << i)) != 0) {
                    const std::array<char, 4> address{'0', 'x', '5', static_cast<char>('0' + i)};
                    expected.text(static_cast<int>(i % 4) * 32, 24 + static_cast<int>(i / 4) * 7,
                                  std::string_view{address.data(), address.size()});
                }
            }
        }
        if (display.bytes() != expected.bytes()) {
            return false;
        }
    }
    return true;
}

bool storage_errors() {
    struct UninitializedScenario {};
    using UninitializedTest = coretx::test::RegisterFixture<UninitializedScenario, I2cRegisters>;
    using Uninitialized = hw::StorageProbe<UninitializedTest::Io, coretx::firmware::Hardware>;
    UninitializedTest::reset_registers();
    const auto untouched = UninitializedTest::registers.cell_count;
    Uninitialized::service(coretx::TimestampUs{0});
    if ((UninitializedTest::registers.cell_count != untouched) ||
        (UninitializedTest::registers.probes != 0))
        return false;

    struct Scenario {};
    using Test = coretx::test::RegisterFixture<Scenario, I2cRegisters>;
    using Probe = hw::StorageProbe<Test::Io, coretx::firmware::Hardware>;

    using i2c = chip::I2cTraits<hw::Hardware::storage.peripheral>;
    for (const auto flag :
         std::array<std::uint32_t, 5>{I2C_ISR_BUSY, 0, I2C_ISR_BERR, I2C_ISR_ARLO, I2C_ISR_OVR}) {
        Test::reset_registers();
        auto &io = Test::registers;
        io.timer_stopped = true;
        io.cell(reg::tim2_cnt) = 0xFFFFFF00U;
        io.stalled = true;

        Probe::initialize();
        const auto start = io.now_us();
        if (flag == I2C_ISR_BUSY) {
            io.cell(i2c::isr) = flag;
        } else {
            Probe::service(coretx::TimestampUs{start});
            io.cell(i2c::isr) = flag;
        }
        Probe::service(coretx::TimestampUs{start + StorageProbe::timeout_us.count() - 1U});
        if ((flag == 0) || (flag == I2C_ISR_BUSY)) {
            if (Probe::state() != hw::StorageProbeState::scanning) {
                return false;
            }
            Probe::service(coretx::TimestampUs{start + StorageProbe::timeout_us.count()});
        }
        const auto expected = flag == I2C_ISR_BUSY   ? hw::I2cError::busy
                              : flag == 0            ? hw::I2cError::timeout
                              : flag == I2C_ISR_BERR ? hw::I2cError::bus
                              : flag == I2C_ISR_ARLO ? hw::I2cError::arbitration
                                                     : hw::I2cError::overrun;
        if ((Probe::state() != hw::StorageProbeState::failed) || (Probe::error() != expected) ||
            (hw::error_message(Probe::error()) == nullptr) || (io.cell(i2c::cr1) != 0)) {
            return false;
        }
        Probe::initialize();
        if ((Probe::state() != hw::StorageProbeState::scanning) ||
            (Probe::error() != hw::I2cError::none) || (Probe::acknowledged() != 0)) {
            return false;
        }
    }
    Test::reset_registers();
    auto &io = Test::registers;
    io.stalled = true;

    Probe::initialize();
    const auto start = io.now_us();
    Probe::service(coretx::TimestampUs{start});
    io.cell(i2c::isr) = I2C_ISR_NACKF | I2C_ISR_BUSY;
    Probe::service(coretx::TimestampUs{start + 1U});
    if (Probe::state() != hw::StorageProbeState::scanning) {
        return false;
    }
    io.cell(i2c::isr) = I2C_ISR_STOPF;
    Probe::service(coretx::TimestampUs{start + 2U});
    return (Probe::acknowledged() == 0) && (Probe::error() == hw::I2cError::none);
}

struct EepromRegisters : FakeRegisters {
    std::array<std::uint8_t, Eeprom::capacity> memory{};
    std::uint8_t responders{1};
    std::uint32_t remaining{};
    std::uint32_t transmitted{};
    std::uint16_t pointer{};
    std::uint16_t page_start{};
    std::size_t payload_writes{};
    std::size_t ready_polls{};
    std::size_t busy_polls{};
    bool autoend{};
    bool write_protected{};
    bool never_ready{};
    bool stalled{};
    bool page_crossed{};

    constexpr EepromRegisters() { memory.fill(0xFF); }

    constexpr std::uint32_t read(const std::uintptr_t address) {
        using i2c = chip::I2cTraits<hw::Hardware::storage.peripheral>;
        if (address == i2c::rxdr) {
            const auto value = memory[pointer++];
            cell(i2c::isr) = --remaining == 0 ? I2C_ISR_STOPF : I2C_ISR_BUSY | I2C_ISR_RXNE;
            return value;
        }
        return FakeRegisters::read(address);
    }

    constexpr void write(const std::uintptr_t address, const std::uint32_t value) {
        using i2c = chip::I2cTraits<hw::Hardware::storage.peripheral>;
        if (address == i2c::icr) {
            cell(i2c::isr) &= ~value;
            return;
        }
        if ((address == reg::rcc_apb1rstr) && ((value & RCC_APB1RSTR_I2C2RST) != 0)) {
            cell(i2c::isr) = 0;
            cell(i2c::cr2) = 0;
        }
        FakeRegisters::write(address, value);
        if ((address == i2c::cr2) && ((value & I2C_CR2_START) != 0)) {
            const auto device = (value & I2C_CR2_SADD) >> 1U;
            remaining = (value & I2C_CR2_NBYTES) >> I2C_CR2_NBYTES_Pos;
            if (stalled) {
                cell(i2c::isr) = I2C_ISR_BUSY;
                return;
            }
            if ((device < 0x50) || (device > 0x57) ||
                ((responders & (1U << (device - 0x50))) == 0)) {
                cell(i2c::isr) = I2C_ISR_STOPF | I2C_ISR_NACKF;
            } else if (remaining == 0) {
                ++ready_polls;
                const bool nack = (busy_polls != 0) || never_ready;
                if (busy_polls != 0) {
                    --busy_polls;
                }
                cell(i2c::isr) = I2C_ISR_STOPF | (nack ? I2C_ISR_NACKF : 0U);
            } else if ((value & I2C_CR2_RD_WRN) != 0) {
                cell(i2c::isr) = I2C_ISR_BUSY | I2C_ISR_RXNE;
            } else {
                transmitted = 0;
                autoend = (value & I2C_CR2_AUTOEND) != 0;
                cell(i2c::isr) = I2C_ISR_BUSY | I2C_ISR_TXIS;
            }
        } else if (address == i2c::txdr) {
            if (transmitted == 0) {
                pointer = static_cast<std::uint16_t>(value << 8U);
            } else if (transmitted == 1) {
                pointer = static_cast<std::uint16_t>(pointer | value);
                page_start = pointer;
            } else {
                page_crossed |= (pointer / Eeprom::page_size) != (page_start / Eeprom::page_size);
                if (pointer >= memory.size()) {
                    std::abort();
                }
                if (!write_protected) {
                    memory[pointer] = static_cast<std::uint8_t>(value);
                }
                ++pointer;
                ++payload_writes;
            }
            ++transmitted;
            if (--remaining == 0) {
                cell(i2c::isr) = autoend ? I2C_ISR_STOPF : I2C_ISR_BUSY | I2C_ISR_TC;
                if (transmitted > 2) {
                    busy_polls = 3;
                }
            }
        }
    }
};

template <typename Scenario>
struct PersistenceFixture : coretx::test::RegisterFixture<Scenario, EepromRegisters> {
    using Base = coretx::test::RegisterFixture<Scenario, EepromRegisters>;
    using Memory = TestMemory<PersistenceFixture>;
    using Storage = hw::CalibrationStorage<Memory>;
    using Probe = hw::StorageProbe<typename Base::Io, coretx::firmware::Hardware>;

    static void reset() noexcept {
        Base::reset_registers();
        Memory::reset();
    }
};

namespace cal = hw::calibration_storage;
constexpr std::size_t cal_first = cal::base_address;
constexpr std::size_t cal_second = cal::base_address + cal::record_size;
constexpr std::size_t cal_end = cal::base_address + 2U * cal::record_size;

template <typename Probe, typename Storage>
bool run_persistence(EepromRegisters &io, Storage &storage, hw::Diagnostics &diagnostics) {
    for (std::size_t step = 0; step < 30'000; ++step) {
        io.cell(reg::tim2_cnt) += 100;
        const auto now = io.now_us();
        Probe::service(coretx::TimestampUs{now});
        storage.service(coretx::TimestampUs{now}, Probe::result(), diagnostics);
        if (!storage.busy()) {
            return true;
        }
    }
    return false;
}

template <typename Storage>
void apply_calibration(hw::Diagnostics &diagnostics, Storage &storage,
                       const std::uint16_t minimum = 400) {
    diagnostics.observe(stock_samples(), coretx::TimestampUs{0});
    diagnostics.begin_calibration();
    storage.handle(hw::Key::ok, diagnostics);
    diagnostics.observe(stock_samples(minimum), coretx::TimestampUs{5'000});
    diagnostics.observe(stock_samples(3600), coretx::TimestampUs{10'000});
    storage.handle(hw::Key::ok, diagnostics);
}

[[nodiscard]] bool outside_calibration(const std::array<std::uint8_t, 16384> &memory,
                                       const std::uint8_t value) {
    const auto same = [value](const auto byte) { return byte == value; };
    return std::all_of(memory.begin(), memory.begin() + cal_first, same) &&
           std::all_of(memory.begin() + cal_end, memory.end(), same);
}

bool persistent_calibration() {
    struct IoTestScenario {};
    using IoTest = PersistenceFixture<IoTestScenario>;
    IoTest::reset();
    auto &io = IoTest::registers;
    std::fill(io.memory.begin(), io.memory.begin() + cal_first, 0xA6);
    std::fill(io.memory.begin() + cal_end, io.memory.end(), 0xA6);
    using Probe = IoTest::Probe;
    hw::Diagnostics diagnostics{};
    IoTest::Storage storage{};
    Probe::initialize();
    storage.handle(hw::Key::ok, diagnostics);
    if ((diagnostics.phase() != hw::CalibrationPhase::idle) ||
        !run_persistence<Probe>(io, storage, diagnostics) ||
        (storage.status() != hw::SettingsStatus::empty) || diagnostics.calibrated() ||
        (io.payload_writes != 0)) {
        return false;
    }
    // Cancelled/invalid drafts must not trigger EEPROM writes.
    diagnostics.observe(stock_samples(), coretx::TimestampUs{0});
    diagnostics.begin_calibration();
    storage.handle(hw::Key::ok, diagnostics);
    storage.handle(hw::Key::ok, diagnostics);
    if (storage.busy() || diagnostics.calibrated()) {
        return false;
    }
    storage.handle(hw::Key::cancel, diagnostics);
    apply_calibration(diagnostics, storage);
    if (!storage.busy() || !diagnostics.calibrated() ||
        !run_persistence<Probe>(io, storage, diagnostics) ||
        (storage.status() != hw::SettingsStatus::saved) || (storage.generation() != 0) ||
        io.page_crossed || (io.payload_writes != (cal::record_size + 1U)) ||
        (std::string_view{storage.label(true)} != coretx::ui::tr::ok_cal)) {
        return false;
    }
    struct RebootTestScenario {};
    using RebootTest = PersistenceFixture<RebootTestScenario>;
    RebootTest::reset();
    auto &reboot = RebootTest::registers;
    reboot.memory = io.memory;
    using NextProbe = RebootTest::Probe;
    RebootTest::Storage next_storage{};
    hw::Diagnostics restored{};
    NextProbe::initialize();
    if (!run_persistence<NextProbe>(reboot, next_storage, restored) ||
        (next_storage.status() != hw::SettingsStatus::ready) || !restored.calibrated() ||
        (reboot.payload_writes != 0) ||
        (restored.setup().calibration != diagnostics.setup().calibration)) {
        return false;
    }
    restored.observe(stock_samples(400), coretx::TimestampUs{0});
    if (!restored.output_valid() || (restored.evaluation().frame.channels[0] != 1024)) {
        return false;
    }
    apply_calibration(restored, next_storage, 600);
    if (!run_persistence<NextProbe>(reboot, next_storage, restored) ||
        (next_storage.status() != hw::SettingsStatus::saved) || (next_storage.generation() != 1) ||
        !std::equal(io.memory.begin() + cal_first, io.memory.begin() + cal_second,
                    reboot.memory.begin() + cal_first)) {
        return false;
    }
    cal::Record second{};
    std::copy_n(reboot.memory.begin() + cal_second, second.size(), second.begin());
    const auto decoded = cal::decode(second);
    return decoded.valid() && (decoded.generation == 1) &&
           (decoded.setup.calibration[0].minimum == 600) &&
           outside_calibration(reboot.memory, 0xA6);
}

bool persistence_failures() {
    for (const auto responders : std::array<std::uint8_t, 2>{0, 3}) {
        struct IoTestScenario {};
        using IoTest = PersistenceFixture<IoTestScenario>;
        IoTest::reset();
        auto &io = IoTest::registers;
        io.responders = responders;
        using Probe = IoTest::Probe;
        IoTest::Storage storage{};
        hw::Diagnostics diagnostics{};
        Probe::initialize();
        if (!run_persistence<Probe>(io, storage, diagnostics) ||
            (storage.status() !=
             (responders == 0 ? hw::SettingsStatus::missing : hw::SettingsStatus::multiple))) {
            return false;
        }
        apply_calibration(diagnostics, storage);
        if (!diagnostics.calibrated() || storage.busy() || (io.payload_writes != 0) ||
            (std::string_view{storage.label(true)} != coretx::ui::tr::ram_cal)) {
            return false;
        }
    }
    for (const bool protect : {false, true}) {
        struct IoTestScenario {};
        using IoTest = PersistenceFixture<IoTestScenario>;
        IoTest::reset();
        auto &io = IoTest::registers;
        using Probe = IoTest::Probe;
        IoTest::Storage storage{};
        hw::Diagnostics diagnostics{};
        Probe::initialize();
        if (!run_persistence<Probe>(io, storage, diagnostics)) {
            return false;
        }
        io.write_protected = protect;
        io.never_ready = !protect;
        apply_calibration(diagnostics, storage);
        if (!run_persistence<Probe>(io, storage, diagnostics) ||
            (storage.status() !=
             (protect ? hw::SettingsStatus::verify : hw::SettingsStatus::io_error)) ||
            (!protect && (IoTest::Memory::error() != hw::I2cError::timeout)) ||
            (std::string_view{storage.label(true)} != coretx::ui::tr::ram_cal)) {
            return false;
        }
        const auto writes = io.payload_writes;
        apply_calibration(diagnostics, storage, 600);
        if ((io.payload_writes != writes) || storage.busy()) {
            return false;
        }
    }
    return true;
}

bool persistence_recovery() {
    hw::Diagnostics calibrated{};
    calibrate(calibrated);
    auto setup = calibrated.setup();
    const auto first = cal::encode(setup, 7);
    setup.calibration[0].minimum = 600;
    auto second = cal::encode(setup, 8);
    for (const auto cut : std::array<std::size_t, 7>{0, 1, 2, 32, 63, 64, 65}) {
        struct IoTestScenario {};
        using IoTest = PersistenceFixture<IoTestScenario>;
        IoTest::reset();
        auto &io = IoTest::registers;
        std::copy(first.begin(), first.end(), io.memory.begin() + cal_first);
        std::copy(first.begin(), first.end(), io.memory.begin() + cal_second);
        using Probe = IoTest::Probe;
        IoTest::Storage storage{};
        hw::Diagnostics diagnostics{};
        Probe::initialize();
        if (!run_persistence<Probe>(io, storage, diagnostics) || !diagnostics.calibrated()) {
            return false;
        }
        apply_calibration(diagnostics, storage, 600);
        for (std::size_t step = 0; (io.payload_writes < cut) && (step < 30'000); ++step) {
            io.cell(reg::tim2_cnt) += 100;
            storage.service(coretx::TimestampUs{io.now_us()}, Probe::result(), diagnostics);
        }
        struct RebootTestScenario {};
        using RebootTest = PersistenceFixture<RebootTestScenario>;
        RebootTest::reset();
        auto &reboot = RebootTest::registers;
        reboot.memory = io.memory;
        using NextProbe = RebootTest::Probe;
        RebootTest::Storage next_storage{};
        hw::Diagnostics restored{};
        NextProbe::initialize();
        if (!run_persistence<NextProbe>(reboot, next_storage, restored) ||
            (next_storage.status() != hw::SettingsStatus::ready) || !restored.calibrated() ||
            ((restored.setup().calibration[0].minimum != 400) &&
             (restored.setup().calibration[0].minimum != 600))) {
            return false;
        }
    }
    // Corrupt the newer slot; the intact older one must still load.
    second[20] ^= 1U;
    struct IoTestScenario {};
    using IoTest = PersistenceFixture<IoTestScenario>;
    IoTest::reset();
    auto &io = IoTest::registers;
    std::copy(first.begin(), first.end(), io.memory.begin() + cal_first);
    std::copy(second.begin(), second.end(), io.memory.begin() + cal_second);
    using Probe = IoTest::Probe;
    IoTest::Storage storage{};
    hw::Diagnostics restored{};
    Probe::initialize();
    if (!run_persistence<Probe>(io, storage, restored) ||
        (storage.status() != hw::SettingsStatus::ready) || (storage.generation() != 7) ||
        (restored.setup().calibration[0].minimum != 400)) {
        return false;
    }
    // Reject a CRC-correct but physically impossible stock calibration without writing.
    setup.calibration[0].maximum = 6000;
    const auto impossible = cal::encode(setup, 9);
    struct InvalidIoTestScenario {};
    using InvalidIoTest = PersistenceFixture<InvalidIoTestScenario>;
    InvalidIoTest::reset();
    auto &invalid_io = InvalidIoTest::registers;
    std::copy(impossible.begin(), impossible.end(), invalid_io.memory.begin() + cal_first);
    using InvalidProbe = InvalidIoTest::Probe;
    InvalidIoTest::Storage invalid_storage{};
    hw::Diagnostics invalid_diagnostics{};
    InvalidProbe::initialize();
    if (!run_persistence<InvalidProbe>(invalid_io, invalid_storage, invalid_diagnostics) ||
        (invalid_storage.status() != hw::SettingsStatus::invalid) ||
        invalid_diagnostics.calibrated() || (invalid_io.payload_writes != 0))
        return false;
    // A new calibration replaces the rejected record in the other slot.
    apply_calibration(invalid_diagnostics, invalid_storage, 600);
    if (!run_persistence<InvalidProbe>(invalid_io, invalid_storage, invalid_diagnostics) ||
        (invalid_storage.status() != hw::SettingsStatus::saved) ||
        (invalid_storage.generation() != 10))
        return false;
    struct ReplacedScenario {};
    using Replaced = PersistenceFixture<ReplacedScenario>;
    Replaced::reset();
    Replaced::registers.memory = invalid_io.memory;
    Replaced::Storage replaced{};
    hw::Diagnostics replaced_diagnostics{};
    Replaced::Probe::initialize();
    return run_persistence<Replaced::Probe>(Replaced::registers, replaced, replaced_diagnostics) &&
           (replaced.status() == hw::SettingsStatus::ready) && replaced_diagnostics.calibrated() &&
           (replaced_diagnostics.setup().calibration[0].minimum == 600);
}

bool persistence_slot_selection() {
    hw::Diagnostics calibrated{};
    calibrate(calibrated);
    auto older_setup = calibrated.setup();
    older_setup.calibration[0].minimum = 600;
    const auto first = cal::encode(calibrated.setup(), 9);
    auto second = cal::encode(older_setup, 8);
    // Slot 1 is decoded last, so a winning slot 0 must be read again into the shared buffer.
    struct NewerFirstScenario {};
    using NewerFirst = PersistenceFixture<NewerFirstScenario>;
    NewerFirst::reset();
    std::copy(first.begin(), first.end(), NewerFirst::registers.memory.begin() + cal_first);
    std::copy(second.begin(), second.end(), NewerFirst::registers.memory.begin() + cal_second);
    NewerFirst::Storage storage{};
    hw::Diagnostics restored{};
    NewerFirst::Probe::initialize();
    if (!run_persistence<NewerFirst::Probe>(NewerFirst::registers, storage, restored) ||
        (storage.status() != hw::SettingsStatus::ready) || (storage.generation() != 9) ||
        (restored.setup().calibration != calibrated.setup().calibration)) {
        return false;
    }
    // Equal generations with different contents stay ambiguous and are never overwritten.
    second = cal::encode(older_setup, 9);
    struct AmbiguousScenario {};
    using Ambiguous = PersistenceFixture<AmbiguousScenario>;
    Ambiguous::reset();
    std::copy(first.begin(), first.end(), Ambiguous::registers.memory.begin() + cal_first);
    std::copy(second.begin(), second.end(), Ambiguous::registers.memory.begin() + cal_second);
    Ambiguous::Storage ambiguous{};
    hw::Diagnostics untouched{};
    Ambiguous::Probe::initialize();
    if (!run_persistence<Ambiguous::Probe>(Ambiguous::registers, ambiguous, untouched) ||
        (ambiguous.status() != hw::SettingsStatus::invalid) || untouched.calibrated())
        return false;
    apply_calibration(untouched, ambiguous);
    return untouched.calibrated() && !ambiguous.busy() &&
           (Ambiguous::registers.payload_writes == 0);
}

bool eeprom_transactions() {
    struct TestScenario {};
    using Test = coretx::test::RegisterFixture<TestScenario, EepromRegisters>;
    Test::reset_registers();
    auto &io = Test::registers;
    using Memory = hw::Eeprom<Test::Io, coretx::firmware::Hardware>;
    Memory::select(0x50);
    std::array<std::uint8_t, 4> bytes{1, 2, 3, 4};
    if (Memory::write(63, bytes) || (Memory::error() != hw::I2cError::invalid_request) ||
        Memory::read(16'383, bytes) || Memory::read(0, {}) || !Memory::write(0x1234, bytes)) {
        return false;
    }
    std::uint32_t now = 0xFFFFF000U;
    for (std::size_t step = 0; Memory::busy() && (step < 1000); ++step) {
        now += 100;
        Memory::service(coretx::TimestampUs{now});
    }
    if (Memory::busy() || (Memory::error() != hw::I2cError::none) || io.page_crossed ||
        !std::equal(bytes.begin(), bytes.end(), io.memory.begin() + 0x1234)) {
        return false;
    }
    bytes.fill(0);
    if (!Memory::read(0x1234, bytes)) {
        return false;
    }
    for (std::size_t step = 0; Memory::busy() && (step < 1000); ++step) {
        now += 100;
        Memory::service(coretx::TimestampUs{now});
    }
    if (Memory::busy() || (Memory::error() != hw::I2cError::none) ||
        (bytes != std::array<std::uint8_t, 4>{1, 2, 3, 4})) {
        return false;
    }
    io.responders = 0;
    if (!Memory::read(0, bytes)) {
        return false;
    }
    Memory::service(coretx::TimestampUs{++now});
    Memory::service(coretx::TimestampUs{++now});
    if (Memory::busy() || (Memory::error() != hw::I2cError::nack)) {
        return false;
    }
    // A transfer that never produces its next byte must fail, including across timer wrap.
    struct StalledTestScenario {};
    using StalledTest = coretx::test::RegisterFixture<StalledTestScenario, EepromRegisters>;
    StalledTest::reset_registers();
    auto &stalled = StalledTest::registers;
    stalled.stalled = true;
    using Waiting = hw::Eeprom<StalledTest::Io, coretx::firmware::Hardware>;
    Waiting::select(0x50);
    if (!Waiting::read(0x1234, bytes)) {
        return false;
    }
    Waiting::service(coretx::TimestampUs{0xFFFFF000U});
    Waiting::service(coretx::TimestampUs{0xFFFFF000U + StorageProbe::timeout_us.count()});
    const auto failure = Waiting::trace();
    return !Waiting::busy() && (Waiting::error() == hw::I2cError::timeout) &&
           (failure.address == 0x1234) && (failure.position == 0) && (failure.stage == 2) &&
           (failure.status == I2C_ISR_BUSY) &&
           (stalled.cell(chip::I2cTraits<hw::Hardware::storage.peripheral>::isr) == 0);
}

bool persistence_format_protection() {
    hw::Diagnostics calibrated{};
    calibrate(calibrated);
    auto record = cal::encode(calibrated.setup(), 3);
    record[3] = '2';
    coretx::storage::detail::Writer checksum{std::span{record}.last(4)};
    checksum.word(coretx::storage::crc32(std::span{record}.first(cal::record_size - 4)));
    struct IoTestScenario {};
    using IoTest = PersistenceFixture<IoTestScenario>;
    IoTest::reset();
    auto &io = IoTest::registers;
    std::copy(record.begin(), record.end(), io.memory.begin() + cal_first);
    using Probe = IoTest::Probe;
    IoTest::Storage storage{};
    hw::Diagnostics diagnostics{};
    Probe::initialize();
    if (!run_persistence<Probe>(io, storage, diagnostics) ||
        (storage.status() != hw::SettingsStatus::version) || diagnostics.calibrated()) {
        return false;
    }
    apply_calibration(diagnostics, storage);
    if (!diagnostics.calibrated() || (io.payload_writes != 0) || storage.busy() ||
        (storage.status() != hw::SettingsStatus::version)) {
        return false;
    }
    // Unrecognized data, e.g. the former 1 KiB calibration record, is replaced only by an
    // explicit, successfully completed calibration.
    struct FactoryTestScenario {};
    using FactoryTest = PersistenceFixture<FactoryTestScenario>;
    FactoryTest::reset();
    auto &factory = FactoryTest::registers;
    factory.memory.fill(0x5A);
    using FactoryProbe = FactoryTest::Probe;
    FactoryTest::Storage factory_storage{};
    hw::Diagnostics factory_diagnostics{};
    FactoryProbe::initialize();
    if (!run_persistence<FactoryProbe>(factory, factory_storage, factory_diagnostics) ||
        (factory_storage.status() != hw::SettingsStatus::invalid) ||
        (factory.payload_writes != 0)) {
        return false;
    }
    apply_calibration(factory_diagnostics, factory_storage);
    if (!run_persistence<FactoryProbe>(factory, factory_storage, factory_diagnostics) ||
        (factory_storage.status() != hw::SettingsStatus::saved) ||
        !outside_calibration(factory.memory, 0x5A))
        return false;
    struct RecreatedTestScenario {};
    using RecreatedTest = PersistenceFixture<RecreatedTestScenario>;
    RecreatedTest::reset();
    auto &recreated = RecreatedTest::registers;
    recreated.memory = factory.memory;
    RecreatedTest::Storage recreated_storage{};
    hw::Diagnostics recreated_diagnostics{};
    RecreatedTest::Probe::initialize();
    return run_persistence<RecreatedTest::Probe>(recreated, recreated_storage,
                                                 recreated_diagnostics) &&
           (recreated_storage.status() == hw::SettingsStatus::ready) &&
           recreated_diagnostics.calibrated() && (recreated.payload_writes == 0);
}

bool cooperative_persistence() {
    struct CooperativeKeys {};
    using Test = PersistenceFixture<CooperativeKeys>;
    using Keys = hw::i6x::Keys<Test::Io>;
    Test::reset();
    using IoTest = Test;
    auto &io = Test::registers;
    using Probe = IoTest::Probe;
    IoTest::Storage storage{};
    hw::Diagnostics diagnostics{};
    using Adc = hw::Adc<Test::Io, coretx::firmware::Hardware, hw::stock_inputs>;
    using Display = coretx::firmware::Lcd<Test::Io, coretx::firmware::Hardware>;
    coretx::ui::Framebuffer display{};
    if ((Adc::initialize(4) != hw::AdcError::none) || (Display::initialize() != LcdError::none)) {
        return false;
    }
    Keys::initialize();
    Probe::initialize();
    Display::begin_frame();
    bool applied{};
    std::size_t scans{};
    for (std::size_t step = 0; step < 30'000; ++step) {
        io.cell(reg::tim2_cnt) += 100;
        const auto now = io.now_us();
        Probe::service(coretx::TimestampUs{now});
        storage.service(coretx::TimestampUs{now}, Probe::result(), diagnostics);
        const auto event = Adc::service(coretx::TimestampUs{now});
        if ((event == hw::ScanEvent::failed) || (Display::service(display) != LcdError::none)) {
            return false;
        }
        if (event == hw::ScanEvent::complete) {
            ++scans;
        }
        (void)Keys::service(coretx::TimestampUs{now});
        if (!storage.busy() && !applied) {
            apply_calibration(diagnostics, storage);
            applied = true;
        }
        if (applied && !storage.busy() && !Display::busy()) {
            return (storage.status() == hw::SettingsStatus::saved) && (scans > 10) &&
                   Adc::valid() && !io.lcd_timing_error && !io.page_crossed;
        }
    }
    return false;
}

static_assert(calibration_and_pipeline());
static_assert(calibration_transaction());
static_assert(calibration_rejects_jitter());
static_assert(stock_switches());
static_assert(stock_dashboard());
static_assert(dashboard_values());
static_assert(system_info_rendering());

} // namespace

int main() {
    struct Check {
        const char *name;
        bool (*run)();
    };
    const std::array checks{
        Check{"adc_scans", adc_scans},
        Check{"isolated_adc_contexts", isolated_adc_contexts},
        Check{"adc_failures", adc_failures<>},
        Check{"adc_failures<false>", adc_failures<false>},
        Check{"single_buffer_scans", single_buffer_scans},
        Check{"delayed_adc_service<false>", delayed_adc_service<false>},
        Check{"delayed_adc_service<true>", delayed_adc_service<true>},
        Check{"dma_autonomous_scans", dma_autonomous_scans},
        Check{"dma_incomplete_sequence", dma_incomplete_sequence},
        Check{"dma_latest_half_and_recovery", dma_latest_half_and_recovery},
        Check{"expandable_scan", expandable_scan},
        Check{"dashboard_values", dashboard_values},
        Check{"stock_scan", stock_scan},
        Check{"sender_battery_scan", sender_battery_scan},
        Check{"calibration_and_pipeline", calibration_and_pipeline},
        Check{"calibration_deferred_pipeline", calibration_deferred_pipeline},
        Check{"editing_deferred_pipeline", editing_deferred_pipeline},
        Check{"calibration_transaction", calibration_transaction},
        Check{"calibration_pages_include_configured_pots",
              calibration_pages_include_configured_pots},
        Check{"calibration_rejects_jitter", calibration_rejects_jitter},
        Check{"stock_switches", stock_switches},
        Check{"stock_keys", stock_keys},
        Check{"stock_dashboard", stock_dashboard},
        Check{"overview_and_latch_lifecycle", overview_and_latch_lifecycle},
        Check{"extended_physical_footer", extended_physical_footer},
        Check{"adc_deferred_enable", adc_deferred_enable},
        Check{"bootloader_message", bootloader_message},
        Check{"lcd_irregular_timing", lcd_irregular_timing},
        Check{"lcd_bounded_transfer", lcd_bounded_transfer},
        Check{"lcd_bus_and_board", lcd_bus_and_board},
        Check{"cooperative_adc_lcd", cooperative_adc_lcd},
        Check{"storage_detection", storage_detection},
        Check{"storage_errors", storage_errors},
        Check{"eeprom_transactions", eeprom_transactions},
        Check{"persistent_calibration", persistent_calibration},
        Check{"persistence_failures", persistence_failures},
        Check{"persistence_recovery", persistence_recovery},
        Check{"persistence_slot_selection", persistence_slot_selection},
        Check{"persistence_format_protection", persistence_format_protection},
        Check{"cooperative_persistence", cooperative_persistence},
        Check{"system_info_rendering", system_info_rendering},
    };
    for (const auto &check : checks) {
        if (!check.run()) {
            std::fprintf(stderr, "FAIL: i6X ADC/dashboard/LCD/I2C: %s\n", check.name);
            return EXIT_FAILURE;
        }
    }
    std::puts("i6X ADC/dashboard/LCD/I2C checks passed");
}
