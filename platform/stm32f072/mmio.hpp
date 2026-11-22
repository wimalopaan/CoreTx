// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "platform/stm32f072/clock.hpp"

namespace coretx::firmware::stm32f072 {

struct Mmio {
    Mmio() = delete;

    static void barrier() noexcept { __DMB(); }
    [[nodiscard]] static std::uint8_t read8(const std::uint8_t *const address) noexcept {
        return *static_cast<volatile const std::uint8_t *>(address);
    }
    [[nodiscard]] static std::uint16_t read16(const std::uint16_t *const address) noexcept {
        return *static_cast<volatile const std::uint16_t *>(address);
    }
    [[nodiscard]] static std::uint32_t read(const std::uintptr_t address) noexcept {
        return *reinterpret_cast<volatile const std::uint32_t *>(address);
    }
    static void write(const std::uintptr_t address, const std::uint32_t value) noexcept {
        *reinterpret_cast<volatile std::uint32_t *>(address) = value;
    }
    [[nodiscard]] static std::uint8_t read8(const std::uintptr_t address) noexcept {
        return *reinterpret_cast<volatile const std::uint8_t *>(address);
    }
    static void write8(const std::uintptr_t address, const std::uint8_t value) noexcept {
        *reinterpret_cast<volatile std::uint8_t *>(address) = value;
    }
    static void write_address(const std::uintptr_t reg, const void *const address) noexcept {
        write(reg, reinterpret_cast<std::uintptr_t>(address));
    }
    [[nodiscard]] static TimestampUs now_us() noexcept { return Clock<Mmio>::sample(); }
};

} // namespace coretx::firmware::stm32f072
