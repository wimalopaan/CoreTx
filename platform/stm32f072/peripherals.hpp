// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "platform/stm32f072/gpio.hpp"

#include <array>
#include <string_view>

namespace coretx::firmware {

[[nodiscard]] constexpr bool same_pin(const Pin a, const Pin b) noexcept {
    return (a.port == b.port) && (a.index == b.index);
}

[[nodiscard]] constexpr std::uint32_t gpio_clock(const Pin pin) noexcept {
    switch (pin.port) {
    case GPIOA_BASE:
        return RCC_AHBENR_GPIOAEN;
    case GPIOB_BASE:
        return RCC_AHBENR_GPIOBEN;
    case GPIOC_BASE:
        return RCC_AHBENR_GPIOCEN;
    case GPIOD_BASE:
        return RCC_AHBENR_GPIODEN;
    case GPIOE_BASE:
        return RCC_AHBENR_GPIOEEN;
    case GPIOF_BASE:
        return RCC_AHBENR_GPIOFEN;
    default:
        return 0;
    }
}

template <typename Range>
[[nodiscard]] constexpr std::uint32_t gpio_clocks(const Range &pins) noexcept {
    std::uint32_t clocks{};
    for (const auto pin : pins)
        clocks |= gpio_clock(pin);
    return clocks;
}

struct AnalogInput {
    Pin pin{};
    std::uint32_t channel{};
    std::string_view label{};
};

struct LcdPins {
    std::uintptr_t data_port;
    Pin rs, reset, rw, select, enable, backlight, brightness;
};

struct I2cPins {
    unsigned peripheral;
    Pin scl, sda;
};

struct UartPins {
    unsigned peripheral;
    Pin tx, rx;
};

struct DigitalInput {
    // Disabled extended inputs return to analog, except the two SWD signals.
    enum class Released : std::uint8_t { analog, swdio, swclk };
    std::uintptr_t port;
    std::uint8_t index;
    Released released;
    constexpr DigitalInput(const Pin input, const Released policy = Released::analog) noexcept
        : port(input.port), index(input.index), released(policy) {}
    [[nodiscard]] constexpr Pin pin() const noexcept { return {port, index}; }
};

namespace stm32f072 {

inline constexpr std::uint8_t invalid_af = 0xFF;

template <Pin Input> struct ExtiTraits {
    static_assert((Input.index < 16) && (gpio_clock(Input) != 0), "Invalid EXTI input");
    static constexpr auto line = std::uint32_t{1} << Input.index;
    static constexpr auto irq = Input.index < 2   ? EXTI0_1_IRQn
                                : Input.index < 4 ? EXTI2_3_IRQn
                                                  : EXTI4_15_IRQn;
    static constexpr auto route_register =
        SYSCFG_BASE + offsetof(SYSCFG_TypeDef, EXTICR) + (Input.index / 4U) * sizeof(std::uint32_t);
    static constexpr auto shift = (Input.index % 4U) * 4U;
    static constexpr auto route_mask = std::uint32_t{0xF} << shift;
    static constexpr std::uint32_t port = Input.port == GPIOA_BASE   ? 0
                                          : Input.port == GPIOB_BASE ? 1
                                          : Input.port == GPIOC_BASE ? 2
                                          : Input.port == GPIOD_BASE ? 3
                                          : Input.port == GPIOE_BASE ? 4
                                                                     : 5;
    static constexpr auto route = port << shift;
};

template <unsigned Number> struct TimerTraits {
    static constexpr bool supported = (Number == 1) || (Number == 2) || (Number == 3) ||
                                      (Number == 6) || (Number == 7) || (Number == 14) ||
                                      (Number == 15) || (Number == 16) || (Number == 17);
    static constexpr bool channel1 = supported && (Number != 6) && (Number != 7);
    static constexpr bool main_output =
        (Number == 1) || (Number == 15) || (Number == 16) || (Number == 17);
    static constexpr std::uintptr_t base = Number == 1    ? TIM1_BASE
                                           : Number == 2  ? TIM2_BASE
                                           : Number == 3  ? TIM3_BASE
                                           : Number == 6  ? TIM6_BASE
                                           : Number == 7  ? TIM7_BASE
                                           : Number == 14 ? TIM14_BASE
                                           : Number == 15 ? TIM15_BASE
                                           : Number == 16 ? TIM16_BASE
                                           : Number == 17 ? TIM17_BASE
                                                          : 0;
    static constexpr auto enable_register =
        (Number == 1) || (Number >= 15) ? registers::rcc_apb2enr : registers::rcc_apb1enr;
    static constexpr auto reset_register = (Number == 1) || (Number >= 15)
                                               ? RCC_BASE + offsetof(RCC_TypeDef, APB2RSTR)
                                               : registers::rcc_apb1rstr;
    static constexpr std::uint32_t enable_mask = Number == 1    ? RCC_APB2ENR_TIM1EN
                                                 : Number == 2  ? RCC_APB1ENR_TIM2EN
                                                 : Number == 3  ? RCC_APB1ENR_TIM3EN
                                                 : Number == 6  ? RCC_APB1ENR_TIM6EN
                                                 : Number == 7  ? RCC_APB1ENR_TIM7EN
                                                 : Number == 14 ? RCC_APB1ENR_TIM14EN
                                                 : Number == 15 ? RCC_APB2ENR_TIM15EN
                                                 : Number == 16 ? RCC_APB2ENR_TIM16EN
                                                 : Number == 17 ? RCC_APB2ENR_TIM17EN
                                                                : 0;
    static constexpr std::uint32_t reset_mask = enable_mask;
    static constexpr auto irq = Number == 1    ? TIM1_CC_IRQn
                                : Number == 2  ? TIM2_IRQn
                                : Number == 3  ? TIM3_IRQn
                                : Number == 6  ? TIM6_DAC_IRQn
                                : Number == 7  ? TIM7_IRQn
                                : Number == 14 ? TIM14_IRQn
                                : Number == 15 ? TIM15_IRQn
                                : Number == 16 ? TIM16_IRQn
                                               : TIM17_IRQn;
    // CH1 mappings used by the drivers; unsupported pin/function pairs fail at compile time.
    [[nodiscard]] static constexpr std::uint8_t alternate(const Pin pin) noexcept {
        if constexpr (Number == 1) {
            if (same_pin(pin, {GPIOA_BASE, 8}))
                return 2;
        } else if constexpr (Number == 2) {
            if (same_pin(pin, {GPIOA_BASE, 0}) || same_pin(pin, {GPIOA_BASE, 5}) ||
                same_pin(pin, {GPIOA_BASE, 15}))
                return 2;
        } else if constexpr (Number == 3) {
            if (same_pin(pin, {GPIOA_BASE, 6}) || same_pin(pin, {GPIOB_BASE, 4}))
                return 1;
            if (same_pin(pin, {GPIOC_BASE, 6}))
                return 0;
        } else if constexpr (Number == 14) {
            if (same_pin(pin, {GPIOA_BASE, 4}) || same_pin(pin, {GPIOA_BASE, 7}))
                return 4;
            if (same_pin(pin, {GPIOB_BASE, 1}))
                return 0;
        } else if constexpr (Number == 15) {
            if (same_pin(pin, {GPIOA_BASE, 2}))
                return 0;
            if (same_pin(pin, {GPIOB_BASE, 14}))
                return 1;
        } else if constexpr (Number == 16) {
            if (same_pin(pin, {GPIOA_BASE, 6}))
                return 5;
            if (same_pin(pin, {GPIOB_BASE, 8}))
                return 2;
        } else if constexpr (Number == 17) {
            if (same_pin(pin, {GPIOA_BASE, 7}))
                return 5;
            if (same_pin(pin, {GPIOB_BASE, 9}))
                return 2;
        }
        return invalid_af;
    }
};

template <unsigned Number, Pin Output>
concept TimerChannel1 =
    TimerTraits<Number>::channel1 && (TimerTraits<Number>::alternate(Output) != invalid_af);

template <unsigned Number> struct DmaTraits {
    static_assert((Number >= 1) && (Number <= 7));
    static constexpr auto base = DMA1_Channel1_BASE + (Number - 1U) * 20U;
    static constexpr auto isr = DMA1_BASE + offsetof(DMA_TypeDef, ISR);
    static constexpr auto ifcr = DMA1_BASE + offsetof(DMA_TypeDef, IFCR);
    static constexpr auto ccr = base + offsetof(DMA_Channel_TypeDef, CCR);
    static constexpr auto cndtr = base + offsetof(DMA_Channel_TypeDef, CNDTR);
    static constexpr auto cpar = base + offsetof(DMA_Channel_TypeDef, CPAR);
    static constexpr auto cmar = base + offsetof(DMA_Channel_TypeDef, CMAR);
    static constexpr auto clear = std::uint32_t{DMA_IFCR_CGIF1} << ((Number - 1U) * 4U);
    static constexpr auto error = std::uint32_t{DMA_ISR_TEIF1} << ((Number - 1U) * 4U);
    static constexpr auto half = std::uint32_t{DMA_ISR_HTIF1} << ((Number - 1U) * 4U);
    static constexpr auto complete = std::uint32_t{DMA_ISR_TCIF1} << ((Number - 1U) * 4U);
};

template <unsigned Number> struct I2cTraits {
    static_assert((Number == 1) || (Number == 2));
    static constexpr auto base = Number == 1 ? I2C1_BASE : I2C2_BASE;
    static constexpr auto enable_register = registers::rcc_apb1enr;
    static constexpr auto reset_register = registers::rcc_apb1rstr;
    static constexpr std::uint32_t enable_mask =
        Number == 1 ? RCC_APB1ENR_I2C1EN : RCC_APB1ENR_I2C2EN;
    static constexpr std::uint32_t reset_mask =
        Number == 1 ? RCC_APB1RSTR_I2C1RST : RCC_APB1RSTR_I2C2RST;
    static constexpr std::uint32_t clock_mask = Number == 1 ? RCC_CFGR3_I2C1SW : 0U;
    static constexpr auto cr1 = base + offsetof(I2C_TypeDef, CR1);
    static constexpr auto cr2 = base + offsetof(I2C_TypeDef, CR2);
    static constexpr auto timingr = base + offsetof(I2C_TypeDef, TIMINGR);
    static constexpr auto isr = base + offsetof(I2C_TypeDef, ISR);
    static constexpr auto icr = base + offsetof(I2C_TypeDef, ICR);
    static constexpr auto txdr = base + offsetof(I2C_TypeDef, TXDR);
    static constexpr auto rxdr = base + offsetof(I2C_TypeDef, RXDR);
    [[nodiscard]] static constexpr std::uint8_t alternate(const Pin pin, const bool scl) noexcept {
        if constexpr (Number == 1) {
            if (same_pin(pin, {GPIOB_BASE, static_cast<std::uint8_t>(scl ? 6 : 7)}) ||
                same_pin(pin, {GPIOB_BASE, static_cast<std::uint8_t>(scl ? 8 : 9)}))
                return 1;
        } else {
            if (same_pin(pin, {GPIOB_BASE, static_cast<std::uint8_t>(scl ? 10 : 11)}))
                return 1;
        }
        return invalid_af;
    }
};

template <unsigned Number> struct UartTraits {
    static_assert((Number == 1) || (Number == 2));
    static constexpr auto base = Number == 1 ? USART1_BASE : USART2_BASE;
    static constexpr auto enable_register =
        Number == 1 ? registers::rcc_apb2enr : registers::rcc_apb1enr;
    static constexpr auto reset_register =
        Number == 1 ? RCC_BASE + offsetof(RCC_TypeDef, APB2RSTR) : registers::rcc_apb1rstr;
    static constexpr std::uint32_t enable_mask =
        Number == 1 ? RCC_APB2ENR_USART1EN : RCC_APB1ENR_USART2EN;
    static constexpr std::uint32_t reset_mask =
        Number == 1 ? RCC_APB2RSTR_USART1RST : RCC_APB1RSTR_USART2RST;
    static constexpr std::uint32_t clock_mask =
        Number == 1 ? RCC_CFGR3_USART1SW : RCC_CFGR3_USART2SW;
    static constexpr std::uint32_t dma_set = Number == 1 ? 0U : SYSCFG_CFGR1_USART2_DMA_RMP;
    static constexpr std::uint32_t dma_clear =
        Number == 1 ? SYSCFG_CFGR1_USART1TX_DMA_RMP | SYSCFG_CFGR1_USART1RX_DMA_RMP : 0U;
    static constexpr unsigned tx_dma = Number == 1 ? 2 : 7;
    static constexpr unsigned rx_dma = Number == 1 ? 3 : 6;
    [[nodiscard]] static constexpr std::uint8_t alternate(const Pin pin, const bool tx) noexcept {
        if constexpr (Number == 1) {
            if (same_pin(pin, {GPIOB_BASE, static_cast<std::uint8_t>(tx ? 6 : 7)}))
                return 0;
            if (same_pin(pin, {GPIOA_BASE, static_cast<std::uint8_t>(tx ? 9 : 10)}))
                return 1;
        } else {
            if (same_pin(pin, {GPIOD_BASE, static_cast<std::uint8_t>(tx ? 5 : 6)}))
                return 0;
            if (same_pin(pin, {GPIOA_BASE, static_cast<std::uint8_t>(tx ? 2 : 3)}) ||
                (!tx && same_pin(pin, {GPIOA_BASE, 15})))
                return 1;
        }
        return invalid_af;
    }
};

template <unsigned Number> struct SpiTraits {
    static_assert(Number == 1);
    static constexpr auto base = SPI1_BASE;
    static constexpr auto enable_register = registers::rcc_apb2enr;
    static constexpr auto reset_register = RCC_BASE + offsetof(RCC_TypeDef, APB2RSTR);
    static constexpr std::uint32_t enable_mask = RCC_APB2ENR_SPI1EN;
    static constexpr std::uint32_t reset_mask = RCC_APB2RSTR_SPI1RST;
    static constexpr auto control1 = base + offsetof(SPI_TypeDef, CR1);
    static constexpr auto control2 = base + offsetof(SPI_TypeDef, CR2);
    static constexpr auto status = base + offsetof(SPI_TypeDef, SR);
    static constexpr auto data = base + offsetof(SPI_TypeDef, DR);
    [[nodiscard]] static constexpr std::uint8_t alternate(const Pin pin,
                                                          const unsigned signal) noexcept {
        if (same_pin(pin, {GPIOE_BASE, static_cast<std::uint8_t>(13U + signal)}))
            return 1;
        if (same_pin(pin, {GPIOA_BASE, static_cast<std::uint8_t>(5U + signal)}) ||
            same_pin(pin, {GPIOB_BASE, static_cast<std::uint8_t>(3U + signal)}))
            return 0;
        return invalid_af;
    }
};

template <unsigned Number> struct AdcTraits {
    static_assert(Number == 1);
    static constexpr auto base = ADC1_BASE;
    static constexpr auto cr = base + offsetof(ADC_TypeDef, CR);
    static constexpr auto isr = base + offsetof(ADC_TypeDef, ISR);
    static constexpr auto dr = base + offsetof(ADC_TypeDef, DR);
    static constexpr auto cfgr1 = base + offsetof(ADC_TypeDef, CFGR1);
    static constexpr auto cfgr2 = base + offsetof(ADC_TypeDef, CFGR2);
    static constexpr auto smpr = base + offsetof(ADC_TypeDef, SMPR);
    static constexpr auto chselr = base + offsetof(ADC_TypeDef, CHSELR);
    static constexpr auto reset = RCC_BASE + offsetof(RCC_TypeDef, APB2RSTR);
    static constexpr std::uint32_t enable_mask = RCC_APB2ENR_ADCEN;
    static constexpr std::uint32_t reset_mask = RCC_APB2RSTR_ADCRST;
    static constexpr unsigned dma = 1;
    [[nodiscard]] static constexpr std::uint32_t channel(const Pin pin) noexcept {
        if ((pin.port == GPIOA_BASE) && (pin.index < 8))
            return std::uint32_t{ADC_CHSELR_CHSEL0} << pin.index;
        if ((pin.port == GPIOB_BASE) && (pin.index < 2))
            return std::uint32_t{ADC_CHSELR_CHSEL8} << pin.index;
        if ((pin.port == GPIOC_BASE) && (pin.index < 6))
            return std::uint32_t{ADC_CHSELR_CHSEL10} << pin.index;
        return 0;
    }
};

} // namespace stm32f072
} // namespace coretx::firmware
