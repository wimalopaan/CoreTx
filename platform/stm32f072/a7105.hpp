// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "platform/stm32f072/radio_gpio.hpp"

#include <algorithm>
#include <array>
#include <span>

namespace coretx::firmware {

enum class A7105State : std::uint8_t { off, starting, checking, ready, failed };
enum class A7105Error : std::uint8_t {
    none,
    not_initialized,
    spi,
    timeout,
    signature,
    verify,
    sequence
};
enum class A7105FrontEnd : std::uint8_t { off, receive, transmit };

struct A7105Info {
    A7105State state{A7105State::off};
    A7105Error error{A7105Error::not_initialized};
    std::uint8_t signature{};
    std::uint8_t mode{};
    A7105FrontEnd front_end{};
};

namespace a7105 {
inline constexpr std::uint8_t mode = 0x00;
inline constexpr std::uint8_t mode_control = 0x01;
inline constexpr std::uint8_t calibration = 0x02;
inline constexpr std::uint8_t fifo_i = 0x03;
inline constexpr std::uint8_t fifo_ii = 0x04;
inline constexpr std::uint8_t fifo_data = 0x05;
inline constexpr std::uint8_t id_data = 0x06;
inline constexpr std::uint8_t coretx_osc_i = 0x07;
inline constexpr std::uint8_t coretx_osc_ii = 0x08;
inline constexpr std::uint8_t coretx_osc_iii = 0x09;
inline constexpr std::uint8_t cko_pin = 0x0A;
inline constexpr std::uint8_t gpio1 = 0x0B;
inline constexpr std::uint8_t gpio2 = 0x0C;
inline constexpr std::uint8_t clock = 0x0D;
inline constexpr std::uint8_t data_rate = 0x0E;
inline constexpr std::uint8_t pll_i = 0x0F;
inline constexpr std::uint8_t pll_ii = 0x10;
inline constexpr std::uint8_t pll_iii = 0x11;
inline constexpr std::uint8_t pll_iv = 0x12;
inline constexpr std::uint8_t pll_v = 0x13;
inline constexpr std::uint8_t tx_i = 0x14;
inline constexpr std::uint8_t tx_ii = 0x15;
inline constexpr std::uint8_t delay_i = 0x16;
inline constexpr std::uint8_t delay_ii = 0x17;
inline constexpr std::uint8_t rx = 0x18;
inline constexpr std::uint8_t rx_gain_i = 0x19;
inline constexpr std::uint8_t rx_gain_iv = 0x1C;
inline constexpr std::uint8_t rssi_threshold = 0x1D;
inline constexpr std::uint8_t adc = 0x1E;
inline constexpr std::uint8_t code_i = 0x1F;
inline constexpr std::uint8_t code_ii = 0x20;
inline constexpr std::uint8_t if_calibration_i = 0x22;
inline constexpr std::uint8_t vco_current_calibration = 0x24;
inline constexpr std::uint8_t vco_sbcal_i = 0x25;
inline constexpr std::uint8_t vco_sbcal_ii = 0x26;
inline constexpr std::uint8_t battery_detect = 0x27;
inline constexpr std::uint8_t tx_test = 0x28;
inline constexpr std::uint8_t rx_demod_test_i = 0x29;
inline constexpr std::uint8_t rx_demod_test_ii = 0x2A;
inline constexpr std::uint8_t cpc = 0x2B;
inline constexpr std::uint8_t xtal_test = 0x2C;
inline constexpr std::uint8_t pll_test = 0x2D;
inline constexpr std::uint8_t vco_test_i = 0x2E;
inline constexpr std::uint8_t vco_test_ii = 0x2F;
inline constexpr std::uint8_t if_test = 0x30;
inline constexpr std::uint8_t rs_scale = 0x31;
inline constexpr std::uint8_t read = 0x40;
inline constexpr std::uint8_t standby = 0xA0;
inline constexpr std::uint8_t receive = 0xC0;
inline constexpr std::uint8_t transmit = 0xD0;
inline constexpr std::uint8_t reset_write_pointer = 0xE0;
inline constexpr std::uint8_t reset_read_pointer = 0xF0;
inline constexpr std::uint8_t mode_trer = 0x01;
inline constexpr std::uint8_t mode_crc_fail = 0x20;
inline constexpr std::uint8_t mode_fec_fail = 0x40;
inline constexpr std::uint8_t calibration_if = 0x01;
inline constexpr std::uint8_t calibration_vco = 0x02;
inline constexpr std::uint8_t if_calibration_fail = 0x10;
inline constexpr std::uint8_t reset = 0x00;
inline constexpr std::uint8_t gpio1_sdo = 0x19;
inline constexpr std::uint8_t gpio2_wtr = 0x01;
inline constexpr std::uint8_t signature = 0x9E;
inline constexpr std::uint8_t probe_mask = 0x5A;
inline constexpr auto settling_time = 1_ms;
// FIFO transfers span main-loop iterations; bound stalled progress, not total packet time.
inline constexpr auto progress_timeout = 1_ms;
inline constexpr std::uint8_t spi_fifo_bytes = 4;
} // namespace a7105

using a7105_registers = stm32f072::SpiTraits<1>;

namespace detail {

template <typename Io, A7105Pins Pins> class A7105StateMachine {
    using Spi = stm32f072::SpiTraits<Pins.peripheral>;
    static_assert((Spi::alternate(Pins.clock, 0) != stm32f072::invalid_af) &&
                      (Spi::alternate(Pins.input, 1) != stm32f072::invalid_af) &&
                      (Spi::alternate(Pins.output, 2) != stm32f072::invalid_af),
                  "Invalid radio SPI pins");

  public:
    constexpr void initialize(const TimestampUs now) noexcept {
        *this = {};
        internal_front_end_off<Io, Pins>();
        namespace reg = stm32f072::registers;
        Io::write(reg::rcc_ahbenr,
                  Io::read(reg::rcc_ahbenr) |
                      (gpio_clocks(std::array{Pins.select, Pins.clock, Pins.input, Pins.output,
                                              Pins.antenna_a, Pins.antenna_b, Pins.status}) &
                       ~(gpio_clock(Pins.rx_enable) | gpio_clock(Pins.tx_enable))));
        (void)Io::read(reg::rcc_ahbenr);
        Io::write(Spi::enable_register, Io::read(Spi::enable_register) | Spi::enable_mask);
        (void)Io::read(reg::rcc_apb2enr);
        constexpr auto reset = Spi::reset_register;
        const auto previous = Io::read(reset);
        Io::write(reset, previous | Spi::reset_mask);
        Io::write(reset, previous & ~std::uint32_t{Spi::reset_mask});
        drive<Io>(Pins.select, true);
        drive<Io>(Pins.antenna_a, true);
        drive<Io>(Pins.antenna_b, false);
        for (const auto pin : {Pins.select, Pins.antenna_a, Pins.antenna_b})
            configure<Io>(pin, false);
        if constexpr ((Spi::alternate(Pins.clock, 0) == Spi::alternate(Pins.input, 1)) &&
                      (Spi::alternate(Pins.clock, 0) == Spi::alternate(Pins.output, 2))) {
            for (const auto pin : {Pins.clock, Pins.input, Pins.output})
                configure_alternate<Io>(pin, Spi::alternate(Pins.clock, 0), false);
        } else {
            configure_alternate<Io>(Pins.clock, Spi::alternate(Pins.clock, 0), false);
            configure_alternate<Io>(Pins.input, Spi::alternate(Pins.input, 1), false);
            configure_alternate<Io>(Pins.output, Spi::alternate(Pins.output, 2), false);
        }
        configure_input<Io>(Pins.status);
        // SPI mode 0, byte transfers, software NSS, PCLK / 8 = 6 MHz.
        Io::write(Spi::control2, (7U << SPI_CR2_DS_Pos) | SPI_CR2_FRXTH);
        Io::write(Spi::control1,
                  SPI_CR1_MSTR | SPI_CR1_BR_1 | SPI_CR1_SSM | SPI_CR1_SSI | SPI_CR1_SPE);
        info_ = {A7105State::starting, A7105Error::none};
        operation_ = Operation::power_settle;
        started_at_ = now;
    }
    constexpr void power_off() noexcept {
        internal_front_end_off<Io, Pins>();
        if (info_.state != A7105State::off) {
            Io::write(Spi::control1, 0);
        }
        drive<Io>(Pins.select, true);
        info_.state = A7105State::off;
        info_.front_end = A7105FrontEnd::off;
        transfer_ = Transfer::idle;
    }
    constexpr void service(const TimestampUs now) noexcept {
        if ((info_.state == A7105State::off) || (info_.state == A7105State::failed))
            return;
        if (info_.state == A7105State::ready) {
            if ((transfer_ != Transfer::idle) && pump(now)) {
                complete_transfer();
            }
            return;
        }
        if ((operation_ == Operation::power_settle) || (operation_ == Operation::reset_wait)) {
            if ((now - started_at_) < a7105::settling_time)
                return;
            if (operation_ == Operation::power_settle)
                begin(Operation::reset, a7105::mode, a7105::reset, now);
            else
                begin(Operation::gpio1, a7105::gpio1, a7105::gpio1_sdo, now);
            return;
        }
        if (!pump(now))
            return;
        switch (operation_) {
        case Operation::reset:
            operation_ = Operation::reset_wait;
            started_at_ = now;
            break;
        case Operation::gpio1:
            begin(Operation::gpio2, a7105::gpio2, a7105::gpio2_wtr, now);
            break;
        case Operation::gpio2:
            info_.state = A7105State::checking;
            begin(Operation::signature, a7105::pll_ii | a7105::read, 0, now);
            break;
        case Operation::signature:
            info_.signature = received_;
            if (received_ != a7105::signature) {
                fail(A7105Error::signature);
                break;
            }
            begin(Operation::mode, a7105::mode | a7105::read, 0, now);
            break;
        case Operation::mode:
            info_.mode = received_;
            begin(Operation::saved_channel, a7105::pll_i | a7105::read, 0, now);
            break;
        case Operation::saved_channel:
            saved_channel_ = received_;
            begin(Operation::write_probe, a7105::pll_i,
                  static_cast<std::uint8_t>(saved_channel_ ^ a7105::probe_mask), now);
            break;
        case Operation::write_probe:
            begin(Operation::read_probe, a7105::pll_i | a7105::read, 0, now);
            break;
        case Operation::read_probe:
            if (received_ != (saved_channel_ ^ a7105::probe_mask)) {
                fail(A7105Error::verify);
                break;
            }
            begin(Operation::restore, a7105::pll_i, saved_channel_, now);
            break;
        case Operation::restore:
            begin(Operation::verify_restore, a7105::pll_i | a7105::read, 0, now);
            break;
        case Operation::verify_restore:
            if (received_ != saved_channel_) {
                fail(A7105Error::verify);
                break;
            }
            begin(Operation::standby, a7105::standby, 0, now);
            break;
        case Operation::standby:
            info_.state = A7105State::ready;
            break;
        case Operation::power_settle:
        case Operation::reset_wait:
        case Operation::user:
            break;
        }
    }

    [[nodiscard]] constexpr A7105Info info() const noexcept { return info_; }
    [[nodiscard]] constexpr bool busy() const noexcept { return transfer_ != Transfer::idle; }
    [[nodiscard]] constexpr bool completed() const noexcept { return completed_; }
    [[nodiscard]] constexpr std::uint8_t value() const noexcept { return received_; }
    [[nodiscard]] constexpr std::span<const std::uint8_t> payload() const noexcept {
        return {incoming_.data(), static_cast<std::size_t>(length_ - 1)};
    }
    [[nodiscard]] constexpr bool command(const std::uint8_t command,
                                         const std::span<const std::uint8_t> bytes,
                                         const std::uint8_t read_count,
                                         const TimestampUs now) noexcept {
        if ((info_.state != A7105State::ready) || busy() ||
            ((bytes.size() + 1) > request_.size()) || (read_count > incoming_.size()) ||
            (!bytes.empty() && (read_count != 0))) {
            fail(A7105Error::sequence);
            return false;
        }
        begin(Operation::user, command, 0, now);
        std::fill(request_.begin() + 1, request_.end(), 0);
        std::copy(bytes.begin(), bytes.end(), request_.begin() + 1);
        length_ = static_cast<std::uint8_t>(1 + (read_count != 0 ? read_count : bytes.size()));
        completed_ = false;
        if (pump(now))
            complete_transfer();
        return info_.state == A7105State::ready;
    }
    constexpr void front_end(const A7105FrontEnd mode) noexcept {
        if (info_.state == A7105State::ready) {
            drive<Io>(Pins.rx_enable, true);
            drive<Io>(Pins.tx_enable, true);
        } else
            internal_front_end_off<Io, Pins>();
        info_.front_end = A7105FrontEnd::off;
        if ((info_.state != A7105State::ready) && (mode != A7105FrontEnd::off)) {
            fail(A7105Error::sequence);
            return;
        }
        if (mode == A7105FrontEnd::receive)
            drive<Io>(Pins.rx_enable, false);
        if (mode == A7105FrontEnd::transmit)
            drive<Io>(Pins.tx_enable, false);
        info_.front_end = mode;
    }

  private:
    enum class Operation : std::uint8_t {
        power_settle,
        reset,
        reset_wait,
        gpio1,
        gpio2,
        signature,
        mode,
        saved_channel,
        write_probe,
        read_probe,
        restore,
        verify_restore,
        standby,
        user
    };
    enum class Transfer : std::uint8_t { idle, exchange, finish };

    constexpr void complete_transfer() noexcept {
        completed_ = true;
        if ((request_[0] == (a7105::read | a7105::mode)) && (length_ == 2))
            info_.mode = received_;
    }
    constexpr void begin(const Operation operation, const std::uint8_t command,
                         const std::uint8_t value, const TimestampUs now) noexcept {
        operation_ = operation;
        request_[0] = command;
        request_[1] = value;
        length_ = operation == Operation::standby ? 1 : 2;
        position_ = 0;
        written_ = 0;
        transfer_ = Transfer::exchange;
        started_at_ = now;
        drive<Io>(Pins.select, false);
    }
    [[nodiscard]] constexpr bool pump(const TimestampUs now) noexcept {
        const unsigned progress_limit = 2U * length_ + 2U;
        for (unsigned budget = 0; budget < progress_limit; ++budget) {
            const auto previous = transfer_;
            const auto position = position_;
            const auto written = written_;
            if (transfer(now))
                return true;
            if ((info_.state == A7105State::failed) ||
                ((previous == transfer_) && (position == position_) && (written == written_)))
                break;
        }
        return false;
    }
    [[nodiscard]] constexpr bool transfer(const TimestampUs now) noexcept {
        constexpr auto status = Spi::status;
        constexpr auto data = Spi::data;
        const auto flags = Io::read(status);
        if ((flags & (SPI_SR_OVR | SPI_SR_MODF | SPI_SR_FRE)) != 0) {
            fail(A7105Error::spi);
            return false;
        }
        switch (transfer_) {
        case Transfer::exchange:
            if ((position_ != written_) && ((flags & SPI_SR_RXNE) != 0)) {
                received_ = Io::read8(data);
                started_at_ = now;
                if (position_ != 0)
                    incoming_[position_ - 1] = received_;
                ++position_;
                if (position_ == length_)
                    transfer_ = Transfer::finish;
                return false;
            }
            // Bound outstanding bytes by RX capacity, including the byte in the shifter.
            if ((written_ < length_) && ((written_ - position_) < a7105::spi_fifo_bytes) &&
                ((flags & SPI_SR_TXE) != 0)) {
                Io::write8(data, request_[written_++]);
                started_at_ = now;
                return false;
            }
            break;
        case Transfer::finish:
            if ((flags & SPI_SR_BSY) == 0) {
                drive<Io>(Pins.select, true);
                transfer_ = Transfer::idle;
                return true;
            }
            break;
        case Transfer::idle:
            return false;
        }
        if ((now - started_at_) >= a7105::progress_timeout) {
            fail(A7105Error::timeout);
        }
        return false;
    }
    constexpr void fail(const A7105Error error) noexcept {
        power_off();
        info_.state = A7105State::failed;
        info_.error = error;
    }

    A7105Info info_{};
    TimestampUs started_at_{};
    std::array<std::uint8_t, 39> request_{};
    std::array<std::uint8_t, 38> incoming_{};
    std::uint8_t length_{1};
    std::uint8_t position_{};
    std::uint8_t written_{};
    std::uint8_t received_{};
    std::uint8_t saved_channel_{};
    Operation operation_{};
    Transfer transfer_{};
    bool completed_{};
};

} // namespace detail

template <typename Io, A7105Pins Pins> class A7105 {
  public:
    A7105() = delete;
    static void initialize(const TimestampUs now) noexcept { state_.initialize(now); }
    static void service(const TimestampUs now) noexcept { state_.service(now); }
    static void power_off() noexcept { state_.power_off(); }
    [[nodiscard]] static A7105Info info() noexcept { return state_.info(); }
    [[nodiscard]] static bool busy() noexcept { return state_.busy(); }
    [[nodiscard]] static bool completed() noexcept { return state_.completed(); }
    [[nodiscard]] static std::uint8_t value() noexcept { return state_.value(); }
    [[nodiscard]] static std::span<const std::uint8_t> payload() noexcept {
        return state_.payload();
    }
    [[nodiscard]] static bool command(const std::uint8_t command,
                                      const std::span<const std::uint8_t> bytes,
                                      const std::uint8_t read_count,
                                      const TimestampUs now) noexcept {
        return state_.command(command, bytes, read_count, now);
    }
    static void front_end(const A7105FrontEnd mode) noexcept { state_.front_end(mode); }

  private:
    inline static constinit detail::A7105StateMachine<Io, Pins> state_{};
};

} // namespace coretx::firmware
