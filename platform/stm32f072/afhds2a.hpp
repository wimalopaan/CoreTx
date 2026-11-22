// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "coretx/radio/afhds2a.hpp"
#include "platform/stm32f072/a7105.hpp"

namespace coretx::firmware {

enum class Afhds2aError : std::uint8_t {
    none,
    identity,
    bus,
    calibration,
    preparation_timeout,
    tx_timeout,
    bind_timeout
};

namespace detail {
struct A7105RegisterSetting {
    std::uint8_t reg;
    std::uint8_t value;
};

// AFHDS 2A 500 kbit/s GFSK profile with the OpenI6X GIO/preamble settings.
namespace afhds2a_config {
inline constexpr A7105RegisterSetting mode_control{a7105::mode_control, 0xE2};
inline constexpr A7105RegisterSetting calibration{a7105::calibration, 0x00};
inline constexpr A7105RegisterSetting fifo_i{a7105::fifo_i, afhds2a::tx_size - 1};
inline constexpr A7105RegisterSetting fifo_ii{a7105::fifo_ii, 0x00};
inline constexpr A7105RegisterSetting coretx_osc_i{a7105::coretx_osc_i, 0x00};
inline constexpr A7105RegisterSetting coretx_osc_ii{a7105::coretx_osc_ii, 0x00};
inline constexpr A7105RegisterSetting coretx_osc_iii{a7105::coretx_osc_iii, 0x00};
inline constexpr A7105RegisterSetting cko_pin{a7105::cko_pin, 0x00};
inline constexpr A7105RegisterSetting gpio1{a7105::gpio1, a7105::gpio1_sdo};
inline constexpr A7105RegisterSetting gpio2{a7105::gpio2, a7105::gpio2_wtr};
inline constexpr A7105RegisterSetting clock{a7105::clock, 0x05};
inline constexpr A7105RegisterSetting data_rate{a7105::data_rate, 0x00};
inline constexpr A7105RegisterSetting pll_i{a7105::pll_i, 0x50};
inline constexpr A7105RegisterSetting pll_ii{a7105::pll_ii, a7105::signature};
inline constexpr A7105RegisterSetting pll_iii{a7105::pll_iii, 0x4B};
inline constexpr A7105RegisterSetting pll_iv{a7105::pll_iv, 0x00};
inline constexpr A7105RegisterSetting pll_v{a7105::pll_v, 0x02};
inline constexpr A7105RegisterSetting tx_i{a7105::tx_i, 0x16};
inline constexpr A7105RegisterSetting tx_ii{a7105::tx_ii, 0x2B};
inline constexpr A7105RegisterSetting delay_i{a7105::delay_i, 0x12};
inline constexpr A7105RegisterSetting delay_ii{a7105::delay_ii, 0x4F};
inline constexpr A7105RegisterSetting rx{a7105::rx, 0x62};
inline constexpr A7105RegisterSetting rx_gain_i{a7105::rx_gain_i, 0x80};
inline constexpr A7105RegisterSetting rx_gain_iv{a7105::rx_gain_iv, 0x2A};
inline constexpr A7105RegisterSetting rssi_threshold{a7105::rssi_threshold, 0x32};
inline constexpr A7105RegisterSetting adc{a7105::adc, 0xC3};
inline constexpr A7105RegisterSetting code_i{a7105::code_i, 0x1E};
inline constexpr A7105RegisterSetting code_ii{a7105::code_ii, 0x1E};
inline constexpr A7105RegisterSetting if_calibration_i{a7105::if_calibration_i, 0x00};
inline constexpr A7105RegisterSetting vco_current_calibration{a7105::vco_current_calibration, 0x00};
inline constexpr A7105RegisterSetting vco_sbcal_i{a7105::vco_sbcal_i, 0x00};
inline constexpr A7105RegisterSetting vco_sbcal_ii{a7105::vco_sbcal_ii, 0x3B};
inline constexpr A7105RegisterSetting battery_detect{a7105::battery_detect, 0x00};
inline constexpr A7105RegisterSetting tx_test_minimum_power{a7105::tx_test, 0x00};
inline constexpr A7105RegisterSetting rx_demod_test_i{a7105::rx_demod_test_i, 0x47};
inline constexpr A7105RegisterSetting rx_demod_test_ii{a7105::rx_demod_test_ii, 0x80};
inline constexpr A7105RegisterSetting cpc{a7105::cpc, 0x03};
inline constexpr A7105RegisterSetting xtal_test{a7105::xtal_test, 0x01};
inline constexpr A7105RegisterSetting pll_test{a7105::pll_test, 0x45};
inline constexpr A7105RegisterSetting vco_test_i{a7105::vco_test_i, 0x18};
inline constexpr A7105RegisterSetting vco_test_ii{a7105::vco_test_ii, 0x00};
inline constexpr A7105RegisterSetting if_test{a7105::if_test, 0x01};
inline constexpr A7105RegisterSetting rs_scale{a7105::rs_scale, 0x0F};
inline constexpr std::uint8_t skip_register = 0xFF;
inline constexpr auto registers = []() consteval {
    std::array<std::uint8_t, a7105::rs_scale + 1> values{};
    values.fill(skip_register);
    for (const auto setting : std::array{mode_control,
                                         calibration,
                                         fifo_i,
                                         fifo_ii,
                                         coretx_osc_i,
                                         coretx_osc_ii,
                                         coretx_osc_iii,
                                         cko_pin,
                                         gpio1,
                                         gpio2,
                                         clock,
                                         data_rate,
                                         pll_i,
                                         pll_ii,
                                         pll_iii,
                                         pll_iv,
                                         pll_v,
                                         tx_i,
                                         tx_ii,
                                         delay_i,
                                         delay_ii,
                                         rx,
                                         rx_gain_i,
                                         rx_gain_iv,
                                         rssi_threshold,
                                         adc,
                                         code_i,
                                         code_ii,
                                         if_calibration_i,
                                         vco_current_calibration,
                                         vco_sbcal_i,
                                         vco_sbcal_ii,
                                         battery_detect,
                                         tx_test_minimum_power,
                                         rx_demod_test_i,
                                         rx_demod_test_ii,
                                         cpc,
                                         xtal_test,
                                         pll_test,
                                         vco_test_i,
                                         vco_test_ii,
                                         if_test,
                                         rs_scale})
        values[setting.reg] = setting.value;
    return values;
}();
inline constexpr std::uint8_t calibrated_vco_current = 0x13;
inline constexpr std::uint8_t calibrated_vco_sbcal = 0x0A;
inline constexpr std::uint8_t vco_channel_low = 0x00;
inline constexpr std::uint8_t vco_channel_high = 0xA0;
inline constexpr auto calibration_timeout = 5_ms;
// MultiProtocol AFHDS2A_WRITE_TIME on STM32 plus its bounded TX-completion wait.
inline constexpr auto transmission_write_window = 1'550_us;
inline constexpr auto transmission_poll_window = 700_us;
inline constexpr auto transmission_timeout = transmission_write_window + transmission_poll_window;
inline constexpr auto poll_interval = 50_us;
inline constexpr auto fifo_interval = 10_us;
} // namespace afhds2a_config

struct A7105CalibrationStep {
    std::uint8_t command;
    std::uint8_t value;
    std::uint8_t wait_mask{};
    std::uint8_t failure_mask{};
};
inline constexpr std::array calibration_steps{
    A7105CalibrationStep{a7105::calibration, a7105::calibration_if},
    A7105CalibrationStep{a7105::read | a7105::calibration, 0, a7105::calibration_if},
    A7105CalibrationStep{a7105::read | a7105::if_calibration_i, 0, 0, a7105::if_calibration_fail},
    A7105CalibrationStep{a7105::vco_current_calibration, afhds2a_config::calibrated_vco_current},
    A7105CalibrationStep{a7105::vco_sbcal_ii, afhds2a_config::vco_sbcal_ii.value},
    A7105CalibrationStep{a7105::pll_i, afhds2a_config::vco_channel_low},
    A7105CalibrationStep{a7105::calibration, a7105::calibration_vco},
    A7105CalibrationStep{a7105::read | a7105::calibration, 0, a7105::calibration_vco},
    A7105CalibrationStep{a7105::pll_i, afhds2a_config::vco_channel_high},
    A7105CalibrationStep{a7105::calibration, a7105::calibration_vco},
    A7105CalibrationStep{a7105::read | a7105::calibration, 0, a7105::calibration_vco},
    A7105CalibrationStep{a7105::vco_sbcal_i, afhds2a_config::calibrated_vco_sbcal},
    A7105CalibrationStep{a7105::tx_test, afhds2a_config::tx_test_minimum_power.value}};
} // namespace detail

template <typename Bus> class Afhds2a {
  public:
    constexpr void initialize(const afhds2a::TxId tx, const afhds2a::RxId rx,
                              const TimestampUs now) noexcept {
        *this = {};
        if (!session_.initialize(tx, rx)) {
            fail(Afhds2aError::identity);
            return;
        }
        Bus::initialize(now);
    }
    constexpr void deactivate() noexcept {
        active_ = false;
        reschedule_ = configured_;
        session_.cancel_bind();
        session_.invalidate_telemetry();
        front(A7105FrontEnd::off);
    }
    constexpr void activate() noexcept { active_ = true; }
    constexpr void set_channel_mode(const afhds2a::ChannelMode mode) noexcept {
        session_.set_channel_mode(mode);
    }
    constexpr void gio2_event() noexcept {
        if (stage_ == Stage::tx_poll)
            tx_done_event_ = true;
        if ((stage_ == Stage::rx_poll) || (stage_ == Stage::rx_result))
            rx_done_event_ = true;
    }
    [[nodiscard]] constexpr bool bind(const TimestampUs now) noexcept {
        if (!active_ || !configured_ || (error_ != Afhds2aError::none))
            return false;
        if (session_.binding())
            session_.cancel_bind();
        else
            session_.start_bind(now);
        reschedule_ = true;
        return true;
    }
    constexpr void service(const TimestampUs now, const afhds2a::Channels &channels,
                           const bool valid, const TimestampUs sampled_at) noexcept {
        if (error_ != Afhds2aError::none)
            return;
        const bool allowed =
            active_ && (session_.binding() || (session_.bound() && valid &&
                                               ((now - sampled_at) < afhds2a::input_freshness)));
        if (configured_ && (!allowed || reschedule_))
            front(A7105FrontEnd::off);
        Bus::service(now);
        if (Bus::info().state == A7105State::failed) {
            fail(Afhds2aError::bus);
            return;
        }
        if (session_.bind_expired(now)) {
            session_.cancel_bind();
            fail(Afhds2aError::bind_timeout);
            return;
        }
        if (pending_) {
            if (Bus::busy())
                return;
            if (!Bus::completed()) {
                fail(Afhds2aError::bus);
                return;
            }
            pending_ = false;
            if (configured_ && (!allowed || reschedule_)) {
                const bool was_running = (stage_ != Stage::idle) || reschedule_;
                reschedule_ = false;
                scheduled_ = false;
                stage_ = Stage::idle;
                if (was_running)
                    strobe(a7105::standby, Stage::idle, now);
                return;
            }
            if (stage_ == Stage::calibration_result) {
                const auto step = detail::calibration_steps[index_];
                const auto value = Bus::value();
                if (((value & step.failure_mask) != 0) ||
                    (((value & step.wait_mask) != 0) &&
                     ((now - step_started_) >= detail::afhds2a_config::calibration_timeout))) {
                    fail(Afhds2aError::calibration);
                    return;
                }
                if ((value & step.wait_mask) == 0) {
                    ++index_;
                    step_started_ = now;
                }
                stage_ = Stage::calibration;
            } else if (stage_ == Stage::tx_poll) {
                transmitted_at_ = now;
            } else if (stage_ == Stage::tx_result) {
                if ((Bus::value() & a7105::mode_trer) != 0) {
                    if (tx_poll_after_deadline_) {
                        fail(Afhds2aError::tx_timeout);
                        return;
                    }
                    stage_ = Stage::tx_poll;
                } else {
                    session_.transmitted();
                    front(session_.binding() ? A7105FrontEnd::off : A7105FrontEnd::receive);
                    strobe(a7105::receive, Stage::rx_poll, now);
                    return;
                }
            } else if (stage_ == Stage::rx_result) {
                rx_done_event_ = false;
                const auto mode = Bus::value();
                stage_ = Stage::rx_poll;
                if ((mode & a7105::mode_trer) == 0) {
                    front(A7105FrontEnd::off);
                    // A corrected FEC error is acceptable when the payload CRC passes (MPM).
                    if ((mode & a7105::mode_crc_fail) == 0)
                        strobe(a7105::reset_read_pointer, Stage::read_fifo, now);
                    else
                        strobe(a7105::standby, Stage::idle, now);
                    return;
                }
            } else if (stage_ == Stage::received) {
                afhds2a::Received received{};
                const auto bytes = Bus::payload();
                if (bytes.size() != received.size()) {
                    fail(Afhds2aError::bus);
                    return;
                }
                std::copy(bytes.begin(), bytes.end(), received.begin());
                (void)session_.receive(received, now);
                strobe(a7105::standby, Stage::idle, now);
                return;
            }
        }
        if (configured_ && (!allowed || reschedule_)) {
            const bool was_running = scheduled_ || (stage_ != Stage::idle) || reschedule_;
            reschedule_ = false;
            scheduled_ = false;
            if (was_running) {
                stage_ = Stage::idle;
                strobe(a7105::standby, Stage::idle, now);
            }
            return;
        }
        switch (stage_) {
        case Stage::waiting:
            if (Bus::info().state == A7105State::ready) {
                constexpr std::array<std::uint8_t, 4> sync{
                    static_cast<std::uint8_t>(afhds2a::sync_id >> 24),
                    static_cast<std::uint8_t>(afhds2a::sync_id >> 16),
                    static_cast<std::uint8_t>(afhds2a::sync_id >> 8),
                    static_cast<std::uint8_t>(afhds2a::sync_id)};
                request(a7105::id_data, sync, 0, Stage::configuration, now);
            }
            break;
        case Stage::configuration:
            while ((index_ < detail::afhds2a_config::registers.size()) &&
                   (detail::afhds2a_config::registers[index_] ==
                    detail::afhds2a_config::skip_register))
                ++index_;
            if (index_ < detail::afhds2a_config::registers.size()) {
                const auto reg = static_cast<std::uint8_t>(index_);
                const auto value = detail::afhds2a_config::registers[index_++];
                write(reg, value, Stage::configuration, now);
            } else {
                index_ = 0;
                step_started_ = now;
                strobe(a7105::standby, Stage::calibration, now);
            }
            break;
        case Stage::calibration:
            if (index_ == detail::calibration_steps.size()) {
                configured_ = true;
                strobe(a7105::standby, Stage::idle, now);
            } else {
                const auto step = detail::calibration_steps[index_];
                if ((step.command & a7105::read) != 0)
                    request(step.command, {}, 1, Stage::calibration_result, now);
                else
                    write(step.command, step.value, Stage::calibration_result, now);
            }
            break;
        case Stage::idle:
        case Stage::rx_poll:
            if (!scheduled_ || ((now - frame_at_) >= afhds2a::frame_period)) {
                plan_ = session_.prepare(channels);
                frame_at_ = scheduled_ ? frame_at_ + afhds2a::frame_period : now;
                scheduled_ = true;
                front(A7105FrontEnd::off);
                strobe(a7105::standby, Stage::reset_fifo, now);
            } else if (stage_ == Stage::rx_poll) {
                request(a7105::read | a7105::mode, {}, 1, Stage::rx_result, now);
            }
            break;
        case Stage::reset_fifo:
            strobe(a7105::reset_write_pointer, Stage::fill_fifo, now);
            break;
        case Stage::fill_fifo:
            request(a7105::fifo_data, plan_.packet, 0, Stage::channel, now);
            break;
        case Stage::channel:
            write(a7105::pll_i, plan_.channel, Stage::transmit, now);
            break;
        case Stage::transmit: {
            const auto elapsed = now - frame_at_;
            if (elapsed >= afhds2a::frame_period) {
                fail(Afhds2aError::preparation_timeout);
                return;
            }
            tx_done_event_ = false;
            rx_done_event_ = false;
            front(A7105FrontEnd::transmit);
            strobe(a7105::transmit, Stage::tx_poll, now);
            break;
        }
        case Stage::tx_poll:
            // Idle during settling is not proof that the packet has finished.
            if (!tx_done_event_ &&
                ((now - transmitted_at_) < detail::afhds2a_config::transmission_write_window))
                break;
            tx_done_event_ = false;
            // A stale BUSY sample cannot prove that TX exceeded its deadline.
            tx_poll_after_deadline_ =
                (now - transmitted_at_) >= detail::afhds2a_config::transmission_timeout;
            request(a7105::read | a7105::mode, {}, 1, Stage::tx_result, now);
            break;
        case Stage::read_fifo:
            request(a7105::read | a7105::fifo_data, {}, afhds2a::rx_size, Stage::received, now);
            break;
        case Stage::calibration_result:
        case Stage::tx_result:
        case Stage::rx_result:
        case Stage::received:
            break;
        }
    }
    [[nodiscard]] constexpr Afhds2aError error() const noexcept { return error_; }
    [[nodiscard]] constexpr bool ready() const noexcept { return configured_; }
    [[nodiscard]] constexpr DurationUs service_interval() const noexcept {
        return (error_ == Afhds2aError::none) && pending_ &&
                       ((stage_ != Stage::rx_result) || rx_done_event_)
                   ? detail::afhds2a_config::fifo_interval
                   : detail::afhds2a_config::poll_interval;
    }
    [[nodiscard]] constexpr const afhds2a::Session &session() const noexcept { return session_; }
    [[nodiscard]] constexpr afhds2a::ReceiverTelemetry telemetry() const noexcept {
        return session_.telemetry();
    }
    [[nodiscard]] constexpr bool take_receiver_changed() noexcept {
        return session_.take_receiver_changed();
    }

  private:
    enum class Stage : std::uint8_t {
        waiting,
        configuration,
        calibration,
        calibration_result,
        idle,
        reset_fifo,
        fill_fifo,
        channel,
        transmit,
        tx_poll,
        tx_result,
        rx_poll,
        rx_result,
        read_fifo,
        received
    };
    constexpr void front(const A7105FrontEnd mode) noexcept {
        if (front_ != mode) {
            Bus::front_end(mode);
            front_ = mode;
        }
    }
    constexpr void fail(const Afhds2aError error) noexcept {
        error_ = error;
        Bus::power_off();
        front_ = A7105FrontEnd::off;
    }
    constexpr void request(const std::uint8_t command, const std::span<const std::uint8_t> bytes,
                           const std::uint8_t read_count, const Stage next,
                           const TimestampUs now) noexcept {
        if (!Bus::command(command, bytes, read_count, now)) {
            fail(Afhds2aError::bus);
            return;
        }
        pending_ = true;
        stage_ = next;
    }
    constexpr void write(const std::uint8_t reg, const std::uint8_t value, const Stage next,
                         const TimestampUs now) noexcept {
        const std::array bytes{value};
        request(reg, bytes, 0, next, now);
    }
    constexpr void strobe(const std::uint8_t command, const Stage next,
                          const TimestampUs now) noexcept {
        request(command, {}, 0, next, now);
    }
    afhds2a::Session session_{};
    afhds2a::Plan plan_{};
    TimestampUs frame_at_{};
    TimestampUs transmitted_at_{};
    TimestampUs step_started_{};
    std::size_t index_{};
    Stage stage_{};
    Afhds2aError error_{};
    A7105FrontEnd front_{};
    bool pending_{};
    bool configured_{};
    bool active_{};
    bool scheduled_{};
    bool reschedule_{};
    bool tx_poll_after_deadline_{};
    bool tx_done_event_{};
    bool rx_done_event_{};
};

} // namespace coretx::firmware
