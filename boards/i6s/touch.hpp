// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/i6s/hardware.hpp"
#include "boards/i6s/touch_navigation.hpp"
#include "platform/stm32f072/gpio.hpp"

namespace coretx::firmware::i6s {

enum class TouchError : std::uint8_t {
    none,
    not_initialized,
    timeout,
    nack,
    bus,
    arbitration,
    overrun,
    packet
};
enum class TouchEvent : std::uint8_t { none, sample, failed };

template <typename Assignment>
struct TouchRegisters : stm32f072::I2cTraits<Assignment::touch.peripheral> {
    using Bus = stm32f072::I2cTraits<Assignment::touch.peripheral>;
    static constexpr auto timing = Bus::timingr;
    static constexpr auto interrupt =
        Assignment::touch_interrupt.port + offsetof(GPIO_TypeDef, IDR);
};
using touch_registers = TouchRegisters<Hardware>;

namespace detail {

template <typename Io, typename Assignment = Hardware>
class TouchState : TouchRegisters<Assignment> {
    using Registers = TouchRegisters<Assignment>;
    using Bus = typename Registers::Bus;
    using Edge = stm32f072::ExtiTraits<Assignment::touch_interrupt>;
    inline static constexpr auto pending = EXTI_BASE + offsetof(EXTI_TypeDef, PR);
    using Registers::cr2;
    using Registers::icr;
    using Registers::interrupt;
    using Registers::isr;
    using Registers::rxdr;
    using Registers::txdr;
    static_assert((Bus::alternate(Assignment::touch.scl, true) != stm32f072::invalid_af) &&
                      (Bus::alternate(Assignment::touch.sda, false) != stm32f072::invalid_af),
                  "Invalid touch I2C pins");

  public:
    constexpr void initialize() noexcept {
        *this = {};
        using namespace stm32f072::registers;
        Io::write(rcc_ahbenr,
                  Io::read(rcc_ahbenr) |
                      gpio_clocks(std::array{reset, Assignment::touch_interrupt,
                                             Assignment::touch.scl, Assignment::touch.sda}));
        (void)Io::read(rcc_ahbenr);
        drive<Io>(reset, false);
        configure<Io>(reset, false);
        configure_input<Io>({Assignment::touch_interrupt.port, Assignment::touch_interrupt.index});
        Io::write(rcc_apb2enr, Io::read(rcc_apb2enr) | RCC_APB2ENR_SYSCFGCOMPEN);
        (void)Io::read(rcc_apb2enr);
        Io::write(Edge::route_register,
                  (Io::read(Edge::route_register) & ~Edge::route_mask) | Edge::route);
        constexpr auto mask = EXTI_BASE + offsetof(EXTI_TypeDef, IMR);
        constexpr auto event_mask = EXTI_BASE + offsetof(EXTI_TypeDef, EMR);
        constexpr auto falling = EXTI_BASE + offsetof(EXTI_TypeDef, FTSR);
        constexpr auto rising = EXTI_BASE + offsetof(EXTI_TypeDef, RTSR);
        // Latch short INT pulses in hardware without enabling an interrupt handler.
        Io::write(mask, Io::read(mask) & ~Edge::line);
        Io::write(event_mask, Io::read(event_mask) & ~Edge::line);
        Io::write(rising, Io::read(rising) & ~Edge::line);
        Io::write(falling, Io::read(falling) | Edge::line);
        Io::write(pending, Edge::line);
        for (const std::uint8_t index :
             {Assignment::touch.scl.index, Assignment::touch.sda.index}) {
            const auto port = index == Assignment::touch.scl.index ? Assignment::touch.scl.port
                                                                   : Assignment::touch.sda.port;
            const auto shift = index * 2U;
            const auto moder = port + offsetof(GPIO_TypeDef, MODER);
            const auto otyper = port + offsetof(GPIO_TypeDef, OTYPER);
            const auto speed = port + offsetof(GPIO_TypeDef, OSPEEDR);
            const auto pull = port + offsetof(GPIO_TypeDef, PUPDR);
            constexpr bool common_bank =
                (Assignment::touch.scl.index / 8U) == (Assignment::touch.sda.index / 8U);
            const auto bank = common_bank ? Assignment::touch.scl.index / 8U : index / 8U;
            const auto afr = port + offsetof(GPIO_TypeDef, AFR) + bank * sizeof(std::uint32_t);
            const auto af_shift = (index - bank * 8U) * 4U;
            Io::write(otyper, Io::read(otyper) | (1U << index));
            Io::write(pull, (Io::read(pull) & ~(3U << shift)) | (1U << shift));
            Io::write(speed, Io::read(speed) | (3U << shift));
            const auto alternate = index == Assignment::touch.scl.index
                                       ? Bus::alternate(Assignment::touch.scl, true)
                                       : Bus::alternate(Assignment::touch.sda, false);
            Io::write(afr, (Io::read(afr) & ~(15U << af_shift)) |
                               (std::uint32_t{alternate} << af_shift));
            Io::write(moder, (Io::read(moder) & ~(3U << shift)) | (2U << shift));
        }
        Io::write(Bus::enable_register, Io::read(Bus::enable_register) | Bus::enable_mask);
        (void)Io::read(rcc_apb1enr);
        constexpr auto clock = RCC_BASE + offsetof(RCC_TypeDef, CFGR3);
        Io::write(clock, Io::read(clock) | Bus::clock_mask);
        Io::write(Bus::reset_register, Io::read(Bus::reset_register) | Bus::reset_mask);
        Io::write(Bus::reset_register, Io::read(Bus::reset_register) & ~Bus::reset_mask);
        Io::write(Registers::cr1, 0);
        Io::write(Registers::timing, 0x00B0'1A4B);
        Io::write(Registers::icr, clear_flags);
        Io::write(Registers::cr1, I2C_CR1_PE);
        since_ = Io::now_us();
        error_ = TouchError::none;
        stage_ = Stage::reset_low;
    }
    [[nodiscard]] constexpr TouchEvent service(const TimestampUs now) noexcept {
        const auto started = Io::now_us();
        // Drain a 400 kHz report without unbounded flight-loop latency.
        for (unsigned step = 0; step < 512; ++step) {
            const auto elapsed = Io::now_us() - started;
            if ((step != 0) && (elapsed >= 300_us))
                break;
            const auto event = advance(now);
            if ((event != TouchEvent::none) || !transfer_)
                return event;
        }
        return TouchEvent::none;
    }
    [[nodiscard]] constexpr TouchError error() const noexcept { return error_; }
    [[nodiscard]] constexpr bool ready() const noexcept { return ready_; }
    [[nodiscard]] constexpr bool received() const noexcept { return received_; }
    [[nodiscard]] constexpr const TouchSample &sample() const noexcept { return sample_; }
    [[nodiscard]] constexpr std::uint32_t frames() const noexcept { return frames_; }
    [[nodiscard]] constexpr const auto &packet() const noexcept { return bytes_; }
    [[nodiscard]] constexpr std::size_t packet_bytes() const noexcept { return position_; }

  private:
    [[nodiscard]] constexpr TouchEvent advance(const TimestampUs now) noexcept {
        if (stage_ == Stage::failed) {
            if ((error_ == TouchError::not_initialized) || (error_ == TouchError::packet) ||
                ((now - since_) < 250'000_us))
                return TouchEvent::failed;
            const auto error = error_;
            const auto frames = frames_;
            initialize();
            // Keep the failure visible until a complete report succeeds.
            error_ = error;
            frames_ = frames;
            return TouchEvent::failed;
        }
        const auto previous_stage = stage_;
        bool received_byte{};
        std::uint32_t flags{};
        if (transfer_) {
            flags = Io::read(isr);
            if ((flags & I2C_ISR_BERR) != 0)
                return fail(TouchError::bus);
            if ((flags & I2C_ISR_ARLO) != 0)
                return fail(TouchError::arbitration);
            if ((flags & I2C_ISR_OVR) != 0)
                return fail(TouchError::overrun);
            if ((flags & I2C_ISR_NACKF) != 0)
                return fail(TouchError::nack);
        }
        switch (stage_) {
        case Stage::reset_low:
            if ((now - since_) >= 20'001_us) {
                drive<Io>(reset, true);
                since_ = Io::now_us();
                stage_ = Stage::boot;
            }
            break;
        case Stage::boot:
            if ((now - since_) >= 60'001_us) {
                ready_ = true;
                Io::write(pending, Edge::line);
                since_ = now - 8'000_us;
                stage_ = Stage::idle;
            }
            break;
        case Stage::idle: {
            const bool edge = (Io::read(pending) & Edge::line) != 0;
            const bool asserted = (Io::read(interrupt) & Edge::line) == 0;
            if (!edge && !asserted && (sample_.contacts != 0) &&
                ((now - sampled_at_) >= TouchNavigation::release_timeout_us)) {
                sample_.contacts = 0;
                sample_.gesture = Gesture::none;
                sample_.position_valid = false;
                sample_.synthetic = true;
                return TouchEvent::sample;
            } else if (edge ||
                       ((asserted || (sample_.contacts != 0)) && ((now - since_) >= 8'000_us))) {
                // Lift need not pulse INT: poll known contacts, never an untouched idle panel.
                Io::write(pending, Edge::line);
                start(now);
            }
            break;
        }
        case Stage::bus_wait:
            if ((flags & I2C_ISR_BUSY) == 0) {
                Io::write(cr2, address | (1U << I2C_CR2_NBYTES_Pos) | I2C_CR2_START);
                stage_ = Stage::reg;
            }
            break;
        case Stage::reg:
            if ((flags & I2C_ISR_TXIS) != 0) {
                Io::write(txdr, 0);
                stage_ = Stage::restart;
            }
            break;
        case Stage::restart:
            if ((flags & I2C_ISR_TC) != 0) {
                position_ = 0;
                Io::write(cr2, address | (7U << I2C_CR2_NBYTES_Pos) | I2C_CR2_START |
                                   I2C_CR2_RD_WRN | I2C_CR2_AUTOEND);
                stage_ = Stage::receive;
            }
            break;
        case Stage::receive:
            if ((flags & I2C_ISR_RXNE) != 0) {
                bytes_[position_++] = static_cast<std::uint8_t>(Io::read(rxdr));
                received_byte = true;
                if (position_ == bytes_.size())
                    stage_ = Stage::stop;
            } else if ((flags & I2C_ISR_STOPF) != 0) {
                return fail(TouchError::packet);
            }
            break;
        case Stage::stop:
            if ((flags & I2C_ISR_STOPF) != 0) {
                Io::write(icr, I2C_ICR_STOPCF);
                transfer_ = false;
                TouchSample decoded{};
                if (!decode_touch(bytes_, decoded))
                    return fail(TouchError::packet);
                sample_ = decoded;
                error_ = TouchError::none;
                sampled_at_ = now;
                received_ = true;
                ++frames_;
                stage_ = Stage::idle;
                return TouchEvent::sample;
            }
            break;
        case Stage::failed:
            break;
        }
        if (transfer_) {
            if ((stage_ != previous_stage) || received_byte)
                progressed_at_ = now;
            else if ((now - progressed_at_) >= 10'000_us)
                return fail(TouchError::timeout);
        }
        return TouchEvent::none;
    }

    enum class Stage : std::uint8_t {
        reset_low,
        boot,
        idle,
        bus_wait,
        reg,
        restart,
        receive,
        stop,
        failed
    };
    inline static constexpr auto reset = Assignment::touch_reset;
    inline static constexpr std::uint32_t address = 0x38U << 1;
    inline static constexpr std::uint32_t clear_flags =
        I2C_ICR_NACKCF | I2C_ICR_STOPCF | I2C_ICR_BERRCF | I2C_ICR_ARLOCF | I2C_ICR_OVRCF;
    constexpr void start(const TimestampUs now) noexcept {
        Io::write(icr, clear_flags);
        since_ = now;
        transfer_ = true;
        stage_ = Stage::bus_wait;
    }
    constexpr TouchEvent fail(const TouchError error) noexcept {
        Io::write(Registers::cr2, I2C_CR2_STOP);
        Io::write(Registers::cr1, 0);
        Io::write(touch_registers::icr, clear_flags);
        sample_ = {};
        error_ = error;
        ready_ = false;
        received_ = false;
        transfer_ = false;
        since_ = Io::now_us();
        stage_ = Stage::failed;
        return TouchEvent::failed;
    }

    TouchSample sample_{};
    std::array<std::uint8_t, 7> bytes_{};
    std::size_t position_{};
    TimestampUs since_{};
    TimestampUs sampled_at_{};
    TimestampUs progressed_at_{};
    std::uint32_t frames_{};
    TouchError error_{TouchError::not_initialized};
    Stage stage_{Stage::failed};
    bool transfer_{};
    bool ready_{};
    bool received_{};
};

} // namespace detail

template <typename Io, typename Assignment = Hardware> class Touch {
  public:
    Touch() = delete;
    static void initialize() noexcept { state_.initialize(); }
    [[nodiscard]] static TouchEvent service(const TimestampUs now) noexcept {
        return state_.service(now);
    }
    [[nodiscard]] static TouchError error() noexcept { return state_.error(); }
    [[nodiscard]] static bool ready() noexcept { return state_.ready(); }
    [[nodiscard]] static bool received() noexcept { return state_.received(); }
    [[nodiscard]] static const TouchSample &sample() noexcept { return state_.sample(); }
    [[nodiscard]] static std::uint32_t frames() noexcept { return state_.frames(); }
    [[nodiscard]] static const auto &packet() noexcept { return state_.packet(); }
    [[nodiscard]] static std::size_t packet_bytes() noexcept { return state_.packet_bytes(); }

  private:
    inline static constinit detail::TouchState<Io, Assignment> state_{};
};

} // namespace coretx::firmware::i6s
