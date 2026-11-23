// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/common/radio_board.hpp"
#include "coretx/features.hpp"
#include "coretx/radio/crsf.hpp"
#include "coretx/radio/crsf_config.hpp"
#include "coretx/radio/multiswitch.hpp"

#include <optional>

namespace coretx::firmware {

struct CrsfBaud {
    std::uint32_t rate{};
    DurationUs period_us{};
};

inline constexpr std::array crsf_baudrates{
    CrsfBaud{115'200, 16'000_us},  CrsfBaud{400'000, 4'000_us},  CrsfBaud{416'666, 4'000_us},
    CrsfBaud{420'000, 4'000_us},   CrsfBaud{921'600, 2'000_us},  CrsfBaud{1'870'000, 2'000_us},
    CrsfBaud{3'750'000, 2'000_us}, CrsfBaud{5'250'000, 2'000_us}};
inline constexpr std::size_t crsf_default_baudrate_index = 1; // 400 kbaud RC-to-TX default.

namespace crsf_uart {
inline constexpr std::uint32_t normal_oversampling = 16;
inline constexpr std::uint32_t over8_fraction_mask = USART_BRR_DIV_FRACTION >> 1;
inline constexpr std::uint32_t bits_per_byte = 10; // 8N1: start + 8 data + stop.
inline constexpr auto receive_error_window = 1'000'000_us;
inline constexpr std::uint32_t microseconds_per_second = receive_error_window.count();
} // namespace crsf_uart

enum class CrsfError : std::uint8_t { none, not_initialized, ready, dma, timeout, sequence };

template <typename Assignment> struct CrsfRegisters {
    using Uart = stm32f072::UartTraits<Assignment::crsf.peripheral>;
    using Rx = stm32f072::DmaTraits<Assignment::crsf_rx_dma>;
    using Tx = stm32f072::DmaTraits<Assignment::crsf_tx_dma>;
    static_assert((Assignment::crsf_rx_dma == Uart::rx_dma) &&
                      (Assignment::crsf_tx_dma == Uart::tx_dma),
                  "Invalid UART DMA channels");
    static constexpr auto uart_base = Uart::base;
    static constexpr auto cr1 = uart_base + offsetof(USART_TypeDef, CR1);
    static constexpr auto cr2 = uart_base + offsetof(USART_TypeDef, CR2);
    static constexpr auto cr3 = uart_base + offsetof(USART_TypeDef, CR3);
    static constexpr auto brr = uart_base + offsetof(USART_TypeDef, BRR);
    static constexpr auto isr = uart_base + offsetof(USART_TypeDef, ISR);
    static constexpr auto icr = uart_base + offsetof(USART_TypeDef, ICR);
    static constexpr auto rdr = uart_base + offsetof(USART_TypeDef, RDR);
    static constexpr auto tdr = uart_base + offsetof(USART_TypeDef, TDR);
    static constexpr auto dma_isr = Rx::isr;
    static constexpr auto dma_ifcr = Rx::ifcr;
    static constexpr auto rx_dma_base = Rx::base;
    static constexpr auto tx_dma_base = Tx::base;
    static constexpr auto rx_ccr = Rx::ccr;
    static constexpr auto rx_cndtr = Rx::cndtr;
    static constexpr auto rx_cpar = Rx::cpar;
    static constexpr auto rx_cmar = Rx::cmar;
    static constexpr auto tx_ccr = Tx::ccr;
    static constexpr auto tx_cndtr = Tx::cndtr;
    static constexpr auto tx_cpar = Tx::cpar;
    static constexpr auto tx_cmar = Tx::cmar;
    static constexpr auto rx_error = Rx::error;
    static constexpr auto tx_error = Tx::error;
    static constexpr auto rx_half = Rx::half;
    static constexpr auto rx_complete = Rx::complete;
    static constexpr auto tx_complete = Tx::complete;
    static constexpr auto clear_rx = Rx::clear;
    static constexpr auto clear_tx = Tx::clear;
    static constexpr auto clear_rx_boundaries = Rx::half | Rx::complete;
};
using crsf_registers = CrsfRegisters<firmware::Hardware>;

namespace detail {

template <typename Io, typename Assignment = firmware::Hardware>
class CrsfState : CrsfRegisters<Assignment> {
    using Registers = CrsfRegisters<Assignment>;
    using Uart = typename Registers::Uart;
    using Registers::brr;
    using Registers::clear_rx;
    using Registers::clear_rx_boundaries;
    using Registers::clear_tx;
    using Registers::cr1;
    using Registers::cr2;
    using Registers::cr3;
    using Registers::dma_ifcr;
    using Registers::dma_isr;
    using Registers::icr;
    using Registers::isr;
    using Registers::rdr;
    using Registers::rx_ccr;
    using Registers::rx_cmar;
    using Registers::rx_cndtr;
    using Registers::rx_complete;
    using Registers::rx_cpar;
    using Registers::rx_error;
    using Registers::rx_half;
    using Registers::tdr;
    using Registers::tx_ccr;
    using Registers::tx_cmar;
    using Registers::tx_cndtr;
    using Registers::tx_complete;
    using Registers::tx_cpar;
    using Registers::tx_error;
    static constexpr auto tx = Assignment::crsf.tx;
    static constexpr auto rx = Assignment::crsf.rx;
    static_assert((Uart::alternate(tx, true) != stm32f072::invalid_af) &&
                      (Uart::alternate(rx, false) != stm32f072::invalid_af),
                  "Invalid UART pins");

  public:
    inline static constexpr DurationUs timeout_us{10'000};
    inline static constexpr DurationUs freshness_us{10'000};
    inline static constexpr std::size_t receive_buffer_size = 256;
    inline static constexpr std::size_t receive_budget = coretx::crsf::CRSF_MAX_PACKET_LEN;
    constexpr void power_off() noexcept {
        external_module_off<Io, Assignment>();
        if (initialized_) {
            stop();
            // Disconnect digital drivers and pull-ups to avoid back-powering the module.
            configure<Io>(tx, true);
            configure<Io>(rx, true);
        }
        powered_ = false;
        initialized_ = false;
        error_ = CrsfError::not_initialized;
    }
    constexpr void initialize(const TimestampUs now) noexcept {
        power_off();
        // Do not create a full transport/configuration temporary on the 2 KiB stack.
        config_.close();
        telemetry_ = {};
        frame_.fill(0);
        active_ = crsf_default_baudrate_index;
        requested_ = crsf_default_baudrate_index;
        frames_ = 0;
        receive_errors_ = 0;
        error_window_count_ = 0;
        receive_errors_per_second_ = 0;
        receive_overruns_ = 0;
        configuration_frame_ = false;
        last_aux_multiswitch_ = false;
        transmit_size_ = 0;
        error_window_started_ = now;
        namespace reg = stm32f072::registers;
        Io::write(reg::rcc_ahbenr,
                  Io::read(reg::rcc_ahbenr) | gpio_clock(tx) | gpio_clock(rx) | RCC_AHBENR_DMA1EN);
        (void)Io::read(reg::rcc_ahbenr);
        Io::write(Uart::enable_register, Io::read(Uart::enable_register) | Uart::enable_mask);
        (void)Io::read(Uart::enable_register);
        Io::write(reg::rcc_apb2enr, Io::read(reg::rcc_apb2enr) | RCC_APB2ENR_SYSCFGEN);
        (void)Io::read(reg::rcc_apb2enr);
        constexpr auto mapping = SYSCFG_BASE + offsetof(SYSCFG_TypeDef, CFGR1);
        const auto dma_mapping = Io::read(mapping);
        Io::write(mapping, (dma_mapping & ~Uart::dma_clear) | Uart::dma_set);
        constexpr auto clock = RCC_BASE + offsetof(RCC_TypeDef, CFGR3);
        Io::write(clock, Io::read(clock) & ~Uart::clock_mask);
        configure_alternate<Io>(tx, Uart::alternate(tx, true), true);
        configure_alternate<Io>(rx, Uart::alternate(rx, false), true);
        initialized_ = true;
        configure_uart(now);
    }
    [[nodiscard]] constexpr bool set_baud(const std::size_t index, const TimestampUs now) noexcept {
        if (!initialized_ || (index >= crsf_baudrates.size()))
            return false;
        if (requested_ == index)
            return true;
        requested_ = index;
        if (!busy_)
            configure_uart(now);
        return true;
    }
    constexpr void next_baud(const TimestampUs now) noexcept {
        if (!initialized_) {
            return;
        }
        requested_ = (requested_ + 1U) % crsf_baudrates.size();
        if (!busy_) {
            configure_uart(now);
        }
    }
    constexpr CrsfError service(const TimestampUs now, const coretx::crsf::Channels &channels,
                                const bool valid, const TimestampUs sampled_at,
                                coretx::multiswitch::Pages *const multiswitch = nullptr) noexcept {
        constexpr auto interval = crsf_uart::receive_error_window;
        const auto elapsed = now - error_window_started_;
        if (elapsed >= interval) {
            // After a service gap, publish only the latest completed window.
            receive_errors_per_second_ = elapsed < (2U * interval) ? error_window_count_ : 0U;
            error_window_count_ = 0;
            error_window_started_ = now - elapsed % interval;
        }
        telemetry_.service(now, config_);
        config_.service(now);
        channels_valid_ = valid && ((now - sampled_at) < freshness_us);
        if (error_ != CrsfError::none) {
            return error_;
        }
        const auto flags = Io::read(isr);
        const auto dma_flags = Io::read(dma_isr);
        if ((dma_flags & (rx_error | tx_error)) != 0) {
            return fail(CrsfError::dma);
        }
        const auto rx_error = acknowledge_receive_errors(flags);
        if (!ready_) {
            constexpr auto acknowledged = USART_ISR_TEACK | USART_ISR_REACK;
            if ((flags & acknowledged) != acknowledged) {
                return (now - started_) >= timeout_us ? fail(CrsfError::ready) : error_;
            }
            ready_ = true;
            if (!powered_) {
                drive<Io>(Assignment::external_module_power,
                          Assignment::external_module_active_high);
                powered_ = true;
            }
        }
        if ((transmit(now, channels, sampled_at, multiswitch) != CrsfError::none) || !ready_) {
            return error_;
        }
        return receive(now, rx_error);
    }

    [[nodiscard]] constexpr CrsfError error() const noexcept { return error_; }
    [[nodiscard]] constexpr bool busy() const noexcept { return busy_; }
    [[nodiscard]] constexpr bool powered() const noexcept { return powered_; }
    [[nodiscard]] constexpr std::uint32_t frames() const noexcept { return frames_; }
    [[nodiscard]] constexpr std::uint32_t receive_errors() const noexcept {
        return receive_errors_;
    }
    [[nodiscard]] constexpr std::uint32_t receive_errors_per_second() const noexcept {
        return receive_errors_per_second_;
    }
    [[nodiscard]] constexpr const coretx::crsf::Telemetry &telemetry() const noexcept {
        return telemetry_;
    }
    [[nodiscard]] constexpr coretx::crsf::config::Client &config() noexcept { return config_; }
    [[nodiscard]] constexpr std::uint32_t receive_overruns() const noexcept {
        return receive_overruns_;
    }
    [[nodiscard]] constexpr std::uint32_t baudrate() const noexcept {
        return crsf_baudrates[requested_].rate;
    }
    [[nodiscard]] constexpr std::uint32_t actual_baudrate() const noexcept {
        return stm32f072::cpu_hz / divider(crsf_baudrates[active_].rate);
    }

  private:
    constexpr bool acknowledge_receive_errors(const std::uint32_t flags) noexcept {
        const auto errors =
            flags & std::uint32_t{USART_ISR_PE | USART_ISR_FE | USART_ISR_NE | USART_ISR_ORE};
        if (errors == 0) {
            return false;
        }
        ++receive_errors_;
        ++error_window_count_;
        Io::write(Registers::icr, errors);
        return true;
    }
    constexpr CrsfError transmit(const TimestampUs now, const coretx::crsf::Channels &channels,
                                 const TimestampUs sampled_at,
                                 coretx::multiswitch::Pages *const multiswitch) noexcept {
        if (busy_) {
            const auto remaining = Io::read(tx_cndtr);
            if (remaining > transmit_size_) {
                return fail(CrsfError::sequence);
            }
            // DMA completion alone does not mean the last stop bit has left the UART.
            if ((remaining != 0) || ((Io::read(dma_isr) & tx_complete) == 0) ||
                ((Io::read(isr) & USART_ISR_TC) == 0)) {
                return (now - started_) >= timeout_us ? fail(CrsfError::timeout) : error_;
            }
            Io::write(tx_ccr, 0);
            Io::write(dma_ifcr, clear_tx);
            Io::write(icr, USART_ICR_TCCF);
            busy_ = false;
            if (!configuration_frame_)
                ++frames_;
        }
        if (requested_ != active_) {
            configure_uart(now);
            return error_;
        }
        if (!channels_valid_) {
            return error_;
        }
        const auto period = crsf_baudrates[active_].period_us;
        const auto elapsed = now - channels_started_;
        configuration_frame_ = sent_ && (elapsed < period);
        std::optional<std::size_t> multiswitch_slot{};
        if (!configuration_frame_) {
            prepare_channels(now, channels);
        } else {
            const auto preparation_started = Io::now_us();
            if constexpr (features::multiswitch)
                multiswitch_slot = multiswitch == nullptr ? std::nullopt : multiswitch->due(now);
            const auto config_size = config_.next_frame_size();
            if ((config_size != 0) && last_aux_multiswitch_)
                multiswitch_slot.reset();
            const auto size = multiswitch_slot ? coretx::multiswitch::Frame{}.size() : config_size;
            const auto wire_us = static_cast<std::uint32_t>(
                (size * crsf_uart::bits_per_byte * crsf_uart::microseconds_per_second +
                 actual_baudrate() - 1U) /
                actual_baudrate());
            if ((size == 0) || ((elapsed.count() + wire_us + 100U) >= period.count()))
                return error_;
            if (multiswitch_slot) {
                const auto frame = multiswitch->frame(*multiswitch_slot);
                std::copy(frame.begin(), frame.end(), frame_.begin());
                transmit_size_ = static_cast<std::uint8_t>(frame.size());
            } else
                transmit_size_ = static_cast<std::uint8_t>(config_.next_frame(now, frame_));
            if (transmit_size_ == 0)
                return error_;
            const auto preparation = Io::now_us() - preparation_started;
            if (((elapsed.count() + preparation.count() + wire_us + 100U) >= period.count()) ||
                (((now + preparation) - sampled_at) >= freshness_us)) {
                if (!multiswitch_slot)
                    config_.defer_frame();
                multiswitch_slot.reset();
                if (((now + preparation) - sampled_at) >= freshness_us)
                    return error_;
                if ((elapsed + preparation) < period)
                    return error_;
                configuration_frame_ = false;
                prepare_channels(now + preparation, channels);
            }
        }
        Io::write(dma_ifcr, clear_tx);
        Io::write(icr, USART_ICR_TCCF);
        Io::write(tx_cndtr, transmit_size_);
        Io::barrier();
        Io::write(tx_ccr, DMA_CCR_MINC | DMA_CCR_DIR | DMA_CCR_EN);
        started_ = now;
        busy_ = true;
        if (configuration_frame_) {
            last_aux_multiswitch_ = multiswitch_slot.has_value();
            if (multiswitch_slot)
                multiswitch->sent(*multiswitch_slot, now);
        }
        return error_;
    }
    constexpr void prepare_channels(const TimestampUs now,
                                    const coretx::crsf::Channels &channels) noexcept {
        const auto frame = coretx::crsf::encode(channels);
        std::copy(frame.begin(), frame.end(), frame_.begin());
        transmit_size_ = static_cast<std::uint8_t>(frame.size());
        channels_started_ = now;
        sent_ = true;
    }
    constexpr CrsfError discard_receive() noexcept {
        Io::write(dma_ifcr, clear_rx_boundaries);
        const auto remaining = Io::read(rx_cndtr);
        if (remaining > receive_buffer_.size()) {
            return fail(CrsfError::sequence);
        }
        receive_tail_ = (receive_buffer_.size() - std::size_t{remaining}) % receive_buffer_.size();
        receive_head_ = receive_tail_;
        telemetry_.reset_stream();
        config_.reset_stream();
        return error_;
    }
    constexpr CrsfError receive(const TimestampUs now, const bool uart_error) noexcept {
        const auto copy_start = Io::now_us();
        const auto flags = Io::read(dma_isr);
        if ((flags & (rx_error | tx_error)) != 0) {
            return fail(CrsfError::dma);
        }
        constexpr std::uint32_t boundaries = rx_half | rx_complete;
        Io::write(dma_ifcr, clear_rx_boundaries);
        const auto remaining = Io::read(rx_cndtr);
        if (remaining > receive_buffer_.size()) {
            return fail(CrsfError::sequence);
        }
        const auto head =
            (receive_buffer_.size() - std::size_t{remaining}) % receive_buffer_.size();
        const auto available =
            (head + receive_buffer_.size() - receive_tail_) % receive_buffer_.size();
        const auto queued =
            (receive_head_ + receive_buffer_.size() - receive_tail_) % receive_buffer_.size();
        const auto produced =
            (head + receive_buffer_.size() - receive_head_) % receive_buffer_.size();
        const auto gap = now - receive_serviced_at_;
        receive_serviced_at_ = now;
        if (uart_error) {
            return discard_receive();
        }
        if (((flags & boundaries) == boundaries) ||
            (produced >= (receive_buffer_.size() - queued)) ||
            ((available != 0) && (gap >= coretx::crsf::Telemetry::stale_us))) {
            ++receive_overruns_;
            return discard_receive();
        }
        if (available == 0) {
            receive_head_ = head;
            telemetry_.service(now, config_);
            return error_;
        }
        const auto count = std::min(available, receive_budget);
        std::array<std::uint8_t, receive_budget> copy{};
        Io::barrier();
        for (std::size_t i = 0; i < count; ++i) {
            copy[i] =
                Io::read8(receive_buffer_.data() + (receive_tail_ + i) % receive_buffer_.size());
        }
        const auto after = Io::read(rx_cndtr);
        const auto after_flags = Io::read(dma_isr);
        const auto elapsed = Io::now_us() - copy_start;
        if ((after_flags & (rx_error | tx_error)) != 0) {
            return fail(CrsfError::dma);
        }
        if (after > receive_buffer_.size()) {
            return fail(CrsfError::sequence);
        }
        if (acknowledge_receive_errors(Io::read(isr))) {
            return discard_receive();
        }
        const auto next_head =
            (receive_buffer_.size() - std::size_t{after}) % receive_buffer_.size();
        const auto advanced = (next_head + receive_buffer_.size() - head) % receive_buffer_.size();
        const DurationUs wrap_us{
            static_cast<std::uint32_t>(receive_buffer_.size() * crsf_uart::bits_per_byte *
                                       crsf_uart::microseconds_per_second / actual_baudrate())};
        // Check before parsing: DMA must not have reached any copied, unread data.
        if ((advanced >= (receive_buffer_.size() - available)) ||
            ((after_flags & boundaries) == boundaries) || (elapsed >= wrap_us)) {
            ++receive_overruns_;
            return discard_receive();
        }
        receive_tail_ = (receive_tail_ + count) % receive_buffer_.size();
        receive_head_ = next_head;
        for (std::size_t i = 0; i < count; ++i) {
            telemetry_.push(copy[i], now, config_);
        }
        return error_;
    }

    [[nodiscard]] static constexpr std::uint32_t divider(const std::uint32_t baud) noexcept {
        return (stm32f072::cpu_hz + baud / 2U) / baud;
    }
    constexpr void stop() noexcept {
        Io::write(tx_ccr, 0);
        Io::write(rx_ccr, 0);
        Io::write(cr1, 0);
        Io::write(cr3, 0);
        busy_ = false;
        ready_ = false;
        telemetry_.invalidate();
        config_.transport_lost();
    }
    constexpr void configure_uart(const TimestampUs now) noexcept {
        stop();
        Io::write(Uart::reset_register, Io::read(Uart::reset_register) | Uart::reset_mask);
        Io::write(Uart::reset_register, Io::read(Uart::reset_register) & ~Uart::reset_mask);
        active_ = requested_;
        const auto ticks = divider(crsf_baudrates[active_].rate);
        const bool over8 = ticks < crsf_uart::normal_oversampling;
        constexpr auto fraction = crsf_uart::over8_fraction_mask;
        Io::write(brr, over8 ? ((ticks & ~fraction) << 1) | (ticks & fraction) : ticks);
        Io::write(cr2, 0);
        Io::write(cr3, USART_CR3_DMAR | USART_CR3_DMAT);
        Io::write(icr, USART_ICR_TCCF | USART_ICR_PECF | USART_ICR_FECF | USART_ICR_NCF |
                           USART_ICR_ORECF | USART_ICR_IDLECF);
        Io::write(dma_ifcr, clear_rx | clear_tx);
        Io::write(rx_cpar, rdr);
        receive_tail_ = 0;
        receive_head_ = 0;
        receive_serviced_at_ = now;
        Io::write_address(rx_cmar, receive_buffer_.data());
        Io::write(rx_cndtr, static_cast<std::uint32_t>(receive_buffer_.size()));
        Io::write(rx_ccr, DMA_CCR_MINC | DMA_CCR_CIRC | DMA_CCR_EN);
        Io::write(tx_cpar, tdr);
        Io::write_address(tx_cmar, frame_.data());
        Io::write(cr1, USART_CR1_UE | USART_CR1_TE | USART_CR1_RE | (over8 ? USART_CR1_OVER8 : 0U));
        started_ = now;
        sent_ = false;
        channels_valid_ = false;
        error_ = CrsfError::none;
    }
    constexpr CrsfError fail(const CrsfError error) noexcept {
        stop();
        channels_valid_ = false;
        error_ = error;
        return error_;
    }

    coretx::crsf::config::Frame frame_{};
    std::array<std::uint8_t, receive_buffer_size> receive_buffer_{};
    coretx::crsf::Telemetry telemetry_{};
    coretx::crsf::config::Client config_{};
    std::size_t receive_tail_{};
    std::size_t receive_head_{};
    std::size_t active_{crsf_default_baudrate_index};
    std::size_t requested_{crsf_default_baudrate_index};
    TimestampUs started_{};
    TimestampUs channels_started_{};
    std::uint32_t frames_{};
    std::uint32_t receive_errors_{};
    TimestampUs error_window_started_{};
    std::uint32_t error_window_count_{};
    std::uint32_t receive_errors_per_second_{};
    TimestampUs receive_serviced_at_{};
    std::uint32_t receive_overruns_{};
    CrsfError error_{CrsfError::not_initialized};
    bool initialized_{};
    bool powered_{};
    bool ready_{};
    bool busy_{};
    bool sent_{};
    bool channels_valid_{};
    bool configuration_frame_{};
    bool last_aux_multiswitch_{};
    std::uint8_t transmit_size_{};
};

} // namespace detail

template <typename Io, typename Assignment = firmware::Hardware> class Crsf {
  public:
    Crsf() = delete;

    static void power_off() noexcept { state().power_off(); }
    static void initialize(const TimestampUs now) noexcept { state().initialize(now); }
    static void next_baud(const TimestampUs now) noexcept { state().next_baud(now); }
    [[nodiscard]] static bool set_baud(const std::size_t index, const TimestampUs now) noexcept {
        return state().set_baud(index, now);
    }
    [[nodiscard]] static CrsfError
    service(const TimestampUs now, const coretx::crsf::Channels &channels, const bool valid,
            const TimestampUs sampled_at,
            coretx::multiswitch::Pages *const multiswitch = nullptr) noexcept {
        return state().service(now, channels, valid, sampled_at, multiswitch);
    }
    [[nodiscard]] static CrsfError error() noexcept { return state().error(); }
    [[nodiscard]] static bool busy() noexcept { return state().busy(); }
    [[nodiscard]] static bool powered() noexcept { return state().powered(); }
    [[nodiscard]] static std::uint32_t frames() noexcept { return state().frames(); }
    [[nodiscard]] static std::uint32_t receive_errors() noexcept {
        return state().receive_errors();
    }
    [[nodiscard]] static std::uint32_t receive_errors_per_second() noexcept {
        return state().receive_errors_per_second();
    }
    [[nodiscard]] static const coretx::crsf::Telemetry &telemetry() noexcept {
        return state().telemetry();
    }
    [[nodiscard]] static coretx::crsf::config::Client &config() noexcept {
        return state().config();
    }
    [[nodiscard]] static std::uint32_t receive_overruns() noexcept {
        return state().receive_overruns();
    }
    [[nodiscard]] static std::uint32_t baudrate() noexcept { return state().baudrate(); }
    [[nodiscard]] static std::uint32_t actual_baudrate() noexcept {
        return state().actual_baudrate();
    }

  private:
    static detail::CrsfState<Io, Assignment> &state() noexcept {
        if (!state_storage_)
            state_storage_.emplace();
        return *state_storage_;
    }
    // Construct in place so the zero-filled transport/configuration buffers stay in BSS.
    inline static constinit std::optional<detail::CrsfState<Io, Assignment>> state_storage_{};
};

} // namespace coretx::firmware
