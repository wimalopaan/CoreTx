// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "platform/stm32f072/i2c.hpp"

#include <span>

namespace coretx::firmware {

namespace detail {

// FT24C128A: 16-bit memory address, 16 KiB, 64-byte write pages.
template <typename Io, typename Assignment>
class EepromState : stm32f072::I2cTraits<Assignment::storage.peripheral> {
    using Bus = ProbeState<Io, Assignment>;
    using Registers = stm32f072::I2cTraits<Assignment::storage.peripheral>;
    using Registers::cr2;
    using Registers::icr;
    using Registers::isr;
    using Registers::rxdr;
    using Registers::txdr;

  public:
    inline static constexpr std::size_t capacity = 16'384;
    inline static constexpr std::size_t page_size = 64;

    constexpr void select(const std::uint8_t address) noexcept { device_ = address; }

    [[nodiscard]] constexpr bool read(const std::uint16_t address,
                                      const std::span<std::uint8_t> bytes) noexcept {
        if (!begin(address, bytes.size(), false)) {
            return false;
        }
        input_ = bytes;
        output_ = {};
        return true;
    }

    [[nodiscard]] constexpr bool write(const std::uint16_t address,
                                       const std::span<const std::uint8_t> bytes) noexcept {
        if (!begin(address, bytes.size(), true)) {
            return false;
        }
        output_ = bytes;
        input_ = {};
        return true;
    }
    constexpr void service(const TimestampUs now) noexcept {
        if (!busy()) {
            return;
        }
        if (stage_ == Stage::start) {
            started_ = now;
            stage_ = Stage::idle;
        }
        const auto status = Io::read(isr);
        if ((status & (I2C_ISR_BERR | I2C_ISR_ARLO | I2C_ISR_OVR)) != 0) {
            fail((status & I2C_ISR_BERR) != 0   ? I2cError::bus
                 : (status & I2C_ISR_ARLO) != 0 ? I2cError::arbitration
                                                : I2cError::overrun);
            return;
        }
        const bool polling = stage_ == Stage::poll_stop;
        if (((status & I2C_ISR_NACKF) != 0) && !polling) {
            fail(I2cError::nack);
            return;
        }
        if ((stage_ == Stage::write_wait) || polling) {
            if ((now - write_finished_) >= Bus::timeout_us) {
                fail(I2cError::timeout);
                return;
            }
        } else if ((now - started_) >= Bus::timeout_us) {
            fail(stage_ == Stage::idle ? I2cError::busy : I2cError::timeout);
            return;
        }
        switch (stage_) {
        case Stage::idle:
            if ((status & I2C_ISR_BUSY) == 0) {
                Io::write(icr, I2C_ICR_STOPCF | I2C_ICR_NACKCF);
                const auto count = writing_ ? output_.size() + 2U : 2U;
                Io::write(cr2, (std::uint32_t{device_} << 1U) |
                                   (static_cast<std::uint32_t>(count) << I2C_CR2_NBYTES_Pos) |
                                   I2C_CR2_START | (writing_ ? I2C_CR2_AUTOEND : 0U));
                stage_ = Stage::address_high;
                started_ = now;
            }
            break;
        case Stage::address_high:
        case Stage::address_low:
            if ((status & I2C_ISR_TXIS) != 0) {
                const bool high = stage_ == Stage::address_high;
                Io::write(txdr, high ? address_ >> 8U : address_ & 0xFFU);
                stage_ = high ? Stage::address_low : writing_ ? Stage::transmit : Stage::restart;
                started_ = now;
            }
            break;
        case Stage::restart:
            if ((status & I2C_ISR_TC) != 0) {
                Io::write(cr2,
                          (std::uint32_t{device_} << 1U) |
                              (static_cast<std::uint32_t>(input_.size()) << I2C_CR2_NBYTES_Pos) |
                              I2C_CR2_RD_WRN | I2C_CR2_AUTOEND | I2C_CR2_START);
                stage_ = Stage::receive;
                started_ = now;
            }
            break;
        case Stage::transmit:
            if ((status & I2C_ISR_TXIS) != 0) {
                Io::write(txdr, output_[position_++]);
                started_ = now;
                if (position_ == output_.size()) {
                    stage_ = Stage::stop;
                }
            }
            break;
        case Stage::receive:
            if ((status & I2C_ISR_RXNE) != 0) {
                input_[position_++] = static_cast<std::uint8_t>(Io::read(rxdr));
                started_ = now;
                if (position_ == input_.size()) {
                    stage_ = Stage::stop;
                }
            }
            break;
        case Stage::stop:
            if ((status & I2C_ISR_STOPF) != 0) {
                Io::write(icr, I2C_ICR_STOPCF);
                if (writing_) {
                    write_finished_ = now;
                    started_ = now;
                    stage_ = Stage::write_wait;
                } else {
                    stage_ = Stage::complete;
                }
            }
            break;
        case Stage::write_wait:
            if (((now - started_) >= 500_us) && ((status & I2C_ISR_BUSY) == 0)) {
                Io::write(icr, I2C_ICR_STOPCF | I2C_ICR_NACKCF);
                Io::write(cr2, (std::uint32_t{device_} << 1U) | I2C_CR2_AUTOEND | I2C_CR2_START);
                nack_ = false;
                stage_ = Stage::poll_stop;
            }
            break;
        case Stage::poll_stop:
            nack_ = nack_ || ((status & I2C_ISR_NACKF) != 0);
            if ((status & I2C_ISR_STOPF) != 0) {
                Io::write(icr, I2C_ICR_STOPCF | I2C_ICR_NACKCF);
                stage_ = nack_ ? Stage::write_wait : Stage::complete;
                started_ = now;
            }
            break;
        default:
            break;
        }
    }

    [[nodiscard]] constexpr bool busy() const noexcept {
        return (stage_ != Stage::complete) && (stage_ != Stage::failed);
    }
    [[nodiscard]] constexpr I2cError error() const noexcept { return error_; }
#ifdef CORETX_I6S_TOUCH_TRACE
    struct Trace {
        std::uint16_t address;
        std::size_t position;
        std::uint8_t stage;
        std::uint32_t status;
    };
    [[nodiscard]] constexpr Trace trace() const noexcept {
        return {address_, position_, static_cast<std::uint8_t>(trace_stage_), trace_status_};
    }
#endif

  private:
    enum class Stage : std::uint8_t {
        start,
        idle,
        address_high,
        address_low,
        restart,
        transmit,
        receive,
        stop,
        write_wait,
        poll_stop,
        complete,
        failed
    };

    [[nodiscard]] constexpr bool begin(const std::uint16_t address, const std::size_t size,
                                       const bool writing) noexcept {
        if (busy() || (device_ < Bus::first_address) || (device_ > Bus::last_address) ||
            (size == 0) || (size > page_size) || ((std::size_t{address} + size) > capacity) ||
            (writing && (((address % page_size) + size) > page_size))) {
            error_ = I2cError::invalid_request;
            stage_ = Stage::failed;
            return false;
        }
        address_ = address;
        position_ = 0;
        writing_ = writing;
        error_ = I2cError::none;
        stage_ = Stage::start;
        return true;
    }
    constexpr void fail(const I2cError error) noexcept {
#ifdef CORETX_I6S_TOUCH_TRACE
        trace_stage_ = stage_;
        trace_status_ = Io::read(isr);
#endif
        Bus::reset();
        Io::write(Registers::cr1, 0);
        error_ = error;
        stage_ = Stage::failed;
    }

    std::span<std::uint8_t> input_{};
    std::span<const std::uint8_t> output_{};
    std::size_t position_{};
    TimestampUs started_{};
    TimestampUs write_finished_{};
    std::uint16_t address_{};
    std::uint8_t device_{};
    Stage stage_{Stage::complete};
    I2cError error_{};
    bool writing_{};
    bool nack_{};
#ifdef CORETX_I6S_TOUCH_TRACE
    Stage trace_stage_{};
    std::uint32_t trace_status_{};
#endif
};

} // namespace detail

template <typename Io, typename Assignment> class Eeprom {
    using State = detail::EepromState<Io, Assignment>;

  public:
    Eeprom() = delete;
    inline static constexpr auto capacity = State::capacity;
    inline static constexpr auto page_size = State::page_size;

    static void select(const std::uint8_t address) noexcept { state_.select(address); }
    [[nodiscard]] static bool read(const std::uint16_t address,
                                   const std::span<std::uint8_t> bytes) noexcept {
        return state_.read(address, bytes);
    }
    [[nodiscard]] static bool write(const std::uint16_t address,
                                    const std::span<const std::uint8_t> bytes) noexcept {
        return state_.write(address, bytes);
    }
    static void service(const TimestampUs now) noexcept { state_.service(now); }
    [[nodiscard]] static bool busy() noexcept { return state_.busy(); }
    [[nodiscard]] static I2cError error() noexcept { return state_.error(); }
#ifdef CORETX_I6S_TOUCH_TRACE
    [[nodiscard]] static auto trace() noexcept { return state_.trace(); }
#endif

  private:
    inline static constinit State state_{};
};

} // namespace coretx::firmware
