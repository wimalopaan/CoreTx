// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "coretx/features.hpp"
#include "platform/stm32f072/radio_gpio.hpp"

namespace coretx::firmware::i6x {

struct Hardware {
    static constexpr std::string_view name = "I6X";
    static constexpr bool sb_three_position = false;
    static constexpr bool power_latch = false;
    static constexpr Pin hold_power{};
    static constexpr unsigned clock_timer = 2;
    static constexpr unsigned buzzer_timer = 1;
    static constexpr Pin buzzer{GPIOA_BASE, 8};
    static constexpr unsigned adc = 1;
    static constexpr unsigned adc_dma = 1;
    static constexpr std::array gimbal_inputs{
        AnalogInput{{GPIOA_BASE, 0}, stm32f072::AdcTraits<adc>::channel({GPIOA_BASE, 0}), "RH PA0"},
        AnalogInput{{GPIOA_BASE, 1}, stm32f072::AdcTraits<adc>::channel({GPIOA_BASE, 1}), "RV PA1"},
        AnalogInput{{GPIOA_BASE, 2}, stm32f072::AdcTraits<adc>::channel({GPIOA_BASE, 2}), "LV PA2"},
        AnalogInput{
            {GPIOA_BASE, 3}, stm32f072::AdcTraits<adc>::channel({GPIOA_BASE, 3}), "LH PA3"}};
    static constexpr std::array stock_inputs{
        gimbal_inputs[0],
        gimbal_inputs[1],
        gimbal_inputs[2],
        gimbal_inputs[3],
        AnalogInput{{GPIOA_BASE, 4}, stm32f072::AdcTraits<adc>::channel({GPIOA_BASE, 4}), "SA"},
        AnalogInput{{GPIOA_BASE, 5}, stm32f072::AdcTraits<adc>::channel({GPIOA_BASE, 5}), "SB"},
        AnalogInput{{GPIOA_BASE, 6}, stm32f072::AdcTraits<adc>::channel({GPIOA_BASE, 6}), "VRA"},
        AnalogInput{{GPIOA_BASE, 7}, stm32f072::AdcTraits<adc>::channel({GPIOA_BASE, 7}), "VRB"},
        AnalogInput{{GPIOB_BASE, 0}, stm32f072::AdcTraits<adc>::channel({GPIOB_BASE, 0}), "SC"},
        AnalogInput{{GPIOB_BASE, 1}, stm32f072::AdcTraits<adc>::channel({GPIOB_BASE, 1}), "SD"}};
    static constexpr Pin battery{GPIOC_BASE, 0};
    static constexpr std::array extra_analog{Pin{GPIOC_BASE, 2}, Pin{GPIOC_BASE, 3},
                                             Pin{GPIOC_BASE, 4}, Pin{GPIOC_BASE, 5}};
    static constexpr auto sender_inputs = [] {
        std::array<AnalogInput,
                   stock_inputs.size() + 1 + (features::extended_inputs ? extra_analog.size() : 0)>
            inputs{};
        for (std::size_t i = 0; i < stock_inputs.size(); ++i)
            inputs[i] = stock_inputs[i];
        inputs[stock_inputs.size()] = {battery, stm32f072::AdcTraits<adc>::channel(battery),
                                       "TX BAT"};
        if constexpr (features::extended_inputs)
            for (std::size_t i = 0; i < extra_analog.size(); ++i)
                inputs[stock_inputs.size() + 1 + i] = {
                    extra_analog[i], stm32f072::AdcTraits<adc>::channel(extra_analog[i]), "EXT"};
        return inputs;
    }();
    static constexpr std::array digital_inputs{DigitalInput{{GPIOC_BASE, 12}},
                                               DigitalInput{{GPIOC_BASE, 15}}};
    static constexpr std::array key_columns{Pin{GPIOC_BASE, 6}, Pin{GPIOC_BASE, 7},
                                            Pin{GPIOC_BASE, 8}};
    static constexpr std::uintptr_t key_rows_port = GPIOD_BASE;
    static constexpr unsigned key_rows_first = 12;
    static constexpr std::uint8_t bootloader_trim_mask = (1U << 1) | (1U << 6);
    static constexpr LcdPins lcd{GPIOE_BASE,      {GPIOB_BASE, 3}, {GPIOB_BASE, 4},
                                 {GPIOB_BASE, 5}, {GPIOD_BASE, 2}, {GPIOD_BASE, 7},
                                 {GPIOF_BASE, 3}, {GPIOC_BASE, 9}};
    static constexpr I2cPins storage{2, {GPIOB_BASE, 10}, {GPIOB_BASE, 11}};
    static constexpr UartPins crsf{2, {GPIOD_BASE, 5}, {GPIOA_BASE, 15}};
    static constexpr unsigned crsf_tx_dma = 7;
    static constexpr unsigned crsf_rx_dma = 6;
    static constexpr Pin external_module_power{GPIOC_BASE, 13};
    static constexpr bool external_module_active_high = false;
    static constexpr A7105Pins radio{
        {GPIOE_BASE, 12}, {GPIOE_BASE, 13}, {GPIOE_BASE, 14}, {GPIOE_BASE, 15}, {GPIOE_BASE, 8},
        {GPIOE_BASE, 9},  {GPIOE_BASE, 10}, {GPIOE_BASE, 11}, {GPIOB_BASE, 2},  1};
};

} // namespace coretx::firmware::i6x
