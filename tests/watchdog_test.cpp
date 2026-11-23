// SPDX-License-Identifier: GPL-2.0-or-later
#include "boards/common/dashboard.hpp"
#include "platform/stm32f072/watchdog.hpp"
#include "static_fixture.hpp"
#include "tests/renderers/touch.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace {

namespace fw = coretx::firmware;
namespace reg = fw::watchdog_registers;
namespace chip = fw::stm32f072::registers;
using Error = fw::WatchdogError;
using namespace coretx::literals;

struct Registers {
    struct Cell {
        std::uintptr_t address{};
        std::uint32_t value{};
    };
    std::array<Cell, 16> cells{};
    std::size_t count{};
    unsigned feeds{};
    unsigned pending{};
    bool started{};
    bool unlocked{};
    bool blocked{};
    bool deny_backup{};
    bool corrupt_backup{};
    bool early_feed{};

    constexpr Registers() { reset(RCC_CSR_PORRSTF); }

    constexpr std::uint32_t &cell(const std::uintptr_t address) {
        for (std::size_t i = 0; i < count; ++i) {
            if (cells[i].address == address)
                return cells[i].value;
        }
        if (count == cells.size())
            std::abort();
        auto &entry = cells[count++];
        entry.address = address;
        return entry.value;
    }

    constexpr void reset(const std::uint32_t flags) {
        cell(reg::reset_flags) = flags;
        cell(reg::power_control) = 0;
        cell(chip::rcc_apb1enr) = 0;
        cell(reg::window) = IWDG_WINR_WIN;
        started = false;
        unlocked = false;
        pending = 0;
        feeds = 0;
    }

    constexpr std::uint32_t read(const std::uintptr_t address) {
        if (address == reg::status) {
            if (blocked || (pending != 0)) {
                if (pending != 0)
                    --pending;
                return IWDG_SR_PVU | IWDG_SR_RVU | IWDG_SR_WVU;
            }
            return 0;
        }
        return cell(address);
    }

    constexpr void write(const std::uintptr_t address, const std::uint32_t value) {
        if (address == reg::key) {
            if (value == 0xCCCC) {
                started = true;
                cell(reg::reset_flags) |= RCC_CSR_LSION | RCC_CSR_LSIRDY;
            } else if (value == 0xAAAA) {
                ++feeds;
                early_feed |= blocked || (pending != 0);
            } else {
                unlocked = value == 0x5555;
            }
            return;
        }
        if ((address == reg::prescaler) || (address == reg::reload) || (address == reg::window)) {
            if (!unlocked)
                return;
            pending = 1;
        }
        if ((address == reg::power_control) && deny_backup)
            return;
        if ((address == reg::backup_magic) || (address == reg::backup_count) ||
            (address == reg::backup_fault) || (address == reg::backup_fault_magic)) {
            if (corrupt_backup || ((cell(chip::rcc_apb1enr) & RCC_APB1ENR_PWREN) == 0) ||
                ((cell(reg::power_control) & PWR_CR_DBP) == 0))
                return;
        }
        cell(address) = (address == reg::reset_flags) && ((value & RCC_CSR_RMVF) != 0)
                            ? value & 0x00FF'FFFFU
                            : value;
    }
};

struct ResetHistory {};
struct StartupRefresh {};
struct StopFeeding {};
struct BackupWindow {};
struct HealthyWatchdog {};
struct FailedWatchdog {};
template <typename Scenario> using Fixture = coretx::test::RegisterFixture<Scenario, Registers>;

bool reset_history() {
    using Test = Fixture<ResetHistory>;
    using Driver = fw::Watchdog<Test::Io>;
    Test::reset_registers();
    auto &io = Test::registers;
    io.cell(reg::backup_count) = 99;
    if ((Driver::initialize() != Error::none) || (Driver::resets() != 0) ||
        ((io.cell(reg::reset_flags) & RCC_CSR_PORRSTF) != 0) ||
        ((io.cell(reg::reset_flags) & RCC_CSR_LSION) == 0) ||
        ((io.cell(reg::power_control) & PWR_CR_DBP) != 0))
        return false;
    unsigned expected{};
    for (const auto cause : std::array<std::uint32_t, 4>{RCC_CSR_IWDGRSTF, RCC_CSR_IWDGRSTF,
                                                         RCC_CSR_SFTRSTF, RCC_CSR_PINRSTF}) {
        io.reset(cause);
        if (cause == RCC_CSR_IWDGRSTF)
            ++expected;
        if ((Driver::initialize() != Error::none) || (Driver::resets() != expected))
            return false;
    }
    io.cell(reg::backup_count) = 0xFFFF'FFFFU;
    io.reset(RCC_CSR_IWDGRSTF);
    if ((Driver::initialize() != Error::none) || (Driver::resets() != 0xFFFF'FFFFU))
        return false;
    io.reset(RCC_CSR_PORRSTF | RCC_CSR_IWDGRSTF);
    return (Driver::initialize() == Error::none) && (Driver::resets() == 0);
}

bool startup_and_refresh() {
    using Test = Fixture<StartupRefresh>;
    using Driver = fw::Watchdog<Test::Io>;
    using State = fw::detail::WatchdogState<Test::Io>;
    for (const std::uint32_t raw : {0U, 0xFFFF'F000U}) {
        Test::reset_registers();
        auto &io = Test::registers;
        if ((Driver::initialize() != Error::none) || !io.started || io.early_feed ||
            (io.cell(reg::prescaler) != IWDG_PR_PR_2) ||
            (io.cell(reg::reload) != State::startup_reload) ||
            ((io.cell(chip::dbg_apb1fz) & DBGMCU_APB1_FZ_DBG_IWDG_STOP) == 0))
            return false;
        const coretx::TimestampUs now{raw};
        if ((Driver::start_runtime(now) != Error::none) ||
            (io.cell(reg::reload) != State::runtime_reload))
            return false;
        const auto initial_feeds = io.feeds;
        if ((Driver::service(now + 49'999_us) != Error::none) || (io.feeds != initial_feeds))
            return false;
        if ((Driver::service(now + 50_ms) != Error::none) || (io.feeds != (initial_feeds + 1)))
            return false;
        for (unsigned i = 0; i < 10; ++i) {
            if (Driver::service(now + 50_ms) != Error::none)
                return false;
        }
        if ((io.feeds != (initial_feeds + 1)) || io.early_feed)
            return false;
    }
    return true;
}

bool fault_history() {
    struct Scenario {};
    using Test = Fixture<Scenario>;
    using Driver = fw::Watchdog<Test::Io>;
    Test::reset_registers();
    auto &io = Test::registers;
    if (Driver::initialize() != Error::none)
        return false;
    Driver::mark(1);
    Driver::record_failure(3);
    io.reset(RCC_CSR_IWDGRSTF);
    if ((Driver::initialize() != Error::none) || (Driver::last_fault() != 0x0301) ||
        (io.cell(reg::backup_fault) != 0) || ((io.cell(reg::power_control) & PWR_CR_DBP) != 0))
        return false;
    Driver::mark(2);
    Driver::mark(0);
    io.reset(RCC_CSR_IWDGRSTF);
    if ((Driver::initialize() != Error::none) || (Driver::last_fault() != 0))
        return false;
    Driver::record_failure(0x84);
    io.reset(RCC_CSR_PORRSTF);
    if ((Driver::initialize() != Error::none) || (Driver::last_fault() != 0))
        return false;
    io.cell(reg::backup_fault_magic) = 0;
    io.cell(reg::backup_fault) = 0xFFFFFFFFU;
    io.reset(RCC_CSR_IWDGRSTF);
    if ((Driver::initialize() != Error::none) || (Driver::last_fault() != 0))
        return false;
    coretx::ui::Framebuffer display{}, expected{};
    fw::SystemInfo info{};
    info.watchdog_fault = 0x0301;
    fw::render_system(info, display);
    expected.compact();
    expected.text(fw::SystemLayout::board_name_x, fw::SystemLayout::sender_y, "F03:01");
    for (int y = 8; y < 14; ++y)
        for (int x = 96; x < 128; ++x)
            if (display.pixel_at(x, y) != expected.pixel_at(x, y))
                return false;
    return true;
}

bool errors_stop_feeding() {
    using Test = Fixture<StopFeeding>;
    using Driver = fw::Watchdog<Test::Io>;
    for (unsigned kind = 0; kind < 4; ++kind) {
        Test::reset_registers();
        auto &io = Test::registers;
        io.blocked = kind == 0;
        io.deny_backup = kind == 1;
        io.corrupt_backup = kind == 2;
        const auto initialized = Driver::initialize();
        const auto expected = kind == 0   ? Error::update_timeout
                              : kind == 1 ? Error::backup_access
                              : kind == 2 ? Error::backup_verify
                                          : Error::none;
        if (initialized != expected)
            return false;
        if ((io.cell(reg::power_control) & PWR_CR_DBP) != 0)
            return false;
        if (kind == 3) {
            io.blocked = true;
            if (Driver::start_runtime(coretx::TimestampUs{}) != Error::update_timeout)
                return false;
        }
        const auto feeds = io.feeds;
        if ((Driver::service(coretx::TimestampUs{1'000'000}) == Error::none) ||
            (io.feeds != feeds) || io.early_feed)
            return false;
    }
    return true;
}

bool backup_and_window() {
    using Test = Fixture<BackupWindow>;
    using Driver = fw::Watchdog<Test::Io>;
    Test::reset_registers();
    auto &io = Test::registers;
    io.reset(RCC_CSR_IWDGRSTF);
    io.cell(reg::power_control) = PWR_CR_DBP;
    io.cell(reg::window) = 1;
    if ((Driver::initialize() != Error::none) || (Driver::resets() != 1) ||
        ((io.cell(reg::power_control) & PWR_CR_DBP) == 0) ||
        (io.cell(reg::window) != IWDG_WINR_WIN) || io.early_feed)
        return false;
    const auto feeds = io.feeds;
    return (Driver::service(coretx::TimestampUs{1'000'000}) == Error::not_initialized) &&
           (io.feeds == feeds);
}

bool independent_watchdogs() {
    using Healthy = Fixture<HealthyWatchdog>;
    using Failed = Fixture<FailedWatchdog>;
    using Driver = fw::Watchdog<Healthy::Io>;
    using Broken = fw::Watchdog<Failed::Io>;
    Healthy::reset_registers();
    Failed::reset_registers();
    Failed::registers.blocked = true;
    if ((Driver::initialize() != Error::none) ||
        (Driver::start_runtime(coretx::TimestampUs{}) != Error::none) ||
        (Broken::initialize() != Error::update_timeout))
        return false;
    const auto feeds = Healthy::registers.feeds;
    const auto broken_feeds = Failed::registers.feeds;
    return (Driver::service(coretx::TimestampUs{50000}) == Error::none) &&
           (Healthy::registers.feeds == (feeds + 1)) &&
           (Broken::service(coretx::TimestampUs{50000}) == Error::update_timeout) &&
           (Failed::registers.feeds == broken_feeds) && (Driver::error() == Error::none);
}

constexpr bool counter_rendering() {
    coretx::ui::Framebuffer display{};
    coretx::ui::Framebuffer expected{};
    fw::SystemInfo system{};
    system.watchdog_resets = 0xFFFF'FFFFU;
    fw::render_system(system, display);
    expected.compact();
    expected.text(0, 55, coretx::ui::tr::watchdog_resets);
    expected.text(87, 55, "4294967295");
    for (int y = 55; y < 61; ++y)
        for (int x = 0; x < 128; ++x)
            if (display.pixel_at(x, y) != expected.pixel_at(x, y))
                return false;
    fw::i6s::render_touch(display, {}, false, false, fw::i6s::TouchError::timeout, fw::Key::none, 0,
                          0xFFFF'FFFFU);
    expected.clear();
    expected.compact(false);
    expected.text(0, 9, coretx::ui::tr::watchdog_resets);
    expected.text(67, 9, "4294967295");
    for (int y = 9; y < 16; ++y)
        for (int x = 0; x < 128; ++x)
            if (display.pixel_at(x, y) != expected.pixel_at(x, y))
                return false;
    return true;
}

static_assert(counter_rendering());

} // namespace

int main() {
    if (!reset_history() || !startup_and_refresh() || !errors_stop_feeding() ||
        !backup_and_window() || !independent_watchdogs() || !counter_rendering() ||
        !fault_history()) {
        std::fputs("Watchdog tests failed\n", stderr);
        return 1;
    }
}
