// SPDX-License-Identifier: GPL-2.0-or-later
#define CORETX_I6S_TOUCH_TRACE
#include "boards/common/sender_input.hpp"
#include "boards/i6s/setup_controls.hpp"
#include "static_fixture.hpp"
#include "tests/renderers/touch.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace {

namespace hw = coretx::firmware::i6s;
using reg = hw::touch_registers;
using coretx::firmware::Key;
constexpr auto exti_pending = EXTI_BASE + offsetof(EXTI_TypeDef, PR);
constexpr auto touch_line = std::uint32_t{1} << 12;

struct Registers {
    struct Cell {
        std::uintptr_t address{};
        std::uint32_t value{};
    };
    std::array<Cell, 48> cells{};
    std::size_t count{};
    std::array<std::uint8_t, 256> transmitted{};
    std::size_t writes{};
    std::array<std::uint8_t, 7> packet{0, 0, 1, 0, 64, 0, 64};
    std::size_t position{};
    std::uint32_t remaining{};
    std::uint32_t clock{};
    std::uint32_t write_cost{};
    std::uint32_t poll_cost{};
    std::uint32_t byte_delay{};
    std::uint32_t ready_at{};
    std::uint32_t pending_flags{};
    std::size_t nack_at{};
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
    constexpr void ready_after_byte(const std::uint32_t flags) {
        if (byte_delay == 0) {
            cell(reg::isr) = flags;
        } else {
            pending_flags = flags;
            ready_at = clock + byte_delay;
            cell(reg::isr) = flags & I2C_ISR_BUSY;
        }
    }
    constexpr std::uint32_t read(const std::uintptr_t address) {
        if (address == reg::isr) {
            clock += poll_cost;
            if ((pending_flags != 0) && (clock - ready_at < 0x80000000U)) {
                cell(reg::isr) = pending_flags;
                pending_flags = 0;
            }
        }
        if (address != reg::rxdr)
            return cell(address);
        const auto byte = packet[position++];
        if (--remaining == 0)
            ready_after_byte(I2C_ISR_STOPF);
        else
            ready_after_byte(I2C_ISR_RXNE | I2C_ISR_BUSY);
        if (position == nack_at) {
            pending_flags = 0;
            cell(reg::isr) |= I2C_ISR_NACKF;
        }
        return byte;
    }
    constexpr void write(const std::uintptr_t address, const std::uint32_t value) {
        clock += write_cost;
        if (address == exti_pending) {
            cell(address) &= ~value;
            return;
        }
        cell(address) = value;
        if (address == reg::icr) {
            cell(reg::isr) &= ~value;
        } else if ((address == reg::cr1) && (value == 0)) {
            cell(reg::isr) = 0;
            pending_flags = 0;
        } else if ((address == reg::cr2) && ((value & I2C_CR2_START) != 0)) {
            remaining = (value >> I2C_CR2_NBYTES_Pos) & 255U;
            automatic = (value & I2C_CR2_AUTOEND) != 0;
            if ((value & I2C_CR2_RD_WRN) != 0) {
                position = 0;
                ready_after_byte(I2C_ISR_RXNE | I2C_ISR_BUSY);
            } else {
                ready_after_byte(I2C_ISR_TXIS | I2C_ISR_BUSY);
            }
        } else if (address == reg::txdr) {
            if (writes == transmitted.size())
                std::abort();
            transmitted[writes++] = static_cast<std::uint8_t>(value);
            if (--remaining == 0) {
                ready_after_byte(automatic ? I2C_ISR_STOPF : I2C_ISR_TC | I2C_ISR_BUSY);
            }
        } else {
            for (const auto port : {GPIOA_BASE, GPIOD_BASE}) {
                if (address == (port + offsetof(GPIO_TypeDef, BSRR))) {
                    auto &odr = cell(port + offsetof(GPIO_TypeDef, ODR));
                    odr = (odr & ~(value >> 16)) | (value & 65535U);
                }
            }
        }
    }
};

struct ResetWaits {};
struct Hardware {};
struct DelayedReports {};
struct LatchedInterrupt {};
struct TraceGaps {};
struct SparseReports {};
struct CooperativeReports {};
struct ReportTaps {};
struct PolledReleaseTaps {};
struct SetupGpio {};
struct RecoveringReports {};
struct InferredTaps {};
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
        if (Driver::ready())
            break;
    }
    return Driver::ready() && (Driver::error() == hw::TouchError::none) && (io.writes == written);
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
        const auto expired = base + hw::TouchNavigation::release_timeout_us.count();
        if (nav.service(coretx::TimestampUs{expired + 1U}, nullptr, true) != Key::none)
            return false;
        sample.gesture = hw::Gesture::right;
        if (nav.service(coretx::TimestampUs{expired + 2U}, &sample, true) != Key::none)
            return false;
        sample.contacts = 0;
        sample.gesture = hw::Gesture::none;
        (void)nav.service(coretx::TimestampUs{expired + 3U}, &sample, true);
        sample.contacts = 1;
        (void)nav.service(coretx::TimestampUs{expired + 4U}, &sample, true);
        sample.gesture = hw::Gesture::left;
        if (nav.service(coretx::TimestampUs{expired + 5U}, &sample, true) != Key::cancel)
            return false;
        if (nav.service(coretx::TimestampUs{expired + 6U}, &sample, false) != Key::none)
            return false;
        if (nav.service(coretx::TimestampUs{expired + 7U}, &sample, true) != Key::none)
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

constexpr bool fast_swipes() {
    constexpr std::array gestures{hw::Gesture::up, hw::Gesture::down, hw::Gesture::right,
                                  hw::Gesture::left};
    constexpr std::array keys{Key::up, Key::down, Key::ok, Key::cancel};
    for (const auto lift_contacts : {std::uint8_t{0}, std::uint8_t{1}}) {
        for (unsigned direction = 0; direction < gestures.size(); ++direction) {
            hw::TouchNavigation hardware{};
            hw::TouchSample sample{0, 0, 64, 32, 1, gestures[direction]};
            if ((hardware.service(coretx::TimestampUs{0}, &sample, true) != keys[direction]) ||
                (hardware.service(coretx::TimestampUs{1}, &sample, true) != Key::none))
                return false;
            hw::TouchNavigation coordinates{};
            std::array<std::uint8_t, 7> packet{0, 0, 1, 0, 64, 0, 128};
            if (!hw::decode_touch(packet, sample) ||
                (coordinates.service(coretx::TimestampUs{0}, &sample, true) != Key::none))
                return false;
            packet[3] = 0x40;
            packet[2] = lift_contacts;
            packet[4] = direction == 0 ? 32 : direction == 1 ? 96 : 64;
            packet[6] = direction == 2 ? 96 : direction == 3 ? 160 : 128;
            if (!hw::decode_touch(packet, sample) ||
                (coordinates.service(coretx::TimestampUs{10'000}, &sample, true) !=
                 keys[direction]) ||
                (coordinates.service(coretx::TimestampUs{10'001}, &sample, true) != Key::none))
                return false;
        }
    }
    hw::TouchNavigation release{};
    hw::TouchSample sample{0, 0, 64, 32, 1, hw::Gesture::none};
    (void)release.service(coretx::TimestampUs{0}, &sample, true);
    const std::array<std::uint8_t, 7> packet{0, 0, 0, 0, 0, 0, 0};
    return hw::decode_touch(packet, sample) &&
           (release.service(coretx::TimestampUs{10'000}, &sample, true) == Key::none);
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
        const auto initial_writes = io.writes;
        for (std::uint32_t i = 80'050; i < 80'052; ++i)
            if (Driver::service(coretx::TimestampUs{base + i}) != hw::TouchEvent::none)
                return false;
        if ((io.writes != initial_writes) || Driver::received())
            return false;
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
        io.cell(reg::interrupt) = 1U << 12;
        for (std::uint32_t i = 80'100; i < 88'052; i += 1000)
            if ((Driver::service(coretx::TimestampUs{base + i}) != hw::TouchEvent::none) ||
                (Driver::sample().contacts != 1))
                return false;
        io.cell(reg::interrupt) = 0;
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
        if ((Driver::service(coretx::TimestampUs{base + 88'101U}) != hw::TouchEvent::none) ||
            (Driver::sample().contacts != 1))
            return false;
        io.packet[2] = 0;
        io.cell(reg::interrupt) = 0;
        for (std::uint32_t i = 96'052; i < 96'100; ++i) {
            const auto event = Driver::service(coretx::TimestampUs{base + i});
            (void)nav.service(coretx::TimestampUs{base + i},
                              event == hw::TouchEvent::sample ? &Driver::sample() : nullptr, true);
        }
        if ((Driver::frames() != 3) || (Driver::sample().contacts != 0))
            return false;
        io.packet[2] = 1;
        bool long_press{};
        coretx::TimestampUs last_sample{};
        for (std::uint32_t batch = 0; batch < 43; ++batch) {
            io.cell(reg::interrupt) = 0;
            for (std::uint32_t step = 0; step < 48; ++step) {
                const auto now = coretx::TimestampUs{base + 104'052U + batch * 20'000U + step};
                const auto event = Driver::service(now);
                if (event == hw::TouchEvent::sample)
                    last_sample = now;
                const auto key = nav.service(
                    now, event == hw::TouchEvent::sample ? &Driver::sample() : nullptr, true);
                if (key == Key::ok_long) {
                    if (long_press)
                        return false;
                    long_press = true;
                } else if (key != Key::none)
                    return false;
            }
            io.cell(reg::interrupt) = 1U << 12;
            const auto now = coretx::TimestampUs{base + 104'052U + batch * 20'000U + 10'000U};
            const auto event = Driver::service(now);
            if ((event != hw::TouchEvent::sample) || (Driver::sample().contacts != 1) ||
                (nav.service(now, &Driver::sample(), true) != Key::none))
                return false;
            last_sample = now;
        }
        if (!long_press || (Driver::sample().contacts != 1))
            return false;
        const auto released_at = last_sample + hw::TouchNavigation::release_timeout_us;
        const auto event = Driver::service(released_at);
        if ((event != hw::TouchEvent::sample) || (Driver::sample().contacts != 0))
            return false;
        (void)nav.service(released_at, &Driver::sample(), true);
        if (Driver::service(released_at + coretx::DurationUs{1000}) != hw::TouchEvent::none)
            return false;
        io.cell(reg::interrupt) = 0;
        io.packet[6] = 64;
        for (unsigned step = 0; step < 48; ++step) {
            const auto now = released_at + coretx::DurationUs{1000U + step};
            const auto next = Driver::service(now);
            if (nav.service(now, next == hw::TouchEvent::sample ? &Driver::sample() : nullptr,
                            true) != Key::none)
                return false;
        }
        io.packet[6] = 32;
        bool swiped{};
        for (unsigned step = 0; step < 48; ++step) {
            const auto now = released_at + coretx::DurationUs{9000U + step};
            const auto next = Driver::service(now);
            const auto key = nav.service(
                now, next == hw::TouchEvent::sample ? &Driver::sample() : nullptr, true);
            if (key == Key::ok)
                swiped = true;
            else if (key != Key::none)
                return false;
        }
        if (!swiped)
            return false;
    }
    return true;
}

bool delayed_reports() {
    using Test = Fixture<DelayedReports>;
    using Driver = hw::Touch<Test::Io>;
    for (const std::uint32_t base : {0U, 0xFFFFF000U}) {
        Test::reset_registers();
        auto &io = Test::registers;
        if (!start<Driver>(io, base))
            return false;
        hw::TouchNavigation nav{};
        unsigned long_presses{};
        coretx::TimestampUs last_sample{};
        for (std::uint32_t batch = 0; batch < 27; ++batch) {
            io.cell(reg::interrupt) = 0;
            bool received{};
            for (std::uint32_t step = 0; step < 12; ++step) {
                const auto now =
                    coretx::TimestampUs{base + 100'000U + batch * 33'000U + step * 750U};
                const auto event = Driver::service(now);
                if (event == hw::TouchEvent::sample) {
                    if (Driver::sample().contacts != 1)
                        return false;
                    received = true;
                    last_sample = now;
                }
                const auto key = nav.service(
                    now, event == hw::TouchEvent::sample ? &Driver::sample() : nullptr, true);
                if (key == Key::ok_long)
                    ++long_presses;
                else if (key != Key::none)
                    return false;
            }
            if (!received || (Driver::error() != hw::TouchError::none))
                return false;
            io.cell(reg::interrupt) = 1U << 12;
            const auto now = coretx::TimestampUs{base + 132'000U + batch * 33'000U};
            if ((Driver::service(now) != hw::TouchEvent::sample) ||
                (Driver::sample().contacts != 1))
                return false;
            last_sample = now;
            const auto key = nav.service(now, &Driver::sample(), true);
            if (key == Key::ok_long)
                ++long_presses;
            else if (key != Key::none)
                return false;
        }
        const auto released_at = last_sample + hw::TouchNavigation::release_timeout_us;
        if ((long_presses != 1) || (Driver::service(released_at) != hw::TouchEvent::sample) ||
            (Driver::sample().contacts != 0) ||
            (Driver::service(released_at + coretx::DurationUs{1000}) != hw::TouchEvent::none))
            return false;
    }
    return true;
}

bool latched_interrupt() {
    using Test = Fixture<LatchedInterrupt>;
    using Driver = hw::Touch<Test::Io>;
    using Edge = coretx::firmware::stm32f072::ExtiTraits<hw::Hardware::touch_interrupt>;
    constexpr auto mask = EXTI_BASE + offsetof(EXTI_TypeDef, IMR);
    constexpr auto falling = EXTI_BASE + offsetof(EXTI_TypeDef, FTSR);
    constexpr auto rising = EXTI_BASE + offsetof(EXTI_TypeDef, RTSR);
    for (const std::uint32_t base : {0U, 0xFFFFF000U}) {
        Test::reset_registers();
        auto &io = Test::registers;
        io.cell(Edge::route_register) = ~Edge::route_mask;
        io.cell(mask) = touch_line | 4U;
        io.cell(falling) = 4U;
        io.cell(rising) = touch_line | 4U;
        io.cell(exti_pending) = touch_line | 4U;
        if (!start<Driver>(io, base) || (io.cell(mask) != 4U) ||
            (io.cell(falling) != (touch_line | 4U)) || (io.cell(rising) != 4U) ||
            (io.cell(exti_pending) != 4U) ||
            (io.cell(Edge::route_register) != (~Edge::route_mask | Edge::route)))
            return false;
        hw::TouchNavigation nav{};
        unsigned long_presses{};
        coretx::TimestampUs last_sample{};
        for (std::uint32_t batch = 0; batch < 43; ++batch) {
            // The falling pulse has ended before service runs; another EXTI line is pending too.
            io.cell(exti_pending) |= touch_line | 4U;
            bool received{};
            for (std::uint32_t step = 0; step < 48; ++step) {
                const auto now = coretx::TimestampUs{base + 100'000U + batch * 20'000U + step};
                const auto event = Driver::service(now);
                if (event == hw::TouchEvent::sample) {
                    if (Driver::sample().contacts != 1)
                        return false;
                    received = true;
                    last_sample = now;
                }
                const auto key = nav.service(
                    now, event == hw::TouchEvent::sample ? &Driver::sample() : nullptr, true);
                if (key == Key::ok_long)
                    ++long_presses;
                else if (key != Key::none)
                    return false;
            }
            if (!received || (io.cell(exti_pending) != 4U) ||
                (Driver::error() != hw::TouchError::none))
                return false;
            if (batch < 42) {
                io.cell(exti_pending) |= touch_line;
                const auto now = coretx::TimestampUs{base + 107'000U + batch * 20'000U};
                if ((Driver::service(now) != hw::TouchEvent::sample) ||
                    (io.cell(exti_pending) != 4U))
                    return false;
                last_sample = now;
                const auto key = nav.service(now, &Driver::sample(), true);
                if (key == Key::ok_long)
                    ++long_presses;
                else if (key != Key::none)
                    return false;
            }
        }
        if ((long_presses != 1) || (Driver::frames() != 85))
            return false;
        io.packet[2] = 0;
        const auto released_at = last_sample + coretx::DurationUs{8000};
        if ((Driver::service(released_at) != hw::TouchEvent::sample) ||
            (Driver::sample().contacts != 0))
            return false;
        const auto written = io.writes;
        for (std::uint32_t elapsed = 1000;
             elapsed < hw::TouchNavigation::release_timeout_us.count(); elapsed += 1000)
            if (Driver::service(released_at + coretx::DurationUs{elapsed}) != hw::TouchEvent::none)
                return false;
        if ((io.writes != written) || (Driver::sample().contacts != 0))
            return false;
    }
    return true;
}

bool trace_gaps() {
    using Test = Fixture<TraceGaps>;
    using Driver = hw::Touch<Test::Io>;
    Test::reset_registers();
    auto &io = Test::registers;
    if (!start<Driver>(io))
        return false;
    coretx::firmware::TouchInput<Driver> input{};
    constexpr auto timeout = hw::TouchNavigation::release_timeout_us.count();
    for (const std::uint32_t started : {100'000U, 150'000U + timeout}) {
        io.cell(reg::interrupt) = 0;
        for (std::uint32_t step = 0; step < 48; ++step)
            (void)input.service(coretx::TimestampUs{started + step}, true);
        if (Driver::sample().contacts != 1)
            return false;
        if (started == 100'000U) {
            io.cell(reg::interrupt) = touch_line;
            (void)input.service(coretx::TimestampUs{100'011U + timeout}, true);
            if (Driver::sample().contacts != 0)
                return false;
        }
    }
    coretx::ui::Framebuffer display{}, expected{};
    input.render_trace(display, Key::none);
    expected.compact();
    expected.unsigned_number(79, 48, 50 + timeout / 1000);
    expected.unsigned_number(63, 56, timeout / 1000 - 1);
    for (int y = 48; y < 62; ++y)
        for (int x = y < 56 ? 56 : 8; x < (y < 56 ? 80 : 64); ++x)
            if (display.pixel_at(x, y) != expected.pixel_at(x, y))
                return false;
    return true;
}

bool sparse_reports() {
    using Test = Fixture<SparseReports>;
    using Driver = hw::Touch<Test::Io>;
    for (const std::uint32_t base : {0U, 0xFFFFF000U}) {
        Test::reset_registers();
        auto &io = Test::registers;
        if (!start<Driver>(io, base))
            return false;
        coretx::firmware::TouchInput<Driver> input{};
        unsigned long_presses{};
        std::uint32_t frames{};
        coretx::TimestampUs last_sample{};
        for (std::uint32_t elapsed = 0; elapsed <= 1'100'000; elapsed += 500) {
            io.cell(reg::interrupt) = elapsed % 242'000 < 2000 ? 0 : touch_line;
            const auto now = coretx::TimestampUs{base + 100'000U + elapsed};
            const auto key = input.service(now, true, {false, 0, true});
            if (Driver::frames() != frames) {
                frames = Driver::frames();
                last_sample = now;
            }
            if (Driver::received() && (Driver::sample().contacts != 1))
                return false;
            if (key == Key::ok_long)
                ++long_presses;
            else if (key != Key::none)
                return false;
        }
        if ((long_presses != 1) || (Driver::frames() <= 5) ||
            (Driver::error() != hw::TouchError::none))
            return false;
        const auto released_at = last_sample + hw::TouchNavigation::release_timeout_us;
        if ((input.service(released_at, true, {false, 0, true}) != Key::none) ||
            (Driver::sample().contacts != 0) ||
            (input.service(released_at + coretx::DurationUs{1000}, true, {false, 0, true}) !=
             Key::none) ||
            (Driver::frames() != frames) || input.tapped())
            return false;
    }
    return true;
}

bool inferred_taps() {
    using Test = Fixture<InferredTaps>;
    using Driver = hw::Touch<Test::Io>;
    namespace fw = coretx::firmware;
    for (const std::uint32_t base : {0U, 0xFFF80000U}) {
        for (unsigned mode = 0; mode < (fw::features::multiswitch ? 3U : 1U); ++mode) {
            Test::reset_registers();
            auto &io = Test::registers;
            if (!start<Driver>(io, base))
                return false;
            fw::TouchInput<Driver> input{};
            const fw::SenderInputContext context{
                mode != 0, static_cast<std::uint8_t>(mode == 2 ? 1 : 0), mode == 0};
            const auto service = [&](const std::uint32_t now) {
                io.clock = base + now;
                return input.service(coretx::TimestampUs{io.clock}, true, context);
            };
            (void)service(90'000);
            io.packet = {0, 0, 1, 0, 32, 0, 248};
            auto now = 100'000U;
            for (std::uint32_t tap = 0; tap < 3; ++tap) {
                io.cell(reg::interrupt) = 0;
                bool received{};
                for (unsigned step = 0; step < 48; ++step, ++now) {
                    if (service(now) != Key::none)
                        return false;
                    if (Driver::frames() == tap + 1) {
                        received = true;
                        break;
                    }
                }
                if (!received || (Driver::sample().contacts != 1) ||
                    ((mode == 2) && (input.held_switch() != 0)))
                    return false;
                io.cell(reg::interrupt) = touch_line;
                constexpr auto timeout = hw::TouchNavigation::release_timeout_us.count();
                if ((service(now + 1000U) != Key::none) || input.tapped() || input.tapped_switch())
                    return false;
                if (service(now + timeout) != (mode == 0 ? Key::ok : Key::none))
                    return false;
                if ((input.tapped().has_value() != (mode == 0)) ||
                    ((mode == 0) && ((input.tapped()->x != 4) || (input.tapped()->y != 16))) ||
                    (input.tapped_switch() !=
                     (mode == 1 ? std::optional<std::size_t>{0} : std::nullopt)) ||
                    input.held_switch() || !Driver::sample().synthetic)
                    return false;
                if ((service(now + timeout + 1) != Key::none) || input.tapped() ||
                    input.tapped_switch())
                    return false;
                now += timeout + 10'000U;
            }
        }
    }
    return true;
}

constexpr bool inferred_tap_guards() {
    for (unsigned reason = 0; reason < 3; ++reason) {
        hw::TouchNavigation navigation{};
        coretx::firmware::MultiSwitchTouch switches{};
        hw::TouchSample sample{};
        (void)switches.service(coretx::TimestampUs{0}, &sample, true, 0);
        sample = {0, 0, 4, 16, 1, hw::Gesture::none};
        (void)navigation.service(coretx::TimestampUs{0}, &sample, true, true);
        (void)switches.service(coretx::TimestampUs{0}, &sample, true, 0);
        sample.x = reason == 0 ? 16 : 20;
        sample.y = reason == 0 ? 28 : 16;
        const auto key = reason == 1 ? Key::ok : Key::none;
        if ((navigation.service(coretx::TimestampUs{1000}, &sample, reason != 2, true) != key) ||
            (switches.service(coretx::TimestampUs{1000}, &sample, reason != 2, 0) != key))
            return false;
        sample.contacts = 0;
        sample.synthetic = true;
        const auto deadline = coretx::TimestampUs{1000} + hw::TouchNavigation::release_timeout_us;
        if ((navigation.service(deadline, &sample, true, true) != Key::none) ||
            navigation.tapped() || (switches.service(deadline, &sample, true, 0) != Key::none) ||
            switches.tapped() || switches.held())
            return false;
    }
    return true;
}

constexpr bool dashboard_taps() {
    for (const bool enabled : {false, true}) {
        hw::TouchNavigation nav{};
        hw::TouchSample sample{0, 0, 40, 24, 1, hw::Gesture::none};
        if (nav.service(coretx::TimestampUs{0}, &sample, true, enabled) != Key::none)
            return false;
        sample.contacts = 0;
        const auto key = nav.service(coretx::TimestampUs{10'000}, &sample, true, enabled);
        if ((key != (enabled ? Key::ok : Key::none)) || (nav.tapped().has_value() != enabled))
            return false;
        if (enabled && ((nav.tapped()->x != 40) || (nav.tapped()->y != 24)))
            return false;
        (void)nav.service(coretx::TimestampUs{10'001}, nullptr, true, enabled);
        if (nav.tapped())
            return false;
    }
    for (const bool synthetic : {false, true}) {
        hw::TouchNavigation nav{};
        hw::TouchSample sample{0, 0, 40, 24, 1, hw::Gesture::none};
        (void)nav.service(coretx::TimestampUs{0}, &sample, true, true);
        sample.contacts = 0;
        sample.synthetic = synthetic;
        sample.position_valid = !synthetic;
        sample.x = 52;
        sample.y = 36;
        if ((nav.service(coretx::TimestampUs{10'000}, &sample, true, true) != Key::none) ||
            nav.tapped())
            return false;
    }
    hw::TouchNavigation swipe{};
    hw::TouchSample sample{0, 0, 40, 24, 1, hw::Gesture::right};
    if ((swipe.service(coretx::TimestampUs{0}, &sample, true, true) != Key::ok) || swipe.tapped())
        return false;
    sample.contacts = 0;
    return (swipe.service(coretx::TimestampUs{10'000}, &sample, true, true) == Key::none) &&
           !swipe.tapped();
}

bool cooperative_reports() {
    using Test = Fixture<CooperativeReports>;
    using Driver = hw::Touch<Test::Io>;
    for (const std::uint32_t base : {0U, 0xFFFE7900U}) {
        for (const std::uint32_t delay : {25U, 5000U}) {
            Test::reset_registers();
            auto &io = Test::registers;
            if (!start<Driver>(io, base))
                return false;
            io.poll_cost = 1;
            io.byte_delay = delay;
            io.cell(reg::interrupt) = 0;
            std::uint32_t completed_at{};
            for (std::uint32_t step = 0; step < 64 && !Driver::received(); ++step) {
                io.clock = base + 100'000U + step * 2000U + (step >= 5 ? 20'000U : 0U);
                const auto before = io.clock;
                const auto event = Driver::service(coretx::TimestampUs{before});
                if ((event == hw::TouchEvent::failed) || (io.clock - before > 300U) ||
                    ((delay == 25) && (event != hw::TouchEvent::sample)))
                    return false;
                completed_at = before;
            }
            if (!Driver::received() || (Driver::frames() != 1) || (Driver::sample().contacts != 1))
                return false;
            io.cell(reg::interrupt) = touch_line;
            const auto released_at =
                coretx::TimestampUs{completed_at} + hw::TouchNavigation::release_timeout_us;
            if ((Driver::service(released_at) != hw::TouchEvent::sample) ||
                (Driver::sample().contacts != 0))
                return false;
            io.clock = released_at.count() + 1000U;
            io.poll_cost = 0;
            io.cell(reg::interrupt) = 0;
            io.cell(reg::isr) = I2C_ISR_BUSY;
            (void)Driver::service(coretx::TimestampUs{io.clock});
            if ((Driver::service(coretx::TimestampUs{io.clock + 9999U}) != hw::TouchEvent::none) ||
                (Driver::service(coretx::TimestampUs{io.clock + 10'000U}) !=
                 hw::TouchEvent::failed) ||
                (Driver::error() != hw::TouchError::timeout))
                return false;
        }
    }
    return true;
}

bool report_taps() {
    using Test = Fixture<ReportTaps>;
    using Driver = hw::Touch<Test::Io>;
    namespace fw = coretx::firmware;
    for (const std::uint32_t base : {0U, 0xFFFE7900U}) {
        for (unsigned mode = 0; mode < (fw::features::multiswitch ? 3U : 1U); ++mode) {
            Test::reset_registers();
            auto &io = Test::registers;
            if (!start<Driver>(io, base))
                return false;
            fw::TouchInput<Driver> input{};
            const fw::SenderInputContext context{
                mode != 0, static_cast<std::uint8_t>(mode == 2 ? 1 : 0), mode == 0};
            io.clock = base + 90'000U;
            (void)input.service(coretx::TimestampUs{io.clock}, true, context);
            io.poll_cost = 1;
            io.byte_delay = 25;
            for (std::uint32_t tap = 0; tap < 3; ++tap) {
                for (std::uint32_t phase = 0; phase < 3; ++phase) {
                    io.clock = base + 100'000U + tap * 10'000U + phase * 1000U;
                    const auto before = io.clock;
                    io.packet = phase == 0
                                    ? std::array<std::uint8_t, 7>{0, 0, 1, 0, 32, 0, 248}
                                    : std::array<std::uint8_t, 7>{
                                          0,
                                          0,
                                          static_cast<std::uint8_t>(phase == 1),
                                          static_cast<std::uint8_t>(phase == 1 ? 0x80 : 0x40),
                                          46,
                                          0,
                                          230};
                    // INT has already returned HIGH; every report is newer than the 8 ms limit.
                    io.cell(exti_pending) |= touch_line;
                    const auto key = input.service(coretx::TimestampUs{before}, true, context);
                    const bool released = phase == 2;
                    if ((io.clock - before > 300U) ||
                        (key != (released && mode == 0 ? Key::ok : Key::none)) ||
                        (Driver::frames() != tap * 3 + phase + 1) ||
                        (input.tapped().has_value() != (released && mode == 0)) ||
                        (input.tapped_switch().has_value() != (released && mode == 1)) ||
                        (input.held_switch().has_value() != (!released && mode == 2)))
                        return false;
                    if ((input.tapped() &&
                         ((input.tapped()->x != 4) || (input.tapped()->y != 16))) ||
                        (input.tapped_switch() && (*input.tapped_switch() != 0)) ||
                        (input.held_switch() && (*input.held_switch() != 0)))
                        return false;
                }
            }
        }
    }
    return true;
}

bool polled_release_taps() {
    using Test = Fixture<PolledReleaseTaps>;
    using Driver = hw::Touch<Test::Io>;
    namespace fw = coretx::firmware;
    for (const std::uint32_t base : {0U, 0xFFFE7900U}) {
        for (unsigned mode = 0; mode < (fw::features::multiswitch ? 3U : 1U); ++mode) {
            Test::reset_registers();
            auto &io = Test::registers;
            if (!start<Driver>(io, base))
                return false;
            fw::TouchInput<Driver> input{};
            const fw::SenderInputContext context{
                mode != 0, static_cast<std::uint8_t>(mode == 2 ? 1 : 0), mode == 0};
            io.clock = base + 90'000U;
            (void)input.service(coretx::TimestampUs{io.clock}, true, context);
            io.poll_cost = 1;
            io.byte_delay = 25;
            for (std::uint32_t tap = 0; tap < 3; ++tap) {
                unsigned actions{};
                std::optional<std::uint32_t> released_at{};
                std::size_t idle_writes{};
                io.cell(exti_pending) |= touch_line;
                for (std::uint32_t elapsed = 0; elapsed <= 40'000; elapsed += 500) {
                    io.clock = base + 100'000U + tap * 50'000U + elapsed;
                    io.packet = {0, 0, static_cast<std::uint8_t>(elapsed < 20'000), 0, 32, 0, 248};
                    // Only the first contact pulses INT; lift clears TD_STATUS without a pulse.
                    if (elapsed >= 30'000)
                        io.packet = {0, 255, 255, 255, 255, 255, 255};
                    const auto before = io.clock;
                    const auto key = input.service(coretx::TimestampUs{before}, true, context);
                    if ((io.clock - before > 300U) || (Driver::error() != hw::TouchError::none) ||
                        ((key != Key::none) && !((mode == 0) && (key == Key::ok))) ||
                        ((mode == 2) && (elapsed < 20'000) && (input.held_switch() != 0)))
                        return false;
                    if (key == Key::ok || input.tapped_switch()) {
                        if ((elapsed < 20'000) || (elapsed > 28'000))
                            return false;
                        ++actions;
                    }
                    if (!released_at && (Driver::sample().contacts == 0))
                        released_at = elapsed;
                    if (released_at && input.held_switch())
                        return false;
                    if (elapsed == 30'000)
                        idle_writes = io.writes;
                    else if ((elapsed > 30'000) && (io.writes != idle_writes))
                        return false;
                }
                if (!released_at || (*released_at < 20'000) || (*released_at > 28'000) ||
                    (actions != (mode == 2 ? 0U : 1U)))
                    return false;
            }
        }
    }
    return true;
}

bool recovering_reports() {
    using Test = Fixture<RecoveringReports>;
    using Driver = hw::Touch<Test::Io>;
    for (const std::uint32_t base : {0U, 0xFFFE0000U}) {
        Test::reset_registers();
        auto &io = Test::registers;
        if (!start<Driver>(io, base))
            return false;
        coretx::firmware::TouchInput<Driver> input{};
        const auto service = [&](const std::uint32_t now) {
            io.clock = base + now;
            return input.service(coretx::TimestampUs{io.clock}, true, {false, 0, true});
        };
        (void)service(90'000);
        io.cell(reg::interrupt) = 0;
        for (std::uint32_t now = 100'000; now < 100'050; ++now)
            if (service(now) != Key::none)
                return false;
        if ((Driver::frames() != 1) || (Driver::sample().contacts != 1))
            return false;
        io.cell(reg::isr) = I2C_ISR_BUSY;
        (void)service(110'000);
        (void)service(120'000);
        if ((Driver::error() != hw::TouchError::timeout) || (input.message() == nullptr) ||
            (Driver::sample().contacts != 0))
            return false;
        io.cell(reg::isr) = 0;
        const auto writes = io.writes;
        for (std::uint32_t now = 121'000; now < 370'000; now += 1000)
            if ((service(now) != Key::none) || (input.message() == nullptr) ||
                (io.writes != writes))
                return false;
        io.packet[1] = 0x14;
        for (std::uint32_t now = 370'000; now < 470'000; now += 100) {
            if (service(now) != Key::none)
                return false;
            if ((Driver::frames() == 1) && (input.message() == nullptr))
                return false;
        }
        if (!Driver::ready() || (Driver::error() != hw::TouchError::none) ||
            (Driver::frames() < 2) || (input.message() != nullptr))
            return false;
        io.packet[2] = 0;
        for (std::uint32_t now = 470'000; now < 480'000; now += 100)
            if (service(now) != Key::none)
                return false;
        io.packet[2] = 1;
        unsigned actions{};
        for (std::uint32_t now = 480'000; now < 500'000; now += 100) {
            const auto key = service(now);
            if (key == Key::ok)
                ++actions;
            else if (key != Key::none)
                return false;
        }
        if (actions != 1)
            return false;
        io.cell(reg::interrupt) = touch_line;
        for (std::uint32_t now = 500'000; now < 503'000; now += 100)
            if (service(now) != Key::none)
                return false;
        io.cell(reg::interrupt) = 0;
        io.cell(reg::isr) = I2C_ISR_BUSY;
        (void)service(510'000);
        for (std::uint32_t now = 520'000; now < 1'520'000; now += 1000) {
            io.cell(reg::isr) = I2C_ISR_BUSY;
            if ((service(now) != Key::none) || (input.message() == nullptr))
                return false;
        }
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
            registers.nack_at = 3;
            if ((Partial::service(coretx::TimestampUs{80'052}) != hw::TouchEvent::failed) ||
                (Partial::error() != hw::TouchError::nack) || Partial::received() ||
                (Partial::frames() != 0) || (Partial::packet_bytes() != 3))
                return false;
        }
        TransferTest::reset_registers();
        auto &io = TransferTest::registers;
        if (!start<Transfer>(io))
            return false;
        io.cell(reg::interrupt) = 0;
        io.cell(reg::isr) = I2C_ISR_BUSY;
        (void)Transfer::service(coretx::TimestampUs{80'052});
        io.cell(reg::isr) = masks[i];
        const auto result = Transfer::service(coretx::TimestampUs{i == 4 ? 90'052U : 80'053U});
        if ((result != hw::TouchEvent::failed) || (Transfer::error() != errors[i]) ||
            Transfer::ready() || Transfer::received() || (io.cell(reg::cr1) != 0))
            return false;
        if ((Transfer::service(coretx::TimestampUs{100'000}) != hw::TouchEvent::failed) ||
            (io.writes != 0))
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
    if ((Packet::error() != hw::TouchError::packet) || Packet::received() ||
        (Packet::packet_bytes() != 7) || (Packet::packet() != io.packet))
        return false;
    return start<Packet>(io) && !Packet::received();
}

constexpr bool rendering() {
    coretx::ui::Framebuffer display{};
    coretx::ui::Framebuffer expected{};
    const hw::TouchSample sample{64, 64, 96, 32, 1, hw::Gesture::none};
    hw::render_touch(display, sample, true, true, hw::TouchError::timeout, Key::none, 0);
    expected.text(0, 18, coretx::ui::tr::touch_timeout);
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

constexpr bool setup_navigation() {
    using coretx::TimestampUs;
    for (const std::uint32_t base : {0U, 0xFFFFF000U}) {
        hw::SetupNavigation nav{};
        auto now = base;
        const auto step = [&](const std::uint32_t elapsed, const std::uint8_t buttons,
                              const std::int16_t vra = 0, const std::int16_t vrb = 0,
                              const bool jog = true) {
            now += elapsed;
            return nav.service(TimestampUs{now}, buttons, true, vra, vrb, jog);
        };
        if ((step(0, 2, 1024) != Key::none) || (step(20'000, 2) != Key::none) ||
            (nav.buttons() != 2))
            return false;
        nav.set_mode(TimestampUs{now}, true);
        if ((step(900'000, 2) != Key::none) || (step(1, 0) != Key::none) ||
            (step(20'000, 0) != Key::none))
            return false;
        if ((step(1, 2) != Key::none) || (step(19'999, 2) != Key::none) ||
            (step(1, 2) != Key::none) || (step(1000, 0) != Key::none) ||
            (step(20'000, 0) != Key::cancel) || (step(1, 0) != Key::none))
            return false;
        if ((step(1, 1) != Key::none) || (step(20'000, 1) != Key::none) ||
            (step(799'999, 1) != Key::none) || (step(1, 1) != Key::ok_long) ||
            (step(1, 0) != Key::none) || (step(20'000, 0) != Key::none))
            return false;
        if ((step(1, 0, 301) != Key::down) || (step(1'000'000, 0, 1024) != Key::none) ||
            (step(1, 0, -1024) != Key::none) || (step(1, 0) != Key::none) ||
            (step(1, 0, 0, -301) != Key::up))
            return false;
        if ((step(1, 0, 0, 0, false) != Key::none) || (step(1, 0, 1024) != Key::none) ||
            (step(1, 0) != Key::none) || (step(1, 0, 1024, -1024) != Key::down))
            return false;
        nav.set_mode(TimestampUs{now}, false);
        if (!nav.led_on() || (step(1, 0, -1024) != Key::none))
            return false;
        hw::SetupNavigation blink{};
        blink.set_mode(TimestampUs{base}, true);
        (void)blink.service(TimestampUs{base + 249'999U}, 0, true, 0, 0, true);
        if (!blink.led_on())
            return false;
        (void)blink.service(TimestampUs{base + 250'000U}, 0, true, 0, 0, true);
        if (blink.led_on())
            return false;
        blink.set_mode(TimestampUs{base + 250'001U}, false);
        if (!blink.led_on())
            return false;
    }
    return true;
}

bool setup_gpio() {
    using Test = Fixture<SetupGpio>;
    Test::reset_registers();
    auto &io = Test::registers;
    constexpr auto a_mode = GPIOA_BASE + offsetof(GPIO_TypeDef, MODER);
    constexpr auto d_mode = GPIOD_BASE + offsetof(GPIO_TypeDef, MODER);
    constexpr auto d_output = GPIOD_BASE + offsetof(GPIO_TypeDef, ODR);
    constexpr auto a_input = GPIOA_BASE + offsetof(GPIO_TypeDef, IDR);
    constexpr auto led_mask = (1U << 10) | (1U << 11);
    io.cell(a_mode) = 3;
    io.cell(d_mode) = 1U << 8;
    io.cell(d_output) = 1U << 4;
    io.cell(a_input) = (1U << 9) | (1U << 10);
    hw::SetupControls<Test::Io> controls{};
    controls.initialize();
    if ((io.cell(a_mode) != 3) || (io.cell(d_mode) != ((1U << 8) | (5U << 20))) ||
        (io.cell(d_output) != ((1U << 4) | led_mask)))
        return false;
    const auto pull = io.cell(GPIOA_BASE + offsetof(GPIO_TypeDef, PUPDR));
    if (((pull >> 18) & 15U) != 5U)
        return false;
    controls.set_mode(coretx::TimestampUs{0}, true);
    (void)controls.service(coretx::TimestampUs{0}, true, 0, 0, true);
    io.cell(a_input) &= ~(1U << 10);
    (void)controls.service(coretx::TimestampUs{1}, true, 0, 0, true);
    (void)controls.service(coretx::TimestampUs{20'001}, true, 0, 0, true);
    if (controls.buttons() != 2)
        return false;
    io.cell(a_input) |= 1U << 10;
    (void)controls.service(coretx::TimestampUs{21'001}, true, 0, 0, true);
    if (controls.service(coretx::TimestampUs{41'001}, true, 0, 0, true) != Key::cancel)
        return false;
    (void)controls.service(coretx::TimestampUs{250'000}, true, 0, 0, true);
    if (io.cell(d_output) != (1U << 4))
        return false;
    controls.set_mode(coretx::TimestampUs{250'001}, false);
    return io.cell(d_output) == ((1U << 4) | led_mask);
}

static_assert(decoding());
static_assert(navigation());
static_assert(rendering());

} // namespace

int main() {
    bool passed = true;
#define RUN(test)                                                                                  \
    if (!test()) {                                                                                 \
        std::fputs("i6S touch: " #test " failed\n", stderr);                                       \
        passed = false;                                                                            \
    }
    RUN(decoding)
    RUN(reset_waits)
    RUN(navigation)
    RUN(fast_swipes)
    RUN(hardware)
    RUN(delayed_reports)
    RUN(latched_interrupt)
    RUN(trace_gaps)
    RUN(sparse_reports)
    RUN(inferred_taps)
    RUN(inferred_tap_guards)
    RUN(dashboard_taps)
    RUN(cooperative_reports)
    RUN(report_taps)
    RUN(polled_release_taps)
    RUN(recovering_reports)
    RUN(failures)
    RUN(rendering)
    RUN(setup_navigation)
    RUN(setup_gpio)
#undef RUN
    return passed ? 0 : 1;
}
