// SPDX-License-Identifier: GPL-2.0-or-later
#include "boards/common/board.hpp"
#include "platform/stm32f072/mmio.hpp"

extern "C" {
constinit volatile std::uint32_t coretx_fault{};
}

namespace {
namespace fw = coretx::firmware;
using Io = fw::stm32f072::Mmio;
using Board = fw::SenderBoard<Io>;
using Touch = fw::i6s::Touch<Io>;
using Display = Board::Display;
using Watchdog = Board::Watchdog;
constinit coretx::ui::Framebuffer framebuffer{};

[[noreturn]] void halt() noexcept {
    Watchdog::record_failure(0x80);
    Board::power_off();
    asm volatile("cpsid i");
    for (;;)
        asm volatile("wfi");
}

constexpr const char *key_name(const fw::Key key) noexcept {
    switch (key) {
    case fw::Key::none:
        return "--";
    case fw::Key::up:
        return "UP";
    case fw::Key::down:
        return "DOWN";
    case fw::Key::ok:
        return "OK";
    case fw::Key::cancel:
        return "BACK";
    case fw::Key::ok_long:
        return "OK LONG";
    case fw::Key::cancel_long:
        return "BACK LONG";
    }
    return "?";
}

void render(const coretx::TimestampUs now, const fw::Key key,
            const std::uint32_t actions) noexcept {
    framebuffer.clear();
    framebuffer.compact();
    framebuffer.text(0, 0, "I6S TOUCH DIAG");
    framebuffer.unsigned_number(127, 0, now.count() / 1'000'000U);
    const auto error = fw::i6s::error_message(Touch::error());
    framebuffer.text(0, 8, error != nullptr ? error : Touch::ready() ? "READY" : "START");
    framebuffer.text(0, 16, "PACKETS");
    framebuffer.unsigned_number(127, 16, Touch::frames());
    framebuffer.text(0, 24, "IRQ");
    framebuffer.number(28, 24,
                       (Io::read(fw::i6s::touch_registers::interrupt) &
                        (1U << Board::Hardware::touch_interrupt.index)) != 0);
    framebuffer.text(40, 24, "CONTACTS");
    framebuffer.unsigned_number(127, 24, Touch::sample().contacts);
    framebuffer.text(0, 32, "RAW");
    framebuffer.text(20, 32, "X");
    framebuffer.text(76, 32, "Y");
    if (Touch::received()) {
        const auto &sample = Touch::sample();
        framebuffer.unsigned_number(68, 32, sample.raw_x);
        framebuffer.unsigned_number(127, 32, sample.raw_y);
    } else {
        framebuffer.text(40, 32, "--");
        framebuffer.text(100, 32, "--");
    }
    constexpr std::string_view hex = "0123456789ABCDEF";
    std::array<char, 21> packet{};
    packet.fill(' ');
    for (std::size_t i = 0; i < Touch::packet().size(); ++i) {
        packet[3 * i] = hex[Touch::packet()[i] >> 4U];
        packet[3 * i + 1] = hex[Touch::packet()[i] & 15U];
    }
    framebuffer.text(0, 40, {packet.data(), packet.size()});
    framebuffer.unsigned_number(127, 40, static_cast<std::uint32_t>(Touch::packet_bytes()));
    framebuffer.text(0, 48, "KEY");
    framebuffer.text(20, 48, key_name(key));
    framebuffer.unsigned_number(127, 48, actions);
    framebuffer.text(0, 56, "RF OFF / EEPROM UNTOUCHED");
}
} // namespace

extern "C" [[noreturn]] int main() noexcept {
    Board::prepare_power();
    Board::power_off();
    const auto boot_ready = Board::bootloader_startup();
    if ((Watchdog::initialize() != fw::WatchdogError::none) || !boot_ready ||
        (Board::initialize() != fw::stm32f072::HardwareError::none) ||
        (Display::initialize() != fw::LcdError::none))
        halt();
    Touch::initialize();
    fw::i6s::TouchNavigation navigation{};
    fw::i6s::PowerButton power{};
    fw::Key last_key{};
    std::uint32_t actions{};
    bool display_ready{};
    bool ready{};
    auto drawn_at = Board::now_us();
    render(drawn_at, last_key, actions);
    Display::begin_frame();
    if (Watchdog::start_runtime(Board::now_us()) != fw::WatchdogError::none)
        halt();
    for (;;) {
        if (power.update(Board::now_us(), Board::power_button_pressed()))
            Board::release_power();
        if (power.requested()) {
            if (Watchdog::service(Board::now_us()) != fw::WatchdogError::none)
                halt();
            continue;
        }
        if (!Board::stack_intact() || (Display::service(framebuffer) != fw::LcdError::none))
            halt();
        const auto now = Board::now_us();
        const auto event = Touch::service(now);
        display_ready = display_ready || !Display::busy();
        const bool healthy =
            display_ready && Touch::ready() && (Touch::error() == fw::i6s::TouchError::none);
        const auto *sample = (event == fw::i6s::TouchEvent::sample) || (healthy != ready)
                                 ? &Touch::sample()
                                 : nullptr;
        ready = healthy;
        const auto key = navigation.service(now, sample, healthy);
        if (key != fw::Key::none) {
            last_key = key;
            ++actions;
        }
        if (!Display::busy() && ((now - drawn_at) >= coretx::DurationUs{50'000})) {
            render(now, last_key, actions);
            Display::begin_frame();
            drawn_at = now;
        }
        if (Watchdog::service(Board::now_us()) != fw::WatchdogError::none)
            halt();
        Watchdog::mark(0);
    }
}
