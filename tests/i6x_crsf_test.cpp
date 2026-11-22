// SPDX-License-Identifier: GPL-2.0-or-later
#include "boards/common/dashboard.hpp"
#include "platform/stm32f072/crsf.hpp"
#include "static_fixture.hpp"
#include "tests/renderers/crsf.hpp"
#include "tests/renderers/telemetry.hpp"

#include <cstdio>
#include <cstdlib>

namespace {

namespace hw = coretx::firmware::i6x;
using reg = hw::crsf_registers;
struct MetadataIo;
using Limits = hw::detail::CrsfState<MetadataIo>;
constexpr auto power_pin = coretx::firmware::external_module_power;
constexpr bool power_on_level = coretx::firmware::external_module_active_high;

struct Registers {
    struct Cell {
        std::uintptr_t address{};
        std::uint32_t value{};
    };
    std::array<Cell, 64> cells{};
    std::size_t count{};
    std::uint8_t *transmit{};
    std::uint8_t *receive{};
    std::uint32_t transfers{};
    std::uint32_t clock{};
    std::uint32_t clock_step{};
    std::size_t receive_length{};
    std::size_t receive_reads{};
    std::size_t inject_after_read{};
    std::size_t inject_bytes{};
    std::uint32_t copy_delay{};
    bool copy_error{};
    bool copy_uart_error{};
    bool acknowledge{true};
    bool gate_glitch{};

    constexpr void barrier() noexcept {}
    constexpr std::uint32_t now_us() noexcept {
        const auto now = clock;
        clock += clock_step;
        return now;
    }

    constexpr std::uint8_t read8(const std::uint8_t *const address) {
        if (++receive_reads == inject_after_read) {
            for (std::size_t i = 0; i < inject_bytes; ++i) {
                feed(0x55);
            }
            clock += copy_delay;
            if (copy_error) {
                cell(reg::dma_isr) |= reg::rx_error;
            }
            if (copy_uart_error) {
                cell(reg::isr) |= USART_ISR_FE;
            }
        }
        return *address;
    }

    constexpr void feed(const std::uint8_t byte) {
        if ((receive == nullptr) || ((cell(reg::rx_ccr) & DMA_CCR_EN) == 0)) {
            std::abort();
        }
        auto &remaining = cell(reg::rx_cndtr);
        receive[receive_length - remaining] = byte;
        --remaining;
        if (remaining == (receive_length / 2U)) {
            cell(reg::dma_isr) |= reg::rx_half;
        }
        if (remaining == 0) {
            cell(reg::dma_isr) |= reg::rx_complete;
            remaining = static_cast<std::uint32_t>(receive_length);
        }
    }

    constexpr void feed(const std::span<const std::uint8_t> bytes) {
        for (const auto byte : bytes) {
            feed(byte);
        }
    }

    constexpr std::uint32_t &cell(const std::uintptr_t address) {
        for (std::size_t i = 0; i < count; ++i) {
            if (cells[i].address == address) {
                return cells[i].value;
            }
        }
        if (count == cells.size()) {
            std::abort();
        }
        cells[count].address = address;
        return cells[count++].value;
    }

    constexpr std::uint32_t read(const std::uintptr_t address) { return cell(address); }

    constexpr void write(const std::uintptr_t address, const std::uint32_t value) {
        cell(address) = value;
        if (address == reg::rx_cndtr) {
            receive_length = value;
        } else if (address == (power_pin.port + offsetof(GPIO_TypeDef, BSRR))) {
            auto &output = cell(power_pin.port + offsetof(GPIO_TypeDef, ODR));
            output = (output | (value & 0xFFFFU)) & ~(value >> 16);
        } else if (address == (power_pin.port + offsetof(GPIO_TypeDef, MODER))) {
            if (((cell(power_pin.port + offsetof(GPIO_TypeDef, ODR)) & (1U << power_pin.index)) !=
                 0) == power_on_level) {
                gate_glitch = true;
            }
        } else if (address == reg::cr1) {
            cell(reg::isr) &= ~std::uint32_t{USART_ISR_TEACK | USART_ISR_REACK};
            if (acknowledge && ((value & USART_CR1_UE) != 0)) {
                cell(reg::isr) |= USART_ISR_TEACK | USART_ISR_REACK;
            }
        } else if (address == reg::icr) {
            cell(reg::isr) &= ~value;
        } else if (address == reg::dma_ifcr) {
            cell(reg::dma_isr) &= ~(value & reg::clear_rx_boundaries);
            if ((value & reg::clear_rx) != 0) {
                cell(reg::dma_isr) &= ~(reg::clear_rx * 15U);
            }
            if ((value & reg::clear_tx) != 0) {
                cell(reg::dma_isr) &= ~(reg::clear_tx * 15U);
            }
        } else if ((address == reg::tx_ccr) && ((value & DMA_CCR_EN) != 0)) {
            ++transfers;
        }
    }

    constexpr void write_address(const std::uintptr_t address, std::uint8_t *const pointer) {
        if (address == reg::tx_cmar) {
            transmit = pointer;
        } else if (address == reg::rx_cmar) {
            receive = pointer;
        } else {
            std::abort();
        }
    }

    constexpr void finish_dma() {
        cell(reg::tx_cndtr) = 0;
        cell(reg::dma_isr) |= reg::tx_complete;
    }
    constexpr void finish_wire() { cell(reg::isr) |= USART_ISR_TC; }
};

inline constexpr std::array<std::uint8_t, 14> link_frame{0xEA, 0x0C, 0x14, 0xB5, 0xAE, 0x61, 0xF8,
                                                         0x01, 0x02, 0x03, 0x5A, 0x58, 0x04, 0x9C};

constexpr void push_frame(coretx::crsf::Telemetry &telemetry,
                          const std::span<const std::uint8_t> bytes, const std::uint32_t now) {
    for (const auto byte : bytes) {
        telemetry.push(byte, coretx::TimestampUs{now});
    }
}

constexpr bool telemetry_protocol() {
    for (const std::uint32_t start : {0U, 0xFFFFF000U}) {
        coretx::crsf::Telemetry telemetry{};
        if (telemetry.received() || telemetry.fresh(coretx::TimestampUs{start})) {
            return false;
        }
        push_frame(telemetry, std::span{link_frame}.first(6), start);
        if (telemetry.received()) {
            return false;
        }
        push_frame(telemetry, std::span{link_frame}.subspan(6), start + 5'000U);
        const auto &link = telemetry.link();
        if (!telemetry.fresh(coretx::TimestampUs{start + 5'000U}) || (link.rssi1_dbm != -75) ||
            (link.rssi2_dbm != -82) || (link.quality != 97) || (link.snr_db != -8)) {
            return false;
        }
        auto other = link_frame;
        other[2] = 0x02;
        other.back() = coretx::crsf::crc8(std::span{other}.subspan(2, 11));
        push_frame(telemetry, other, start + 1'000'000U);
        if ((telemetry.frames() != 2) ||
            !telemetry.fresh(coretx::TimestampUs{start + 2'004'999U}) ||
            telemetry.fresh(coretx::TimestampUs{start + 2'005'000U})) {
            return false;
        }
        telemetry.service(coretx::TimestampUs{start + 2'005'000U});
        // Expired data must stay stale even after a complete timer wrap.
        if (telemetry.fresh(coretx::TimestampUs{start + 5'000U})) {
            return false;
        }
        auto reported = link_frame;
        reported[3] = 0xDD;
        reported[4] = 0xDD;
        reported.back() = coretx::crsf::crc8(std::span{reported}.subspan(2, 11));
        push_frame(telemetry, reported, start + 2'500'000U);
        if ((telemetry.link().rssi1_dbm != -35) || (telemetry.link().rssi2_dbm != -35)) {
            return false;
        }
        auto extended = link_frame;
        extended[0] = 0xC8;
        extended[3] = 0x80;
        extended[4] = 255;
        extended[5] = 0;
        extended[6] = 0x80;
        extended.back() = coretx::crsf::crc8(std::span{extended}.subspan(2, 11));
        push_frame(telemetry, extended, start + 3'000'000U);
        if (!telemetry.fresh(coretx::TimestampUs{start + 3'000'000U}) ||
            (telemetry.link().rssi1_dbm != -128) || (telemetry.link().rssi2_dbm != -1) ||
            (telemetry.link().snr_db != -128) || (telemetry.link().quality != 0)) {
            return false;
        }
        telemetry.invalidate();
        if (!telemetry.received() || telemetry.fresh(coretx::TimestampUs{start + 3'000'001U})) {
            return false;
        }
    }
    return true;
}

constexpr bool telemetry_resynchronization() {
    coretx::crsf::Telemetry telemetry{};
    const std::array<std::uint8_t, 7> noise{0x55, 0xEA, 0, 0xEA, 63, 0xEE, 255};
    push_frame(telemetry, noise, 0);
    auto corrupt = link_frame;
    corrupt.back() ^= 1U;
    push_frame(telemetry, corrupt, 1);
    push_frame(telemetry, link_frame, 2);
    if (!telemetry.fresh(coretx::TimestampUs{2}) || (telemetry.crc_errors() != 1) ||
        (telemetry.framing_errors() != 3) || (telemetry.frames() != 1)) {
        return false;
    }
    auto invalid = link_frame;
    invalid[5] = 101;
    invalid.back() = coretx::crsf::crc8(std::span{invalid}.subspan(2, 11));
    push_frame(telemetry, invalid, 1'000'000);
    if ((telemetry.link().quality != 97) || telemetry.fresh(coretx::TimestampUs{2'000'002})) {
        return false;
    }
    coretx::crsf::Telemetry truncated{};
    const std::array<std::uint8_t, 3> prefix{0xEA, 62, 0x02};
    push_frame(truncated, prefix, 0);
    push_frame(truncated, link_frame, 1);
    if (truncated.received()) {
        return false;
    }
    truncated.service(coretx::TimestampUs{3'000'000});
    if (!truncated.received() || truncated.fresh(coretx::TimestampUs{3'000'000})) {
        return false;
    }
    coretx::crsf::Telemetry maximum{};
    std::array<std::uint8_t, 64> full{};
    full.fill(0x55);
    full[0] = 0xEA;
    full[1] = 62;
    full[2] = 0x02;
    for (std::size_t i = 0; i < link_frame.size(); ++i) {
        full[10 + i] = link_frame[i];
    }
    full.back() = coretx::crsf::crc8(std::span{full}.subspan(2, 61));
    push_frame(maximum, full, 0);
    if (maximum.received() || (maximum.frames() != 1)) {
        return false;
    }
    push_frame(maximum, std::span{link_frame}.first(13), 1);
    push_frame(maximum, link_frame, 2);
    if (!maximum.fresh(coretx::TimestampUs{2}) || (maximum.crc_errors() == 0)) {
        return false;
    }
    coretx::crsf::Telemetry short_link{};
    const std::array<std::uint8_t, 4> empty{0xEA, 2, 0x14, coretx::crsf::crc8_byte(0, 0x14)};
    push_frame(short_link, empty, 0);
    return !short_link.received() && (short_link.framing_errors() == 1);
}

bool dma_reception() {
    struct Scenario {};
    using Test = coretx::test::RegisterFixture<Scenario, Registers>;
    using Driver = hw::Crsf<Test::Io>;

    Test::reset_registers();
    auto &io = Test::registers;

    coretx::crsf::Channels channels{};
    Driver::initialize(coretx::TimestampUs{0});
    (void)Driver::service(coretx::TimestampUs{0}, channels, true, coretx::TimestampUs{0});
    io.feed(std::span{link_frame}.first(6));
    (void)Driver::service(coretx::TimestampUs{1}, channels, true, coretx::TimestampUs{0});
    if (Driver::telemetry().received()) {
        return false;
    }
    io.feed(std::span{link_frame}.subspan(6));
    (void)Driver::service(coretx::TimestampUs{2}, channels, true, coretx::TimestampUs{0});
    if (!Driver::busy() || !Driver::telemetry().fresh(coretx::TimestampUs{2}) ||
        (Driver::telemetry().frames() != 1)) {
        return false;
    }
    for (std::uint32_t i = 0; i < 236; ++i) {
        io.feed(0x55);
        if ((i % 60U) == 59U) {
            (void)Driver::service(coretx::TimestampUs{3U + i}, channels, false,
                                  coretx::TimestampUs{0});
        }
    }
    (void)Driver::service(coretx::TimestampUs{250}, channels, false, coretx::TimestampUs{0});
    io.feed(link_frame);
    (void)Driver::service(coretx::TimestampUs{251}, channels, false, coretx::TimestampUs{0});
    if ((Driver::telemetry().frames() != 2) || (Driver::receive_overruns() != 0)) {
        return false;
    }
    for (unsigned i = 0; i < 100; ++i) {
        io.feed(0x55);
    }
    const auto reads = io.receive_reads;
    (void)Driver::service(coretx::TimestampUs{252}, channels, false, coretx::TimestampUs{0});
    if ((io.receive_reads - reads) != 64) {
        return false;
    }
    (void)Driver::service(coretx::TimestampUs{253}, channels, false, coretx::TimestampUs{0});
    if (((io.receive_reads - reads) != 100) || (Driver::error() != hw::CrsfError::none)) {
        return false;
    }
    io.finish_dma();
    io.finish_wire();
    (void)Driver::service(coretx::TimestampUs{300}, channels, false, coretx::TimestampUs{0});
    Driver::next_baud(coretx::TimestampUs{301});
    return !Driver::telemetry().fresh(coretx::TimestampUs{301}) && Driver::powered() &&
           (Driver::frames() == 1);
}

bool dma_receive_recovery() {
    struct Scenario {};
    using Test = coretx::test::RegisterFixture<Scenario, Registers>;
    using Driver = hw::Crsf<Test::Io>;

    coretx::crsf::Channels channels{};
    for (unsigned kind = 0; kind < 7; ++kind) {
        Test::reset_registers();
        auto &io = Test::registers;

        Driver::initialize(coretx::TimestampUs{0});
        (void)Driver::service(coretx::TimestampUs{0}, channels, false, coretx::TimestampUs{0});
        if (kind == 0) {
            for (std::size_t i = 0; i < Limits::receive_buffer_size; ++i) {
                io.feed(0x55);
            }
        } else if (kind == 1) {
            for (unsigned i = 0; i < 160; ++i) {
                io.feed(0x55);
            }
            (void)Driver::service(coretx::TimestampUs{1}, channels, false, coretx::TimestampUs{0});
            for (unsigned i = 0; i < 200; ++i) {
                io.feed(0x55);
            }
        } else {
            io.feed(link_frame);
            if ((kind == 2) || (kind == 3)) {
                io.inject_after_read = 1;
                io.inject_bytes = kind == 2 ? Limits::receive_buffer_size : 0U;
                io.copy_delay = kind == 3 ? 7'000U : 0U;
            }
            if (kind == 5) {
                io.cell(reg::isr) |= USART_ISR_ORE;
            }
            if (kind == 6) {
                io.inject_after_read = 1;
                io.copy_uart_error = true;
            }
        }
        const std::uint32_t now = kind == 4 ? 3'000'000U : 2U;
        if ((Driver::service(coretx::TimestampUs{now}, channels, false, coretx::TimestampUs{0}) !=
             hw::CrsfError::none) ||
            Driver::telemetry().received() ||
            (Driver::receive_overruns() != (kind >= 5 ? 0U : 1U)) ||
            (Driver::receive_errors() != (kind >= 5 ? 1U : 0U))) {
            return false;
        }
        io.feed(link_frame);
        if ((Driver::service(coretx::TimestampUs{now + 1U}, channels, false,
                             coretx::TimestampUs{0}) != hw::CrsfError::none) ||
            !Driver::telemetry().fresh(coretx::TimestampUs{now + 1U})) {
            return false;
        }
    }
    for (unsigned kind = 0; kind < 3; ++kind) {
        Test::reset_registers();
        auto &io = Test::registers;

        Driver::initialize(coretx::TimestampUs{0});
        (void)Driver::service(coretx::TimestampUs{0}, channels, false, coretx::TimestampUs{0});
        io.feed(link_frame);
        if (kind == 0) {
            io.cell(reg::rx_cndtr) = static_cast<std::uint32_t>(Limits::receive_buffer_size + 1U);
        } else if (kind == 1) {
            io.cell(reg::dma_isr) |= reg::rx_error;
        } else {
            io.inject_after_read = 1;
            io.copy_error = true;
        }
        const auto expected = kind == 0 ? hw::CrsfError::sequence : hw::CrsfError::dma;
        if ((Driver::service(coretx::TimestampUs{1}, channels, false, coretx::TimestampUs{0}) !=
             expected) ||
            Driver::telemetry().received() || !Driver::powered()) {
            return false;
        }
    }
    return true;
}

constexpr bool protocol_frames() {
    const std::array<std::uint8_t, 9> text{'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    if (coretx::crsf::crc8(text) != 0xBC) {
        return false;
    }
    coretx::crsf::Channels channels{};
    for (std::size_t i = 0; i < channels.size(); ++i) {
        channels[i] = static_cast<std::int16_t>(-1024 + static_cast<int>(i) * 128);
    }
    // Independent bit-by-bit packing; payload also matches the EdgeTX reference vector.
    constexpr coretx::crsf::ChannelsFrame expected{
        0xEE, 0x18, 0x16, 0xAD, 0xA0, 0x88, 0x5E, 0xC0, 0x73, 0xA4, 0x56, 0x51, 0x4C,
        0x6F, 0xE0, 0x33, 0x22, 0x2B, 0x27, 0x9A, 0x57, 0xF0, 0x1A, 0x99, 0xD5, 0x5B};
    if (coretx::crsf::encode(channels) != expected) {
        return false;
    }
    constexpr std::array<std::int16_t, 8> inputs{-32768, -1536, -1024, -1, 0, 1, 1024, 32767};
    constexpr std::array<std::uint16_t, 8> values{0, 0, 173, 992, 992, 992, 1811, 1984};
    for (std::size_t test = 0; test < inputs.size(); ++test) {
        channels.fill(inputs[test]);
        const auto frame = coretx::crsf::encode(channels);
        for (std::size_t channel = 0; channel < channels.size(); ++channel) {
            std::uint32_t decoded{};
            for (std::size_t bit = 0; bit < 11; ++bit) {
                const auto position = channel * 11U + bit;
                decoded |= ((std::uint32_t{frame[3 + position / 8]} >> (position % 8)) & 1U) << bit;
            }
            if (decoded != values[test]) {
                return false;
            }
        }
    }
    return true;
}

bool wiring_and_baudrates() {
    struct Scenario {};
    using Test = coretx::test::RegisterFixture<Scenario, Registers>;
    using Driver = hw::Crsf<Test::Io>;

    Test::reset_registers();
    auto &io = Test::registers;

    coretx::crsf::Channels channels{};
    if (Driver::set_baud(2, coretx::TimestampUs{0}))
        return false;
    // Seed unrelated shared GPIO/DMA settings to catch destructive register writes.
    io.cell(GPIOC_BASE + offsetof(GPIO_TypeDef, MODER)) = 1U << 18;
    io.cell(GPIOD_BASE + offsetof(GPIO_TypeDef, MODER)) = 1U << 14;
    constexpr auto adc_and_touch_modes =
        0xFFFFU | (coretx::firmware::features::i6s ? 1U << 30 : 0U);
    io.cell(GPIOA_BASE + offsetof(GPIO_TypeDef, MODER)) = adc_and_touch_modes;
    io.cell(reg::dma_isr) = DMA_ISR_TCIF1;
    Driver::initialize(coretx::TimestampUs{0});
    if (Driver::set_baud(hw::crsf_baudrates.size(), coretx::TimestampUs{0}))
        return false;
    if (Driver::powered() || io.gate_glitch || (Driver::baudrate() != 400'000) ||
        ((io.cell(power_pin.port + offsetof(GPIO_TypeDef, MODER)) &
          (3U << (power_pin.index * 2))) != (1U << (power_pin.index * 2))) ||
        ((io.cell(GPIOC_BASE + offsetof(GPIO_TypeDef, MODER)) & (3U << 18)) != (1U << 18)) ||
        ((io.cell(GPIOD_BASE + offsetof(GPIO_TypeDef, MODER)) & (3U << 14)) != (1U << 14)) ||
        ((io.cell(GPIOA_BASE + offsetof(GPIO_TypeDef, MODER)) & 0xFFFFU) != 0xFFFFU) ||
        (io.cell(reg::tx_cpar) != reg::tdr) || (io.cell(reg::rx_cpar) != reg::rdr) ||
        (io.cell(reg::rx_ccr) != (DMA_CCR_MINC | DMA_CCR_CIRC | DMA_CCR_EN)) ||
        (io.cell(reg::rx_cndtr) != Limits::receive_buffer_size) || (io.receive == nullptr) ||
        (io.transmit == nullptr) || ((io.cell(reg::cr3) & USART_CR3_HDSEL) != 0)) {
        return false;
    }
    if constexpr (coretx::firmware::features::i6s) {
        if ((reg::tdr != (USART1_BASE + offsetof(USART_TypeDef, TDR))) ||
            (reg::rx_ccr != DMA1_Channel3_BASE) || (reg::tx_ccr != DMA1_Channel2_BASE) ||
            ((io.cell(GPIOB_BASE + offsetof(GPIO_TypeDef, MODER)) & 0xF000U) != 0xA000U) ||
            (io.cell(GPIOA_BASE + offsetof(GPIO_TypeDef, MODER)) != adc_and_touch_modes) ||
            ((io.cell(GPIOB_BASE + offsetof(GPIO_TypeDef, AFR)) & 0xFF000000U) != 0) ||
            ((io.cell(SYSCFG_BASE + offsetof(SYSCFG_TypeDef, CFGR1)) &
              (SYSCFG_CFGR1_USART1TX_DMA_RMP | SYSCFG_CFGR1_USART1RX_DMA_RMP)) != 0))
            return false;
    } else {
        if (((io.cell(GPIOD_BASE + offsetof(GPIO_TypeDef, MODER)) & (3U << 10)) != (2U << 10)) ||
            ((io.cell(GPIOA_BASE + offsetof(GPIO_TypeDef, MODER)) & (3U << 30)) != (2U << 30)) ||
            (((io.cell(GPIOA_BASE + offsetof(GPIO_TypeDef, AFR) + 4) >> 28) & 0xFU) != 1) ||
            (((io.cell(GPIOD_BASE + offsetof(GPIO_TypeDef, AFR)) >> 20) & 0xFU) != 0) ||
            ((io.cell(SYSCFG_BASE + offsetof(SYSCFG_TypeDef, CFGR1)) &
              SYSCFG_CFGR1_USART2_DMA_RMP) == 0))
            return false;
    }
    constexpr std::array<std::uint32_t, 8> brr{417, 120, 115, 114, 52, 26, 0x15, 0x11};
    constexpr std::array<std::uint32_t, 8> periods{16'000, 4'000, 4'000, 4'000,
                                                   2'000,  2'000, 2'000, 2'000};
    for (std::size_t step = 0; step < hw::crsf_baudrates.size(); ++step) {
        const auto index = (step + 1U) % hw::crsf_baudrates.size();
        const auto baud = hw::crsf_baudrates[index].rate;
        const auto now = 100U + static_cast<std::uint32_t>(step) * 20'000U;
        if ((Driver::service(coretx::TimestampUs{now}, channels, false, coretx::TimestampUs{now}) !=
             hw::CrsfError::none) ||
            (Driver::baudrate() != baud) || (io.cell(reg::brr) != brr[index]) ||
            (((io.cell(reg::cr1) & USART_CR1_OVER8) != 0) != (index >= 6)) || !Driver::powered() ||
            (io.transfers != (step * 2U)) || ((io.cell(reg::dma_isr) & DMA_ISR_TCIF1) == 0)) {
            return false;
        }
        const auto actual = Driver::actual_baudrate();
        const auto difference = actual > baud ? actual - baud : baud - actual;
        if ((difference * 100U) > (baud * 2U)) {
            return false;
        }
        const auto before = io.transfers;
        if ((Driver::service(coretx::TimestampUs{now}, channels, true, coretx::TimestampUs{now}) !=
             hw::CrsfError::none) ||
            (io.transfers != (before + 1U))) {
            return false;
        }
        io.finish_dma();
        io.finish_wire();
        const auto due = now + periods[index];
        if ((Driver::service(coretx::TimestampUs{due - 1U}, channels, true,
                             coretx::TimestampUs{due - 1U}) != hw::CrsfError::none) ||
            (io.transfers != (before + 1U)) ||
            (Driver::service(coretx::TimestampUs{due}, channels, true, coretx::TimestampUs{due}) !=
             hw::CrsfError::none) ||
            (io.transfers != (before + 2U))) {
            return false;
        }
        io.finish_dma();
        io.finish_wire();
        if (Driver::service(coretx::TimestampUs{due + 1U}, channels, false,
                            coretx::TimestampUs{due}) != hw::CrsfError::none) {
            return false;
        }
        Driver::next_baud(coretx::TimestampUs{due + 1U});
    }
    Driver::power_off();
    return !Driver::powered() && (io.cell(reg::tx_ccr) == 0) && (io.cell(reg::rx_ccr) == 0) &&
           (io.cell(reg::cr1) == 0) &&
           (((io.cell(power_pin.port + offsetof(GPIO_TypeDef, ODR)) & (1U << power_pin.index)) !=
             0) != power_on_level);
}

bool cooperative_transmission() {
    struct Scenario {};
    using Test = coretx::test::RegisterFixture<Scenario, Registers>;
    using Driver = hw::Crsf<Test::Io>;

    Test::reset_registers();
    auto &io = Test::registers;

    coretx::crsf::Channels channels{};
    constexpr std::uint32_t start = 0xFFFFF000U;
    Driver::initialize(coretx::TimestampUs{start});
    if ((Driver::service(coretx::TimestampUs{start}, channels, false, coretx::TimestampUs{start}) !=
         hw::CrsfError::none) ||
        !Driver::powered() || (io.transfers != 0)) {
        return false;
    }

    if ((Driver::service(coretx::TimestampUs{start}, channels, true, coretx::TimestampUs{start}) !=
         hw::CrsfError::none) ||
        !Driver::busy() || (io.transfers != 1) || (io.cell(reg::tx_cndtr) != 26)) {
        return false;
    }
    const auto expected = coretx::crsf::encode(channels);
    channels[0] = 1024;
    io.finish_dma();
    if ((Driver::service(coretx::TimestampUs{start + 100U}, channels, true,
                         coretx::TimestampUs{start}) != hw::CrsfError::none) ||
        !Driver::busy() || (Driver::frames() != 0) || (io.transfers != 1)) {
        return false;
    }
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (io.transmit[i] != expected[i]) {
            return false;
        }
    }
    io.finish_wire();
    if ((Driver::service(coretx::TimestampUs{start + 200U}, channels, true,
                         coretx::TimestampUs{start}) != hw::CrsfError::none) ||
        Driver::busy() || (Driver::frames() != 1) ||
        (Driver::service(coretx::TimestampUs{start + 3'999U}, channels, true,
                         coretx::TimestampUs{start}) != hw::CrsfError::none) ||
        (io.transfers != 1) ||
        (Driver::service(coretx::TimestampUs{start + 4'000U}, channels, true,
                         coretx::TimestampUs{start}) != hw::CrsfError::none) ||
        (io.transfers != 2)) {
        return false;
    }
    io.finish_dma();
    io.finish_wire();
    // Completion wins over a late poll, but stale controls never start another frame.
    if ((Driver::service(coretx::TimestampUs{start + 30'000U}, channels, true,
                         coretx::TimestampUs{start}) != hw::CrsfError::none) ||
        Driver::busy() || (Driver::frames() != 2) || (io.transfers != 2) ||
        (Driver::service(coretx::TimestampUs{start + 31'000U}, channels, false,
                         coretx::TimestampUs{start + 31'000U}) != hw::CrsfError::none) ||
        (io.transfers != 2)) {
        return false;
    }
    if ((Driver::service(coretx::TimestampUs{start + 40'000U}, channels, true,
                         coretx::TimestampUs{start + 40'000U}) != hw::CrsfError::none) ||
        (io.transfers != 3)) {
        return false;
    }
    const auto old_brr = io.cell(reg::brr);
    if (!Driver::set_baud(2, coretx::TimestampUs{start + 40'001U}))
        return false;
    if ((io.cell(reg::brr) != old_brr) || (Driver::baudrate() != 416'666)) {
        return false;
    }
    io.finish_dma();
    io.finish_wire();
    return (Driver::service(coretx::TimestampUs{start + 41'000U}, channels, true,
                            coretx::TimestampUs{start + 40'000U}) == hw::CrsfError::none) &&
           (io.cell(reg::brr) == 115) && !Driver::busy() && Driver::powered() &&
           (Driver::frames() == 3) && (io.transfers == 3);
}

bool configuration_arbitration() {
    struct ConfigurationScenario {};
    using Test = coretx::test::RegisterFixture<ConfigurationScenario, Registers>;
    using Driver = hw::Crsf<Test::Io>;
    Test::reset_registers();
    auto &io = Test::registers;
    coretx::crsf::Channels channels{};
    for (std::size_t i = 0; i < channels.size(); ++i)
        channels[i] = static_cast<std::int16_t>(-1024 + static_cast<int>(i) * 128);
    Driver::initialize(coretx::TimestampUs{0});
    Driver::config().open(coretx::TimestampUs{0});
    if (Driver::service(coretx::TimestampUs{0}, channels, true, coretx::TimestampUs{0}) !=
        hw::CrsfError::none)
        return false;
    const auto rc = coretx::crsf::encode(channels);
    io.finish_dma();
    (void)Driver::service(coretx::TimestampUs{500}, channels, true, coretx::TimestampUs{500});
    if ((io.transfers != 1) || !std::equal(rc.begin(), rc.end(), io.transmit))
        return false;
    io.finish_wire();
    (void)Driver::service(coretx::TimestampUs{700}, channels, true, coretx::TimestampUs{700});
    if ((io.transfers != 2) || (io.cell(reg::tx_cndtr) != 6) ||
        (io.transmit[2] != coretx::crsf::CRSF_FRAMETYPE_DEVICE_PING) || (Driver::frames() != 1))
        return false;
    const std::array<std::uint8_t, 6> ping{io.transmit[0], io.transmit[1], io.transmit[2],
                                           io.transmit[3], io.transmit[4], io.transmit[5]};
    Driver::config().close();
    Driver::config().open(coretx::TimestampUs{800});
    (void)Driver::service(coretx::TimestampUs{800}, channels, true, coretx::TimestampUs{800});
    if ((io.transfers != 2) || !std::equal(ping.begin(), ping.end(), io.transmit))
        return false;
    io.finish_dma();
    io.finish_wire();
    for (auto &channel : channels)
        channel = static_cast<std::int16_t>(-channel);
    (void)Driver::service(coretx::TimestampUs{4000}, channels, true, coretx::TimestampUs{4000});
    const auto updated = coretx::crsf::encode(channels);
    if ((io.transfers != 3) || (Driver::frames() != 1) ||
        !std::equal(updated.begin(), updated.end(), io.transmit))
        return false;
    io.finish_dma();
    io.finish_wire();
    io.clock_step = 500;
    (void)Driver::service(coretx::TimestampUs{7300}, channels, true, coretx::TimestampUs{7300});
    io.clock_step = 0;
    if ((io.transfers != 3) || (Driver::config().next_frame_size() != 6))
        return false; // Preparation time consumed the remaining slot; the ping stays pending.
    (void)Driver::service(coretx::TimestampUs{7800}, channels, true, coretx::TimestampUs{7800});
    if (io.transfers != 3)
        return false;
    (void)Driver::service(coretx::TimestampUs{8000}, channels, true, coretx::TimestampUs{8000});
    if ((io.transfers != 4) || (io.transmit[2] != coretx::crsf::CRSF_FRAMETYPE_RC_CHANNELS_PACKED))
        return false;
    Driver::power_off();
    return (Driver::config().error() == coretx::crsf::config::Error::link) &&
           (io.cell(reg::tx_ccr) == 0);
}

bool multiswitch_arbitration() {
    struct Scenario {};
    using Test = coretx::test::RegisterFixture<Scenario, Registers>;
    using Driver = hw::Crsf<Test::Io>;
    Test::reset_registers();
    auto &io = Test::registers;
    coretx::Model model{};
    model.multiswitch[0].address = 42;
    model.multiswitch[3].address = 255;
    coretx::multiswitch::Pages pages{};
    pages.configure(model);
    coretx::crsf::Channels channels{};
    for (std::size_t i = 0; i < channels.size(); ++i)
        channels[i] = static_cast<std::int16_t>(-1024 + static_cast<int>(i) * 128);
    const auto service = [&](const std::uint32_t now, const bool valid = true) {
        return Driver::service(coretx::TimestampUs{now}, channels, valid, coretx::TimestampUs{now},
                               &pages);
    };
    const auto finish = [&] {
        io.finish_dma();
        io.finish_wire();
    };
    Driver::initialize(coretx::TimestampUs{0});
    Driver::config().open(coretx::TimestampUs{0});
    if (service(0) != hw::CrsfError::none)
        return false;
    const auto rc = coretx::crsf::encode(channels);
    if (!std::equal(rc.begin(), rc.end(), io.transmit))
        return false;
    io.finish_dma();
    service(500);
    if ((io.transfers != 1) || (pages.due(coretx::TimestampUs{500}) != 0))
        return false;
    io.finish_wire();
    service(700);
    if constexpr (!coretx::firmware::features::multiswitch) {
        if ((io.cell(reg::tx_cndtr) != 6) ||
            (io.transmit[2] != coretx::crsf::CRSF_FRAMETYPE_DEVICE_PING) ||
            (pages.due(coretx::TimestampUs{700}) != 0))
            return false;
        finish();
        Driver::config().close();
        const auto transfers = io.transfers;
        service(1000);
        if (io.transfers != transfers)
            return false;
        service(4000);
        return std::equal(rc.begin(), rc.end(), io.transmit);
    }
    const auto first = coretx::multiswitch::encode(42, 0);
    if ((io.cell(reg::tx_cndtr) != 12) || !std::equal(first.begin(), first.end(), io.transmit) ||
        (Driver::config().next_frame_size() != 6))
        return false;
    finish();
    service(1000);
    if ((io.transmit[2] != coretx::crsf::CRSF_FRAMETYPE_DEVICE_PING) || (Driver::frames() != 1))
        return false;
    Driver::config().close();
    finish();
    service(1300);
    const auto last = coretx::multiswitch::encode(255, 0);
    if (!std::equal(last.begin(), last.end(), io.transmit) || pages.due(coretx::TimestampUs{1300}))
        return false;
    finish();
    service(4000);
    if (!std::equal(rc.begin(), rc.end(), io.transmit) || (Driver::frames() != 1))
        return false;
    finish();
    pages.update(0, std::nullopt, 7);
    const auto transfers = io.transfers;
    io.clock_step = 500;
    service(7300);
    io.clock_step = 0;
    if ((io.transfers != transfers) || (pages.due(coretx::TimestampUs{7300}) != 0))
        return false;
    service(7800);
    if ((io.transfers != transfers) || (pages.due(coretx::TimestampUs{7800}) != 0))
        return false;
    service(8000);
    if ((io.transmit[2] != coretx::crsf::CRSF_FRAMETYPE_RC_CHANNELS_PACKED) ||
        (pages.due(coretx::TimestampUs{8000}) != 0))
        return false;
    finish();
    service(8700, false);
    if ((pages.due(coretx::TimestampUs{8700}) != 0) || Driver::busy())
        return false;
    service(8800);
    const auto changed = coretx::multiswitch::encode(42, 0x80);
    if (!std::equal(changed.begin(), changed.end(), io.transmit))
        return false;
    finish();
    service(100'699);
    if (pages.due(coretx::TimestampUs{100'699}))
        return false;
    finish();
    service(101'300);
    return std::equal(last.begin(), last.end(), io.transmit) &&
           !pages.due(coretx::TimestampUs{101'300});
}

bool transmission_faults() {
    struct Scenario {};
    using Test = coretx::test::RegisterFixture<Scenario, Registers>;
    using Driver = hw::Crsf<Test::Io>;

    coretx::crsf::Channels channels{};
    struct StartupScenario {};
    using StartupTest = coretx::test::RegisterFixture<StartupScenario, Registers>;
    using Startup = hw::Crsf<StartupTest::Io>;
    StartupTest::reset_registers();
    auto &not_ready = StartupTest::registers;
    not_ready.acknowledge = false;

    Startup::initialize(coretx::TimestampUs{0});
    if ((Startup::service(coretx::TimestampUs{Limits::timeout_us.count()}, channels, true,
                          coretx::TimestampUs{Limits::timeout_us.count()}) !=
         hw::CrsfError::ready) ||
        Startup::powered()) {
        return false;
    }
    for (const auto error : {hw::CrsfError::dma, hw::CrsfError::sequence, hw::CrsfError::timeout}) {
        Test::reset_registers();
        auto &io = Test::registers;

        Driver::initialize(coretx::TimestampUs{0});
        (void)Driver::service(coretx::TimestampUs{0}, channels, true, coretx::TimestampUs{0});
        if (error == hw::CrsfError::dma) {
            io.cell(reg::dma_isr) |= reg::tx_error;
        } else if (error == hw::CrsfError::sequence) {
            io.cell(reg::tx_cndtr) = 27;
        }
        if ((Driver::service(coretx::TimestampUs{Limits::timeout_us.count()}, channels, true,
                             coretx::TimestampUs{Limits::timeout_us.count()}) != error) ||
            Driver::busy() || !Driver::powered() || (io.cell(reg::tx_ccr) != 0) ||
            (io.cell(reg::rx_ccr) != 0) || (io.cell(reg::cr1) != 0) ||
            (Driver::service(coretx::TimestampUs{Limits::timeout_us.count() + 1U}, channels, true,
                             coretx::TimestampUs{Limits::timeout_us.count()}) != error) ||
            (io.transfers != 1)) {
            return false;
        }
        Driver::next_baud(coretx::TimestampUs{Limits::timeout_us.count() + 2U});
        if ((Driver::service(coretx::TimestampUs{Limits::timeout_us.count() + 3U}, channels, true,
                             coretx::TimestampUs{Limits::timeout_us.count()}) !=
             hw::CrsfError::none) ||
            (io.transfers != 2)) {
            return false;
        }
    }
    Test::reset_registers();
    auto &io = Test::registers;

    Driver::initialize(coretx::TimestampUs{0});
    io.cell(reg::isr) |= USART_ISR_FE | USART_ISR_ORE;
    if ((Driver::service(coretx::TimestampUs{0}, channels, false, coretx::TimestampUs{0}) !=
         hw::CrsfError::none) ||
        (Driver::receive_errors() != 1) ||
        ((io.cell(reg::isr) & (USART_ISR_FE | USART_ISR_ORE)) != 0)) {
        return false;
    }
    io.cell(reg::dma_isr) |= reg::rx_error;
    return Driver::service(coretx::TimestampUs{1}, channels, false, coretx::TimestampUs{0}) ==
           hw::CrsfError::dma;
}

bool receive_error_rate() {
    struct Scenario {};
    using Test = coretx::test::RegisterFixture<Scenario, Registers>;
    using Driver = hw::Crsf<Test::Io>;

    for (const std::uint32_t start : {0U, 0xFFFFF000U}) {
        Test::reset_registers();
        auto &io = Test::registers;

        coretx::crsf::Channels channels{};
        Driver::initialize(coretx::TimestampUs{start});
        const auto poll = [&](const std::uint32_t offset, const std::uint32_t flags = 0) {
            io.cell(reg::isr) |= flags;
            (void)Driver::service(coretx::TimestampUs{start + offset}, channels, false,
                                  coretx::TimestampUs{start});
        };
        poll(100, USART_ISR_NE);
        poll(999'999, USART_ISR_FE | USART_ISR_ORE);
        if ((Driver::receive_errors_per_second() != 0) || (Driver::receive_errors() != 2)) {
            return false;
        }
        poll(1'000'000, USART_ISR_PE);
        if ((Driver::receive_errors_per_second() != 2) || (Driver::receive_errors() != 3)) {
            return false;
        }
        poll(1'999'999);
        if (Driver::receive_errors_per_second() != 2) {
            return false;
        }
        poll(2'000'000);
        if (Driver::receive_errors_per_second() != 1) {
            return false;
        }
        poll(3'000'000);
        if ((Driver::receive_errors_per_second() != 0) || (Driver::receive_errors() != 3)) {
            return false;
        }
        poll(3'000'001, USART_ISR_FE);
        Driver::next_baud(coretx::TimestampUs{start + 3'500'000U});
        poll(4'000'000);
        if ((Driver::receive_errors_per_second() != 1) || (Driver::receive_errors() != 4)) {
            return false;
        }
        poll(4'000'001, USART_ISR_NE);
        poll(6'000'123);
        if ((Driver::receive_errors_per_second() != 0) || (Driver::receive_errors() != 5)) {
            return false;
        }
        poll(6'000'124, USART_ISR_NE);
        poll(7'000'000);
        if (Driver::receive_errors_per_second() != 1) {
            return false;
        }
        io.cell(reg::dma_isr) |= reg::rx_error;
        poll(7'000'001);
        poll(8'000'000);
        if ((Driver::error() != hw::CrsfError::dma) || (Driver::receive_errors_per_second() != 0) ||
            (Driver::receive_errors() != 6)) {
            return false;
        }
        const auto restart = start + 9'123'456U;
        Driver::initialize(coretx::TimestampUs{restart});
        if ((Driver::receive_errors_per_second() != 0) || (Driver::receive_errors() != 0)) {
            return false;
        }
        io.cell(reg::isr) |= USART_ISR_FE;
        (void)Driver::service(coretx::TimestampUs{restart + 999'999U}, channels, false,
                              coretx::TimestampUs{restart});
        if (Driver::receive_errors_per_second() != 0) {
            return false;
        }
        (void)Driver::service(coretx::TimestampUs{restart + 1'000'000U}, channels, false,
                              coretx::TimestampUs{restart});
        if ((Driver::receive_errors_per_second() != 1) || (Driver::receive_errors() != 1)) {
            return false;
        }
    }
    return true;
}

bool pipeline_gating() {
    struct Scenario {};
    using Test = coretx::test::RegisterFixture<Scenario, Registers>;
    using Driver = hw::Crsf<Test::Io>;

    Test::reset_registers();
    auto &io = Test::registers;

    hw::Diagnostics diagnostics{};
    hw::StockSamples samples{};
    samples.fill(2000);
    Driver::initialize(coretx::TimestampUs{0});
    diagnostics.observe(samples, coretx::TimestampUs{0});
    if ((Driver::service(coretx::TimestampUs{0}, diagnostics.evaluation().frame.channels,
                         diagnostics.output_valid(),
                         coretx::TimestampUs{0}) != hw::CrsfError::none) ||
        (io.transfers != 0) || !Driver::powered()) {
        return false;
    }
    auto setup = diagnostics.setup();
    setup.calibration.fill({400, 2000, 3600});
    if (!diagnostics.restore_calibration(setup) || !diagnostics.output_valid()) {
        return false;
    }
    samples[0] = 3600;
    diagnostics.observe(samples, coretx::TimestampUs{100});
    if ((Driver::service(coretx::TimestampUs{100}, diagnostics.evaluation().frame.channels,
                         diagnostics.output_valid(),
                         coretx::TimestampUs{100}) != hw::CrsfError::none) ||
        (io.transfers != 1)) {
        return false;
    }
    const auto expected = coretx::crsf::encode(diagnostics.evaluation().frame.channels);
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (io.transmit[i] != expected[i]) {
            return false;
        }
    }
    io.finish_dma();
    io.finish_wire();
    diagnostics.begin_calibration();
    if ((Driver::service(coretx::TimestampUs{5'000}, diagnostics.evaluation().frame.channels,
                         diagnostics.output_valid(),
                         coretx::TimestampUs{100}) != hw::CrsfError::none) ||
        (io.transfers != 1) || !Driver::powered()) {
        return false;
    }
    diagnostics.handle(hw::Key::cancel);
    if ((Driver::service(coretx::TimestampUs{6'000}, diagnostics.evaluation().frame.channels,
                         diagnostics.output_valid(),
                         coretx::TimestampUs{100}) != hw::CrsfError::none) ||
        (io.transfers != 2)) {
        return false;
    }
    io.finish_dma();
    io.finish_wire();
    diagnostics.invalidate();
    return (Driver::service(coretx::TimestampUs{12'000}, diagnostics.evaluation().frame.channels,
                            diagnostics.output_valid(),
                            coretx::TimestampUs{100}) == hw::CrsfError::none) &&
           (io.transfers == 2) && Driver::powered();
}

constexpr bool crsf_rendering() {
    if (!coretx::firmware::features::crsf)
        return true;
    coretx::ui::Framebuffer display{};
    coretx::ui::Framebuffer expected{};
    const coretx::test::CrsfInfo crsf{5'250'000, 5'333'333, 123, 4, coretx::ui::tr::tx_channels};
    coretx::test::render_crsf(crsf, display);
    expected.text(0, 45, coretx::ui::tr::rx_errors);
    expected.number(127, 45, 4);
    expected.small_text(0, 58, coretx::ui::tr::tx_channels);
    for (int y = 45; y < 64; ++y) {
        for (int x = 0; x < 128; ++x) {
            if (display.pixel_at(x, y) != expected.pixel_at(x, y)) {
                return false;
            }
        }
    }
    return true;
}

constexpr bool telemetry_rendering() {
    coretx::ui::Framebuffer display{};
    coretx::ui::Framebuffer expected{};
    coretx::telemetry::Pool pool{};
    pool.publish(coretx::telemetry::transmitter_battery, 6200, coretx::TimestampUs{1});
    coretx::firmware::render_telemetry(pool, display, coretx::TimestampUs{1}, 0,
                                       coretx::telemetry::Source::local);
    expected.text(0, 0, coretx::ui::tr::telemetry_title);
    expected.unsigned_number(110, 0, 1);
    expected.text(112, 0, "/");
    expected.unsigned_number(127, 0, 1);
    expected.small_text(0, 10, coretx::ui::tr::telemetry_local);
    expected.text(0, 18, "L");
    expected.text(10, 18, coretx::ui::tr::telemetry_tx_battery);
    expected.text(92, 18, "6.20 V");
    expected.small_text(0, 58, coretx::ui::tr::telemetry_next);
    if (display.bytes() != expected.bytes())
        return false;
    return true;
}

bool consecutive_maximum_frames() {
    struct Scenario {};
    using Test = coretx::test::RegisterFixture<Scenario, Registers>;
    using Driver = hw::Crsf<Test::Io>;

    for (const unsigned offset : {0U, 192U}) {
        Test::reset_registers();
        auto &io = Test::registers;

        coretx::crsf::Channels channels{};
        Driver::initialize(coretx::TimestampUs{});
        (void)Driver::service(coretx::TimestampUs{}, channels, false, coretx::TimestampUs{});
        for (unsigned i = 0; i < (offset / 64); ++i) {
            for (unsigned j = 0; j < 64; ++j)
                io.feed(0x55);
            (void)Driver::service(coretx::TimestampUs{i + 1}, channels, false,
                                  coretx::TimestampUs{});
        }
        std::array<std::uint8_t, 64> frame{};
        frame.fill(0x55);
        frame[0] = 0xEA;
        frame[1] = 62;
        frame[2] = 0x02;
        frame.back() = coretx::crsf::crc8(std::span{frame}.subspan(2, 61));
        io.feed(frame);
        io.feed(frame);
        const auto reads = io.receive_reads;
        if ((Driver::service(coretx::TimestampUs{100}, channels, false, coretx::TimestampUs{}) !=
             hw::CrsfError::none) ||
            (Driver::telemetry().frames() != 1) || ((io.receive_reads - reads) != 64))
            return false;
        if ((Driver::service(coretx::TimestampUs{101}, channels, false, coretx::TimestampUs{}) !=
             hw::CrsfError::none) ||
            (Driver::telemetry().frames() != 2) || ((io.receive_reads - reads) != 128) ||
            (Driver::receive_overruns() != 0) || (Driver::telemetry().crc_errors() != 0))
            return false;
    }
    return true;
}

bool lazy_state_lifecycle() {
    struct Scenario {};
    using Test = coretx::test::RegisterFixture<Scenario, Registers>;
    using Driver = hw::Crsf<Test::Io>;
    Test::reset_registers();
    auto &io = Test::registers;
    auto &client = Driver::config();
    if ((Driver::error() != hw::CrsfError::not_initialized) || Driver::powered() ||
        Driver::busy() || (Driver::frames() != 0) ||
        (Driver::baudrate() != hw::crsf_baudrates[hw::crsf_default_baudrate_index].rate) ||
        (client.mode() != coretx::crsf::config::Mode::closed) || (io.count != 0))
        return false;
    client.open(coretx::TimestampUs{0});
    Driver::power_off();
    if ((&Driver::config() != &client) || (client.mode() != coretx::crsf::config::Mode::devices) ||
        Driver::powered())
        return false;
    Driver::initialize(coretx::TimestampUs{1});
    if ((&Driver::config() != &client) || (client.mode() != coretx::crsf::config::Mode::closed) ||
        (Driver::error() != hw::CrsfError::none))
        return false;
    Driver::power_off();
    Driver::initialize(coretx::TimestampUs{2});
    return (&Driver::config() == &client) &&
           (client.mode() == coretx::crsf::config::Mode::closed) && !io.gate_glitch;
}

static_assert(telemetry_protocol());
static_assert(telemetry_resynchronization());
static_assert(telemetry_rendering());
static_assert(protocol_frames());
static_assert(crsf_rendering());

} // namespace

int main() {
    if (!telemetry_protocol() || !telemetry_resynchronization() || !dma_reception() ||
        !dma_receive_recovery() || !telemetry_rendering() || !protocol_frames() ||
        !wiring_and_baudrates() || !cooperative_transmission() || !configuration_arbitration() ||
        !multiswitch_arbitration() || !transmission_faults() || !receive_error_rate() ||
        !pipeline_gating() || !crsf_rendering() || !consecutive_maximum_frames() ||
        !lazy_state_lifecycle()) {
        std::fputs("FAIL: i6X CRSF checks\n", stderr);
        return EXIT_FAILURE;
    }
    std::puts("i6X CRSF checks passed");
}
