// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "coretx/radio/afhds2a.hpp"
#include "coretx/storage/storage.hpp"
#include "platform/stm32f072/peripherals.hpp"

namespace coretx::firmware {

struct A7105Pins {
    Pin select;
    Pin clock;
    Pin input;
    Pin output;
    Pin rx_enable;
    Pin tx_enable;
    Pin antenna_a;
    Pin antenna_b;
    Pin status;
    unsigned peripheral;
};

template <typename Io> [[nodiscard]] constexpr afhds2a::TxId transmitter_id() noexcept {
    std::array<std::uint8_t, 12> bytes{};
    for (std::size_t word = 0; word < 3; ++word) {
        const auto value = Io::read(UID_BASE + word * sizeof(std::uint32_t));
        for (std::size_t byte = 0; byte < sizeof(value); ++byte)
            bytes[word * sizeof(value) + byte] = static_cast<std::uint8_t>(value >> (byte * 8));
    }
    auto id = coretx::storage::crc32(bytes);
    if (!afhds2a::valid_id(id))
        id ^= 0xA71052A1U;
    return {id};
}

template <typename Io, A7105Pins Pins> constexpr void internal_front_end_off() noexcept {
    namespace reg = stm32f072::registers;
    Io::write(reg::rcc_ahbenr,
              Io::read(reg::rcc_ahbenr) | gpio_clock(Pins.rx_enable) | gpio_clock(Pins.tx_enable));
    (void)Io::read(reg::rcc_ahbenr);
    // Both front-end enables are active low; preload before enabling outputs.
    drive<Io>(Pins.rx_enable, true);
    drive<Io>(Pins.tx_enable, true);
    configure<Io>(Pins.rx_enable, false);
    configure<Io>(Pins.tx_enable, false);
}

} // namespace coretx::firmware
