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

// FT6236: chip ID, firmware, threshold, monitor control, monitor delay, active/monitor period,
// interrupt mode.
constexpr std::array<std::uint8_t, 8> register_indices{0xA3, 0xA6, 0x80, 0x86,
                                                       0x87, 0x88, 0x89, 0xA4};
constexpr std::array<std::string_view, 8> register_labels{"ID", "FW", "TH", "CT",
                                                          "ET", "AR", "MR", "GM"};

struct Registers {
    std::array<std::uint8_t, register_indices.size()> values{};
    coretx::TimestampUs read_at{};
    std::uint16_t reads{};
    bool valid{};

    // Re-read periodically: the controller may answer zeros until its firmware has booted.
    void service(const coretx::TimestampUs now) noexcept {
        if ((reads != 0) && ((now - read_at).count() < 1'000'000U))
            return;
        read_at = now;
        ++reads;
        valid = false;
        for (std::size_t i = 0; i < values.size(); ++i) {
            const auto value = Touch::read_register(register_indices[i]);
            if (!value)
                return;
            values[i] = *value;
        }
        valid = true;
    }
};

enum class Lift : std::uint8_t { none, packet, polled, inferred };

// Timing and travel of the most recent contact, as reported by the controller.
struct Stroke {
    coretx::TimestampUs first{}, last{};
    std::uint32_t max_gap_ms{}, duration_ms{}, release_ms{};
    std::uint16_t reports{};
    std::uint8_t start_x{}, start_y{}, gesture{};
    std::int8_t dx{}, dy{};
    Lift lift{};
    bool active{};

    void observe(const coretx::TimestampUs now, const fw::i6s::TouchSample &sample,
                 const std::uint8_t gesture_byte) noexcept {
        if (gesture_byte != 0)
            gesture = gesture_byte;
        if (sample.contacts != 0) {
            if (!active) {
                *this = {};
                active = true;
                first = now;
                start_x = sample.x;
                start_y = sample.y;
                gesture = gesture_byte;
            } else
                max_gap_ms = std::max(max_gap_ms, (now - last).count() / 1000U);
            last = now;
            ++reports;
            travel(sample);
            duration_ms = (now - first).count() / 1000U;
            return;
        }
        if (!active)
            return;
        active = false;
        release_ms = (now - last).count() / 1000U;
        lift = sample.synthetic        ? Lift::inferred
               : sample.position_valid ? Lift::packet
                                       : Lift::polled;
        if (lift == Lift::packet) {
            travel(sample);
            duration_ms = (now - first).count() / 1000U;
        }
    }

  private:
    void travel(const fw::i6s::TouchSample &sample) noexcept {
        dx = static_cast<std::int8_t>(int{sample.x} - start_x);
        dy = static_cast<std::int8_t>(int{sample.y} - start_y);
    }
};

void hex_byte(const int x, const int y, const std::uint8_t value) noexcept {
    constexpr std::string_view hex = "0123456789ABCDEF";
    const std::array<char, 2> text{hex[value >> 4U], hex[value & 15U]};
    framebuffer.text(x, y, {text.data(), text.size()});
}

void render(const coretx::TimestampUs now, const fw::Key key, const std::uint32_t actions,
            const Registers &registers, const Stroke &stroke) noexcept {
    framebuffer.clear();
    framebuffer.compact();
    framebuffer.text(0, 0, "TOUCH DIAG RF OFF");
    framebuffer.unsigned_number(127, 0, now.count() / 1'000'000U);
    const auto error = fw::i6s::error_message(Touch::error());
    framebuffer.text(0, 8, error != nullptr ? error : Touch::ready() ? "READY" : "START");
    framebuffer.text(64, 8, "P");
    framebuffer.unsigned_number(95, 8, Touch::frames());
    framebuffer.text(100, 8, "A");
    framebuffer.unsigned_number(127, 8, actions);
    framebuffer.text(0, 16, "IRQ");
    framebuffer.number(19, 16,
                       (Io::read(fw::i6s::touch_registers::interrupt) &
                        (1U << Board::Hardware::touch_interrupt.index)) != 0);
    framebuffer.text(24, 16, "C");
    framebuffer.unsigned_number(35, 16, Touch::sample().contacts);
    framebuffer.text(40, 16, "X");
    framebuffer.text(84, 16, "Y");
    if (Touch::received()) {
        framebuffer.unsigned_number(75, 16, Touch::sample().raw_x);
        framebuffer.unsigned_number(127, 16, Touch::sample().raw_y);
    }
    for (std::size_t i = 0; i < Touch::packet().size(); ++i)
        hex_byte(static_cast<int>(i) * 12, 24, Touch::packet()[i]);
    framebuffer.unsigned_number(127, 24, static_cast<std::uint32_t>(Touch::packet_bytes()));
    for (std::size_t i = 0; i < register_labels.size(); ++i) {
        const int x = static_cast<int>(i % 4) * 32, y = 32 + static_cast<int>(i / 4) * 8;
        framebuffer.text(x, y, register_labels[i]);
        if (registers.valid)
            hex_byte(x + 10, y, registers.values[i]);
        else
            framebuffer.text(x + 10, y, "--");
    }
    framebuffer.text(0, 48, "N");
    framebuffer.unsigned_number(23, 48, stroke.reports);
    framebuffer.text(28, 48, "G");
    framebuffer.unsigned_number(55, 48, stroke.max_gap_ms);
    framebuffer.text(60, 48, "T");
    framebuffer.unsigned_number(91, 48, stroke.duration_ms);
    framebuffer.text(96, 48, "R");
    framebuffer.unsigned_number(127, 48, stroke.release_ms);
    framebuffer.text(0, 56, "DX");
    framebuffer.number(23, 56, stroke.dx);
    framebuffer.text(28, 56, "DY");
    framebuffer.number(51, 56, stroke.dy);
    framebuffer.text(56, 56, "L");
    framebuffer.unsigned_number(63, 56, static_cast<unsigned>(stroke.lift));
    framebuffer.text(68, 56, "H");
    hex_byte(72, 56, stroke.gesture);
    framebuffer.text(88, 56, key_name(key));
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
    Registers registers{};
    Stroke stroke{};
    bool display_ready{};
    bool ready{};
    auto drawn_at = Board::now_us();
    render(drawn_at, last_key, actions, registers, stroke);
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
        if (event == fw::i6s::TouchEvent::sample)
            stroke.observe(now, Touch::sample(), Touch::packet()[1]);
        if (!healthy)
            registers.valid = false;
        else if (!stroke.active)
            registers.service(now);
        const auto key = navigation.service(now, sample, healthy);
        if (key != fw::Key::none) {
            last_key = key;
            ++actions;
        }
        if (!Display::busy() && ((now - drawn_at) >= coretx::DurationUs{50'000})) {
            render(now, last_key, actions, registers, stroke);
            Display::begin_frame();
            drawn_at = now;
        }
        if (Watchdog::service(Board::now_us()) != fw::WatchdogError::none)
            halt();
        Watchdog::mark(0);
    }
}
