// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/common/hardware.hpp"
#include "coretx/ui/translation.hpp"

#include "platform/stm32f072/gpio.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <string_view>

namespace coretx::firmware::i6x {

using firmware::AnalogInput;
inline constexpr auto gimbal_inputs = firmware::Hardware::gimbal_inputs;
inline constexpr auto stock_inputs = firmware::Hardware::stock_inputs;

inline constexpr std::size_t battery_index = stock_inputs.size();
inline constexpr auto sender_inputs = firmware::Hardware::sender_inputs;
inline constexpr std::size_t extra_adc_count = sender_inputs.size() - stock_inputs.size() - 1;

[[nodiscard]] constexpr std::int32_t battery_millivolts(const std::uint16_t raw) noexcept {
    // OpenI6X nominal stock scaling, translated from half-resolution ADC to mV.
    return static_cast<std::int32_t>((std::uint32_t{raw} * 1000U + 210U) / 421U + 200U);
}

enum class AdcError : std::uint8_t {
    none,
    not_initialized,
    calibration,
    ready,
    overrun,
    timeout,
    sequence,
    dma
};
enum class ScanEvent : std::uint8_t { pending, complete, failed };

using adc_registers = stm32f072::AdcTraits<firmware::Hardware::adc>;

struct dma_registers : stm32f072::DmaTraits<firmware::Hardware::adc_dma> {
    inline static constexpr std::uint32_t configuration =
        DMA_CCR_MINC | DMA_CCR_PSIZE_0 | DMA_CCR_MSIZE_0 | DMA_CCR_PL_1 | DMA_CCR_CIRC;
};

#if defined(CORETX_ADC_COHERENT_SNAPSHOTS) && CORETX_ADC_COHERENT_SNAPSHOTS
inline constexpr bool coherent_adc_snapshots = true;
#else
inline constexpr bool coherent_adc_snapshots = false;
#endif

namespace detail {

template <typename Io, const auto &Inputs = gimbal_inputs, bool Coherent = coherent_adc_snapshots,
          typename Assignment = firmware::Hardware>
class AdcState {
    using Registers = stm32f072::AdcTraits<Assignment::adc>;
    using Dma = stm32f072::DmaTraits<Assignment::adc_dma>;
    static_assert(Assignment::adc_dma == Registers::dma, "Invalid ADC DMA channel");
    static constexpr auto configuration = dma_registers::configuration;

  public:
    using Samples = std::array<std::uint16_t, Inputs.size()>;
    static constexpr DurationUs scan_period_us{5'000};
    static constexpr DurationUs scan_timeout_us{10'000};
    // (239.5 sampling + 12.5 conversion) cycles at 12 MHz per channel.
    static constexpr DurationUs half_period_us{Inputs.size() * 21U};
    [[nodiscard]] constexpr AdcError
    initialize(const std::uint32_t attempts = stm32f072::ready_timeout) noexcept {
        using namespace stm32f072::registers;
        Io::write(rcc_ahbenr, Io::read(rcc_ahbenr) | RCC_AHBENR_DMA1EN);
        (void)Io::read(rcc_ahbenr);
        // Stop DMA before clearing or reusing its destination buffer.
        Io::write(Dma::ccr, 0);
        const bool extended = extended_;
        *this = {};
        extended_ = extended;
        scan_count_ = extended_ ? Inputs.size() : std::min<std::size_t>(Inputs.size(), 11);
        Io::write(Dma::ifcr, Dma::clear);
        Io::write(Dma::cpar, Registers::dr);
        Io::write_address(Dma::cmar, pending_.data());
        Io::write(Dma::ccr, configuration);
        error_ = AdcError::none;
        // The reset guarantees ADEN=0, required before ADC calibration.
        Io::write(rcc_ahbenr, Io::read(rcc_ahbenr) | gpio_clocks());
        (void)Io::read(rcc_ahbenr);
        Io::write(rcc_apb2enr, Io::read(rcc_apb2enr) | Registers::enable_mask);
        (void)Io::read(rcc_apb2enr);
        Io::write(Registers::reset, Io::read(Registers::reset) | Registers::reset_mask);
        Io::write(Registers::reset, Io::read(Registers::reset) & ~Registers::reset_mask);
        for (std::size_t i = 0; i < scan_count_; ++i) {
            configure<Io>(Inputs[i].pin, true);
        }
        Io::write(Registers::cfgr2, ADC_CFGR2_CKMODE_1); // 48 MHz PCLK / 4 = 12 MHz ADC clock.
        Io::write(Registers::cr, ADC_CR_ADCAL);
        if (!stm32f072::wait_for<Io>(Registers::cr, ADC_CR_ADCAL, 0, attempts)) {
            return fail(AdcError::calibration);
        }
        Io::write(Registers::chselr, channel_mask(scan_count_));
        Io::write(Registers::smpr, ADC_SMPR_SMP); // 239.5-cycle sampling, suitable for pots.
        Io::write(Registers::cfgr1, ADC_CFGR1_DMAEN | ADC_CFGR1_DMACFG | ADC_CFGR1_CONT);
        Io::write(Registers::isr, ADC_ISR_ADRDY | ADC_ISR_EOSEQ | ADC_ISR_OVR);
        // Four ADC clocks after calibration; defer enabling instead of busy-waiting.
        started_ = Io::now_us();
        enabling_ = true;
        initialized_ = true;
        return error_;
    }
    [[nodiscard]] constexpr ScanEvent service(const TimestampUs now) noexcept {
        constexpr auto cr = Registers::cr;
        constexpr auto isr = Registers::isr;
        if (!initialized_ || (error_ != AdcError::none)) {
            return ScanEvent::failed;
        }
        if (enabling_) {
            if ((now - started_) >= scan_timeout_us) {
                (void)fail(AdcError::ready);
                return ScanEvent::failed;
            }
            if (!enabled_ && ((now - started_) >= 2_us)) {
                Io::write(cr, ADC_CR_ADEN);
                enabled_ = true;
            }
            if (enabled_ && ((Io::read(isr) & ADC_ISR_ADRDY) != 0)) {
                enabling_ = false;
            }
            return ScanEvent::pending;
        }
        if (!busy_) {
            Io::write(isr, ADC_ISR_EOSEQ | ADC_ISR_OVR);
            Io::write(Dma::ifcr, Dma::clear);
            Io::write(Dma::cndtr, static_cast<std::uint32_t>(scan_count_ * (Coherent ? 2 : 1)));
            Io::write(Dma::ccr, configuration | DMA_CCR_EN);
            Io::write(cr, Io::read(cr) | ADC_CR_ADSTART);
            started_ = now;
            busy_ = true;
            return ScanEvent::pending;
        }
        const auto flags = Io::read(isr);
        const auto dma_flags = Io::read(Dma::isr);
        if ((dma_flags & Dma::error) != 0) {
            (void)fail(AdcError::dma);
            return ScanEvent::failed;
        }
        if ((flags & ADC_ISR_OVR) != 0) {
            (void)fail(AdcError::overrun);
            return ScanEvent::failed;
        }
        if (valid_ && ((now - started_) < scan_period_us)) {
            return ScanEvent::pending;
        }
        TimestampUs copy_start{};
        if constexpr (Coherent) {
            copy_start = Io::now_us();
        }
        const auto remaining = Io::read(Dma::cndtr);
        if ((remaining > (scan_count_ * (Coherent ? 2 : 1))) ||
            ((Io::read(cr) & ADC_CR_ADSTART) == 0)) {
            (void)fail(AdcError::sequence);
            return ScanEvent::failed;
        }
        bool second_half{};
        auto ready_flag = Dma::complete;
        if constexpr (Coherent) {
            second_half = remaining > scan_count_;
            // A zero count is the circular reload boundary, not a stable half.
            ready_flag = remaining == 0 ? 0U : second_half ? Dma::complete : Dma::half;
        }
        if ((dma_flags & ready_flag) == 0) {
            // A late service call is not a stalled ADC if DMA has completed a scan.
            if ((now - started_) >= scan_timeout_us) {
                (void)fail(AdcError::timeout);
                return ScanEvent::failed;
            }
            return ScanEvent::pending;
        }
        Io::write(Dma::ifcr, Dma::half | Dma::complete);
        Samples candidate{};
        const auto offset = second_half ? scan_count_ : 0U;
        for (std::size_t i = 0; i < scan_count_; ++i) {
            candidate[i] = Io::read16(pending_.data() + offset + i);
        }
        bool raced{};
        if constexpr (Coherent) {
            const auto after = Io::read(Dma::cndtr);
            const auto elapsed = Io::now_us() - copy_start;
            // The time bound rules out a full ring wrap (same half before and after).
            raced = (after == 0) || (after > (scan_count_ * 2)) ||
                    ((after > scan_count_) != second_half) ||
                    (elapsed >= DurationUs{static_cast<std::uint32_t>(scan_count_ * 21U)});
        }
        if ((Io::read(Dma::isr) & Dma::error) != 0) {
            (void)fail(AdcError::dma);
            return ScanEvent::failed;
        }
        if ((Io::read(isr) & ADC_ISR_OVR) != 0) {
            (void)fail(AdcError::overrun);
            return ScanEvent::failed;
        }
        if (raced) {
            ++discarded_;
            if ((now - started_) >= scan_timeout_us) {
                (void)fail(AdcError::timeout);
                return ScanEvent::failed;
            }
            return ScanEvent::pending;
        }
        samples_ = candidate;
        valid_ = true;
        started_ = now;
        return ScanEvent::complete;
    }

    [[nodiscard]] constexpr const Samples &samples() const noexcept { return samples_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return valid_; }
    [[nodiscard]] constexpr AdcError error() const noexcept { return error_; }
    [[nodiscard]] constexpr bool initializing() const noexcept { return enabling_; }
    [[nodiscard]] constexpr std::uint32_t discarded() const noexcept { return discarded_; }
    [[nodiscard]] constexpr AdcError set_extended(const bool enabled) noexcept {
        extended_ = enabled && (Inputs.size() > 11);
        return initialize(stm32f072::ready_timeout);
    }

  private:
    static constexpr std::uint32_t channel_mask(const std::size_t count = Inputs.size()) noexcept {
        std::uint32_t mask{};
        for (std::size_t i = 0; i < count; ++i) {
            mask |= Inputs[i].channel;
        }
        return mask;
    }

    static constexpr bool inputs_valid() noexcept {
        std::uint32_t previous{};
        for (const auto &input : Inputs) {
            const auto &pin = input.pin;
            const auto expected = Registers::channel(pin);
            if ((input.channel != expected) || (input.channel <= previous)) {
                return false;
            }
            previous = input.channel;
        }
        return !Inputs.empty() &&
               (static_cast<std::size_t>(std::popcount(channel_mask())) == Inputs.size());
    }
    static_assert(inputs_valid(), "ADC inputs must be valid, unique and in ascending scan order");

    static constexpr std::uint32_t gpio_clocks() noexcept {
        std::uint32_t clocks{};
        for (const auto &input : Inputs) {
            clocks |= gpio_clock(input.pin);
        }
        return clocks;
    }
    constexpr AdcError fail(const AdcError error) noexcept {
        Io::write(Dma::ccr, configuration);
        if ((Io::read(Registers::cr) & ADC_CR_ADSTART) != 0) {
            Io::write(Registers::cr, Io::read(Registers::cr) | ADC_CR_ADSTP);
        }
        error_ = error;
        valid_ = false;
        busy_ = false;
        enabling_ = false;
        return error_;
    }

    std::array<std::uint16_t, Inputs.size() * (Coherent ? 2 : 1)> pending_{};
    Samples samples_{};
    TimestampUs started_{};
    std::uint32_t discarded_{};
    AdcError error_{AdcError::not_initialized};
    bool initialized_{};
    bool enabling_{};
    bool enabled_{};
    bool busy_{};
    bool valid_{};
    bool extended_{};
    std::size_t scan_count_{std::min<std::size_t>(Inputs.size(), 11)};
};

} // namespace detail

template <typename Io, const auto &Inputs = gimbal_inputs, bool Coherent = coherent_adc_snapshots,
          typename Assignment = firmware::Hardware>
class Adc {
    using State = detail::AdcState<Io, Inputs, Coherent, Assignment>;

  public:
    Adc() = delete;
    using Samples = typename State::Samples;
    inline static constexpr auto scan_period_us = State::scan_period_us;
    inline static constexpr auto scan_timeout_us = State::scan_timeout_us;
    inline static constexpr auto half_period_us = State::half_period_us;

    [[nodiscard]] static AdcError
    initialize(const std::uint32_t attempts = stm32f072::ready_timeout) noexcept {
        return state_.initialize(attempts);
    }
    [[nodiscard]] static ScanEvent service(const TimestampUs now) noexcept {
        return state_.service(now);
    }
    [[nodiscard]] static const Samples &samples() noexcept { return state_.samples(); }
    [[nodiscard]] static bool valid() noexcept { return state_.valid(); }
    [[nodiscard]] static AdcError error() noexcept { return state_.error(); }
    [[nodiscard]] static bool initializing() noexcept { return state_.initializing(); }
    [[nodiscard]] static std::uint32_t discarded() noexcept { return state_.discarded(); }
    [[nodiscard]] static AdcError set_extended(const bool enabled) noexcept {
        return state_.set_extended(enabled);
    }

  private:
    inline static constinit State state_{};
};

[[nodiscard]] constexpr const char *error_message(const AdcError error) noexcept {
    switch (error) {
    case AdcError::none:
        return nullptr;
    case AdcError::not_initialized:
        return coretx::ui::tr::not_initialized;
    case AdcError::calibration:
        return coretx::ui::tr::cal_timeout;
    case AdcError::ready:
        return coretx::ui::tr::ready_timeout;
    case AdcError::overrun:
        return coretx::ui::tr::overrun;
    case AdcError::timeout:
        return coretx::ui::tr::scan_timeout;
    case AdcError::sequence:
        return coretx::ui::tr::scan_length;
    case AdcError::dma:
        return coretx::ui::tr::dma_transfer;
    }
    return coretx::ui::tr::unknown_adc_error;
}

} // namespace coretx::firmware::i6x
