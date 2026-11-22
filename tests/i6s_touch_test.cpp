// SPDX-License-Identifier: GPL-2.0-or-later
#include "static_fixture.hpp"
#include "tests/renderers/touch.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace {

namespace hw = coretx::firmware::i6s;
using reg = hw::touch_registers;
using coretx::firmware::Key;

struct Registers {
    struct Cell {
        std::uintptr_t address{};
        std::uint32_t value{};
    };
    std::array<Cell, 40> cells{};
    std::size_t count{};
    std::array<std::uint8_t, 16> transmitted{};
    std::size_t writes{};
    std::array<std::uint8_t, 7> packet{0, 0, 1, 0, 64, 0, 64};
    std::size_t position{};
    std::uint32_t remaining{};
    std::uint32_t clock{};
    std::uint32_t write_cost{};
    bool automatic{};

    constexpr std::uint32_t now_us() noexcept { return clock; }

    constexpr std::uint32_t &cell(const std::uintptr_t address) {
        for (std::size_t i = 0; i < count; ++i) {
            if (cells[i].address == address)
                return cells[i].value;
        }
        if (count == cells.size())
            std::abort();
        cells[count].address = address;
        return cells[count++].value;
    }
    constexpr std::uint32_t read(const std::uintptr_t address) {
        if (address != reg::rxdr)
            return cell(address);
        const auto byte = packet[position++];
        if (--remaining == 0)
            cell(reg::isr) = I2C_ISR_STOPF;
        return byte;
    }
    constexpr void write(const std::uintptr_t address, const std::uint32_t value) {
        clock += write_cost;
        cell(address) = value;
        if (address == reg::icr) {
            cell(reg::isr) &= ~value;
        } else if ((address == reg::cr1) && (value == 0)) {
            cell(reg::isr) = 0;
        } else if ((address == reg::cr2) && ((value & I2C_CR2_START) != 0)) {
            remaining = (value >> I2C_CR2_NBYTES_Pos) & 255U;
            automatic = (value & I2C_CR2_AUTOEND) != 0;
            if ((value & I2C_CR2_RD_WRN) != 0) {
                position = 0;
                cell(reg::isr) = I2C_ISR_RXNE | I2C_ISR_BUSY;
            } else {
                cell(reg::isr) = I2C_ISR_TXIS | I2C_ISR_BUSY;
            }
        } else if (address == reg::txdr) {
            if (writes == transmitted.size())
                std::abort();
            transmitted[writes++] = static_cast<std::uint8_t>(value);
            if (--remaining == 0) {
                cell(reg::isr) = automatic ? I2C_ISR_STOPF : I2C_ISR_TC | I2C_ISR_BUSY;
            }
        } else if (address == (GPIOA_BASE + offsetof(GPIO_TypeDef, BSRR))) {
            auto &odr = cell(GPIOA_BASE + offsetof(GPIO_TypeDef, ODR));
            odr = (odr & ~(value >> 16)) | (value & 65535U);
        }
    }
};

struct ResetWaits {};
struct Hardware {};
struct PartialFailure {};
struct TransferFailure {};
struct PacketFailure {};
template <typename Scenario> using Fixture = coretx::test::RegisterFixture<Scenario, Registers>;

template <typename Driver> bool start(Registers &io, const std::uint32_t base = 0) {
    const auto written = io.writes;
    io.clock = base;
    io.cell(reg::interrupt) = 1U << 12;
    Driver::initialize();
    if (Driver::ready() ||
        (Driver::service(coretx::TimestampUs{base + 20'000U}) != hw::TouchEvent::none) ||
        ((io.cell(GPIOA_BASE + offsetof(GPIO_TypeDef, ODR)) & (1U << 15)) != 0))
        return false;
    io.clock = base + 20'001U;
    (void)Driver::service(coretx::TimestampUs{io.clock});
    if ((io.cell(GPIOA_BASE + offsetof(GPIO_TypeDef, ODR)) & (1U << 15)) == 0)
        return false;
    for (std::uint32_t i = 80'002; i < 80'050; ++i) {
        (void)Driver::service(coretx::TimestampUs{base + i});
    }
    return Driver::ready() && (Driver::error() == hw::TouchError::none) &&
           (io.writes == (written + 6)) && (io.transmitted[written] == 0xA4) &&
           (io.transmitted[written + 1] == 0) && (io.transmitted[written + 2] == 0x80) &&
           (io.transmitted[written + 3] == 22) && (io.transmitted[written + 4] == 0x88) &&
           (io.transmitted[written + 5] == 14);
}

constexpr bool decoding() {
    hw::TouchSample sample{};
    std::array<std::uint8_t, 7> bytes{0, 0x14, 1, 0, 64, 0, 64};
    if (!hw::decode_touch(bytes, sample) || (sample.raw_x != 64) || (sample.raw_y != 64) ||
        (sample.x != 96) || (sample.y != 32) || (sample.gesture != hw::Gesture::right))
        return false;
    bytes = {0, 0, 1, 0x0F, 255, 0x0F, 255};
    if (!hw::decode_touch(bytes, sample) || (sample.x != 0) || (sample.y != 63) ||
        (sample.raw_x != 4095))
        return false;
    bytes = {0, 0, 1, 0, 0, 0, 0};
    if (!hw::decode_touch(bytes, sample) || (sample.x != 127) || (sample.y != 0))
        return false;
    bytes[3] = 0x40;
    if (!hw::decode_touch(bytes, sample) || (sample.contacts != 0))
        return false;
    bytes[2] = 2;
    if (!hw::decode_touch(bytes, sample) || (sample.contacts != 2))
        return false;
    bytes[2] = 3;
    if (hw::decode_touch(bytes, sample))
        return false;
    bytes[2] = 1;
    bytes[3] = 0xC0;
    return !hw::decode_touch(bytes, sample);
}

bool reset_waits() {
    using Test = Fixture<ResetWaits>;
    using Driver = hw::Touch<Test::Io>;
    Test::reset_registers();
    auto &io = Test::registers;
    io.write_cost = 50;
    Driver::initialize();
    const auto requested = io.clock + 20'001U;
    io.clock = requested;
    (void)Driver::service(coretx::TimestampUs{requested});
    // The boot deadline starts after the GPIO write, not at entry to service().
    for (std::uint32_t i = 60'001; i < 60'020; ++i) {
        (void)Driver::service(coretx::TimestampUs{requested + i});
    }
    return (io.cell(reg::cr2) == 0) && (io.writes == 0) && !Driver::ready();
}

constexpr bool navigation() {
    for (const std::uint32_t base : {0U, 0xFFFFF000U}) {
        constexpr std::array expected{Key::up, Key::down, Key::ok, Key::cancel};
        for (unsigned direction = 0; direction < 4; ++direction) {
            hw::TouchNavigation nav{};
            hw::TouchSample sample{0, 0, 64, 32, 1, hw::Gesture::none};
            if (nav.service(coretx::TimestampUs{base}, &sample, true) != Key::none)
                return false;
            sample.x = direction == 2 ? 80 : direction == 3 ? 48 : 64;
            sample.y = direction == 0 ? 16 : direction == 1 ? 48 : 32;
            if (nav.service(coretx::TimestampUs{base + 1U}, &sample, true) != expected[direction])
                return false;
            sample.gesture = hw::Gesture::right;
            if (nav.service(coretx::TimestampUs{base + 2U}, &sample, true) != Key::none)
                return false;
            sample.contacts = 0;
            if (nav.service(coretx::TimestampUs{base + 3U}, &sample, true) != Key::none)
                return false;
            if (nav.service(coretx::TimestampUs{base + 4U}, &sample, true) != Key::none)
                return false;
        }
        hw::TouchNavigation nav{};
        hw::TouchSample sample{0, 0, 64, 32, 1, hw::Gesture::none};
        (void)nav.service(coretx::TimestampUs{base}, &sample, true);
        sample.x = 76;
        sample.y = 44;
        if (nav.service(coretx::TimestampUs{base + 1U}, &sample, true) != Key::none)
            return false;
        if (nav.service(coretx::TimestampUs{base + 40'001U}, nullptr, true) != Key::none)
            return false;
        sample.gesture = hw::Gesture::right;
        if (nav.service(coretx::TimestampUs{base + 40'002U}, &sample, true) != Key::none)
            return false;
        sample.contacts = 0;
        sample.gesture = hw::Gesture::none;
        (void)nav.service(coretx::TimestampUs{base + 40'003U}, &sample, true);
        sample.contacts = 1;
        (void)nav.service(coretx::TimestampUs{base + 40'004U}, &sample, true);
        sample.gesture = hw::Gesture::left;
        if (nav.service(coretx::TimestampUs{base + 40'005U}, &sample, true) != Key::cancel)
            return false;
        if (nav.service(coretx::TimestampUs{base + 40'006U}, &sample, false) != Key::none)
            return false;
        if (nav.service(coretx::TimestampUs{base + 40'007U}, &sample, true) != Key::none)
            return false;
    }
    hw::TouchNavigation multi{};
    hw::TouchSample sample{0, 0, 64, 32, 2, hw::Gesture::right};
    if (multi.service(coretx::TimestampUs{0}, &sample, true) != Key::none)
        return false;
    sample.contacts = 1;
    if (multi.service(coretx::TimestampUs{1}, &sample, true) != Key::none)
        return false;
    sample.contacts = 0;
    if (multi.service(coretx::TimestampUs{2}, &sample, true) != Key::none)
        return false;
    sample.contacts = 1;
    sample.gesture = hw::Gesture::none;
    if (multi.service(coretx::TimestampUs{3}, &sample, true) != Key::none)
        return false;
    sample.x = 80;
    return multi.service(coretx::TimestampUs{4}, &sample, true) == Key::ok;
}

bool hardware() {
    using Test = Fixture<Hardware>;
    using Driver = hw::Touch<Test::Io>;
    for (const std::uint32_t base : {0U, 0xFFFFF000U}) {
        Test::reset_registers();
        auto &io = Test::registers;
        io.cell(GPIOB_BASE + offsetof(GPIO_TypeDef, MODER)) = 1U << 30;
        io.cell(GPIOB_BASE + offsetof(GPIO_TypeDef, ODR)) = 1U << 15;
        if (!start<Driver>(io, base))
            return false;
        const auto mode = io.cell(GPIOB_BASE + offsetof(GPIO_TypeDef, MODER));
        const auto af = io.cell(GPIOB_BASE + offsetof(GPIO_TypeDef, AFR) + sizeof(std::uint32_t));
        if ((((mode >> 16) & 15U) != 10U) || ((af & 255U) != 0x11U) || ((mode >> 30) != 1U) ||
            (io.cell(GPIOB_BASE + offsetof(GPIO_TypeDef, ODR)) != (1U << 15)))
            return false;
        hw::TouchNavigation nav{};
        hw::TouchSample released{};
        (void)nav.service(coretx::TimestampUs{base + 80'050U}, nullptr, false);
        (void)nav.service(coretx::TimestampUs{base + 80'051U}, &released, true);
        io.cell(reg::interrupt) = 0;
        std::uint32_t actions{};
        for (std::uint32_t i = 80'052; i < 80'100; ++i) {
            const auto event = Driver::service(coretx::TimestampUs{base + i});
            const auto key = nav.service(
                coretx::TimestampUs{base + i},
                event == hw::TouchEvent::sample ? &Driver::sample() : nullptr, Driver::ready());
            if (key != Key::none)
                ++actions;
        }
        if ((Driver::frames() != 1) || (Driver::sample().x != 96) || (actions != 0))
            return false;
        io.packet[6] = 32;
        for (std::uint32_t i = 88'052; i < 88'100; ++i) {
            const auto event = Driver::service(coretx::TimestampUs{base + i});
            const auto key = nav.service(
                coretx::TimestampUs{base + i},
                event == hw::TouchEvent::sample ? &Driver::sample() : nullptr, Driver::ready());
            if (key != Key::none) {
                if (key != Key::ok)
                    return false;
                ++actions;
            }
        }
        if ((Driver::frames() != 2) || (actions != 1))
            return false;
        io.cell(reg::interrupt) = 1U << 12;
        if ((Driver::service(coretx::TimestampUs{base + 88'101U}) != hw::TouchEvent::sample) ||
            (Driver::sample().contacts != 0))
            return false;
    }
    return true;
}

bool failures() {
    using PartialTest = Fixture<PartialFailure>;
    using Partial = hw::Touch<PartialTest::Io>;
    using TransferTest = Fixture<TransferFailure>;
    using Transfer = hw::Touch<TransferTest::Io>;
    using PacketTest = Fixture<PacketFailure>;
    using Packet = hw::Touch<PacketTest::Io>;
    constexpr std::array masks{std::uint32_t{I2C_ISR_NACKF}, std::uint32_t{I2C_ISR_BERR},
                               std::uint32_t{I2C_ISR_ARLO}, std::uint32_t{I2C_ISR_OVR},
                               std::uint32_t{I2C_ISR_BUSY}};
    constexpr std::array errors{hw::TouchError::nack, hw::TouchError::bus,
                                hw::TouchError::arbitration, hw::TouchError::overrun,
                                hw::TouchError::timeout};
    for (std::size_t i = 0; i < masks.size(); ++i) {
        {
            PartialTest::reset_registers();
            auto &registers = PartialTest::registers;
            if (!start<Partial>(registers))
                return false;
            registers.cell(reg::interrupt) = 0;
            for (std::uint32_t now = 80'052; (now < 80'070) && (registers.position < 3); ++now) {
                (void)Partial::service(coretx::TimestampUs{now});
            }
            registers.cell(reg::isr) |= I2C_ISR_NACKF;
            if ((Partial::service(coretx::TimestampUs{80'071}) != hw::TouchEvent::failed) ||
                (Partial::error() != hw::TouchError::nack) || Partial::received() ||
                (Partial::frames() != 0))
                return false;
        }
        TransferTest::reset_registers();
        auto &io = TransferTest::registers;
        if (!start<Transfer>(io))
            return false;
        io.cell(reg::interrupt) = 0;
        (void)Transfer::service(coretx::TimestampUs{80'052});
        io.cell(reg::isr) = masks[i];
        const auto result = Transfer::service(coretx::TimestampUs{i == 4 ? 90'052U : 80'053U});
        if ((result != hw::TouchEvent::failed) || (Transfer::error() != errors[i]) ||
            Transfer::ready() || Transfer::received() || (io.cell(reg::cr1) != 0))
            return false;
        if ((Transfer::service(coretx::TimestampUs{100'000}) != hw::TouchEvent::failed) ||
            (io.writes != 6))
            return false;
    }
    PacketTest::reset_registers();
    auto &io = PacketTest::registers;
    if (!start<Packet>(io))
        return false;
    io.packet[2] = 3;
    io.cell(reg::interrupt) = 0;
    for (std::uint32_t i = 80'052; i < 80'100; ++i)
        (void)Packet::service(coretx::TimestampUs{i});
    if ((Packet::error() != hw::TouchError::packet) || Packet::received())
        return false;
    return start<Packet>(io) && !Packet::received();
}

constexpr bool rendering() {
    coretx::ui::Framebuffer display{};
    coretx::ui::Framebuffer expected{};
    const hw::TouchSample sample{64, 64, 96, 32, 1, hw::Gesture::none};
    hw::render_touch(display, sample, true, true, hw::TouchError::timeout, Key::none, 0);
    expected.text(0, 18, coretx::ui::tr::timeout);
    expected.text(49, 27, "----");
    expected.text(103, 27, "----");
    for (int y = 18; y < 25; ++y) {
        for (int x = 0; x < 128; ++x) {
            if (display.pixel_at(x, y) != expected.pixel_at(x, y))
                return false;
        }
    }
    for (int y = 27; y < 34; ++y) {
        for (int x = 49; x < 73; ++x) {
            if (display.pixel_at(x, y) != expected.pixel_at(x, y))
                return false;
        }
    }
    return true;
}

static_assert(decoding());
static_assert(navigation());
static_assert(rendering());

} // namespace

int main() {
    if (!decoding() || !reset_waits() || !navigation() || !hardware() || !failures() ||
        !rendering()) {
        std::fputs("i6S touch tests failed\n", stderr);
        return 1;
    }
}
