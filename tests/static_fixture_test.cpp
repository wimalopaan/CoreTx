// SPDX-License-Identifier: GPL-2.0-or-later
#include "boards/i6s/touch_input.hpp"
#include "platform/stm32f072/clock.hpp"
#include "platform/stm32f072/gpio.hpp"
#include "static_fixture.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace {

namespace fw = coretx::firmware;
namespace touch = fw::i6s;
namespace reg = fw::stm32f072::registers;

struct Registers {
    std::uint32_t now{};
    std::uint32_t output{};
    std::uint8_t byte{};
    std::uint16_t *dma_destination{};
    unsigned barriers{};

    inline static constexpr auto output_address = GPIOA_BASE + offsetof(GPIO_TypeDef, BSRR);

    std::uint32_t read(const std::uintptr_t address) {
        if (address == reg::tim2_cnt)
            return now;
        if (address == output_address)
            return output;
        std::abort();
    }
    void write(const std::uintptr_t address, const std::uint32_t value) {
        if (address != output_address)
            std::abort();
        output = value;
    }
    std::uint8_t read8(const std::uintptr_t address) {
        if (address != output_address)
            std::abort();
        return byte;
    }
    void write8(const std::uintptr_t address, const std::uint8_t value) {
        if (address != output_address)
            std::abort();
        byte = value;
    }
    std::uint8_t read8(const std::uint8_t *const address) { return *address; }
    std::uint16_t read16(const std::uint16_t *const address) { return *address; }
    void write_address(const std::uintptr_t address, std::uint16_t *const destination) {
        if (address != output_address)
            std::abort();
        dma_destination = destination;
    }
    void barrier() { ++barriers; }
    std::uint32_t now_us() { return now; }
};

struct FirstRegisters {};
struct SecondRegisters {};
using First = coretx::test::RegisterFixture<FirstRegisters, Registers>;
using Second = coretx::test::RegisterFixture<SecondRegisters, Registers>;

bool register_isolation() {
    First::reset_registers();
    Second::reset_registers();
    First::registers.now = 0xFFFF'FFFFU;
    Second::registers.now = 42;
    fw::drive<First::Io>({GPIOA_BASE, 8}, true);
    fw::drive<Second::Io>({GPIOA_BASE, 8}, false);
    if ((First::Io::read(Registers::output_address) != GPIO_BSRR_BS_8) ||
        (Second::Io::read(Registers::output_address) != GPIO_BSRR_BR_8) ||
        (fw::Clock<First::Io>::sample() != coretx::TimestampUs{0xFFFF'FFFFU}) ||
        (fw::Clock<Second::Io>::sample() != coretx::TimestampUs{42}))
        return false;
    const auto before_wrap = First::Io::now_us();
    First::registers.now = 0;
    if ((fw::Clock<First::Io>::sample() - before_wrap) != coretx::DurationUs{1})
        return false;

    First::Io::write8(Registers::output_address, 0xA5);
    std::array<std::uint16_t, 2> samples{1234, 4321};
    std::array<std::uint8_t, 2> bytes{0x3C, 0xC3};
    First::Io::write_address(Registers::output_address, samples.data());
    First::Io::barrier();
    if ((First::Io::read8(Registers::output_address) != 0xA5) ||
        (First::Io::read8(bytes.data() + 1) != 0xC3) ||
        (First::Io::read16(First::registers.dma_destination + 1) != 4321) ||
        (First::registers.barriers != 1) || (Second::registers.barriers != 0) ||
        (Second::registers.dma_destination != nullptr))
        return false;

    First::reset_registers();
    return (First::registers.dma_destination == nullptr) && (First::registers.barriers == 0) &&
           (First::Io::read(Registers::output_address) == 0) &&
           (First::Io::read8(Registers::output_address) == 0) &&
           (fw::Clock<First::Io>::sample() == coretx::TimestampUs{}) &&
           (Second::Io::read(Registers::output_address) == GPIO_BSRR_BR_8) &&
           (fw::Clock<Second::Io>::sample() == coretx::TimestampUs{42});
}

struct Context {
    bool ready{};
    touch::TouchError error{};
    touch::TouchEvent event{touch::TouchEvent::sample};
    touch::TouchSample sample{};
    std::array<std::uint8_t, 64> eeprom{};
};

template <typename Fixture> struct TouchBoard {
    TouchBoard() = delete;

    struct Touch {
        inline static unsigned polls{};

        static void initialize() { polls = 0; }
        static touch::TouchEvent service(coretx::TimestampUs) {
            ++polls;
            return Fixture::context.event;
        }
        static bool ready() { return Fixture::context.ready; }
        static touch::TouchError error() { return Fixture::context.error; }
        static const touch::TouchSample &sample() { return Fixture::context.sample; }
        static unsigned poll_count() { return polls; }
    };

    using Input = fw::TouchInput<Touch>;

    static void reset() { Touch::polls = 0; }
};

struct FirstSender {};
struct SecondSender {};
struct RebootSender {};
using Sender = coretx::test::FakeApplication<FirstSender, Context, TouchBoard>;
using OtherSender = coretx::test::FakeApplication<SecondSender, Context, TouchBoard>;
using Reboot = coretx::test::FakeApplication<RebootSender, Context, TouchBoard>;

bool application_isolation() {
    Sender::reset();
    OtherSender::reset();
    Sender::context.ready = OtherSender::context.ready = true;
    Sender::Board::Input input{};
    OtherSender::Board::Input other{};
    input.initialize();
    other.initialize();
    if ((input.service(coretx::TimestampUs{}, true) != fw::Key::none) ||
        (other.service(coretx::TimestampUs{}, true) != fw::Key::none))
        return false;
    Sender::context.sample = {0, 0, 64, 32, 1, touch::Gesture::none};
    if (input.service(coretx::TimestampUs{1}, true) != fw::Key::none)
        return false;
    Sender::context.sample.x = 80;
    if ((input.service(coretx::TimestampUs{2}, true) != fw::Key::ok) ||
        (other.service(coretx::TimestampUs{2}, true) != fw::Key::none) ||
        (Sender::Board::Touch::poll_count() != 3) || (OtherSender::Board::Touch::poll_count() != 2))
        return false;
    Sender::context.error = touch::TouchError::nack;
    if ((input.service(coretx::TimestampUs{3}, true) != fw::Key::none) ||
        (input.message() != touch::error_message(touch::TouchError::nack)) ||
        (other.message() != nullptr))
        return false;

    OtherSender::context.sample = {0, 0, 64, 32, 1, touch::Gesture::none};
    if (other.service(coretx::TimestampUs{3}, true) != fw::Key::none)
        return false;
    OtherSender::context.sample.x = 48;
    return (other.service(coretx::TimestampUs{4}, true) == fw::Key::cancel) &&
           (Sender::Board::Touch::poll_count() == 4) &&
           (OtherSender::Board::Touch::poll_count() == 4);
}

bool application_reset_and_reboot() {
    OtherSender::reset();
    OtherSender::context.ready = true;
    {
        OtherSender::Board::Input other{};
        other.initialize();
        if (other.service(coretx::TimestampUs{}, true) != fw::Key::none)
            return false;
    }
    const auto other_polls = OtherSender::Board::Touch::poll_count();
    Sender::reset();
    Sender::context.eeprom[17] = 0xA5;
    Sender::context.ready = true;
    Sender::context.error = touch::TouchError::timeout;
    Sender::context.sample.contacts = 1;
    {
        Sender::Board::Input input{};
        input.initialize();
        if (input.service(coretx::TimestampUs{1}, true) != fw::Key::none)
            return false;
    }

    Reboot::reset();
    Reboot::context.eeprom = Sender::context.eeprom;
    if ((Reboot::context.eeprom[17] != 0xA5) || Reboot::context.ready ||
        (Reboot::context.error != touch::TouchError::none) ||
        (Reboot::context.sample.contacts != 0) || (Reboot::Board::Touch::poll_count() != 0) ||
        (Sender::Board::Touch::poll_count() != 1) ||
        (Sender::context.error != touch::TouchError::timeout))
        return false;

    Sender::reset();
    if ((Sender::context.eeprom[17] != 0) || Sender::context.ready ||
        (Sender::context.error != touch::TouchError::none) ||
        (Sender::context.sample.contacts != 0) || (Sender::Board::Touch::poll_count() != 0) ||
        (Reboot::context.eeprom[17] != 0xA5) ||
        (OtherSender::Board::Touch::poll_count() != other_polls))
        return false;
    Sender::context.ready = true;
    Sender::Board::Input fresh{};
    fresh.initialize();
    if (fresh.service(coretx::TimestampUs{}, true) != fw::Key::none)
        return false;
    Sender::context.sample = {0, 0, 64, 32, 1, touch::Gesture::none};
    if (fresh.service(coretx::TimestampUs{1}, true) != fw::Key::none)
        return false;
    Sender::context.sample.x = 80;
    return fresh.service(coretx::TimestampUs{2}, true) == fw::Key::ok;
}

} // namespace

int main() {
    if (!register_isolation()) {
        std::fputs("FAIL: static register fixture isolation/reset\n", stderr);
        return EXIT_FAILURE;
    }
    if (!application_isolation()) {
        std::fputs("FAIL: static application fixture isolation\n", stderr);
        return EXIT_FAILURE;
    }
    if (!application_reset_and_reboot()) {
        std::fputs("FAIL: static application fixture reset/reboot\n", stderr);
        return EXIT_FAILURE;
    }
    std::puts("Static test fixtures passed");
}
