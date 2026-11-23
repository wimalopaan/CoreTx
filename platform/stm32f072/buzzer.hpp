// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "platform/stm32f072/peripherals.hpp"

namespace coretx::firmware {

enum class ClickTone : std::uint8_t { control, multiswitch };

template <typename Io, unsigned Timer, Pin Output>
    requires stm32f072::TimerChannel1<Timer, Output>
class Buzzer {
    using Peripheral = stm32f072::TimerTraits<Timer>;

  public:
    Buzzer() = delete;

    static void initialize() noexcept {
        using namespace stm32f072::registers;
        Io::write(rcc_ahbenr, Io::read(rcc_ahbenr) | gpio_clock(Output));
        Io::write(Peripheral::enable_register,
                  Io::read(Peripheral::enable_register) | Peripheral::enable_mask);
        (void)Io::read(Peripheral::enable_register);
        configure_alternate<Io>(Output, Peripheral::alternate(Output), false);
        write(offsetof(TIM_TypeDef, CR1), 0);
        write(offsetof(TIM_TypeDef, DIER), 0);
        write(offsetof(TIM_TypeDef, PSC), stm32f072::cpu_hz / timer_hz - 1U);
        write(offsetof(TIM_TypeDef, CCMR1), TIM_CCMR1_OC1M_1 | TIM_CCMR1_OC1M_2 | TIM_CCMR1_OC1PE);
        write(offsetof(TIM_TypeDef, CCER), TIM_CCER_CC1P);
        if constexpr (Peripheral::main_output)
            write(offsetof(TIM_TypeDef, BDTR), TIM_BDTR_MOE);
        until_ = {};
        active_ = false;
    }
    static void click(const TimestampUs now, const ClickTone tone = ClickTone::control) noexcept {
        play(now, tone == ClickTone::multiswitch ? multiswitch_hz : click_hz, click_duration);
    }
    static void play(const TimestampUs now, const std::uint32_t hz,
                     const DurationUs duration) noexcept {
        const auto period = timer_hz / hz;
        write(offsetof(TIM_TypeDef, ARR), period - 1U);
        write(offsetof(TIM_TypeDef, CCR1), period / 2U);
        write(offsetof(TIM_TypeDef, CNT), 0);
        write(offsetof(TIM_TypeDef, EGR), TIM_EGR_UG);
        write(offsetof(TIM_TypeDef, CCER), TIM_CCER_CC1E | TIM_CCER_CC1P);
        write(offsetof(TIM_TypeDef, CR1), TIM_CR1_ARPE | TIM_CR1_CEN);
        until_ = now + duration;
        active_ = true;
    }
    static void service(const TimestampUs now) noexcept {
        if (active_ && ((now - until_) < DurationUs{0x8000'0000U}))
            stop();
    }
    static void stop() noexcept {
        write(offsetof(TIM_TypeDef, CCER), TIM_CCER_CC1P);
        write(offsetof(TIM_TypeDef, CR1), 0);
        active_ = false;
    }

  private:
    static constexpr std::uint32_t timer_hz = 1'000'000U;
    static constexpr std::uint32_t click_hz = 2'200U;
    static constexpr std::uint32_t multiswitch_hz = 1'100U;
    static constexpr auto click_duration = 10'000_us;

    static void write(const std::size_t offset, const std::uint32_t value) noexcept {
        Io::write(Peripheral::base + offset, value);
    }
    inline static TimestampUs until_{};
    inline static bool active_{};
};

} // namespace coretx::firmware
