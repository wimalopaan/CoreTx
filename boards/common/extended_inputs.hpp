// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/common/hardware.hpp"
#include "coretx/features.hpp"
#include "platform/stm32f072/buzzer.hpp"

#include <array>

namespace coretx::firmware {

template <typename Io, typename Assignment> class ExtendedInputs {
    using Sound = Buzzer<Io, Assignment::buzzer_timer, Assignment::buzzer>;
    static constexpr bool restores_swd = [] {
        for (const auto input : Assignment::digital_inputs)
            if (input.released != DigitalInput::Released::analog)
                return true;
        return false;
    }();

  public:
    ExtendedInputs() = delete;

    static void initialize() noexcept {
        enabled_ = 0;
        Sound::initialize();
    }
    static void configure(const std::uint8_t enabled) noexcept {
        if constexpr (features::extended_inputs) {
            if (enabled == enabled_)
                return;
            using namespace stm32f072::registers;
            constexpr auto clocks = [] {
                std::uint32_t result{};
                for (const auto input : Assignment::digital_inputs)
                    result |= gpio_clock(input.pin());
                return result;
            }();
            Io::write(rcc_ahbenr, Io::read(rcc_ahbenr) | clocks);
            (void)Io::read(rcc_ahbenr);
            for (std::size_t i = 0; i < Assignment::digital_inputs.size(); ++i) {
                if (((enabled ^ enabled_) & (1U << i)) == 0)
                    continue;
                const auto input = Assignment::digital_inputs[i];
                const auto pin = input.pin();
                if ((enabled & (1U << i)) != 0)
                    configure_input<Io>(pin);
                else if constexpr (restores_swd) {
                    if (input.released != DigitalInput::Released::analog) {
                        configure_alternate<Io>(pin, 0, true);
                        if (input.released == DigitalInput::Released::swclk) {
                            const auto address = pin.port + offsetof(GPIO_TypeDef, PUPDR);
                            const auto shift = 2U * pin.index;
                            Io::write(address,
                                      (Io::read(address) & ~(3U << shift)) | (2U << shift));
                        }
                    } else
                        coretx::firmware::configure<Io>(pin, true);
                } else
                    coretx::firmware::configure<Io>(pin, true);
            }
            enabled_ = enabled;
        } else {
            (void)enabled;
        }
    }
    [[nodiscard]] static std::uint8_t read() noexcept {
        std::uint8_t result{};
        if constexpr (features::extended_inputs) {
            if (enabled_)
                for (std::size_t i = 0; i < Assignment::digital_inputs.size(); ++i) {
                    const auto pin = Assignment::digital_inputs[i].pin();
                    if (((enabled_ & (1U << i)) != 0) &&
                        ((Io::read(pin.port + offsetof(GPIO_TypeDef, IDR)) & (1U << pin.index)) ==
                         0))
                        result |= static_cast<std::uint8_t>(1U << i);
                }
        }
        return result;
    }
    static void service(const TimestampUs now, const bool click,
                        const ClickTone tone = ClickTone::control) noexcept {
        if (click)
            Sound::click(now, tone);
        Sound::service(now);
    }
    static void stop() noexcept { Sound::stop(); }

  private:
    inline static std::uint8_t enabled_{};
};

} // namespace coretx::firmware
