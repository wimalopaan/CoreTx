// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "platform/stm32f072/platform.hpp"

#include <limits>

namespace coretx::firmware {

enum class WatchdogError : std::uint8_t {
    none,
    not_initialized,
    update_timeout,
    backup_access,
    backup_verify
};

namespace watchdog_registers {
inline constexpr auto key = IWDG_BASE + offsetof(IWDG_TypeDef, KR);
inline constexpr auto prescaler = IWDG_BASE + offsetof(IWDG_TypeDef, PR);
inline constexpr auto reload = IWDG_BASE + offsetof(IWDG_TypeDef, RLR);
inline constexpr auto status = IWDG_BASE + offsetof(IWDG_TypeDef, SR);
inline constexpr auto window = IWDG_BASE + offsetof(IWDG_TypeDef, WINR);
inline constexpr auto reset_flags = RCC_BASE + offsetof(RCC_TypeDef, CSR);
inline constexpr auto power_control = PWR_BASE + offsetof(PWR_TypeDef, CR);
inline constexpr auto backup_magic = RTC_BASE + offsetof(RTC_TypeDef, BKP0R);
inline constexpr auto backup_count = RTC_BASE + offsetof(RTC_TypeDef, BKP1R);
inline constexpr auto backup_fault = RTC_BASE + offsetof(RTC_TypeDef, BKP2R);
inline constexpr auto backup_fault_magic = RTC_BASE + offsetof(RTC_TypeDef, BKP3R);
} // namespace watchdog_registers

namespace detail {

template <typename Io> class WatchdogState {
  public:
    inline static constexpr auto refresh_interval = 50_ms;
    // At nominal 40 kHz LSI with /64: 2 s startup, 500.8 ms runtime.
    inline static constexpr std::uint32_t startup_reload = 1'249;
    inline static constexpr std::uint32_t runtime_reload = 312;
    [[nodiscard]] constexpr WatchdogError
    initialize(const std::uint32_t attempts = stm32f072::ready_timeout) noexcept {
        *this = {};
        namespace reg = stm32f072::registers;
        Io::write(reg::rcc_apb2enr, Io::read(reg::rcc_apb2enr) | RCC_APB2ENR_DBGMCUEN);
        (void)Io::read(reg::rcc_apb2enr);
        Io::write(reg::dbg_apb1fz, Io::read(reg::dbg_apb1fz) | DBGMCU_APB1_FZ_DBG_IWDG_STOP);
        // Starting the IWDG also starts LSI; it cannot be stopped until reset.
        Io::write(watchdog_registers::key, 0xCCCC);
        if (!configure(startup_reload, attempts)) {
            return fail(WatchdogError::update_timeout);
        }
        if (!record_reset(attempts)) {
            return error_;
        }
        initialized_ = true;
        error_ = WatchdogError::none;
        return error_;
    }
    [[nodiscard]] constexpr WatchdogError
    start_runtime(const TimestampUs now,
                  const std::uint32_t attempts = stm32f072::ready_timeout) noexcept {
        if (!initialized_ || (error_ != WatchdogError::none)) {
            return error_;
        }
        if (!configure(runtime_reload, attempts)) {
            return fail(WatchdogError::update_timeout);
        }
        refreshed_at_ = now;
        runtime_ = true;
        return error_;
    }
    [[nodiscard]] constexpr WatchdogError service(const TimestampUs now) noexcept {
        if (error_ != WatchdogError::none) {
            return error_;
        }
        if (!runtime_) {
            return fail(WatchdogError::not_initialized);
        }
        if ((now - refreshed_at_) >= refresh_interval) {
            Io::write(watchdog_registers::key, 0xAAAA);
            refreshed_at_ = now;
        }
        return error_;
    }

    [[nodiscard]] constexpr std::uint32_t resets() const noexcept { return resets_; }
    [[nodiscard]] constexpr std::uint32_t last_fault() const noexcept { return last_fault_; }
    static void mark(const std::uint8_t stage) noexcept {
        if ((stage == 0) && (Io::read(watchdog_registers::backup_fault) == 0))
            return;
        const auto power = Io::read(watchdog_registers::power_control);
        Io::write(watchdog_registers::power_control, power | PWR_CR_DBP);
        (void)Io::read(watchdog_registers::power_control);
        Io::write(watchdog_registers::backup_fault, stage);
        Io::write(watchdog_registers::power_control, power);
    }
    static void record_failure(const std::uint8_t failure) noexcept {
        const auto stage = Io::read(watchdog_registers::backup_fault) & 0xFFU;
        const auto power = Io::read(watchdog_registers::power_control);
        Io::write(watchdog_registers::power_control, power | PWR_CR_DBP);
        (void)Io::read(watchdog_registers::power_control);
        Io::write(watchdog_registers::backup_fault,
                  (static_cast<std::uint32_t>(failure) << 8) | stage);
        Io::write(watchdog_registers::power_control, power);
    }
    [[nodiscard]] constexpr WatchdogError error() const noexcept { return error_; }

  private:
    [[nodiscard]] constexpr bool ready(const std::uint32_t attempts) noexcept {
        return stm32f072::wait_for<Io>(watchdog_registers::status,
                                       IWDG_SR_PVU | IWDG_SR_RVU | IWDG_SR_WVU, 0, attempts);
    }
    [[nodiscard]] constexpr bool configure(const std::uint32_t reload_value,
                                           const std::uint32_t attempts) noexcept {
        using namespace watchdog_registers;
        Io::write(key, 0x5555);
        Io::write(prescaler, IWDG_PR_PR_2);
        Io::write(reload, reload_value);
        if (!ready(attempts)) {
            return false;
        }
        if (Io::read(window) != IWDG_WINR_WIN) {
            Io::write(window, IWDG_WINR_WIN);
            if (!ready(attempts)) {
                return false;
            }
        }
        Io::write(key, 0xAAAA);
        Io::write(key, 0);
        return true;
    }
    [[nodiscard]] constexpr bool record_reset(const std::uint32_t attempts) noexcept {
        using namespace watchdog_registers;
        namespace reg = stm32f072::registers;
        const auto flags = Io::read(reset_flags);
        Io::write(reg::rcc_apb1enr, Io::read(reg::rcc_apb1enr) | RCC_APB1ENR_PWREN);
        (void)Io::read(reg::rcc_apb1enr);
        const auto power = Io::read(power_control);
        Io::write(power_control, power | PWR_CR_DBP);
        if (!stm32f072::wait_for<Io>(power_control, PWR_CR_DBP, PWR_CR_DBP, attempts)) {
            Io::write(power_control, power);
            (void)fail(WatchdogError::backup_access);
            return false;
        }
        // Backup registers survive MCU resets, including when VBAT outlives VDD.
        constexpr std::uint32_t magic = 0x57444731;
        constexpr std::uint32_t fault_magic = 0x464C5431;
        const bool power_on = (flags & RCC_CSR_PORRSTF) != 0;
        if (!power_on && (Io::read(backup_magic) == magic)) {
            resets_ = Io::read(backup_count);
            if (((flags & RCC_CSR_IWDGRSTF) != 0) && (Io::read(backup_fault_magic) == fault_magic))
                last_fault_ = Io::read(backup_fault);
        }
        if (!power_on && ((flags & RCC_CSR_IWDGRSTF) != 0) &&
            (resets_ != std::numeric_limits<std::uint32_t>::max())) {
            ++resets_;
        }
        Io::write(backup_count, resets_);
        Io::write(backup_magic, magic);
        Io::write(backup_fault, 0);
        Io::write(backup_fault_magic, fault_magic);
        const bool verified =
            (Io::read(backup_count) == resets_) && (Io::read(backup_magic) == magic);
        if ((power & PWR_CR_DBP) == 0) {
            Io::write(power_control, Io::read(power_control) & ~std::uint32_t{PWR_CR_DBP});
        }
        if (!verified) {
            (void)fail(WatchdogError::backup_verify);
            return false;
        }
        Io::write(reset_flags, Io::read(reset_flags) | RCC_CSR_RMVF);
        return true;
    }

    constexpr WatchdogError fail(const WatchdogError error) noexcept {
        error_ = error;
        return error_;
    }

    TimestampUs refreshed_at_{};
    std::uint32_t resets_{};
    std::uint32_t last_fault_{};
    WatchdogError error_{WatchdogError::not_initialized};
    bool initialized_{};
    bool runtime_{};
};

} // namespace detail

template <typename Io> class Watchdog {
  public:
    Watchdog() = delete;

    [[nodiscard]] static WatchdogError initialize() noexcept { return state_.initialize(); }
    [[nodiscard]] static WatchdogError start_runtime(const TimestampUs now) noexcept {
        return state_.start_runtime(now);
    }
    [[nodiscard]] static WatchdogError service(const TimestampUs now) noexcept {
        return state_.service(now);
    }
    [[nodiscard]] static std::uint32_t resets() noexcept { return state_.resets(); }
    [[nodiscard]] static std::uint32_t last_fault() noexcept { return state_.last_fault(); }
    static void mark(const std::uint8_t stage) noexcept { state_.mark(stage); }
    static void record_failure(const std::uint8_t failure) noexcept {
        state_.record_failure(failure);
    }
    [[nodiscard]] static WatchdogError error() noexcept { return state_.error(); }

  private:
    inline static constinit detail::WatchdogState<Io> state_{};
};

} // namespace coretx::firmware
