// SPDX-License-Identifier: GPL-2.0-or-later
#include "boards/common/dashboard.hpp"
#include "boards/common/radio_board.hpp"
#include "platform/stm32f072/radio_timer.hpp"
#include "static_fixture.hpp"
#include "tests/renderers/radio.hpp"

#include <cstdio>
#include <cstdlib>

namespace {

namespace fw = coretx::firmware;
using spi = fw::a7105_registers;
constexpr auto pins = fw::i6x_radio_pins;

struct Registers {
    struct Cell {
        std::uintptr_t address{};
        std::uint32_t value{};
    };
    std::array<Cell, 64> cells{};
    std::array<std::uint8_t, 64> chip{};
    std::array<std::uint8_t, 38> fifo{};
    std::size_t count{};
    unsigned spi_accesses{};
    unsigned channel_writes{};
    unsigned transactions{};
    unsigned gpio_configurations{};
    std::uint8_t command{};
    struct Reply {
        std::uint8_t value{};
        std::uint32_t queued_at{};
        std::uint32_t delay{};
    };
    std::array<Reply, 4> replies{};
    std::size_t head{};
    std::size_t pending{};
    unsigned byte{};
    bool selected{};
    bool standby{};
    bool rf_command{};
    bool glitch{};
    bool absent{};
    bool reject_probe{};
    bool reject_restore{};
    bool block_receive{};
    bool stuck_busy{};
    bool spi_error{};
    bool delayed_spi{};
    std::uint32_t clock{};
    std::uint32_t transmitted_at{};
    std::uint32_t transmission_duration{};
    std::uint32_t tx_duration{};
    bool transmitting{};
    bool settling_reports_idle{};
    bool auto_bind{};
    bool rx_available{};
    std::array<std::uint8_t, 37> received{};
    bool receiving{};
    unsigned tx_count{};
    unsigned aborted_transmissions{};
    std::uint32_t maximum_tx_gap{};
    std::uint32_t minimum_tx_gap{};
    std::uint32_t access_cost{};
    std::uint32_t packet_work{};
    std::uint32_t receive_delay{2'800};

    constexpr Registers() {
        chip[0x10] = 0x9E;
        chip[0x0F] = 0x50;
    }

    constexpr std::uint32_t &cell(const std::uintptr_t address) {
        for (std::size_t i = 0; i < count; ++i)
            if (cells[i].address == address)
                return cells[i].value;
        if (count == cells.size())
            std::abort();
        cells[count].address = address;
        return cells[count++].value;
    }

    constexpr std::uint32_t read(const std::uintptr_t address) {
        clock += access_cost;
        if ((address >= SPI1_BASE) && (address < (SPI1_BASE + sizeof(SPI_TypeDef))))
            ++spi_accesses;
        if (address == spi::status)
            return SPI_SR_TXE |
                   (pending && !block_receive &&
                            (!delayed_spi ||
                             ((clock - replies[head].queued_at) >= replies[head].delay))
                        ? SPI_SR_RXNE
                        : 0U) |
                   (stuck_busy ? SPI_SR_BSY : 0U) | (spi_error ? SPI_SR_OVR : 0U);
        return cell(address);
    }

    constexpr void write(const std::uintptr_t address, const std::uint32_t value) {
        clock += access_cost;
        if (address == spi::data)
            std::abort(); // DR must be accessed as a byte, never a packed halfword/word.
        if ((address >= SPI1_BASE) && (address < (SPI1_BASE + sizeof(SPI_TypeDef))))
            ++spi_accesses;
        cell(address) = value;
        for (const auto port : {GPIOE_BASE, GPIOC_BASE, GPIOF_BASE}) {
            if (address == (port + offsetof(GPIO_TypeDef, BSRR))) {
                auto &odr = cell(port + offsetof(GPIO_TypeDef, ODR));
                odr = (odr | (value & 0xFFFFU)) & ~(value >> 16);
            }
        }
        if (address == (GPIOE_BASE + offsetof(GPIO_TypeDef, BSRR))) {
            if ((value & (1U << 28)) != 0) {
                selected = true;
                byte = 0;
            }
            if ((value & (1U << 12)) != 0) {
                if (selected) {
                    if (pending && ((cell(spi::control1) & SPI_CR1_SPE) != 0))
                        std::abort();
                    ++transactions;
                }
                selected = false;
            }
        }
        if (address == (GPIOE_BASE + offsetof(GPIO_TypeDef, MODER))) {
            ++gpio_configurations;
            for (const unsigned pin : {8U, 9U}) {
                if ((((value >> (pin * 2)) & 3U) == 1) &&
                    ((cell(GPIOE_BASE + offsetof(GPIO_TypeDef, ODR)) & (1U << pin)) == 0))
                    glitch = true;
            }
        }
    }

    constexpr void write8(const std::uintptr_t address, const std::uint8_t value) {
        clock += access_cost;
        if ((address != spi::data) || !selected || (pending == replies.size()) ||
            ((cell(spi::control1) & SPI_CR1_SPE) == 0))
            std::abort();
        ++spi_accesses;
        std::uint8_t response{};
        if (byte++ == 0) {
            command = value;
            rf_command |= (command == 0xC0) || (command == 0xD0);
            standby |= command == 0xA0;
            if ((command == 0xA0) || (command == 0xC0) || (command == 0xD0)) {
                if (transmitting && ((clock - transmitted_at) < tx_duration))
                    ++aborted_transmissions;
                transmitting = false;
                receiving = command == 0xC0;
            }
            if (command == 0xD0) {
                if (tx_count != 0) {
                    maximum_tx_gap = std::max(maximum_tx_gap, clock - transmitted_at);
                    minimum_tx_gap = minimum_tx_gap == 0
                                         ? clock - transmitted_at
                                         : std::min(minimum_tx_gap, clock - transmitted_at);
                }
                ++tx_count;
                transmitted_at = clock;
                transmitting = true;
                const auto code = chip[0x1F];
                const auto preamble_bytes = static_cast<unsigned>(code & 3U) + 1U;
                const auto id_bytes = (code & 4U) != 0 ? 4U : 2U;
                const auto crc_bytes = (code & 8U) != 0 ? 2U : 0U;
                const auto payload_bits = (static_cast<unsigned>(chip[0x03]) + 1U + crc_bytes) * 8U;
                const auto coded_bits = (code & 0x10U) != 0 ? payload_bits * 7U / 4U : payload_bits;
                const auto bit_time = 2U * (static_cast<unsigned>(chip[0x0E]) + 1U);
                tx_duration =
                    transmission_duration != 0
                        ? transmission_duration
                        : 130U + ((preamble_bytes + id_bytes) * 8U + coded_bits) * bit_time;
            } else if ((command == 0xC0) && auto_bind && ((fifo[0] == 0xBB) || (fifo[0] == 0xBC)) &&
                       (fifo[9] != 2)) {
                received.fill(0);
                received[0] = 0xBC;
                std::copy_n(fifo.begin() + 1, 4, received.begin() + 1);
                received[5] = 0xBE;
                received[6] = 0xBA;
                received[7] = 0xFE;
                received[8] = 0xCA;
                received[9] = 1;
                rx_available = true;
            }
        } else if (command == 0x05) {
            fifo[byte - 2] = value;
        } else if (command == 0x45) {
            response = auto_bind ? received[byte - 2] : fifo[byte - 2];
            if (auto_bind && (byte == 38))
                rx_available = false;
        } else if ((command & 0x40U) != 0) {
            response = absent ? 0xFF : command == 0x42 ? 0 : chip[command & 0x3FU];
            if (!absent && (command == 0x40) && transmitting &&
                (!settling_reports_idle || ((clock - transmitted_at) >= 130)) &&
                ((clock - transmitted_at) < tx_duration))
                response |= fw::a7105::mode_trer;
            if (!absent && (command == 0x40) && receiving &&
                (!rx_available || ((clock - transmitted_at) < receive_delay)))
                response |= fw::a7105::mode_trer;
        } else {
            if (command == 0x0F) {
                ++channel_writes;
            }
            if ((command != 0x0F) || !(reject_probe || (reject_restore && (channel_writes == 2))))
                chip[command] = value;
        }
        const auto tail = (head + pending) % replies.size();
        replies[tail] = {response, clock, static_cast<std::uint32_t>(2 * (pending + 1))};
        ++pending;
    }

    constexpr std::uint8_t read8(const std::uintptr_t address) {
        clock += access_cost;
        if ((address != spi::data) || !pending || block_receive ||
            (delayed_spi && ((clock - replies[head].queued_at) < replies[head].delay)))
            std::abort();
        ++spi_accesses;
        const auto response = replies[head].value;
        head = (head + 1) % replies.size();
        --pending;
        return response;
    }
};

template <typename Driver> void run(Registers &, const std::uint32_t base) {
    for (std::uint32_t elapsed = 0; elapsed < 10'000; elapsed += 10) {
        Driver::service(coretx::TimestampUs{base + elapsed});
        if ((Driver::info().state == fw::A7105State::ready) ||
            (Driver::info().state == fw::A7105State::failed))
            return;
    }
}

bool initialization() {
    struct Scenario {};
    using Test = coretx::test::RegisterFixture<Scenario, Registers>;
    using Driver = fw::A7105<Test::Io, pins>;

    for (const std::uint32_t base : {0U, 0xFFFF'F800U}) {
        Test::reset_registers();
        auto &io = Test::registers;

        io.cell(GPIOE_BASE + offsetof(GPIO_TypeDef, MODER)) = 1;
        Driver::initialize(coretx::TimestampUs{base});
        if (io.glitch || (Driver::info().state != fw::A7105State::starting) ||
            (io.cell(spi::control2) != 0x1700) ||
            (io.cell(spi::control1) !=
             (SPI_CR1_MSTR | SPI_CR1_BR_1 | SPI_CR1_SSM | SPI_CR1_SSI | SPI_CR1_SPE)))
            return false;
        Driver::service(coretx::TimestampUs{base + 999});
        if (io.selected || (io.transactions != 0))
            return false;
        run<Driver>(io, base + 1'000U);
        const auto info = Driver::info();
        if ((info.state != fw::A7105State::ready) || (info.error != fw::A7105Error::none) ||
            (info.signature != 0x9E) || (io.chip[0x0F] != 0x50) || (io.channel_writes != 2) ||
            !io.standby || io.rf_command || io.glitch || io.selected ||
            ((io.cell(GPIOE_BASE + offsetof(GPIO_TypeDef, ODR)) & 0x300U) != 0x300U) ||
            ((io.cell(GPIOE_BASE + offsetof(GPIO_TypeDef, MODER)) & 3U) != 1))
            return false;
        Driver::power_off();
        if ((Driver::info().state != fw::A7105State::off) || (io.cell(spi::control1) != 0))
            return false;
    }
    return true;
}

bool failures() {
    struct Scenario {};
    using Test = coretx::test::RegisterFixture<Scenario, Registers>;
    using Driver = fw::A7105<Test::Io, pins>;

    constexpr std::array errors{fw::A7105Error::signature, fw::A7105Error::verify,
                                fw::A7105Error::verify,    fw::A7105Error::timeout,
                                fw::A7105Error::timeout,   fw::A7105Error::spi};
    for (unsigned kind = 0; kind < errors.size(); ++kind) {
        Test::reset_registers();
        auto &io = Test::registers;

        io.absent = kind == 0;
        io.reject_probe = kind == 1;
        io.reject_restore = kind == 2;
        io.block_receive = kind == 3;
        io.stuck_busy = kind == 4;
        io.spi_error = kind == 5;
        Driver::initialize(coretx::TimestampUs{});
        run<Driver>(io, 0);
        if ((Driver::info().state != fw::A7105State::failed) ||
            (Driver::info().error != errors[kind]) || io.rf_command || io.glitch ||
            (io.cell(spi::control1) != 0) || io.selected)
            return false;
        const auto accesses = io.spi_accesses;
        Driver::service(coretx::TimestampUs{20'000});
        if (io.spi_accesses != accesses)
            return false;
    }
    return true;
}

bool disabled_safety() {
    struct Scenario {};
    using Test = coretx::test::RegisterFixture<Scenario, Registers>;

    Test::reset_registers();
    auto &io = Test::registers;
    fw::external_module_off<Test::Io>();
    fw::internal_front_end_off<Test::Io, pins>();
    return (io.spi_accesses == 0) && !io.glitch &&
           ((io.cell(GPIOE_BASE + offsetof(GPIO_TypeDef, ODR)) & 0x300U) == 0x300U) &&
           (((io.cell(fw::external_module_power.port + offsetof(GPIO_TypeDef, ODR)) &
              (1U << fw::external_module_power.index)) != 0) != fw::external_module_active_high) &&
           ((io.cell(fw::stm32f072::registers::rcc_apb2enr) & RCC_APB2ENR_SPI1EN) == 0);
}

constexpr bool page_selection() {
    fw::Diagnostics diagnostics{};
    const auto expected_count = 2U;
    if (fw::Diagnostics::page_count != expected_count)
        return false;
    for (std::size_t page = 0; page < expected_count; ++page) {
        if (diagnostics.page() != page)
            return false;
        diagnostics.handle(fw::Key::ok);
        if (diagnostics.phase() != fw::CalibrationPhase::idle)
            return false;
        diagnostics.handle(fw::Key::down);
    }
    return diagnostics.page() == fw::Diagnostics::overview_page;
}

constexpr bool afhds_format_rendering() {
    if (fw::features::a7105) {
        for (const auto mode :
             {coretx::afhds2a::ChannelMode::standard14, coretx::afhds2a::ChannelMode::extended16}) {
            for (const auto output :
                 {fw::RadioOutput::off, fw::RadioOutput::internal, fw::RadioOutput::external}) {
                coretx::ui::Framebuffer display{};
                coretx::ui::Framebuffer expected{};
                fw::RadioInfo info{};
                info.channel_mode = mode;
                info.output = output;
                info.ready = true;
                info.receiver = {0xCAFEBABE};
                info.storage = fw::RadioStorageStatus::io_error;
                fw::render_afhds2a(info, display, 12);
                expected.text(0, 0, "AFHDS 2A");
                expected.text(56, 0,
                              mode == coretx::afhds2a::ChannelMode::extended16 ? "16 RC + LQI"
                                                                               : "14 RC");
                expected.text(0, 10, coretx::ui::tr::watchdog_resets);
                expected.unsigned_number(127, 10, 12);
                expected.text(0, 20,
                              output == fw::RadioOutput::internal ? coretx::ui::tr::radio_waiting_rx
                                                                  : coretx::ui::tr::radio_off);
                expected.text(0, 30, "RX ID");
                expected.text(79, 30, "CAFEBABE");
                expected.text(0, 40, "TX");
                expected.unsigned_number(127, 40, 0);
                expected.small_text(0, 52,
                                    output == fw::RadioOutput::off
                                        ? coretx::ui::tr::i2c_error
                                        : coretx::ui::tr::radio_center_failsafe);
                expected.small_text(0, 58,
                                    output == fw::RadioOutput::off ? coretx::ui::tr::radio_format
                                    : output == fw::RadioOutput::internal
                                        ? coretx::ui::tr::radio_bind
                                        : coretx::ui::tr::radio_select_internal);
                if (display.bytes() != expected.bytes())
                    return false;
            }
        }
    }
    return true;
}

bool fifo_transfers(const bool delayed = false) {
    struct Scenario {};
    using Test = coretx::test::RegisterFixture<Scenario, Registers>;
    using Driver = fw::A7105<Test::Io, pins>;

    Test::reset_registers();
    auto &io = Test::registers;

    Driver::initialize(coretx::TimestampUs{});
    run<Driver>(io, 0);
    io.delayed_spi = delayed;
    std::array<std::uint8_t, 38> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<std::uint8_t>(i + 11);
    if (!Driver::command(0x05, bytes, 0, coretx::TimestampUs{10'000}))
        return false;
    for (std::uint32_t t = 10'000; Driver::busy() && (t < 11'000); t += 10) {
        io.clock = t;
        Driver::service(coretx::TimestampUs{t});
    }
    if (!Driver::completed() || Driver::busy() || (io.fifo != bytes) || io.selected)
        return false;
    if (!Driver::command(0x45, {}, 37, coretx::TimestampUs{11'000}))
        return false;
    for (std::uint32_t t = 11'000; Driver::busy() && (t < 12'000); t += 10) {
        io.clock = t;
        Driver::service(coretx::TimestampUs{t});
    }
    if (!Driver::completed() || Driver::busy() || (Driver::payload().size() != 37) ||
        !std::equal(Driver::payload().begin(), Driver::payload().end(), bytes.begin()) ||
        io.selected)
        return false;
    for (const auto mode :
         {fw::A7105FrontEnd::transmit, fw::A7105FrontEnd::receive, fw::A7105FrontEnd::off}) {
        Driver::front_end(mode);
        const auto enables = io.cell(GPIOE_BASE + offsetof(GPIO_TypeDef, ODR)) & 0x300U;
        const auto expected = mode == fw::A7105FrontEnd::transmit  ? 0x100U
                              : mode == fw::A7105FrontEnd::receive ? 0x200U
                                                                   : 0x300U;
        if ((enables != expected) || (Driver::info().front_end != mode))
            return false;
    }
    return true;
}

bool frontend_switching() {
    struct Scenario {};
    using Test = coretx::test::RegisterFixture<Scenario, Registers>;
    using Driver = fw::A7105<Test::Io, pins>;

    Test::reset_registers();
    auto &io = Test::registers;

    Driver::initialize(coretx::TimestampUs{});
    run<Driver>(io, 0);
    const auto configurations = io.gpio_configurations;
    for (const auto sampled_phase :
         {fw::A7105FrontEnd::off, fw::A7105FrontEnd::transmit, fw::A7105FrontEnd::receive}) {
        Driver::front_end(fw::A7105FrontEnd::transmit);
        Driver::front_end(fw::A7105FrontEnd::receive);
        Driver::front_end(sampled_phase);
        if ((io.gpio_configurations != configurations) ||
            (Driver::info().front_end != sampled_phase))
            return false;
    }
    Driver::front_end(fw::A7105FrontEnd::transmit);
    Driver::front_end(fw::A7105FrontEnd::receive);
    Driver::power_off();
    if (Driver::info().front_end != fw::A7105FrontEnd::off)
        return false;
    Driver::front_end(fw::A7105FrontEnd::transmit);
    return (Driver::info().state == fw::A7105State::failed) &&
           (Driver::info().front_end == fw::A7105FrontEnd::off);
}

constexpr bool preparation_timeout_status() {
    fw::RadioInfo info{};
    info.message = fw::error_message(fw::Afhds2aError::preparation_timeout);
    coretx::ui::Framebuffer display{};
    coretx::ui::Framebuffer expected{};
    fw::render_afhds2a(info, display, 0);
    expected.text(0, 20, coretx::ui::tr::radio_preparation_timeout);
    expected.small_text(0, 52, coretx::ui::tr::radio_loading);
    expected.small_text(0, 58, coretx::ui::tr::radio_format);
    for (unsigned y = 20; y < 64; ++y) {
        if ((y >= 29) && (y < 52))
            continue;
        const auto mask = 1U << (y % 8);
        for (unsigned x = 0; x < coretx::ui::Framebuffer::width; ++x) {
            const auto offset = (y / 8) * coretx::ui::Framebuffer::width + x;
            if ((display.bytes()[offset] & mask) != (expected.bytes()[offset] & mask))
                return false;
        }
    }
    return true;
}

constexpr bool paired_link_status() {
    fw::RadioInfo info{};
    info.output = fw::RadioOutput::internal;
    info.receiver = {0xCAFEBABE};
    info.ready = true;
    for (const bool linked : {false, true}) {
        info.linked = linked;
        coretx::ui::Framebuffer display{};
        coretx::ui::Framebuffer expected{};
        fw::render_afhds2a(info, display, 0);
        expected.text(0, 20, linked ? coretx::ui::tr::live : coretx::ui::tr::radio_waiting_rx);
        for (unsigned y = 20; y < 29; ++y) {
            const auto mask = 1U << (y % 8);
            for (unsigned x = 0; x < coretx::ui::Framebuffer::width; ++x) {
                const auto offset = (y / 8) * coretx::ui::Framebuffer::width + x;
                if ((display.bytes()[offset] & mask) != (expected.bytes()[offset] & mask))
                    return false;
            }
        }
    }
    return true;
}

bool binding_with_delayed_spi(const std::uint32_t interval = 50,
                              const std::uint32_t transmission_duration = 0,
                              const fw::Afhds2aError expected = fw::Afhds2aError::none,
                              const bool settling_reports_idle = false) {
    struct Scenario {};
    using Test = coretx::test::RegisterFixture<Scenario, Registers>;
    using Driver = fw::A7105<Test::Io, pins>;

    Test::reset_registers();
    auto &io = Test::registers;
    io.delayed_spi = true;
    io.transmission_duration = transmission_duration;
    io.settling_reports_idle = settling_reports_idle;
    fw::Afhds2a<Driver> radio{};
    coretx::afhds2a::Channels channels{};
    radio.initialize({0x12345678}, {}, coretx::TimestampUs{});
    radio.activate();
    bool binding{};
    for (std::uint32_t now = 0; now < 50'000; now += interval) {
        io.clock = now;
        radio.service(coretx::TimestampUs{now}, channels, false, coretx::TimestampUs{});
        if (radio.error() != fw::Afhds2aError::none)
            return (radio.error() == expected) && (radio.session().frames() == 0) &&
                   (Driver::info().front_end == fw::A7105FrontEnd::off) &&
                   ((expected == fw::Afhds2aError::preparation_timeout) ||
                    ((Driver::info().mode & fw::a7105::mode_trer) != 0));
        if (radio.ready() && !binding) {
            if (!radio.bind(coretx::TimestampUs{now}))
                return false;
            binding = true;
        }
        if (radio.session().frames() != 0)
            return (expected == fw::Afhds2aError::none) && io.rf_command &&
                   (io.aborted_transmissions == 0) &&
                   ((Driver::info().mode & fw::a7105::mode_trer) == 0);
    }
    return false;
}

bool steady_radio(const std::uint32_t interval,
                  const fw::Afhds2aError expected = fw::Afhds2aError::none) {
    struct Scenario {};
    using Test = coretx::test::RegisterFixture<Scenario, Registers>;
    using Driver = fw::A7105<Test::Io, pins>;

    for (const std::uint32_t base : {0U, 0xFFFF0000U}) {
        Test::reset_registers();
        auto &io = Test::registers;
        io.delayed_spi = true;
        fw::Afhds2a<Driver> radio{};
        coretx::afhds2a::Channels channels{};
        radio.initialize({0x12345678}, {0xCAFEBABE}, coretx::TimestampUs{base});
        radio.activate();
        for (std::uint32_t t = 0; t < 100'000; t += interval) {
            io.clock = base + t;
            radio.service(coretx::TimestampUs{base + t}, channels, true,
                          coretx::TimestampUs{base + t});
        }
        if (expected != fw::Afhds2aError::none) {
            if ((radio.error() != expected) || (Driver::info().front_end != fw::A7105FrontEnd::off))
                return false;
            continue;
        }
        if ((radio.error() != fw::Afhds2aError::none) || (radio.session().frames() < 10) ||
            (io.tx_count < 10) ||
            (io.maximum_tx_gap > (coretx::afhds2a::frame_period.count() + (2 * interval)))) {
            std::fprintf(stderr, "AFHDS %u us: error=%u frames=%u TX=%u gap=%u\n", interval,
                         static_cast<unsigned>(radio.error()), radio.session().frames(),
                         io.tx_count, io.maximum_tx_gap);
            return false;
        }
    }
    return true;
}

struct TestInterruptGuard {
    inline static unsigned depth{};
    TestInterruptGuard() { ++depth; }
    ~TestInterruptGuard() { --depth; }
};

template <typename Test> struct InterruptBackend {
    inline static bool enabled{};
    [[nodiscard]] static bool is_enabled() noexcept { return enabled; }
    static std::uint32_t read(const std::uintptr_t address) {
        if (address == fw::stm32f072::registers::tim2_cnt)
            return Test::registers.clock;
        return Test::registers.read(address);
    }
    static void write(const std::uintptr_t address, const std::uint32_t value) {
        if (address == (NVIC_BASE + offsetof(NVIC_Type, ISER)))
            enabled = true;
        if (address == (NVIC_BASE + offsetof(NVIC_Type, ICER)))
            enabled = false;
        if (address == (EXTI_BASE + offsetof(EXTI_TypeDef, PR)))
            Test::registers.cell(address) &= ~value;
        else if (address == fw::stm32f072::registers::tim2_sr)
            Test::registers.cell(address) &= value;
        else
            Test::registers.write(address, value);
    }
};

template <typename Test> struct TimedBus : fw::A7105<typename Test::Io, pins> {
    using Base = fw::A7105<typename Test::Io, pins>;
    static bool command(std::uint8_t command, std::span<const std::uint8_t> bytes,
                        std::uint8_t count, coretx::TimestampUs now) {
        if (command == fw::a7105::fifo_data)
            Test::registers.clock += Test::registers.packet_work;
        return Base::command(command, bytes, count, now);
    }
};

bool timer_wrap_delay_and_stop() {
    struct Scenario {};
    using Test = coretx::test::RegisterFixture<Scenario, Registers>;
    using InterruptIo = InterruptBackend<Test>;
    using TestTimer = fw::RadioTimer<InterruptIo, TestInterruptGuard>;

    Test::reset_registers();
    auto &io = Test::registers;
    InterruptIo::enabled = false;
    io.clock = 0xFFFFFFF0U;
    std::array<unsigned, 2> calls{};
    TestTimer::start(
        [](void *object, const bool gio2) noexcept {
            ++(*static_cast<std::array<unsigned, 2> *>(object))[gio2 ? 1U : 0U];
            return TestTimer::interval;
        },
        &calls);
    constexpr auto compare = TIM2_BASE + offsetof(TIM_TypeDef, CCR1);
    if (!InterruptIo::is_enabled() || (io.cell(compare) != 34))
        return false;
    TestTimer::interrupt();
    if ((calls[0] != 0) || (calls[1] != 0))
        return false;
    for (const auto now : {34U, 1'271U}) {
        io.clock = now;
        io.cell(fw::stm32f072::registers::tim2_sr) |= TIM_SR_CC1IF;
        TestTimer::interrupt();
        if ((io.cell(compare) != (now + TestTimer::interval.count())) ||
            ((io.cell(fw::stm32f072::registers::tim2_sr) & TIM_SR_CC1IF) != 0))
            return false;
    }
    constexpr auto gpio_pending = EXTI_BASE + offsetof(EXTI_TypeDef, PR);
    io.cell(gpio_pending) = EXTI_PR_PR2 | EXTI_PR_PR3;
    TestTimer::gio2_interrupt();
    if ((calls[0] != 2) || (calls[1] != 1) || (io.cell(gpio_pending) != EXTI_PR_PR3))
        return false;
    TestTimer::stop();
    io.cell(fw::stm32f072::registers::tim2_sr) |= TIM_SR_CC1IF;
    TestTimer::interrupt();
    io.cell(gpio_pending) |= EXTI_PR_PR2;
    TestTimer::gio2_interrupt();
    return (calls[0] == 2) && (calls[1] == 1) && !InterruptIo::is_enabled() &&
           (TestInterruptGuard::depth == 0);
}

struct RemappedInterruptHardware : fw::i6x::Hardware {
    static constexpr auto radio = [] {
        auto pins = fw::i6x::Hardware::radio;
        pins.status = {GPIOC_BASE, 3};
        return pins;
    }();
};
struct RemappedInterruptScenario {};
bool remapped_radio_interrupt() {
    using Test = coretx::test::RegisterFixture<RemappedInterruptScenario, Registers>;
    using Io = InterruptBackend<Test>;
    using Timer = fw::RadioTimer<Io, TestInterruptGuard, RemappedInterruptHardware>;
    Test::reset_registers();
    Io::enabled = false;
    auto &registers = Test::registers;
    unsigned calls{};
    constexpr auto route = SYSCFG_BASE + offsetof(SYSCFG_TypeDef, EXTICR);
    registers.cell(route) = 0xFFFF;
    Timer::start(
        [](void *const object, const bool gio2) noexcept {
            if (gio2)
                ++*static_cast<unsigned *>(object);
            return Timer::interval;
        },
        &calls);
    if ((registers.cell(route) != 0x2FFF) ||
        ((registers.cell(EXTI_BASE + offsetof(EXTI_TypeDef, IMR)) & EXTI_PR_PR3) == 0))
        return false;
    constexpr auto pending = EXTI_BASE + offsetof(EXTI_TypeDef, PR);
    registers.cell(pending) = EXTI_PR_PR2;
    Timer::gio2_interrupt();
    if ((calls != 0) || (registers.cell(pending) != EXTI_PR_PR2))
        return false;
    registers.cell(pending) |= EXTI_PR_PR3;
    Timer::gio2_interrupt();
    Timer::stop();
    return (calls == 1) && (registers.cell(pending) == EXTI_PR_PR2) && !Io::enabled &&
           (TestInterruptGuard::depth == 0);
}

bool interrupt_driven_radio() {
    struct Scenario {};
    using Test = coretx::test::RegisterFixture<Scenario, Registers>;
    using Driver = fw::A7105<Test::Io, pins>;
    using InterruptIo = InterruptBackend<Test>;
    using TestTimer = fw::RadioTimer<InterruptIo, TestInterruptGuard>;
    using InterruptBus = TimedBus<Test>;

    for (const std::uint32_t base : {0U, 0xFFFF0000U}) {
        Test::reset_registers();
        auto &io = Test::registers;
        io.delayed_spi = true;
        io.clock = base;
        InterruptIo::enabled = false;
        fw::TimedAfhds2a<InterruptBus, TestTimer, TestInterruptGuard> radio{};
        radio.initialize({0x12345678}, {0xCAFEBABE}, coretx::TimestampUs{base});
        radio.activate();
        coretx::afhds2a::Channels channels{};
        unsigned stopped_count{};
        unsigned stale_count{};
        bool cadence_checked{};
        for (std::uint32_t t = 0; t < 90'000; ++t) {
            io.clock = base + t;
            if (((t % 1'500) == 0) && ((t < 30'000) || ((t >= 60'000) && (t < 75'000)))) {
                channels.fill(128);
                radio.service(coretx::TimestampUs{base + t}, channels, true,
                              coretx::TimestampUs{base + t});
                channels.fill(-1024);
            }
            if (t == 30'000) {
                if ((radio.error() != fw::Afhds2aError::none) || (radio.session().frames() < 4) ||
                    (io.minimum_tx_gap < 3'750) || (io.maximum_tx_gap > 3'950) ||
                    (io.aborted_transmissions != 0))
                    return false;
                cadence_checked = true;
            }
            if (t == 40'000)
                stale_count = io.tx_count;
            if ((t == 50'000) && ((io.tx_count != stale_count) ||
                                  (Driver::info().front_end != fw::A7105FrontEnd::off)))
                return false;
            if (t == 75'000) {
                radio.deactivate();
                stopped_count = io.tx_count;
            }
            if (io.clock == io.cell(TIM2_BASE + offsetof(TIM_TypeDef, CCR1)))
                io.cell(fw::stm32f072::registers::tim2_sr) |= TIM_SR_CC1IF;
            // Model a short masked handover, while the main loop itself can stall for 1500 us.
            if (InterruptIo::is_enabled() && !(((t % 1'000) >= 48) && ((t % 1'000) < 60)) &&
                ((io.cell(fw::stm32f072::registers::tim2_sr) & TIM_SR_CC1IF) != 0)) {
                TestTimer::interrupt();
                if ((io.fifo[0] == 0x58) && ((io.fifo[9] | ((io.fifo[10] & 0x0F) << 8)) != 1564) &&
                    (io.byte >= 39))
                    return false;
            }
        }
        TestTimer::stop();
        if (!cadence_checked || (radio.error() != fw::Afhds2aError::none) ||
            (io.tx_count != stopped_count) || (io.tx_count <= stale_count) ||
            (Driver::info().front_end != fw::A7105FrontEnd::off) ||
            (TestInterruptGuard::depth != 0))
            return false;
    }
    return true;
}

bool interrupt_driven_binding(const std::uint32_t access_cost = 0,
                              const std::uint32_t receive_delay = 2'800,
                              const std::uint32_t packet_work = 0) {
    struct Scenario {};
    using Test = coretx::test::RegisterFixture<Scenario, Registers>;
    using Driver = fw::A7105<Test::Io, pins>;
    using InterruptIo = InterruptBackend<Test>;
    using TestTimer = fw::RadioTimer<InterruptIo, TestInterruptGuard>;
    using InterruptBus = TimedBus<Test>;

    Test::reset_registers();
    auto &io = Test::registers;
    io.delayed_spi = true;
    io.auto_bind = true;
    io.access_cost = access_cost;
    io.receive_delay = receive_delay;
    io.packet_work = packet_work;
    InterruptIo::enabled = false;
    fw::TimedAfhds2a<InterruptBus, TestTimer, TestInterruptGuard> radio{};
    radio.initialize({0x12345678}, {}, coretx::TimestampUs{});
    radio.activate();
    bool requested{};
    bool receiver_changed{};
    unsigned tx_interrupt{};
    unsigned rx_interrupt{};
    coretx::afhds2a::Channels channels{};
    std::uint32_t next_main{};
    while (io.clock < 60'000) {
        const auto t = io.clock;
        if (t >= next_main) {
            next_main = t + 1'500;
            radio.service(coretx::TimestampUs{t}, channels, false, coretx::TimestampUs{});
            if (radio.ready() && !requested) {
                if (!radio.bind(coretx::TimestampUs{t}))
                    return false;
                requested = true;
            }
            receiver_changed |= radio.take_receiver_changed();
        }
        if (io.transmitting && ((io.clock - io.transmitted_at) >= io.tx_duration) &&
            (tx_interrupt != io.tx_count)) {
            tx_interrupt = io.tx_count;
            io.cell(EXTI_BASE + offsetof(EXTI_TypeDef, PR)) |= EXTI_PR_PR2;
            TestTimer::gio2_interrupt();
        }
        if (io.receiving && io.rx_available &&
            ((io.clock - io.transmitted_at) >= io.receive_delay) && (rx_interrupt != io.tx_count)) {
            rx_interrupt = io.tx_count;
            io.cell(EXTI_BASE + offsetof(EXTI_TypeDef, PR)) |= EXTI_PR_PR2;
            TestTimer::gio2_interrupt();
        }
        if ((io.clock - io.cell(TIM2_BASE + offsetof(TIM_TypeDef, CCR1))) < 0x80000000U) {
            io.cell(fw::stm32f072::registers::tim2_sr) |= TIM_SR_CC1IF;
            TestTimer::interrupt();
        }
        ++io.clock;
    }
    TestTimer::stop();
    const bool success =
        requested && receiver_changed && !radio.session().binding() &&
        (radio.session().receiver().value == 0xCAFEBABE) && (radio.session().frames() == 5) &&
        (io.tx_count == 5) && (radio.error() == fw::Afhds2aError::none) &&
        (io.aborted_transmissions == 0) && (Driver::info().front_end == fw::A7105FrontEnd::off) &&
        !InterruptIo::is_enabled();
    if (!success)
        std::fprintf(stderr, "Timed bind: error=%u TX=%u\n", static_cast<unsigned>(radio.error()),
                     io.tx_count);
    return success;
}

bool immediate_command_progress() {
    struct Scenario {};
    using Test = coretx::test::RegisterFixture<Scenario, Registers>;
    using Driver = fw::A7105<Test::Io, pins>;

    Test::reset_registers();
    auto &io = Test::registers;

    Driver::initialize(coretx::TimestampUs{});
    run<Driver>(io, 0);
    io.delayed_spi = true;
    io.clock = 10'000;
    std::array<std::uint8_t, 38> packet{};
    if (!Driver::command(fw::a7105::fifo_data, packet, 0, coretx::TimestampUs{10'000}) ||
        (io.byte != 4) || (io.pending != 4) || !Driver::busy() || Driver::completed())
        return false;
    for (std::uint32_t now = 10'000; (now < 12'000) && Driver::busy(); now += 25) {
        io.clock = now;
        Driver::service(coretx::TimestampUs{now});
    }
    if (Driver::busy() || !Driver::completed())
        return false;
    io.delayed_spi = false;
    io.chip[0] = fw::a7105::mode_crc_fail;
    if (!Driver::command(fw::a7105::read | fw::a7105::mode, {}, 1, coretx::TimestampUs{12'000}) ||
        Driver::busy() || !Driver::completed() || (Driver::info().mode != fw::a7105::mode_crc_fail))
        return false;
    io.spi_error = true;
    return !Driver::command(fw::a7105::standby, {}, 0, coretx::TimestampUs{12'001}) &&
           (Driver::info().state == fw::A7105State::failed) &&
           (Driver::info().error == fw::A7105Error::spi) && !Driver::busy() && !io.selected;
}

bool stalled_fifo_times_out() {
    struct Scenario {};
    using Test = coretx::test::RegisterFixture<Scenario, Registers>;
    using Driver = fw::A7105<Test::Io, pins>;

    Test::reset_registers();
    auto &io = Test::registers;

    Driver::initialize(coretx::TimestampUs{});
    run<Driver>(io, 0);
    io.delayed_spi = true;
    Driver::front_end(fw::A7105FrontEnd::transmit);
    std::array<std::uint8_t, 38> packet{};
    if (!Driver::command(fw::a7105::fifo_data, packet, 0, coretx::TimestampUs{10'000}))
        return false;
    std::uint32_t stalled_at{};
    for (std::uint32_t now = 10'000; now < 13'000; now += 50) {
        io.clock = now;
        Driver::service(coretx::TimestampUs{now});
        if ((io.byte >= 6) && !io.block_receive) {
            io.block_receive = true;
            stalled_at = now;
        }
        if (Driver::info().state == fw::A7105State::failed)
            return ((now - stalled_at) == 1'000) &&
                   (Driver::info().error == fw::A7105Error::timeout) &&
                   (Driver::info().front_end == fw::A7105FrontEnd::off) && !io.selected &&
                   ((io.cell(GPIOE_BASE + offsetof(GPIO_TypeDef, ODR)) & 0x300U) == 0x300U);
    }
    return false;
}

static_assert(page_selection());
static_assert(afhds_format_rendering());
static_assert(preparation_timeout_status());
static_assert(paired_link_status());

} // namespace

int main() {
    if (!initialization() || !failures() || !disabled_safety() || !page_selection() ||
        !afhds_format_rendering() || !fifo_transfers() || !fifo_transfers(true) ||
        !binding_with_delayed_spi() || !binding_with_delayed_spi(150) ||
        !binding_with_delayed_spi(250) || !steady_radio(50) ||
        !binding_with_delayed_spi(10, 0, fw::Afhds2aError::none, true) ||
        !steady_radio(150, fw::Afhds2aError::preparation_timeout) ||
        !binding_with_delayed_spi(50, 950) || !timer_wrap_delay_and_stop() ||
        !remapped_radio_interrupt() || !interrupt_driven_radio() || !interrupt_driven_binding() ||
        !interrupt_driven_binding(1, 3'300, 60) ||
        !binding_with_delayed_spi(50, 5'000, fw::Afhds2aError::tx_timeout) ||
        !stalled_fifo_times_out() || !frontend_switching() || !immediate_command_progress() ||
        !preparation_timeout_status() || !paired_link_status()) {
        std::fputs("A7105 tests failed\n", stderr);
        return 1;
    }
}
