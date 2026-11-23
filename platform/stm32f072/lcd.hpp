// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "coretx/ui/ui.hpp"
#include "platform/stm32f072/gpio.hpp"
#include "platform/stm32f072/peripherals.hpp"

namespace coretx::firmware {

enum class LcdError : std::uint8_t { none, not_initialized, frame_busy };

namespace detail {

template <typename Io, typename Assignment> class LcdState {
  public:
    [[nodiscard]] constexpr LcdError initialize() noexcept {
        using namespace stm32f072::registers;
        *this = {};
        Io::write(rcc_ahbenr, Io::read(rcc_ahbenr) | gpio_clock({Assignment::lcd.data_port, 0}) |
                                  gpio_clocks(std::array{rs, reset, rw, select, enable, backlight,
                                                         brightness}));
        (void)Io::read(rcc_ahbenr);
        for (std::uint8_t i = 0; i < 8; ++i) {
            const Pin pin{Assignment::lcd.data_port, i};
            drive<Io>(pin, false);
            configure<Io>(pin, false);
        }
        for (const auto &pin : {rs, reset, rw, select, enable, backlight, brightness}) {
            drive<Io>(pin, false);
            configure<Io>(pin, false);
        }
        drive<Io>(backlight, true);
        drive<Io>(brightness, true);
        error_ = LcdError::none;
        since_ = Io::now_us();
        return error_;
    }

    constexpr void begin_frame() noexcept {
        if (frame_pending_) {
            error_ = LcdError::frame_busy;
            return;
        }
        page_ = 0;
        column_ = 0;
        prefix_ = 0;
        frame_pending_ = true;
    }
    [[nodiscard]] constexpr LcdError service(const coretx::ui::Framebuffer &display) noexcept {
        if (error_ != LcdError::none)
            return error_;
        const auto started = Io::now_us();
        for (unsigned step = 0; step < service_step_limit; ++step) {
            if ((advance(display) != LcdError::none) || !busy())
                break;
            if ((bus_ == Bus::idle) && (stage_ != Stage::ready))
                break;
            if ((Io::now_us() - started) >= service_budget)
                break;
        }
        return error_;
    }

  private:
    [[nodiscard]] constexpr LcdError advance(const coretx::ui::Framebuffer &display) noexcept {
        if (error_ != LcdError::none) {
            return error_;
        }
        const auto now = Io::now_us();
        if (bus_ != Bus::idle) {
            // Two rounded microsecond ticks guarantee at least one real microsecond.
            if ((now - since_) < 2_us) {
                return error_;
            }
            switch (bus_) {
            case Bus::setup:
                drive<Io>(enable, true);
                bus_ = Bus::high;
                break;
            case Bus::high:
                drive<Io>(enable, false);
                bus_ = Bus::hold;
                break;
            case Bus::hold:
                bus_ = Bus::idle;
                complete_byte();
                break;
            case Bus::idle:
                break;
            }
            since_ = Io::now_us();
            return error_;
        }
        switch (stage_) {
        case Stage::reset_low:
            if ((now - since_) >= 1'001_us) {
                drive<Io>(reset, true);
                since_ = Io::now_us();
                stage_ = Stage::reset_high;
            }
            break;
        case Stage::reset_high:
            if ((now - since_) >= 1'001_us) {
                start_byte(software_reset, false);
                stage_ = Stage::software_reset;
            }
            break;
        case Stage::software_reset:
            break;
        case Stage::software_wait:
            if ((now - since_) >= 10'001_us) {
                stage_ = Stage::commands;
            }
            break;
        case Stage::commands:
            start_byte(initialization[initialization_index_], false);
            break;
        case Stage::power_wait:
            if ((now - since_) >= 20'001_us) {
                stage_ = Stage::ready;
            }
            break;
        case Stage::ready:
            if (frame_pending_) {
                if (prefix_ < 3) {
                    const auto command = prefix_ == 0
                                             ? static_cast<std::uint8_t>(page_start | page_)
                                         : prefix_ == 1 ? column_low
                                                        : column_high;
                    start_byte(command, false);
                } else {
                    start_byte(display.bytes()[page_ * coretx::ui::Framebuffer::width + column_],
                               true);
                }
            }
            break;
        case Stage::display_on:
            start_byte(display_on, false);
            break;
        }
        return error_;
    }

  public:
    [[nodiscard]] constexpr bool busy() const noexcept {
        return (stage_ != Stage::ready) || frame_pending_;
    }
    [[nodiscard]] constexpr LcdError error() const noexcept { return error_; }

  private:
    inline static constexpr auto service_budget = 100_us;
    // Also bound work when the timebase is stopped.
    inline static constexpr unsigned service_step_limit = 128;
    enum class Bus : std::uint8_t { idle, setup, high, hold };
    enum class Stage : std::uint8_t {
        reset_low,
        reset_high,
        software_reset,
        software_wait,
        commands,
        power_wait,
        ready,
        display_on
    };
    inline static constexpr auto rs = Assignment::lcd.rs;
    inline static constexpr auto reset = Assignment::lcd.reset;
    inline static constexpr auto rw = Assignment::lcd.rw;
    inline static constexpr auto select = Assignment::lcd.select;
    inline static constexpr auto enable = Assignment::lcd.enable;
    inline static constexpr auto backlight = Assignment::lcd.backlight;
    inline static constexpr auto brightness = Assignment::lcd.brightness;
    inline static constexpr std::uint8_t software_reset = 0xE2;
    inline static constexpr std::uint8_t display_off = 0xAE;
    inline static constexpr std::uint8_t display_on = 0xAF;
    inline static constexpr std::uint8_t ram_display = 0xA4;
    inline static constexpr std::uint8_t bias_1_7 = 0xA3;
    inline static constexpr std::uint8_t com_normal = 0xC0;
    inline static constexpr std::uint8_t seg_inverse = 0xA1;
    inline static constexpr std::uint8_t power_all_on = 0x2F;
    inline static constexpr std::uint8_t regulator_ratio = 0x23;
    inline static constexpr std::uint8_t electronic_volume = 0x81;
    inline static constexpr std::uint8_t contrast = 37;
    inline static constexpr std::uint8_t start_line = 0x40;
    inline static constexpr std::uint8_t page_start = 0xB0;
    inline static constexpr std::uint8_t column_low = 0x04;
    inline static constexpr std::uint8_t column_high = 0x10;
    inline static constexpr std::array initialization{
        display_off,     ram_display,       bias_1_7, com_normal, seg_inverse, power_all_on,
        regulator_ratio, electronic_volume, contrast, start_line, page_start};

    constexpr void complete_byte() noexcept {
        switch (stage_) {
        case Stage::software_reset:
            stage_ = Stage::software_wait;
            break;
        case Stage::commands:
            if (++initialization_index_ == initialization.size()) {
                stage_ = Stage::power_wait;
            }
            break;
        case Stage::ready:
            if (prefix_ < 3) {
                ++prefix_;
            } else if (++column_ == coretx::ui::Framebuffer::width) {
                column_ = 0;
                prefix_ = 0;
                if (++page_ == (coretx::ui::Framebuffer::height / 8)) {
                    if (visible_) {
                        frame_pending_ = false;
                    } else {
                        stage_ = Stage::display_on;
                    }
                }
            }
            break;
        case Stage::display_on:
            visible_ = true;
            frame_pending_ = false;
            stage_ = Stage::ready;
            break;
        default:
            break;
        }
    }
    constexpr void start_byte(const std::uint8_t byte, const bool data) noexcept {
        drive<Io>(rs, data);
        constexpr std::uint32_t data_mask = GPIO_BSRR_BS_0 | GPIO_BSRR_BS_1 | GPIO_BSRR_BS_2 |
                                            GPIO_BSRR_BS_3 | GPIO_BSRR_BS_4 | GPIO_BSRR_BS_5 |
                                            GPIO_BSRR_BS_6 | GPIO_BSRR_BS_7;
        Io::write(Assignment::lcd.data_port + offsetof(GPIO_TypeDef, BSRR),
                  ((data_mask & ~std::uint32_t{byte}) << bsrr_reset_shift) | byte);
        since_ = Io::now_us();
        bus_ = Bus::setup;
    }

    std::size_t page_{};
    std::size_t column_{};
    std::uint8_t prefix_{};
    std::size_t initialization_index_{};
    TimestampUs since_{};
    LcdError error_{LcdError::not_initialized};
    Stage stage_{};
    Bus bus_{};
    bool frame_pending_{};
    bool visible_{};
};

} // namespace detail

template <typename Io, typename Assignment> class Lcd {
  public:
    Lcd() = delete;

    [[nodiscard]] static LcdError initialize() noexcept { return state_.initialize(); }
    static void begin_frame() noexcept { state_.begin_frame(); }
    [[nodiscard]] static LcdError service(const coretx::ui::Framebuffer &display) noexcept {
        return state_.service(display);
    }
    [[nodiscard]] static bool busy() noexcept { return state_.busy(); }
    [[nodiscard]] static LcdError error() noexcept { return state_.error(); }

  private:
    inline static constinit detail::LcdState<Io, Assignment> state_{};
};

} // namespace coretx::firmware
