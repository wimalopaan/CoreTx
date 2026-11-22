// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "platform/stm32f072/platform.hpp"

#include <bit>

namespace coretx::firmware {

inline constexpr auto bsrr_reset_shift = std::countr_zero(std::uint32_t{GPIO_BSRR_BR_0});

struct Pin {
    std::uintptr_t port{};
    std::uint8_t index{};
};

template <typename Io> constexpr void drive(const Pin &pin, const bool high) noexcept {
    Io::write(pin.port + offsetof(GPIO_TypeDef, BSRR),
              (high ? std::uint32_t{GPIO_BSRR_BS_0} : std::uint32_t{GPIO_BSRR_BR_0}) << pin.index);
}

template <typename Io> constexpr void configure(const Pin &pin, const bool analog) noexcept {
    constexpr auto mode_bits = GPIO_MODER_MODER1_Pos - GPIO_MODER_MODER0_Pos;
    const auto shift = mode_bits * pin.index;
    const auto moder = pin.port + offsetof(GPIO_TypeDef, MODER);
    const auto pupdr = pin.port + offsetof(GPIO_TypeDef, PUPDR);
    const auto otyper = pin.port + offsetof(GPIO_TypeDef, OTYPER);
    Io::write(moder,
              (Io::read(moder) & ~(std::uint32_t{GPIO_MODER_MODER0} << shift)) |
                  ((analog ? std::uint32_t{GPIO_MODER_MODER0} : std::uint32_t{GPIO_MODER_MODER0_0})
                   << shift));
    Io::write(pupdr, Io::read(pupdr) & ~(std::uint32_t{GPIO_PUPDR_PUPDR0} << shift));
    if (!analog) {
        Io::write(otyper, Io::read(otyper) & ~(std::uint32_t{GPIO_OTYPER_OT_0} << pin.index));
    }
}

template <typename Io> constexpr void configure_input(const Pin &pin) noexcept {
    constexpr auto mode_bits = GPIO_MODER_MODER1_Pos - GPIO_MODER_MODER0_Pos;
    const auto shift = mode_bits * pin.index;
    const auto moder = pin.port + offsetof(GPIO_TypeDef, MODER);
    const auto pupdr = pin.port + offsetof(GPIO_TypeDef, PUPDR);
    Io::write(moder, Io::read(moder) & ~(std::uint32_t{GPIO_MODER_MODER0} << shift));
    Io::write(pupdr, (Io::read(pupdr) & ~(std::uint32_t{GPIO_PUPDR_PUPDR0} << shift)) |
                         (std::uint32_t{GPIO_PUPDR_PUPDR0_0} << shift));
}

template <typename Io>
constexpr void configure_alternate(const Pin pin, const std::uint8_t function,
                                   const bool pull_up) noexcept {
    constexpr auto mode_width = GPIO_MODER_MODER1_Pos - GPIO_MODER_MODER0_Pos;
    constexpr auto af_width = GPIO_AFRL_AFSEL1_Pos - GPIO_AFRL_AFSEL0_Pos;
    constexpr auto pins_per_afr = sizeof(std::uint32_t) * 8U / af_width;
    const auto shift = pin.index * mode_width;
    const auto moder = pin.port + offsetof(GPIO_TypeDef, MODER);
    const auto pupdr = pin.port + offsetof(GPIO_TypeDef, PUPDR);
    const auto otyper = pin.port + offsetof(GPIO_TypeDef, OTYPER);
    const auto speed = pin.port + offsetof(GPIO_TypeDef, OSPEEDR);
    const auto afr =
        pin.port + offsetof(GPIO_TypeDef, AFR) + (pin.index / pins_per_afr) * sizeof(std::uint32_t);
    const auto af_shift = (pin.index % pins_per_afr) * af_width;
    configure_input<Io>(pin);
    if (!pull_up)
        Io::write(pupdr, Io::read(pupdr) & ~(std::uint32_t{GPIO_PUPDR_PUPDR0} << shift));
    Io::write(otyper, Io::read(otyper) & ~(std::uint32_t{GPIO_OTYPER_OT_0} << pin.index));
    Io::write(speed, Io::read(speed) | (std::uint32_t{GPIO_OSPEEDR_OSPEEDR0} << shift));
    Io::write(afr, (Io::read(afr) & ~(std::uint32_t{GPIO_AFRL_AFSEL0} << af_shift)) |
                       (std::uint32_t{function} << af_shift));
    Io::write(moder, (Io::read(moder) & ~(std::uint32_t{GPIO_MODER_MODER0} << shift)) |
                         (std::uint32_t{GPIO_MODER_MODER0_1} << shift));
}

} // namespace coretx::firmware
