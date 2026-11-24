// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/common/extended_inputs.hpp"
#include "boards/common/radio_board.hpp"
#include "boards/common/rf_modules.hpp"
#include "boards/i6s/power.hpp"
#include "boards/i6s/setup_controls.hpp"
#include "boards/i6s/touch_input.hpp"
#include "boards/i6x/keys.hpp"
#include "platform/stm32f072/adc.hpp"
#include "platform/stm32f072/bootloader.hpp"
#include "platform/stm32f072/eeprom.hpp"
#include "platform/stm32f072/lcd.hpp"
#include "platform/stm32f072/measurements.hpp"
#include "platform/stm32f072/watchdog.hpp"

#include <string_view>
#include <type_traits>

extern "C" {
extern std::uint32_t __stack_limit;
}

namespace coretx::firmware {

template <typename Io, typename Assignment, bool Touch = requires { Assignment::touch; }>
struct SenderInput {
    using Type = PhysicalInput<i6x::Keys<Io, Assignment>>;
};
template <typename Io, typename Assignment> struct SenderInput<Io, Assignment, true> {
    using Type = TouchInput<i6s::Touch<Io, Assignment>, i6s::SetupControls<Io, Assignment>>;
};

template <typename Io, typename Assignment = Hardware> struct SenderBoard {
    SenderBoard() = delete;
    using Hardware = Assignment;
    static_assert(Hardware::buzzer_timer != Hardware::clock_timer,
                  "Buzzer must not reconfigure the system clock");
    inline static constexpr std::string_view name = Hardware::name;
    inline static constexpr bool sb_three_position = Hardware::sb_three_position;
    using Adc = firmware::Adc<Io, Hardware, Hardware::sender_inputs, coherent_adc_snapshots>;
    using Input = typename SenderInput<Io, Hardware>::Type;
    using Inputs = ExtendedInputs<Io, Hardware>;
    using Display = Lcd<Io, Hardware>;
    using Probe = StorageProbe<Io, Hardware>;
    using Memory = Eeprom<Io, Hardware>;
    using Watchdog = firmware::Watchdog<Io>;
    // The i6S switches itself off with a held power button; the i6X has a mechanical switch.
    using Power = std::conditional_t<Hardware::power_latch, i6s::PowerButton, void>;
    using Bootloader = stm32f072::Bootloader<Io>;
    using Modules = RfModules<Io, Hardware>;
    using ExternalRFModule = typename Modules::ExternalRFModule;
    using RadioTimer = typename Modules::RadioTimer;
    using InternalRFModule = typename Modules::InternalRFModule;
    using Radio = typename Modules::Radio;

    static void prepare_power() noexcept {
        // The i6X has a mechanical switch; only the i6S needs the PB15 latch.
        if constexpr (Hardware::power_latch) {
            i6s::hold_power<Io, Hardware>(!Bootloader::cold_start());
        }
    }
    [[nodiscard]] static bool power_on_startup() noexcept {
        if constexpr (Hardware::power_latch) {
            if (Bootloader::cold_start()) {
                i6s::initialize_power_button<Io, Hardware>();
                Bootloader::start_sampling();
                auto pressed = power_button_pressed();
                std::uint32_t held_ms{};
                while (held_ms < (Power::startup_hold_duration.count() / 1'000U)) {
                    if (!Bootloader::wait_tick()) {
                        Bootloader::stop_sampling();
                        return false;
                    }
                    const auto previous = pressed;
                    pressed = power_button_pressed();
                    held_ms = pressed && previous ? held_ms + 1U : 0U;
                }
                Bootloader::stop_sampling();
                i6s::hold_power<Io, Hardware>();
            }
        }
        return true;
    }
    [[nodiscard]] static stm32f072::BootloaderRequest bootloader_request() noexcept {
        using Request = stm32f072::BootloaderRequest;
        if (!Bootloader::cold_start())
            return Request::none;
        using Buttons = std::conditional_t<Hardware::power_latch, i6s::SetupControls<Io, Hardware>,
                                           i6x::detail::KeysState<Io, Hardware>>;
        Buttons buttons{};
        if constexpr (Hardware::power_latch)
            buttons.initialize();
        else
            buttons.initialize(TimestampUs{});
        Bootloader::start_sampling();
        bool sampled = true;
        for (std::uint32_t ms = 1; ms <= 30; ++ms) {
            if (!Bootloader::wait_tick()) {
                sampled = false;
                break;
            }
            const auto now = TimestampUs{ms * 1'000U};
            if constexpr (Hardware::power_latch)
                (void)buttons.service(now, false, 0, 0, false);
            else
                (void)buttons.service(now);
        }
        Bootloader::stop_sampling();
        if constexpr (!Hardware::power_latch)
            for (const auto pin : Hardware::key_columns)
                drive<Io>(pin, true);
        if (!sampled)
            return Request::error;
        if constexpr (Hardware::power_latch)
            return (buttons.buttons() & buttons.sample_buttons()) == 3 ? Request::requested
                                                                       : Request::none;
        else
            return (buttons.trim_buttons() & Hardware::bootloader_trim_mask) ==
                           Hardware::bootloader_trim_mask
                       ? Request::requested
                       : Request::none;
    }
    [[nodiscard]] static bool bootloader_message() noexcept {
        // The LCD needs a microsecond clock, but ROM DFU must not depend on the HSE/PLL.
        if ((Clock<Io, Hardware::clock_timer>::initialize_timer(8'000'000U) !=
             stm32f072::HardwareError::none) ||
            (Display::initialize() != LcdError::none))
            return false;
        coretx::ui::Framebuffer display{};
        display.text(22, 28, "DFU-Bootloader");
        Display::begin_frame();
        const auto started = Io::now_us();
        for (std::uint32_t attempt = 0; attempt < stm32f072::ready_timeout; ++attempt) {
            if ((Display::service(display) != LcdError::none) ||
                ((Io::now_us() - started) >= 250_ms))
                return false;
            if (!Display::busy())
                return true;
        }
        return false;
    }
    [[nodiscard]] static bool bootloader_startup() noexcept {
        if (!power_on_startup())
            return false;
        const auto request = bootloader_request();
        if (request == stm32f072::BootloaderRequest::error)
            return false;
        if (request == stm32f072::BootloaderRequest::requested) {
            if (!bootloader_message())
                return false;
            __disable_irq();
            if (!Bootloader::prepare())
                return false;
            coretx_enter_rom_bootloader(Bootloader::address);
        }
        return true;
    }
    [[nodiscard]] static afhds2a::TxId transmitter_id() noexcept {
        return firmware::transmitter_id<Io>();
    }
    static void power_off() noexcept {
        if constexpr (!std::is_void_v<RadioTimer>)
            RadioTimer::stop();
        external_module_off<Io, Hardware>();
        internal_front_end_off<Io, Hardware::radio>();
        if constexpr (!std::is_void_v<ExternalRFModule>)
            ExternalRFModule::power_off();
        if constexpr (!std::is_void_v<Radio>)
            Radio::power_off();
    }
    [[nodiscard]] static stm32f072::HardwareError initialize() noexcept {
        const auto error = Clock<Io, Hardware::clock_timer>::initialize();
        if constexpr (Hardware::power_latch)
            if (error == stm32f072::HardwareError::none)
                i6s::initialize_power_button<Io, Hardware>();
        return error;
    }
    [[nodiscard]] static bool power_button_pressed() noexcept
        requires(Hardware::power_latch)
    {
        return i6s::power_button_pressed<Io, Hardware>();
    }
    static void release_power() noexcept
        requires(Hardware::power_latch)
    {
        i6s::release_power<Io, Hardware>();
    }
    [[nodiscard]] static TimestampUs now_us() noexcept { return Io::now_us(); }
    [[nodiscard]] static bool stack_intact() noexcept {
        const volatile std::uint32_t *const guard = &__stack_limit;
        return inspect_stack(guard, 4).guard_intact;
    }
};

} // namespace coretx::firmware
