// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/common/hardware.hpp"
#include "coretx/features.hpp"
#include "coretx/ui/translation.hpp"
#include "platform/stm32f072/gpio.hpp"

#include <array>

namespace coretx::firmware {

using i2c_registers = stm32f072::I2cTraits<firmware::Hardware::storage.peripheral>;

enum class StorageProbeState : std::uint8_t { idle, scanning, found, not_found, failed };
enum class I2cError : std::uint8_t {
    none,
    busy,
    timeout,
    bus,
    arbitration,
    overrun,
    nack,
    invalid_request
};

[[nodiscard]] constexpr const char *error_message(const I2cError error) noexcept {
    switch (error) {
    case I2cError::none:
        return nullptr;
    case I2cError::busy:
        return coretx::ui::tr::bus_busy;
    case I2cError::timeout:
        return coretx::ui::tr::timeout;
    case I2cError::bus:
        return coretx::ui::tr::bus_error;
    case I2cError::arbitration:
        return coretx::ui::tr::arbitration;
    case I2cError::overrun:
        return coretx::ui::tr::overrun;
    case I2cError::nack:
        return coretx::ui::tr::no_ack;
    case I2cError::invalid_request:
        return coretx::ui::tr::bad_request;
    }
    return coretx::ui::tr::unknown;
}

struct StorageProbeResult {
    inline static constexpr std::uint8_t first_address = 0x50;
    inline static constexpr std::uint8_t last_address = 0x57;
    StorageProbeState state{};
    I2cError error{};
    std::uint8_t addresses{};
};

namespace detail {

template <typename Io, typename Assignment = firmware::Hardware>
class ProbeState : stm32f072::I2cTraits<Assignment::storage.peripheral> {
    using Registers = stm32f072::I2cTraits<Assignment::storage.peripheral>;
    using Registers::cr2;
    using Registers::icr;
    using Registers::isr;
    static_assert((Registers::alternate(Assignment::storage.scl, true) != stm32f072::invalid_af) &&
                      (Registers::alternate(Assignment::storage.sda, false) !=
                       stm32f072::invalid_af),
                  "Invalid storage I2C pins");

  public:
    inline static constexpr auto first_address = StorageProbeResult::first_address;
    inline static constexpr auto last_address = StorageProbeResult::last_address;
    inline static constexpr DurationUs timeout_us{10'000};
    // Fast mode at 48 MHz, analog filter on, DNF=0; allow up to 300 ns rise/fall.
    // 41.67 ns prescaled ticks; SCLDEL=416.67 ns, SDADEL=208.33 ns.
    inline static constexpr std::uint32_t fast_timing =
        (1U << I2C_TIMINGR_PRESC_Pos) | (9U << I2C_TIMINGR_SCLDEL_Pos) |
        (5U << I2C_TIMINGR_SDADEL_Pos) | (19U << I2C_TIMINGR_SCLH_Pos) |
        (36U << I2C_TIMINGR_SCLL_Pos);
    // Standard mode at 48 MHz: 250 ns prescaled ticks, >= 5 us low/high.
    inline static constexpr std::uint32_t standard_timing =
        (11U << I2C_TIMINGR_PRESC_Pos) | (3U << I2C_TIMINGR_SCLDEL_Pos) |
        (1U << I2C_TIMINGR_SDADEL_Pos) | (19U << I2C_TIMINGR_SCLH_Pos) |
        (19U << I2C_TIMINGR_SCLL_Pos);
    inline static constexpr std::uint32_t timing =
        features::i2c_fast_mode ? fast_timing : standard_timing;
    constexpr void initialize() noexcept {
        namespace reg = stm32f072::registers;
        Io::write(reg::rcc_ahbenr, Io::read(reg::rcc_ahbenr) | gpio_clock(Assignment::storage.scl) |
                                       gpio_clock(Assignment::storage.sda));
        (void)Io::read(reg::rcc_ahbenr);
        for (const std::uint32_t index : std::array{std::uint32_t{Assignment::storage.scl.index},
                                                    std::uint32_t{Assignment::storage.sda.index}}) {
            const auto port = index == Assignment::storage.scl.index ? Assignment::storage.scl.port
                                                                     : Assignment::storage.sda.port;
            const auto moder = port + offsetof(GPIO_TypeDef, MODER);
            const auto otyper = port + offsetof(GPIO_TypeDef, OTYPER);
            const auto pupdr = port + offsetof(GPIO_TypeDef, PUPDR);
            const auto speed = port + offsetof(GPIO_TypeDef, OSPEEDR);
            constexpr bool common_bank =
                (Assignment::storage.scl.index / 8U) == (Assignment::storage.sda.index / 8U);
            const auto bank = common_bank ? Assignment::storage.scl.index / 8U : index / 8U;
            const auto afr = port + offsetof(GPIO_TypeDef, AFR) + bank * sizeof(std::uint32_t);
            const auto shift = index * 2U;
            const auto af_shift = (index - bank * 8U) * 4U;
            Io::write(moder, (Io::read(moder) & ~(3U << shift)) | (2U << shift));
            Io::write(otyper, Io::read(otyper) | (1U << index));
            Io::write(pupdr, (Io::read(pupdr) & ~(3U << shift)) | (1U << shift));
            Io::write(speed, (Io::read(speed) & ~(3U << shift)) | (3U << shift));
            const auto alternate = index == Assignment::storage.scl.index
                                       ? Registers::alternate(Assignment::storage.scl, true)
                                       : Registers::alternate(Assignment::storage.sda, false);
            Io::write(afr, (Io::read(afr) & ~(15U << af_shift)) |
                               (std::uint32_t{alternate} << af_shift));
        }
        Io::write(Registers::enable_register,
                  Io::read(Registers::enable_register) | Registers::enable_mask);
        (void)Io::read(reg::rcc_apb1enr);
        reset();
        state_ = StorageProbeState::scanning;
        error_ = I2cError::none;
        address_ = first_address;
        acknowledged_ = 0;
        active_ = false;
        nack_ = false;
        started_ = Io::now_us();
    }
    constexpr void service(const TimestampUs now) noexcept {
        if (state_ != StorageProbeState::scanning) {
            return;
        }
        const auto status = Io::read(isr);
        if ((status & (I2C_ISR_BERR | I2C_ISR_ARLO | I2C_ISR_OVR)) != 0) {
            fail((status & I2C_ISR_BERR) != 0   ? I2cError::bus
                 : (status & I2C_ISR_ARLO) != 0 ? I2cError::arbitration
                                                : I2cError::overrun);
            return;
        }
        if (!active_) {
            if ((status & I2C_ISR_BUSY) != 0) {
                if ((now - started_) >= timeout_us) {
                    fail(I2cError::busy);
                }
                return;
            }
            Io::write(icr, I2C_ICR_NACKCF | I2C_ICR_STOPCF);
            // Address-only write: no memory address or payload, so stored contents stay untouched.
            Io::write(cr2, (std::uint32_t{address_} << 1U) | I2C_CR2_AUTOEND | I2C_CR2_START);
            active_ = true;
            nack_ = false;
            started_ = now;
            return;
        }
        nack_ = nack_ || ((status & I2C_ISR_NACKF) != 0);
        if ((status & I2C_ISR_STOPF) != 0) {
            Io::write(icr, I2C_ICR_NACKCF | I2C_ICR_STOPCF);
            if (!nack_) {
                acknowledged_ |= static_cast<std::uint8_t>(1U << (address_ - first_address));
            }
            active_ = false;
            started_ = now;
            if (address_ == last_address) {
                state_ =
                    acknowledged_ == 0 ? StorageProbeState::not_found : StorageProbeState::found;
            } else {
                ++address_;
            }
        } else if ((now - started_) >= timeout_us) {
            fail(I2cError::timeout);
        }
    }

    [[nodiscard]] constexpr StorageProbeState state() const noexcept { return state_; }
    [[nodiscard]] constexpr I2cError error() const noexcept { return error_; }
    [[nodiscard]] constexpr std::uint8_t acknowledged() const noexcept { return acknowledged_; }
    [[nodiscard]] constexpr StorageProbeResult result() const noexcept {
        return {state_, error_, acknowledged_};
    }
    static constexpr void reset() noexcept {
        Io::write(Registers::reset_register,
                  Io::read(Registers::reset_register) | Registers::reset_mask);
        Io::write(Registers::reset_register,
                  Io::read(Registers::reset_register) & ~Registers::reset_mask);
        Io::write(Registers::cr1, 0);
        Io::write(Registers::timingr, timing);
        Io::write(Registers::icr, I2C_ICR_NACKCF | I2C_ICR_STOPCF | I2C_ICR_BERRCF |
                                      I2C_ICR_ARLOCF | I2C_ICR_OVRCF);
        Io::write(Registers::cr1, I2C_CR1_PE);
    }

  private:
    constexpr void fail(const I2cError error) noexcept {
        reset();
        Io::write(Registers::cr1, 0);
        error_ = error;
        state_ = StorageProbeState::failed;
    }

    TimestampUs started_{};
    StorageProbeState state_{};
    I2cError error_{};
    std::uint8_t address_{first_address};
    std::uint8_t acknowledged_{};
    bool active_{};
    bool nack_{};
};

} // namespace detail

template <typename Io, typename Assignment = firmware::Hardware> class StorageProbe {
    using State = detail::ProbeState<Io, Assignment>;

  public:
    StorageProbe() = delete;
    inline static constexpr auto first_address = State::first_address;
    inline static constexpr auto last_address = State::last_address;
    inline static constexpr auto timeout_us = State::timeout_us;
    inline static constexpr auto timing = State::timing;

    static void initialize() noexcept { state_.initialize(); }
    static void service(const TimestampUs now) noexcept { state_.service(now); }
    [[nodiscard]] static StorageProbeState state() noexcept { return state_.state(); }
    [[nodiscard]] static I2cError error() noexcept { return state_.error(); }
    [[nodiscard]] static std::uint8_t acknowledged() noexcept { return state_.acknowledged(); }
    [[nodiscard]] static StorageProbeResult result() noexcept { return state_.result(); }

  private:
    inline static constinit State state_{};
};

} // namespace coretx::firmware
