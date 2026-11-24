// SPDX-License-Identifier: GPL-2.0-or-later
#include "boards/common/board.hpp"
#include "boards/common/model_converter.hpp"
#include "platform/stm32f072/mmio.hpp"

extern "C" {
constinit volatile std::uint32_t coretx_fault{};
}

namespace {
namespace fw = coretx::firmware;
using Io = fw::stm32f072::Mmio;
using Board = fw::SenderBoard<Io>;
using Converter = fw::ModelConverter<Board::Memory>;
using Buttons =
    std::conditional_t<Board::Hardware::power_latch, fw::i6s::SetupControls<Io, Board::Hardware>,
                       fw::i6x::detail::KeysState<Io, Board::Hardware>>;
constinit Converter converter{};
constinit fw::RadioStorage<Board::Memory> identity{};
constinit coretx::ui::Framebuffer display{};
constinit Buttons buttons{};
constinit fw::i6s::PowerButton power{};

[[noreturn]] void halt() noexcept {
    Board::power_off();
    asm volatile("cpsid i");
    for (;;)
        asm volatile("wfi");
}

template <typename Keys> void initialize_buttons(Keys &keys) noexcept {
    if constexpr (Board::Hardware::power_latch) {
        keys.initialize();
        keys.set_mode(Board::now_us(), true);
    } else
        keys.initialize(Board::now_us());
}
template <typename Keys> fw::Key key(Keys &keys, const coretx::TimestampUs now) noexcept {
    if constexpr (Board::Hardware::power_latch)
        return keys.service(now, true, 0, 0, false);
    else
        return keys.service(now);
}
template <typename Keys> bool released(const Keys &keys) noexcept {
    if constexpr (Board::Hardware::power_latch)
        return keys.buttons() == 0;
    else
        return !keys.ok_held();
}
template <typename Sender> void service_power(const coretx::TimestampUs now) noexcept {
    if constexpr (Sender::Hardware::power_latch) {
        (void)power.update(now, Sender::power_button_pressed());
        if (power.requested() && (converter.state() != Converter::State::converting)) {
            Sender::release_power();
            halt();
        }
    }
}
void render(const char *error, const bool ready) noexcept {
    display.clear();
    display.compact();
    display.text(0, 0, "CoreTX CONVERTER RF OFF");
    if (error) {
        display.text(0, 16, error);
        display.text(0, 32, "No main firmware / RF");
        display.text(0, 48, "Use main recovery/reset");
        return;
    }
    if (!ready) {
        display.text(0, 24, "Reading EEPROM...");
        return;
    }
    switch (converter.state()) {
    case Converter::State::reading:
        display.text(0, 24, "Validating all banks...");
        break;
    case Converter::State::preview: {
        display.text(0, 12,
                     converter.repairing_marker() ? "Interrupted marker repair ->"
                     : converter.resuming()       ? "Resume to format"
                                                  : "Convert to format");
        display.unsigned_number(127, 12, fw::converter_target);
        display.text(0, 20, "Source:");
        int x = 28, y = 20;
        for (unsigned version = coretx::storage::oldest_format_version;
             version <= fw::converter_target; ++version) {
            if ((version >= sizeof(converter.versions()) * 8) ||
                ((converter.versions() & (1U << version)) == 0))
                continue;
            int width = 4;
            for (auto number = version; number >= 10; number /= 10)
                width += 4;
            if ((x + width) > 128) {
                x = 0;
                y += 8;
            }
            display.unsigned_number(x + width - 1, y, version);
            x += width + 4;
        }
        display.text(0, 38, "Models:");
        display.unsigned_number(35, 38, converter.model_count());
        display.text(40, 38, "/");
        display.unsigned_number(55, 38, fw::models::count);
        display.text(0, 48, Board::Hardware::power_latch ? "Rear LEFT: confirm" : "OK: confirm");
        display.text(0, 56,
                     Board::Hardware::power_latch ? "Rear RIGHT: cancel" : "CANCEL: no writes");
        break;
    }
    case Converter::State::converting:
        display.text(0, 24, "Converting / verifying");
        display.text(0, 40, "Do not switch off!");
        break;
    case Converter::State::complete:
        display.text(0, 16, "Conversion complete");
        display.text(0, 32, "Switch off. Flash");
        display.text(0, 44, "matching main firmware.");
        break;
    case Converter::State::failed:
        display.text(0, 16, converter.error());
        display.text(0, 32, "Stopped. Keep backup.");
        display.text(0, 48, "Use main recovery/reset");
        break;
    }
}
} // namespace

extern "C" [[noreturn]] int main() noexcept {
    Board::prepare_power();
    Board::power_off();
    if (!Board::bootloader_startup() ||
        (Board::Watchdog::initialize() != fw::WatchdogError::none) ||
        (Board::initialize() != fw::stm32f072::HardwareError::none) ||
        (Board::Display::initialize() != fw::LcdError::none))
        halt();
    initialize_buttons(buttons);
    Board::Probe::initialize();
    identity.initialize(Board::transmitter_id());
    if (Board::Watchdog::start_runtime(Board::now_us()) != fw::WatchdogError::none)
        halt();
    bool selected{}, ready{}, cancelled{}, preview_visible{}, confirmation_armed{};
    const char *error{};
    render(nullptr, false);
    Board::Display::begin_frame();
    auto shown = converter.state();
    for (;;) {
        const auto now = Board::now_us();
        const auto pressed = key(buttons, now);
        if (!selected && !error) {
            Board::Probe::service(now);
            const auto selection = fw::select_memory<Board::Memory>(Board::Probe::result());
            selected = selection == fw::ProbeSelection::selected;
            if ((selection == fw::ProbeSelection::multiple) ||
                (selection == fw::ProbeSelection::missing))
                error = "Need exactly 1 EEPROM";
        }
        if (selected && !ready && !error) {
            identity.service(now, Board::Probe::result());
            if (identity.loaded()) {
                if ((identity.status() != fw::RadioStorageStatus::ready) &&
                    (identity.status() != fw::RadioStorageStatus::empty))
                    error = "Radio identity invalid";
                else {
                    converter.initialize(identity.settings().transmitter);
                    ready = true;
                }
            }
        }
        if (ready && !cancelled) {
            converter.service(now);
            if ((converter.state() == Converter::State::preview) && confirmation_armed) {
                if (pressed == fw::Key::ok)
                    (void)converter.confirm();
                if (pressed == fw::Key::cancel) {
                    cancelled = true;
                    error = "Cancelled. No writes.";
                }
            }
        }
        if (preview_visible && released(buttons))
            confirmation_armed = true;
        if (Board::Display::service(display) != fw::LcdError::none)
            halt();
        if (!Board::Display::busy() && (shown == Converter::State::preview))
            preview_visible = true;
        if (!Board::Display::busy() && (error || (shown != converter.state()) || ready)) {
            shown = converter.state();
            render(error, ready);
            Board::Display::begin_frame();
        }
        service_power<Board>(now);
        if (!Board::stack_intact() || (Board::Watchdog::service(now) != fw::WatchdogError::none))
            halt();
        Board::Watchdog::mark(0);
    }
}
