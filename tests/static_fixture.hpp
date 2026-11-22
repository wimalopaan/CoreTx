// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "coretx/utility/time.hpp"

#include <cstdint>

namespace coretx::test {

template <typename Fixture> struct FakeMmio {
    FakeMmio() = delete;

    [[nodiscard]] static std::uint32_t read(const std::uintptr_t address) noexcept {
        return Fixture::registers.read(address);
    }
    static void write(const std::uintptr_t address, const std::uint32_t value) noexcept {
        Fixture::registers.write(address, value);
    }
    [[nodiscard]] static std::uint8_t read8(const std::uintptr_t address) noexcept {
        return Fixture::registers.read8(address);
    }
    static void write8(const std::uintptr_t address, const std::uint8_t value) noexcept {
        Fixture::registers.write8(address, value);
    }
    [[nodiscard]] static std::uint8_t read8(const std::uint8_t *const address) noexcept {
        return Fixture::registers.read8(address);
    }
    [[nodiscard]] static std::uint16_t read16(const std::uint16_t *const address) noexcept {
        return Fixture::registers.read16(address);
    }
    template <typename Value>
    static void write_address(const std::uintptr_t reg, Value *const address) noexcept {
        Fixture::registers.write_address(reg, address);
    }
    static void barrier() noexcept { Fixture::registers.barrier(); }
    [[nodiscard]] static TimestampUs now_us() noexcept {
        return TimestampUs{Fixture::registers.now_us()};
    }
};

template <typename Scenario, typename Registers> struct RegisterFixture {
    RegisterFixture() = delete;

    inline static constinit Registers registers{};
    using Io = FakeMmio<RegisterFixture>;

    // Only resets the register bank; static driver state needs its own initialization.
    static void reset_registers() noexcept { registers = Registers{}; }
};

template <typename Scenario, typename Context, template <typename> typename BoardPolicy>
struct FakeApplication {
    FakeApplication() = delete;

    inline static constinit Context context{};
    using Board = BoardPolicy<FakeApplication>;

    // Call before constructing a sender. Board::reset must reset all static mock state.
    static void reset() noexcept {
        context = Context{};
        Board::reset();
    }
};

} // namespace coretx::test
