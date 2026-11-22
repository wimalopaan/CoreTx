// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "platform/stm32f072/afhds2a.hpp"
#include "platform/stm32f072/clock.hpp"

namespace coretx::firmware {

class InterruptGuard {
  public:
    InterruptGuard() noexcept : previous_(__get_PRIMASK()) { __disable_irq(); }
    ~InterruptGuard() {
        __DMB();
        __set_PRIMASK(previous_);
    }
    InterruptGuard(const InterruptGuard &) = delete;
    InterruptGuard &operator=(const InterruptGuard &) = delete;

  private:
    std::uint32_t previous_;
};

template <typename Io, typename Guard = InterruptGuard, typename Assignment = Hardware>
class RadioTimer {
    using Timer = stm32f072::TimerTraits<Assignment::clock_timer>;
    using Exti = stm32f072::ExtiTraits<Assignment::radio.status>;
    static_assert(Assignment::clock_timer == 2, "Radio compare shares the system TIM2 clock");
    inline static constexpr auto compare = Timer::base + offsetof(TIM_TypeDef, CCR1);
    inline static constexpr auto enable = NVIC_BASE + offsetof(NVIC_Type, ISER);
    inline static constexpr auto disable = NVIC_BASE + offsetof(NVIC_Type, ICER);
    inline static constexpr auto clear_pending = NVIC_BASE + offsetof(NVIC_Type, ICPR);
    inline static constexpr auto irq_mask =
        (std::uint32_t{1} << Timer::irq) | (std::uint32_t{1} << Exti::irq);
    inline static constexpr auto exti_mask = EXTI_BASE + offsetof(EXTI_TypeDef, IMR);
    inline static constexpr auto exti_pending = EXTI_BASE + offsetof(EXTI_TypeDef, PR);
    inline static constexpr auto gio2_line = Exti::line;
    using Handler = DurationUs (*)(void *, bool) noexcept;

  public:
    RadioTimer() = delete;
    inline static constexpr auto interval = detail::afhds2a_config::poll_interval;

    [[nodiscard]] static TimestampUs now() noexcept {
        return Clock<Io, Assignment::clock_timer>::sample();
    }

    static void start(const Handler handler, void *const object) noexcept {
        const Guard guard{};
        stop();
        handler_ = handler;
        object_ = object;
        for (const auto irq : {Timer::irq, Exti::irq}) {
            const auto priority =
                NVIC_BASE + offsetof(NVIC_Type, IP) + (static_cast<unsigned>(irq) / 4U) * 4U;
            Io::write(priority, Io::read(priority) & ~(std::uint32_t{0xFF} << ((irq % 4) * 8)));
        }
        namespace reg = stm32f072::registers;
        Io::write(reg::rcc_apb2enr, Io::read(reg::rcc_apb2enr) | RCC_APB2ENR_SYSCFGCOMPEN);
        (void)Io::read(reg::rcc_apb2enr);
        constexpr auto route = Exti::route_register;
        Io::write(route, (Io::read(route) & ~Exti::route_mask) | Exti::route);
        constexpr auto falling = EXTI_BASE + offsetof(EXTI_TypeDef, FTSR);
        constexpr auto rising = EXTI_BASE + offsetof(EXTI_TypeDef, RTSR);
        Io::write(falling, Io::read(falling) | gio2_line);
        Io::write(rising, Io::read(rising) & ~gio2_line);
        Io::write(exti_pending, gio2_line);
        Io::write(exti_mask, Io::read(exti_mask) | gio2_line);
        Io::write(compare, (now() + interval).count());
        Io::write(stm32f072::registers::tim2_dier,
                  Io::read(stm32f072::registers::tim2_dier) | TIM_DIER_CC1IE);
        Io::write(enable, irq_mask);
    }

    static void stop() noexcept {
        const Guard guard{};
        Io::write(disable, irq_mask);
        Io::write(exti_mask, Io::read(exti_mask) & ~gio2_line);
        Io::write(exti_pending, gio2_line);
        Io::write(stm32f072::registers::tim2_dier,
                  Io::read(stm32f072::registers::tim2_dier) & ~std::uint32_t{TIM_DIER_CC1IE});
        Io::write(stm32f072::registers::tim2_sr, ~std::uint32_t{TIM_SR_CC1IF});
        Io::write(clear_pending, irq_mask);
        handler_ = nullptr;
        object_ = nullptr;
    }

    static void interrupt() noexcept {
        if ((Io::read(stm32f072::registers::tim2_sr) & TIM_SR_CC1IF) == 0)
            return;
        Io::write(stm32f072::registers::tim2_sr, ~std::uint32_t{TIM_SR_CC1IF});
        if (handler_ == nullptr)
            return;
        dispatch(false);
    }

    static void gio2_interrupt() noexcept {
        if ((Io::read(exti_pending) & gio2_line) == 0)
            return;
        Io::write(exti_pending, gio2_line);
        if (handler_ != nullptr)
            dispatch(true);
    }

  private:
    static void dispatch(const bool gio2) noexcept {
        const auto delay = handler_(object_, gio2);
        // Rearm from the live counter: a delayed interrupt must not miss a compare until wrap.
        Io::write(compare, (now() + delay).count());
    }
    inline static Handler handler_{};
    inline static void *object_{};
};

template <typename Bus, typename Timer, typename Guard = InterruptGuard> class TimedAfhds2a {
  public:
    TimedAfhds2a() noexcept = default;
    TimedAfhds2a(const TimedAfhds2a &) = delete;
    TimedAfhds2a &operator=(const TimedAfhds2a &) = delete;

    struct Radio : Bus {
        [[nodiscard]] static A7105Info info() noexcept {
            const Guard guard{};
            return Bus::info();
        }
    };

    void initialize(const afhds2a::TxId tx, const afhds2a::RxId rx,
                    const TimestampUs now) noexcept {
        const Guard guard{};
        Timer::stop();
        valid_ = false;
        sampled_at_ = now;
        core_.initialize(tx, rx, now);
        Timer::start(interrupt, this);
    }
    void activate() noexcept {
        const Guard guard{};
        core_.activate();
    }
    void deactivate() noexcept {
        const Guard guard{};
        core_.deactivate();
    }
    void set_channel_mode(const afhds2a::ChannelMode mode) noexcept {
        const Guard guard{};
        core_.set_channel_mode(mode);
    }
    [[nodiscard]] bool bind(const TimestampUs now) noexcept {
        const Guard guard{};
        return core_.bind(now);
    }
    void service(const TimestampUs, const afhds2a::Channels &channels, const bool valid,
                 const TimestampUs sampled_at) noexcept {
        const Guard guard{};
        channels_ = channels;
        valid_ = valid;
        sampled_at_ = sampled_at;
    }
    [[nodiscard]] bool ready() const noexcept {
        const Guard guard{};
        return core_.ready();
    }
    [[nodiscard]] Afhds2aError error() const noexcept {
        const Guard guard{};
        return core_.error();
    }
    [[nodiscard]] afhds2a::Session session() const noexcept {
        const Guard guard{};
        return core_.session();
    }
    [[nodiscard]] afhds2a::ReceiverTelemetry telemetry() const noexcept {
        const Guard guard{};
        return core_.telemetry();
    }
    [[nodiscard]] bool take_receiver_changed() noexcept {
        const Guard guard{};
        return core_.take_receiver_changed();
    }

  private:
    static DurationUs interrupt(void *const object, const bool gio2) noexcept {
        auto &self = *static_cast<TimedAfhds2a *>(object);
        if (gio2)
            self.core_.gio2_event();
        self.core_.service(Timer::now(), self.channels_, self.valid_, self.sampled_at_);
        return self.core_.service_interval();
    }
    Afhds2a<Bus> core_{};
    afhds2a::Channels channels_{};
    TimestampUs sampled_at_{};
    bool valid_{};
};

} // namespace coretx::firmware
