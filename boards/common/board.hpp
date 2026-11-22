// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/common/extended_inputs.hpp"
#include "boards/common/radio_board.hpp"
#include "boards/common/sender_input.hpp"
#include "boards/i6s/power.hpp"
#include "boards/i6x/keys.hpp"
#include "platform/stm32f072/adc.hpp"
#include "platform/stm32f072/eeprom.hpp"
#include "platform/stm32f072/lcd.hpp"
#include "platform/stm32f072/measurements.hpp"
#include "platform/stm32f072/watchdog.hpp"
#if CORETX_A7105
#include "platform/stm32f072/radio_timer.hpp"
#endif
#if CORETX_CRSF
#include "platform/stm32f072/crsf.hpp"
#endif

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
    using Type = TouchInput<i6s::Touch<Io, Assignment>>;
};

template <typename Io, typename Assignment = Hardware> struct SenderBoard {
    SenderBoard() = delete;
    using Hardware = Assignment;
    static_assert(Hardware::buzzer_timer != Hardware::clock_timer,
                  "Buzzer must not reconfigure the system clock");
    inline static constexpr std::string_view name = Hardware::name;
    inline static constexpr bool sb_three_position = Hardware::sb_three_position;
    using Adc = i6x::Adc<Io, Hardware::sender_inputs, i6x::coherent_adc_snapshots, Hardware>;
    using Input = typename SenderInput<Io, Hardware>::Type;
    using Inputs = ExtendedInputs<Io, Hardware>;
    using Display = Lcd<Io, Hardware>;
    using Probe = i6x::StorageProbe<Io, Hardware>;
    using Memory = i6x::Eeprom<Io, Hardware>;
    using Watchdog = firmware::Watchdog<Io>;
#if CORETX_CRSF
    using Transmitter = i6x::Crsf<Io, Hardware>;
#endif
#if CORETX_A7105
    using RadioTimer = firmware::RadioTimer<Io, InterruptGuard, Hardware>;
    using Internal = TimedAfhds2a<A7105<Io, Hardware::radio>, RadioTimer>;
    using Radio = typename Internal::Radio;
#endif

    static void prepare_power() noexcept {
        // The i6X has a mechanical switch; only the i6S needs the PB15 latch.
        if constexpr (Hardware::power_latch) {
            i6s::hold_power<Io, Hardware>();
        }
    }
    [[nodiscard]] static afhds2a::TxId transmitter_id() noexcept {
        return firmware::transmitter_id<Io>();
    }
    static void power_off() noexcept {
#if CORETX_A7105
        RadioTimer::stop();
#endif
        external_module_off<Io, Hardware>();
        internal_front_end_off<Io, Hardware::radio>();
#if CORETX_CRSF
        Transmitter::power_off();
#endif
#if CORETX_A7105
        Radio::power_off();
#endif
    }
    [[nodiscard]] static stm32f072::HardwareError initialize() noexcept {
        return Clock<Io, Hardware::clock_timer>::initialize();
    }
    [[nodiscard]] static TimestampUs now_us() noexcept { return Io::now_us(); }
    [[nodiscard]] static bool stack_intact() noexcept {
        const volatile std::uint32_t *const guard = &__stack_limit;
        return inspect_stack(guard, 4).guard_intact;
    }
};

} // namespace coretx::firmware
