// SPDX-License-Identifier: GPL-2.0-or-later
#include "boards/common/application.hpp"
#include "platform/stm32f072/crsf.hpp"
#include "static_fixture.hpp"

#include <cstdio>
#include <cstdlib>

namespace {

namespace s = coretx::firmware::i6s;
namespace x = coretx::firmware::i6x;
using coretx::firmware::Key;
using coretx::firmware::LcdError;
using coretx::firmware::stm32f072::HardwareError;

struct Context {
    std::uint32_t now{};
    HardwareError hardware_error{};
    LcdError lcd_init_error{};
    LcdError lcd_error{};
    bool stack_ok{true};
    bool break_stack_on_display{};
    bool busy{};
    bool finish_frame{true};
    unsigned display_initializations{};
    unsigned display_calls{};
    unsigned draws{};
    std::array<std::uint8_t, 1024> display_bytes{};
    unsigned touch_initializations{};
    unsigned touch_calls{};
    bool touch_ready{};
    s::TouchError touch_error{};
    s::TouchEvent touch_event{};
    s::TouchSample touch_sample{};
    x::AdcError adc_error{};
    x::ScanEvent adc_event{x::ScanEvent::pending};
    std::array<std::uint16_t, x::sender_inputs.size()> samples{};
    Key key{};
    bool ok_held{};
    std::uint8_t trim_buttons{};
    unsigned adc_calls{};
    std::uint32_t adc_reconfigure_delay{};
    std::uint32_t adc_started_at{};
    bool adc_enabling{};
    unsigned key_calls{};
    unsigned probe_calls{};
    unsigned tx_calls{};
    unsigned baud_changes{};
    unsigned radio_initializations{};
    unsigned radio_calls{};
    bool powered{};
    bool internal_active{};
    bool internal_binding{};
    bool receiver_changed{};
    bool tx_valid{};
    bool extra_analog{};
    bool extra_digital{};
    std::uint8_t digital_pressed{};
    unsigned center_clicks{};
    std::uint32_t sampled_at{};
    coretx::afhds2a::Channels radio_channels{};
    coretx::TimestampUs radio_sampled_at{};
    bool radio_valid{};
    x::StorageProbeState probe_state{x::StorageProbeState::not_found};
    coretx::crsf::Telemetry telemetry{};
    coretx::crsf::config::Client crsf_config{};
    coretx::crsf::Channels tx_channels{};
    std::array<std::uint8_t, 4> multiswitch_states{};
    coretx::afhds2a::ChannelMode internal_mode{};
    std::array<std::uint8_t, 16384> eeprom{};
    std::size_t baud_index{1};
};

template <typename Test> struct Display {
    static LcdError initialize() {
        auto &c = Test::context;
        ++c.display_initializations;
        return c.lcd_init_error;
    }
    static LcdError service(const coretx::ui::Framebuffer &display) {
        auto &c = Test::context;
        c.display_bytes = display.bytes();
        ++c.display_calls;
        if (c.finish_frame)
            c.busy = false;
        if (c.break_stack_on_display)
            c.stack_ok = false;
        return c.lcd_error;
    }
    static bool busy() {
        auto &c = Test::context;
        return c.busy;
    }
    static void begin_frame() {
        auto &c = Test::context;
        ++c.draws;
        c.busy = true;
    }
};

template <typename Test> struct InputHardware {
    static void initialize() {}
    static void configure(bool enabled) {
        auto &c = Test::context;
        c.extra_digital = enabled;
    }
    static std::uint8_t read() {
        auto &c = Test::context;
        return c.digital_pressed;
    }
    static void service(coretx::TimestampUs, bool click) {
        auto &c = Test::context;
        if (click)
            ++c.center_clicks;
    }
    static void stop() {}
};

template <typename Test> struct Platform {
    inline static constexpr std::string_view name = "I6X";
    inline static constexpr bool sb_three_position = false;
    using Inputs = InputHardware<Test>;
    struct Radio {
        static void initialize(coretx::TimestampUs) {
            auto &c = Test::context;
            ++c.radio_initializations;
        }
        static void service(coretx::TimestampUs) {
            auto &c = Test::context;
            ++c.radio_calls;
        }
        static coretx::firmware::A7105Info info() {
            return {coretx::firmware::A7105State::ready, coretx::firmware::A7105Error::none};
        }
    };
    struct Watchdog {
        static std::uint32_t resets() { return 7; }
        static std::uint32_t last_fault() { return 0; }
        static void mark(std::uint8_t) {}
    };
    class Internal {
      public:
        constexpr void initialize(coretx::afhds2a::TxId tx, coretx::afhds2a::RxId rx,
                                  coretx::TimestampUs now) {
            if (!session_.initialize(tx, rx))
                std::abort();
            Radio::initialize(now);
        }
        constexpr void activate() { active_ = true; }
        constexpr void set_channel_mode(coretx::afhds2a::ChannelMode mode) {
            session_.set_channel_mode(mode);
        }
        constexpr void deactivate() {
            auto &c = Test::context;
            active_ = false;
            c.internal_active = false;
            session_.cancel_bind();
        }
        constexpr void service(coretx::TimestampUs now, const coretx::afhds2a::Channels &channels,
                               bool valid, coretx::TimestampUs sampled_at) {
            auto &c = Test::context;
            c.radio_channels = channels;
            c.radio_valid = valid;
            c.radio_sampled_at = sampled_at;
            if (active_ && c.powered)
                std::abort();
            c.internal_active = active_;
            c.internal_binding = session_.binding();
            c.internal_mode = session_.channel_mode();
            receiver_changed_ = c.receiver_changed;
            c.receiver_changed = false;
            Radio::service(now);
        }
        constexpr bool bind(coretx::TimestampUs now) {
            if (!active_)
                return false;
            if (session_.binding())
                session_.cancel_bind();
            else
                session_.start_bind(now);
            return true;
        }
        constexpr bool ready() const { return true; }
        constexpr coretx::firmware::Afhds2aError error() const { return {}; }
        constexpr const coretx::afhds2a::Session &session() const { return session_; }
        constexpr coretx::afhds2a::ReceiverTelemetry telemetry() const {
            return session_.telemetry();
        }
        constexpr bool take_receiver_changed() {
            const bool changed = receiver_changed_;
            receiver_changed_ = false;
            return changed;
        }

      private:
        coretx::afhds2a::Session session_{};
        bool active_{};
        bool receiver_changed_{};
    };
    static coretx::afhds2a::TxId transmitter_id() { return {0x12345678}; }
    static HardwareError initialize() {
        auto &c = Test::context;
        return c.hardware_error;
    }
    static coretx::TimestampUs now_us() {
        auto &c = Test::context;
        return coretx::TimestampUs{c.now};
    }
    static bool stack_intact() {
        auto &c = Test::context;
        return c.stack_ok;
    }
    static void power_off() {
        auto &c = Test::context;
        c.powered = c.internal_active = false;
    }
};

template <typename Test> struct Touch {
    static void initialize() {
        auto &c = Test::context;
        ++c.touch_initializations;
    }
    static s::TouchEvent service(coretx::TimestampUs) {
        auto &c = Test::context;
        ++c.touch_calls;
        return c.touch_event;
    }
    static bool ready() {
        auto &c = Test::context;
        return c.touch_ready;
    }
    static s::TouchError error() {
        auto &c = Test::context;
        return c.touch_error;
    }
    static const s::TouchSample &sample() {
        auto &c = Test::context;
        return c.touch_sample;
    }
};

template <typename Test> struct Adc {
    static x::AdcError initialize() {
        auto &c = Test::context;
        return c.adc_error;
    }
    static x::AdcError error() {
        auto &c = Test::context;
        return c.adc_error;
    }
    static x::ScanEvent service(coretx::TimestampUs now) {
        auto &c = Test::context;
        ++c.adc_calls;
        if (c.adc_enabling) {
            const auto elapsed = now - coretx::TimestampUs{c.adc_started_at};
            if (elapsed >= coretx::DurationUs{10'000}) {
                c.adc_error = x::AdcError::ready;
                return x::ScanEvent::failed;
            }
            if (elapsed >= coretx::DurationUs{2})
                c.adc_enabling = false;
            return x::ScanEvent::pending;
        }
        return c.adc_event;
    }
    static const auto &samples() {
        auto &c = Test::context;
        return c.samples;
    }
    static x::AdcError set_extended(bool enabled) {
        auto &c = Test::context;
        c.extra_analog = enabled;
        if (c.adc_reconfigure_delay != 0) {
            c.now += c.adc_reconfigure_delay;
            c.adc_started_at = c.now;
            c.adc_enabling = true;
        }
        return c.adc_error;
    }
};

template <typename Test> struct Keys {
    static void initialize() {}
    static Key service(coretx::TimestampUs) {
        auto &c = Test::context;
        ++c.key_calls;
        return c.key;
    }
    static bool ok_held() { return Test::context.ok_held; }
    static std::uint8_t trim_buttons() { return Test::context.trim_buttons; }
};

template <typename Test> struct Probe {
    static void initialize() {}
    static void service(coretx::TimestampUs) {
        auto &c = Test::context;
        ++c.probe_calls;
    }
    static x::StorageProbeState state() {
        auto &c = Test::context;
        return c.probe_state;
    }
    static x::I2cError error() {
        auto &c = Test::context;
        return c.probe_state == x::StorageProbeState::failed ? x::I2cError::bus : x::I2cError::none;
    }
    static std::uint8_t acknowledged() { return 0; }
    static x::StorageProbeResult result() {
        auto &c = Test::context;
        return {state(), error(),
                static_cast<std::uint8_t>(c.probe_state == x::StorageProbeState::found ? 1 : 0)};
    }
};

template <typename Test> struct Memory {
    inline static constexpr std::size_t page_size = 64;
    static void select(std::uint8_t address) {
        if (address != 0x50)
            std::abort();
    }
    static void service(coretx::TimestampUs) {}
    static bool busy() { return false; }
    static x::I2cError error() { return x::I2cError::none; }
    static bool read(std::uint16_t address, std::span<std::uint8_t> bytes) {
        auto &c = Test::context;
        if ((address + bytes.size()) > c.eeprom.size())
            std::abort();
        std::copy_n(c.eeprom.begin() + address, bytes.size(), bytes.begin());
        return true;
    }
    static bool write(std::uint16_t address, std::span<const std::uint8_t> bytes) {
        auto &c = Test::context;
        if ((address + bytes.size()) > c.eeprom.size())
            std::abort();
        std::copy(bytes.begin(), bytes.end(), c.eeprom.begin() + address);
        return true;
    }
};

template <typename Test> struct Transmitter {
    static void power_off() {
        auto &c = Test::context;
        c.powered = false;
    }
    static void initialize(coretx::TimestampUs) {
        auto &c = Test::context;
        if (c.internal_active)
            std::abort();
        c.powered = true;
    }
    static void next_baud(coretx::TimestampUs) {
        auto &c = Test::context;
        ++c.baud_changes;
    }
    static bool set_baud(const std::size_t index, coretx::TimestampUs) {
        auto &c = Test::context;
        if (index >= x::crsf_baudrates.size())
            return false;
        c.baud_index = index;
        return true;
    }
    static x::CrsfError service(coretx::TimestampUs now, const coretx::crsf::Channels &channels,
                                bool valid, coretx::TimestampUs sampled_at,
                                coretx::multiswitch::Pages *const pages = nullptr) {
        auto &c = Test::context;
        ++c.tx_calls;
        c.tx_valid = valid;
        c.tx_channels = channels;
        c.crsf_config.service(now);
        c.sampled_at = sampled_at.count();
        if (pages != nullptr)
            for (std::size_t i = 0; i < c.multiswitch_states.size(); ++i)
                c.multiswitch_states[i] = pages->switches(i);
        return x::CrsfError::none;
    }
    static const coretx::crsf::Telemetry &telemetry() {
        auto &c = Test::context;
        return c.telemetry;
    }
    static coretx::crsf::config::Client &config() { return Test::context.crsf_config; }
    static x::CrsfError error() { return x::CrsfError::none; }
    static std::uint32_t baudrate() { return 400'000; }
    static std::uint32_t actual_baudrate() { return 400'000; }
    static std::uint32_t frames() { return 0; }
    static std::uint32_t receive_errors_per_second() { return 0; }
    static std::uint32_t receive_overruns() { return 0; }
};

template <typename Test> struct SenderBoard : Platform<Test> {
    using Display = ::Display<Test>;
    using Adc = ::Adc<Test>;
    using Keys = ::Keys<Test>;
    using Probe = ::Probe<Test>;
    using Memory = ::Memory<Test>;
    using Transmitter = ::Transmitter<Test>;
    using Input = coretx::firmware::PhysicalInput<Keys>;
    static void reset() noexcept {}
};
template <typename Test> struct TouchSenderBoard : SenderBoard<Test> {
    using Input = coretx::firmware::TouchInput<::Touch<Test>>;
    inline static constexpr std::string_view name = "I6S";
    inline static constexpr bool sb_three_position = true;
};

template <typename App> auto step(App &application, Context &context, const std::uint32_t now) {
    context.now = now;
    return application.service(coretx::TimestampUs{now});
}

template <typename App> void press_key(App &app, Context &c, const Key key) {
    c.key = key;
    (void)step(app, c, c.now + 1U);
}

template <typename App> void settle_menu(App &app, Context &c) {
    c.key = Key::none;
    for (unsigned i = 0; i < 1000; ++i)
        (void)step(app, c, c.now + 1U);
}

template <typename App> void open_sender_setup(App &app, Context &c) {
    press_key(app, c, Key::cancel_long);
    press_key(app, c, Key::ok_long);
    press_key(app, c, Key::down);
    press_key(app, c, Key::down);
    press_key(app, c, Key::ok);
}

template <typename App> void open_rf_setup(App &app, Context &c) {
    press_key(app, c, Key::cancel_long);
    press_key(app, c, Key::ok_long);
    press_key(app, c, Key::down);
    press_key(app, c, Key::ok);
    for (unsigned i = 0; i < 3; ++i)
        press_key(app, c, Key::down);
    press_key(app, c, Key::ok);
}

template <typename App>
void set_output(App &app, Context &c, const coretx::firmware::RadioOutput output) {
    settle_menu(app, c);
    open_rf_setup(app, c);
    press_key(app, c, Key::ok);
    for (unsigned i = 0; (i < 3) && (app.menu().requested_output() != output); ++i)
        press_key(app, c, Key::up);
    press_key(app, c, Key::ok);
    settle_menu(app, c);
    press_key(app, c, Key::cancel_long);
}

template <typename App>
void set_format(App &app, Context &c, const coretx::afhds2a::ChannelMode mode) {
    settle_menu(app, c);
    open_rf_setup(app, c);
    press_key(app, c, Key::down);
    press_key(app, c, Key::ok);
    if (app.menu().requested_format() != mode)
        press_key(app, c, Key::up);
    press_key(app, c, Key::ok);
    settle_menu(app, c);
    press_key(app, c, Key::cancel_long);
}

template <typename App> void bind_receiver(App &app, Context &c) {
    settle_menu(app, c);
    open_rf_setup(app, c);
    press_key(app, c, Key::down);
    press_key(app, c, Key::down);
    press_key(app, c, Key::ok);
}

bool touch_lifecycle() {
    for (const std::uint32_t base : {0U, 0xFFFFF000U}) {
        struct TestScenario {};
        using Test = coretx::test::FakeApplication<TestScenario, Context, TouchSenderBoard>;
        Test::reset();
        auto &c = Test::context;
        c.now = base;
        c.busy = true;
        c.finish_frame = false;
        c.adc_event = x::ScanEvent::complete;
        c.samples.fill(2000);
        x::Application<Test::Board, Test::Board::Input> app{};
        if ((app.initialize() != x::ApplicationState::starting) || (c.touch_initializations != 1))
            return false;
        c.touch_ready = true;
        c.touch_event = s::TouchEvent::sample;
        if ((step(app, c, base) != x::ApplicationState::running) ||
            (app.diagnostics().phase() != x::CalibrationPhase::idle) || (c.touch_calls != 1) ||
            (c.display_calls != 1))
            return false;
        c.finish_frame = true;
        // Touch received while the first display frame was busy must not arm a gesture.
        c.touch_event = s::TouchEvent::none;
        if (step(app, c, base + 1U) != x::ApplicationState::running)
            return false;
        c.touch_event = s::TouchEvent::sample;
        c.touch_sample = {0, 0, 64, 32, 0, s::Gesture::none};
        (void)step(app, c, base + 1U);
        c.touch_sample.contacts = 1;
        (void)step(app, c, base + 1U);
        c.touch_sample.gesture = s::Gesture::down;
        (void)step(app, c, base + 1U);
        c.touch_sample.contacts = 0;
        c.touch_sample.gesture = s::Gesture::none;
        (void)step(app, c, base + 1U);
        c.touch_event = s::TouchEvent::sample;
        c.touch_sample = {0, 0, 64, 32, 1, s::Gesture::none};
        (void)step(app, c, base + 2U);
        c.touch_sample.x = 80;
        (void)step(app, c, base + 3U);
        if ((app.diagnostics().phase() != x::CalibrationPhase::idle) || app.menu().visible())
            return false;
        (void)step(app, c, base + 4U);
        if (app.diagnostics().phase() != x::CalibrationPhase::idle)
            return false;
        const auto swipe = [&](const s::Gesture gesture) {
            c.touch_sample = {0, 0, 64, 32, 0, s::Gesture::none};
            (void)step(app, c, c.now + 1U);
            c.touch_sample.contacts = 1;
            (void)step(app, c, c.now + 1U);
            c.touch_sample.gesture = gesture;
            (void)step(app, c, c.now + 1U);
            c.touch_sample.contacts = 0;
            c.touch_sample.gesture = s::Gesture::none;
            (void)step(app, c, c.now + 1U);
        };
        c.touch_sample.contacts = 0;
        (void)step(app, c, c.now + 1U);
        c.touch_sample = {0, 0, 64, 32, 1, s::Gesture::none};
        (void)step(app, c, c.now + 1U);
        for (unsigned i = 0; i < 80; ++i)
            (void)step(app, c, c.now + 10'000U);
        if (!app.menu().visible())
            return false;
        swipe(s::Gesture::down);
        swipe(s::Gesture::down);
        swipe(s::Gesture::right);
        swipe(s::Gesture::down);
        swipe(s::Gesture::right);
        if ((app.diagnostics().phase() != x::CalibrationPhase::center) || app.menu().visible())
            return false;
        const auto draws = c.draws;
        c.touch_error = s::TouchError::nack;
        c.touch_ready = false;
        c.touch_event = s::TouchEvent::failed;
        if (step(app, c, c.now + 1U) != x::ApplicationState::running)
            return false;
        (void)step(app, c, c.now + 50'000U);
        if ((app.diagnostics().phase() != x::CalibrationPhase::center) ||
            (c.draws != (draws + 1)) || (c.touch_calls != c.display_calls) || (c.key_calls != 0) ||
            c.powered || c.internal_active)
            return false;

        c.touch_error = s::TouchError::none;
        c.touch_ready = true;
        c.touch_event = s::TouchEvent::sample;
        c.touch_sample.gesture = s::Gesture::right;
        (void)step(app, c, c.now + 1U);
        if (app.diagnostics().phase() != x::CalibrationPhase::center)
            return false;
        c.touch_sample.contacts = 0;
        c.touch_sample.gesture = s::Gesture::none;
        (void)step(app, c, c.now + 1U);
        c.touch_sample = {0, 0, 64, 32, 1, s::Gesture::none};
        (void)step(app, c, c.now + 1U);
        c.touch_sample.x = 80;
        (void)step(app, c, c.now + 1U);
        if ((app.diagnostics().phase() != x::CalibrationPhase::range) || (c.key_calls != 0))
            return false;
    }
    return true;
}

bool startup_errors() {
    struct TouchTestScenario {};
    using TouchTest = coretx::test::FakeApplication<TouchTestScenario, Context, TouchSenderBoard>;
    TouchTest::reset();
    auto &touch = TouchTest::context;
    touch.busy = true;
    touch.finish_frame = false;
    touch.adc_event = x::ScanEvent::complete;
    touch.samples.fill(2000);
    x::Application<TouchTest::Board, TouchTest::Board::Input> touch_app{};
    if (touch_app.initialize() != x::ApplicationState::starting)
        return false;
    const auto initial_draws = touch.draws;
    touch.touch_error = s::TouchError::timeout;
    touch.touch_event = s::TouchEvent::failed;
    if ((step(touch_app, touch, 1) != x::ApplicationState::running) ||
        (touch.draws != initial_draws))
        return false;
    touch.finish_frame = true;
    if ((step(touch_app, touch, 50'001) != x::ApplicationState::running) ||
        (touch.draws != (initial_draws + 1)) ||
        (touch_app.diagnostics().phase() != x::CalibrationPhase::idle) || (touch.key_calls != 0) ||
        touch.powered || touch.internal_active)
        return false;

    struct SenderTestScenario {};
    using SenderTest = coretx::test::FakeApplication<SenderTestScenario, Context, SenderBoard>;
    SenderTest::reset();
    auto &sender = SenderTest::context;
    sender.adc_error = x::AdcError::calibration;
    x::Application<SenderTest::Board> sender_app{};
    if (sender_app.initialize() != x::ApplicationState::input_error)
        return false;
    sender.key = Key::down;
    return (step(sender_app, sender, 1) == x::ApplicationState::input_error) && !sender.powered &&
           !sender.tx_valid && (sender.tx_calls == 0) && (sender_app.diagnostics().page() == 1);
}

bool sender_lifecycle() {
    struct TestScenario {};
    using Test = coretx::test::FakeApplication<TestScenario, Context, SenderBoard>;
    Test::reset();
    auto &c = Test::context;
    c.probe_state = x::StorageProbeState::found;
    c.eeprom.fill(0xFF);
    c.samples.fill(2000);
    for (const auto index : x::switch_indices)
        c.samples[index] = 0;
    x::Application<typename Test::Board> app{};
    if (app.initialize() != x::ApplicationState::starting)
        return false;
    (void)step(app, c, 0);
    if ((app.state() != x::ApplicationState::starting) || c.tx_valid)
        return false;
    c.adc_event = x::ScanEvent::complete;
    if ((step(app, c, 5'000) != x::ApplicationState::running) || c.tx_valid)
        return false;
    settle_menu(app, c);
    open_sender_setup(app, c);
    press_key(app, c, Key::down);
    press_key(app, c, Key::ok);
    c.key = Key::ok;
    (void)step(app, c, 10'000);
    (void)step(app, c, 15'000);
    c.key = Key::none;
    for (const auto index : x::analog_indices)
        c.samples[index] = 400;
    (void)step(app, c, 20'000);
    for (const auto index : x::analog_indices)
        c.samples[index] = 3600;
    (void)step(app, c, 25'000);
    c.key = Key::ok;
    (void)step(app, c, 30'000);
    if (!app.diagnostics().calibrated() || c.tx_valid || (c.tx_calls != 0) || c.powered)
        return false;
    if constexpr (coretx::firmware::features::crsf) {
        set_output(app, c, coretx::firmware::RadioOutput::external);
        c.key = Key::none;
        (void)step(app, c, 32'000);
        if (!c.tx_valid || (c.sampled_at != 32'000) || !c.powered || c.internal_active)
            return false;
    }
    c.adc_error = x::AdcError::overrun;
    c.adc_event = x::ScanEvent::failed;
    c.key = Key::down;
    if ((step(app, c, 35'000) != x::ApplicationState::input_error) || c.tx_valid ||
        (c.powered != coretx::firmware::features::crsf))
        return false;
    if constexpr (coretx::firmware::features::crsf) {
        set_output(app, c, coretx::firmware::RadioOutput::off);
        open_rf_setup(app, c);
        for (unsigned i = 0; i < (coretx::firmware::features::a7105 ? 3U : 1U); ++i)
            press_key(app, c, Key::down);
        press_key(app, c, Key::ok);
        press_key(app, c, Key::up);
        press_key(app, c, Key::ok);
        settle_menu(app, c);
        set_output(app, c, coretx::firmware::RadioOutput::external);
    }
    return (c.baud_index == (coretx::firmware::features::crsf ? 2U : 1U)) &&
           (c.powered == coretx::firmware::features::crsf) && !c.tx_valid &&
           (c.probe_calls == c.adc_calls) &&
           (coretx::firmware::features::crsf ? c.tx_calls > 0 : c.tx_calls == 0) &&
           (c.radio_initializations == (coretx::firmware::features::a7105 ? 2U : 0U)) &&
           (coretx::firmware::features::a7105
                ? ((c.radio_calls > 0) && (c.radio_calls < c.adc_calls))
                : (c.radio_calls == 0));
}

template <template <typename> typename BoardPolicy> bool sender_fatal_priority() {
    for (unsigned kind = 0; kind < 5; ++kind) {
        struct TestScenario {};
        using Test = coretx::test::FakeApplication<TestScenario, Context, BoardPolicy>;
        Test::reset();
        auto &c = Test::context;
        x::Application<typename Test::Board, typename Test::Board::Input> app{};
        if (kind == 0)
            c.hardware_error = HardwareError::pll_ready;
        if (kind == 1)
            c.lcd_init_error = LcdError::frame_busy;
        const auto initialized = app.initialize();
        if (kind < 2) {
            if ((initialized != x::ApplicationState::fatal) || c.powered ||
                (c.touch_initializations != 0) ||
                (app.failure() !=
                 (kind == 0 ? x::ApplicationFailure::hardware : x::ApplicationFailure::lcd)))
                return false;
            continue;
        }
        c.key = Key::ok;
        c.touch_ready = true;
        c.touch_event = s::TouchEvent::sample;
        c.touch_sample = {0, 0, 80, 32, 1, s::Gesture::right};
        c.adc_event = x::ScanEvent::complete;
        if (kind == 2)
            c.lcd_error = LcdError::frame_busy;
        if (kind == 3)
            c.stack_ok = false;
        if (kind == 4)
            c.break_stack_on_display = true;
        if ((step(app, c, 1) != x::ApplicationState::fatal) || c.powered || c.internal_active ||
            (c.key_calls != 0) || (c.touch_calls != 0) || (c.adc_calls != 0) || (c.tx_calls != 0) ||
            (app.diagnostics().phase() != x::CalibrationPhase::idle) ||
            (app.failure() !=
             (kind == 2 ? x::ApplicationFailure::lcd : x::ApplicationFailure::stack)))
            return false;
        (void)step(app, c, 2);
        if ((c.tx_calls != 0) || (c.touch_calls != 0) || (c.key_calls != 0) || (c.adc_calls != 0))
            return false;
    }
    return true;
}

bool shared_touch_sender() {
    struct TestScenario {};
    using Test = coretx::test::FakeApplication<TestScenario, Context, TouchSenderBoard>;
    Test::reset();
    auto &c = Test::context;
    c.touch_ready = true;
    c.adc_event = x::ScanEvent::complete;
    c.samples.fill(2000);
    x::Application<Test::Board, Test::Board::Input> app{};
    if ((app.initialize() == x::ApplicationState::fatal) ||
        (step(app, c, 0) != x::ApplicationState::running) ||
        (app.diagnostics().raw().switches[1] != coretx::SwitchPosition::middle))
        return false;
    c.touch_event = s::TouchEvent::sample;
    c.touch_sample = {0, 0, 64, 32, 0, s::Gesture::none};
    (void)step(app, c, 0);
    c.touch_sample.contacts = 1;
    (void)step(app, c, 0);
    c.touch_sample.gesture = s::Gesture::down;
    (void)step(app, c, 0);
    c.touch_sample.contacts = 0;
    c.touch_sample.gesture = s::Gesture::none;
    (void)step(app, c, 0);
    c.touch_sample = {0, 0, 64, 32, 1, s::Gesture::none};
    (void)step(app, c, 1);
    c.touch_sample.x = 80;
    (void)step(app, c, 2);
    if ((app.diagnostics().phase() != x::CalibrationPhase::idle) ||
        (app.diagnostics().page() != x::Diagnostics::system_page))
        return false;
    c.touch_error = s::TouchError::nack;
    c.touch_ready = false;
    (void)step(app, c, 50'001);
    return (c.key_calls == 0) && (c.touch_calls == 8) && (c.adc_calls == 8) && !c.powered &&
           (app.diagnostics().phase() == x::CalibrationPhase::idle);
}

bool shared_touch_menus() {
    struct TestScenario {};
    using Test = coretx::test::FakeApplication<TestScenario, Context, TouchSenderBoard>;
    Test::reset();
    auto &c = Test::context;
    c.touch_ready = true;
    c.touch_event = s::TouchEvent::sample;
    c.touch_sample = {0, 0, 64, 32, 1, s::Gesture::none};
    x::Application<Test::Board, Test::Board::Input> app{};
    (void)app.initialize();
    for (unsigned now = 0; now <= 800000; now += 20000)
        (void)step(app, c, now);
    if (!app.menu().visible() || (app.menu().screen() != x::MenuScreen::root) ||
        (app.diagnostics().phase() != x::CalibrationPhase::idle))
        return false;
    c.touch_sample.contacts = 0;
    (void)step(app, c, 820000);
    c.touch_sample.contacts = 1;
    c.touch_sample.x = 80;
    (void)step(app, c, 840000);
    c.touch_sample.x = 64;
    (void)step(app, c, 860000);
    return !app.menu().visible() && (c.key_calls == 0);
}

template <template <typename> typename BoardPolicy> bool input_setup_recovery() {
    struct Scenario {};
    using Test = coretx::test::FakeApplication<Scenario, Context, BoardPolicy>;
    using Board = typename Test::Board;
    Test::reset();
    auto &c = Test::context;
    c.probe_state = x::StorageProbeState::found;
    c.eeprom.fill(0xFF);
    const auto base = coretx::firmware::input_storage::base_address;
    std::fill_n(c.eeprom.begin() + base, 256, 0);
    coretx::storage::State legacy{x::diagnostic_model, {}};
    for (auto &axis : legacy.setup.calibration)
        axis = {0, 2048, 4095};
    legacy.setup.calibration[4] = {100, 1000, 3500};
    legacy.setup.mapping[0].reversed = legacy.setup.mapping[1].reversed = true;
    coretx::storage::Record calibration{};
    if (!coretx::storage::encode(legacy, 0, calibration))
        return false;
    std::copy(calibration.begin(), calibration.end(), c.eeprom.begin());
    x::Application<Board> app{};
    (void)app.initialize();
    const auto press = [&](const Key key) {
        c.key = key;
        (void)step(app, c, c.now + 1000U);
    };
    for (unsigned i = 0; i < 300; ++i)
        press(Key::none);
    const auto before = c.eeprom;
    if (app.diagnostics().input_setup().calibration[4].center != 1000)
        return false;
    press(Key::ok_long);
    press(Key::down);
    press(Key::down);
    press(Key::ok);
    press(Key::down);
    press(Key::down);
    press(Key::ok);
    press(Key::ok);
    press(Key::down);
    press(Key::ok);
    for (unsigned i = 0; i < 100; ++i)
        press(Key::none);
    if ((app.diagnostics().input_setup() != coretx::InputSetup{Board::sb_three_position}) ||
        (app.diagnostics().setup().calibration[4].center != 1000) ||
        (app.output() != coretx::firmware::RadioOutput::off) ||
        !std::equal(c.eeprom.begin(), c.eeprom.begin() + base, before.begin()) ||
        !std::equal(c.eeprom.begin() + base + 256, c.eeprom.end(), before.begin() + base + 256))
        return false;
    press(Key::ok);
    press(Key::up);
    press(Key::ok);
    for (unsigned i = 0; i < 100; ++i)
        press(Key::none);
    return coretx::firmware::features::extended_inputs
               ? app.diagnostics().input_setup().extra_digital
               : app.diagnostics().input_setup().modes[0] == coretx::InputMode::three_position;
}

bool display_refresh_interval() {
    struct Scenario {};
    using Test = coretx::test::FakeApplication<Scenario, Context, SenderBoard>;
    Test::reset();
    auto &c = Test::context;
    x::Application<typename Test::Board> app{};
    (void)app.initialize();
    const auto initial = c.draws;
    (void)step(app, c, 19999);
    if (c.draws != initial)
        return false;
    (void)step(app, c, 20000);
    if (c.draws != (initial + 1))
        return false;
    c.finish_frame = false;
    (void)step(app, c, 40000);
    if (c.draws != (initial + 1))
        return false;
    c.finish_frame = true;
    (void)step(app, c, 40001);
    return c.draws == (initial + 2);
}

bool overview_keeps_channels_live() {
    if constexpr (!coretx::firmware::features::a7105)
        return true;
    struct Scenario {};
    using Test = coretx::test::FakeApplication<Scenario, Context, SenderBoard>;
    Test::reset();
    auto &c = Test::context;
    c.probe_state = x::StorageProbeState::found;
    c.eeprom.fill(0xFF);
    c.adc_event = x::ScanEvent::complete;
    c.samples.fill(2048);
    coretx::storage::State calibration{x::diagnostic_model, {}};
    calibration.setup.calibration.fill({0, 2048, 4095});
    calibration.setup.mapping[0].reversed = calibration.setup.mapping[1].reversed = true;
    coretx::storage::Record record{};
    if (!coretx::storage::encode(calibration, 0, record))
        return false;
    std::copy(record.begin(), record.end(), c.eeprom.begin());
    x::models::Data model{};
    model.radio.transmitter = {0x12345678};
    model.radio.receiver = {0x87654321};
    model.radio.output = coretx::firmware::RadioOutput::internal;
    x::models::Record model_record{};
    if (!x::models::encode(model, 0, true, 0, model_record))
        return false;
    std::copy(model_record.begin(), model_record.end(), c.eeprom.begin() + x::models::base);
    x::Application<typename Test::Board> app{};
    (void)app.initialize();
    for (unsigned i = 0; i < 300; ++i)
        (void)step(app, c, c.now + 1000U);
    if (!app.diagnostics().output_valid() || !c.internal_active)
        return false;
    for (unsigned i = 0; i < 2; ++i) {
        press_key(app, c, Key::ok);
        if ((app.output() != coretx::firmware::RadioOutput::internal) || c.internal_binding ||
            (app.diagnostics().phase() != x::CalibrationPhase::idle) ||
            (c.internal_mode != coretx::afhds2a::ChannelMode::standard14) || (c.baud_index != 1))
            return false;
        press_key(app, c, Key::down);
    }
    if (app.diagnostics().page() != x::Diagnostics::overview_page)
        return false;
    c.key = Key::none;
    c.finish_frame = false;
    for (unsigned i = 0; !c.busy && (i < 40); ++i)
        (void)step(app, c, c.now + 1000U);
    if (!c.busy)
        return false;
    const auto draws = c.draws;
    c.finish_frame = true;
    (void)step(app, c, c.now + 25000U);
    if (c.draws != draws)
        return false;
    auto previous = c.radio_channels[1];
    for (unsigned i = 0; i < 8; ++i) {
        c.samples[1] = static_cast<std::uint16_t>(100U + i * 100U);
        const auto calls = c.radio_calls;
        (void)step(app, c, c.now + 1000U);
        if ((c.draws != draws) || (c.radio_calls != (calls + 1)) || !c.radio_valid ||
            (c.radio_sampled_at != coretx::TimestampUs{c.now}) ||
            (c.radio_channels[1] != app.diagnostics().evaluation().frame.channels[1]) ||
            (c.radio_channels[1] == previous))
            return false;
        previous = c.radio_channels[1];
    }
    (void)step(app, c, c.now + 1000U);
    if (c.draws != (draws + 1))
        return false;
    (void)step(app, c, c.now + 25000U);
    if (c.draws != (draws + 1))
        return false;
    c.key = Key::ok_long;
    (void)step(app, c, c.now + 25000U);
    if (!app.menu().visible() || (c.draws != (draws + 1)))
        return false;
    const auto menu_band = [&] {
        const auto calls = c.radio_calls;
        const auto old = c.radio_channels[1];
        c.samples[1] = c.samples[1] == 1000 ? 3000 : 1000;
        c.key = Key::none;
        (void)step(app, c, c.now + 1000U);
        return (c.radio_calls == (calls + 1)) && c.radio_valid &&
               (c.radio_sampled_at == coretx::TimestampUs{c.now}) &&
               (c.radio_channels[1] == app.diagnostics().evaluation().frame.channels[1]) &&
               (c.radio_channels[1] != old);
    };
    for (unsigned i = 0; i < 3; ++i)
        if (!menu_band() || (c.draws != (draws + 1)))
            return false;
    c.key = Key::down;
    (void)step(app, c, c.now + 1000U);
    for (unsigned i = 0; i < 6; ++i)
        if (!menu_band() || (c.draws != (draws + 1)))
            return false;
    if (!menu_band() || (c.draws != (draws + 2)))
        return false;
    c.key = Key::none;
    (void)step(app, c, c.now + 1U);
    coretx::ui::Framebuffer expected_menu{};
    app.menu().render(app.models(), app.diagnostics(), app.output(), expected_menu);
    if (c.display_bytes != expected_menu.bytes())
        return false;
    c.key = Key::none;
    (void)step(app, c, c.now + 25000U);
    if (c.draws != (draws + 2))
        return false;
    c.key = Key::cancel;
    (void)step(app, c, c.now + 25000U);
    if (app.menu().visible() || (c.draws != (draws + 2)))
        return false;
    c.key = Key::none;
    for (unsigned i = 0; i < 9; ++i)
        (void)step(app, c, c.now + 1000U);
    if (c.draws != (draws + 3))
        return false;
    // The real input-setup menu uses the same eight-band path.
    set_output(app, c, coretx::firmware::RadioOutput::off);
    open_sender_setup(app, c);
    press_key(app, c, Key::down);
    press_key(app, c, Key::down);
    press_key(app, c, Key::ok);
    c.key = Key::none;
    const auto input_draws = c.draws;
    (void)step(app, c, c.now + 25000U);
    for (unsigned i = 0; i < 3; ++i)
        if (!menu_band() || (c.draws != input_draws))
            return false;
    c.key = Key::down;
    (void)step(app, c, c.now + 1000U);
    for (unsigned i = 0; i < 6; ++i)
        if (!menu_band() || (c.draws != input_draws))
            return false;
    if (!menu_band() || (c.draws != (input_draws + 1)))
        return false;
    c.key = Key::none;
    (void)step(app, c, c.now + 1U);
    coretx::firmware::InputMenu input_menu{};
    input_menu.open();
    auto diagnostics = app.diagnostics();
    (void)input_menu.handle(Key::down, diagnostics, app.output(), true, false, nullptr, false);
    expected_menu.clear();
    input_menu.render(diagnostics, expected_menu, nullptr, false);
    if (c.display_bytes != expected_menu.bytes())
        return false;
    c.key = Key::none;
    (void)step(app, c, c.now + 25000U);
    c.key = Key::cancel_long;
    (void)step(app, c, c.now + 1000U);
    if (c.draws != (input_draws + 1))
        return false;
    c.key = Key::cancel_long;
    (void)step(app, c, c.now + 1000U);
    c.key = Key::none;
    (void)step(app, c, c.now + 25000U);
    for (unsigned i = 0; i < 9; ++i)
        (void)step(app, c, c.now + 1000U);
    return !app.menu().visible() && (c.draws == (input_draws + 2));
}

bool failed_input_storage_blocks_rf() {
    struct TestScenario {};
    using Test = coretx::test::FakeApplication<TestScenario, Context, SenderBoard>;
    Test::reset();
    auto &c = Test::context;
    c.probe_state = x::StorageProbeState::failed;
    x::Application<typename Test::Board> app{};
    (void)app.initialize();
    (void)step(app, c, 0);
    if constexpr (coretx::firmware::features::a7105)
        set_output(app, c, coretx::firmware::RadioOutput::internal);
    if constexpr (coretx::firmware::features::crsf)
        set_output(app, c, coretx::firmware::RadioOutput::external);
    return (app.output() == coretx::firmware::RadioOutput::off) && !c.powered && !c.internal_active;
}

bool exclusive_outputs() {
    struct TestScenario {};
    using Test = coretx::test::FakeApplication<TestScenario, Context, SenderBoard>;
    Test::reset();
    auto &c = Test::context;
    c.probe_state = x::StorageProbeState::found;
    c.eeprom.fill(0xFF);
    x::Application<typename Test::Board> app{};
    (void)app.initialize();
    (void)step(app, c, 0);
    if ((app.output() != coretx::firmware::RadioOutput::off) || c.powered || c.internal_active)
        return false;
    if constexpr (coretx::firmware::features::a7105 || coretx::firmware::features::crsf) {
        for (const auto output :
             {coretx::firmware::RadioOutput::internal, coretx::firmware::RadioOutput::external,
              coretx::firmware::RadioOutput::off}) {
            if (((output == coretx::firmware::RadioOutput::internal) &&
                 !coretx::firmware::features::a7105) ||
                ((output == coretx::firmware::RadioOutput::external) &&
                 !coretx::firmware::features::crsf))
                continue;
            set_output(app, c, output);
            if (app.output() != output)
                return false;
            if ((c.powered != (app.output() == coretx::firmware::RadioOutput::external)) ||
                (c.internal_active != (app.output() == coretx::firmware::RadioOutput::internal)) ||
                (c.powered && c.internal_active))
                return false;
        }
    }
    return app.output() == coretx::firmware::RadioOutput::off;
}

bool channel_mode_persistence() {
    if constexpr (!coretx::firmware::features::a7105)
        return true;
    using Mode = coretx::afhds2a::ChannelMode;
    struct TestScenario {};
    using Test = coretx::test::FakeApplication<TestScenario, Context, SenderBoard>;
    Test::reset();
    auto &c = Test::context;
    c.probe_state = x::StorageProbeState::found;
    c.eeprom.fill(0xFF);
    x::Application<typename Test::Board> app{};
    (void)app.initialize();
    for (unsigned i = 0; i < 1000; ++i)
        (void)step(app, c, i);
    set_format(app, c, Mode::extended16);
    if ((c.internal_mode != Mode::extended16) || c.internal_binding || c.internal_active ||
        c.powered)
        return false;
    c.key = Key::none;
    for (unsigned i = 0; i < 100; ++i)
        (void)step(app, c, 2000 + i);
    struct RebootTestScenario {};
    using RebootTest = coretx::test::FakeApplication<RebootTestScenario, Context, SenderBoard>;
    RebootTest::reset();
    auto &reboot = RebootTest::context;
    reboot.probe_state = x::StorageProbeState::found;
    reboot.eeprom = c.eeprom;
    x::Application<RebootTest::Board> restored{};
    (void)restored.initialize();
    for (unsigned i = 0; i < 1000; ++i)
        (void)step(restored, reboot, i);
    if (reboot.internal_mode != Mode::extended16)
        return false;
    set_output(restored, reboot, coretx::firmware::RadioOutput::internal);
    if (restored.output() != coretx::firmware::RadioOutput::internal)
        return false;
    bind_receiver(restored, reboot);
    if (!reboot.internal_binding || (reboot.internal_mode != Mode::extended16))
        return false;
    press_key(restored, reboot, Key::cancel);
    if ((reboot.internal_mode != Mode::extended16) || !reboot.internal_active ||
        reboot.internal_binding)
        return false;
    if constexpr (coretx::firmware::features::crsf) {
        set_output(restored, reboot, coretx::firmware::RadioOutput::external);
        if (restored.output() != coretx::firmware::RadioOutput::external)
            return false;
        bind_receiver(restored, reboot);
        if ((reboot.internal_mode != Mode::extended16) || reboot.internal_binding ||
            reboot.internal_active || !reboot.powered)
            return false;
    }

    set_output(restored, reboot, coretx::firmware::RadioOutput::off);
    set_format(restored, reboot, Mode::standard14);
    if ((reboot.internal_mode != Mode::standard14) || reboot.internal_active ||
        reboot.internal_binding || reboot.powered)
        return false;
    reboot.key = Key::none;
    for (unsigned i = 0; i < 100; ++i)
        (void)step(restored, reboot, 3000 + i);
    x::ModelStorage<RebootTest::Board::Memory> journal{};
    journal.initialize({0x12345678});
    for (unsigned i = 0; i < 1000; ++i)
        journal.service(coretx::TimestampUs{i}, RebootTest::Board::Probe::result(),
                        x::diagnostic_model, coretx::firmware::RadioSettings{{0x12345678}});
    return journal.loaded() && (journal.data().radio.channel_mode == Mode::standard14) &&
           std::all_of(reboot.eeprom.begin(), reboot.eeprom.begin() + 2048,
                       [](const auto byte) { return byte == 0xFF; });
}

bool unified_sender_telemetry() {
    namespace tm = coretx::telemetry;
    struct TestScenario {};
    using Test = coretx::test::FakeApplication<TestScenario, Context, SenderBoard>;
    Test::reset();
    auto &c = Test::context;
    c.probe_state = x::StorageProbeState::found;
    c.eeprom.fill(0xFF);
    c.samples.fill(2000);
    c.samples[x::battery_index] = 2526;
    c.adc_event = x::ScanEvent::complete;
    x::Application<typename Test::Board> app{};
    (void)app.initialize();
    (void)step(app, c, 100);
    const auto *battery = app.telemetry().find(tm::transmitter_battery);
    if ((battery == nullptr) || (battery->value != 6200) ||
        (battery->updated_at != coretx::TimestampUs{100}))
        return false;
    c.adc_event = x::ScanEvent::pending;
    if constexpr (coretx::firmware::features::crsf) {
        set_output(app, c, coretx::firmware::RadioOutput::external);
        const auto received_at = c.now + 100U;
        std::array<std::uint8_t, 14> frame{0xEA, 0x0C, 0x14, 0xB5, 0xAE, 97, 0xF8};
        frame.back() = coretx::crsf::crc8(std::span{frame}.subspan(2, 11));
        for (const auto byte : frame)
            c.telemetry.push(byte, coretx::TimestampUs{received_at});
        c.key = Key::none;
        (void)step(app, c, received_at + 1U);
        const tm::Key quality{tm::Source::crsf, tm::Quantity::link_quality};
        if ((app.telemetry().entries().size() != 5) || (app.telemetry().find(quality)->value != 97))
            return false;
        set_output(app, c, coretx::firmware::RadioOutput::off);
        if ((app.output() != coretx::firmware::RadioOutput::off) ||
            (app.telemetry().find(quality) != nullptr))
            return false;
    }
    battery = app.telemetry().find(tm::transmitter_battery);
    if ((app.telemetry().entries().size() != 1) || (battery->value != 6200) ||
        (battery->updated_at != coretx::TimestampUs{100}))
        return false;
    press_key(app, c, Key::down);
    press_key(app, c, Key::ok);
    if (app.diagnostics().phase() != x::CalibrationPhase::idle)
        return false;
    c.adc_error = x::AdcError::overrun;
    c.adc_event = x::ScanEvent::failed;
    c.key = Key::none;
    (void)step(app, c, c.now + 1U);
    return !app.telemetry().find(tm::transmitter_battery)->valid;
}

template <template <typename> typename BoardPolicy> bool transmitter_menu_models() {
    struct TestScenario {};
    using Test = coretx::test::FakeApplication<TestScenario, Context, BoardPolicy>;
    using Board = typename Test::Board;
    Test::reset();
    auto &c = Test::context;
    c.probe_state = x::StorageProbeState::found;
    c.eeprom.fill(0xFF);
    c.adc_event = x::ScanEvent::complete;
    c.samples.fill(2048);
    c.samples[0] = 0;
    coretx::storage::State legacy{x::diagnostic_model, {}};
    for (auto &axis : legacy.setup.calibration)
        axis = {0, 2048, 4095};
    legacy.setup.mapping[0].reversed = legacy.setup.mapping[1].reversed = true;
    coretx::storage::Record calibration{};
    if (!coretx::storage::encode(legacy, 0, calibration))
        return false;
    std::copy(calibration.begin(), calibration.end(), c.eeprom.begin());
    x::models::Data first{}, second{};
    first.radio = {{0x12345678},
                   {0x87654321},
                   coretx::firmware::RadioOutput::off,
                   coretx::afhds2a::ChannelMode::standard14};
    if constexpr (coretx::firmware::features::extended_inputs) {
        first.model.inputs[0].source.index = 10;
        c.samples[11] = 4095;
        coretx::InputSetup inputs{Board::sb_three_position};
        inputs.extra_analog = true;
        inputs.modes[6] = coretx::InputMode::pot;
        const auto input_record = coretx::firmware::input_storage::encode(inputs, 0);
        std::copy(input_record.begin(), input_record.end(),
                  c.eeprom.begin() + coretx::firmware::input_storage::base_address);
    }
    second = first;
    second.model = x::models::four_channel_model();
    second.radio.receiver = {0x10203040};
    second.radio.channel_mode = coretx::afhds2a::ChannelMode::extended16;
    second.radio.output =
        coretx::firmware::features::a7105  ? coretx::firmware::RadioOutput::internal
        : coretx::firmware::features::crsf ? coretx::firmware::RadioOutput::external
                                           : coretx::firmware::RadioOutput::off;
    x::models::default_name(second.name, 1);
    x::models::Record record{};
    if (!x::models::encode(first, 0, true, 0, record))
        return false;
    std::copy(record.begin(), record.end(), c.eeprom.begin() + x::models::base);
    if (!x::models::encode(second, 1, true, 0, record))
        return false;
    std::copy(record.begin(), record.end(),
              c.eeprom.begin() + x::models::base + 2 * x::models::record_size);
    x::Application<Board> app{};
    (void)app.initialize();
    const auto press = [&](const Key key) {
        c.key = key;
        (void)step(app, c, c.now + 1);
    };
    const auto settle = [&] {
        c.key = Key::none;
        for (unsigned i = 0; i < 1000; ++i)
            (void)step(app, c, c.now + 1);
    };
    settle();
    const auto initial = app.diagnostics().evaluation().frame.channels[0];
    press(Key::ok_long);
    if (!app.menu().visible() || (app.diagnostics().phase() != x::CalibrationPhase::idle))
        return false;
    press(Key::down);
    press(Key::ok);
    press(Key::ok);
    press(Key::ok);
    press(Key::down);
    press(Key::ok);
    press(Key::down);
    if (!app.diagnostics().output_valid() ||
        (app.models().data().model.inputs[0].weight_percent.value() != 99) ||
        (app.diagnostics().evaluation().frame.channels[0] == initial))
        return false;
    press(Key::ok);
    settle();
    if ((app.models().data().model.inputs[0].weight_percent.value() != 99) ||
        (app.diagnostics().evaluation().frame.channels[0] == initial))
        return false;
    if constexpr (coretx::firmware::features::a7105) {
        press(Key::ok);
        press(Key::down);
        c.receiver_changed = true;
        settle();
        if (!app.menu().pending_model_edit() || !app.models().busy() ||
            (app.models().data().model.inputs[0].weight_percent.value() != 98))
            return false;
        press(Key::cancel);
        settle();
        if (app.models().busy() ||
            (app.models().data().model.inputs[0].weight_percent.value() != 99))
            return false;
    }
    press(Key::cancel_long);
    press(Key::ok_long);
    press(Key::down);
    press(Key::ok);
    for (std::size_t i = 0; i < x::menu_detail::trim_row; ++i)
        press(Key::down);
    press(Key::ok);
    if (app.menu().screen() != x::MenuScreen::trims)
        return false;
    if constexpr (!coretx::firmware::features::i6s)
        press(Key::down);
    press(Key::ok);
    press(Key::up);
    press(Key::ok);
    if (!app.diagnostics().output_valid() || (app.models().data().model.trims[0].value() != 1) ||
        (app.diagnostics().prepared().controls.trims[0].value() != 1))
        return false;
    settle();
    press(Key::cancel_long);
    press(Key::ok_long);
    press(Key::ok);
    press(Key::down);
    press(Key::ok);
    press(Key::ok);
    settle();
    if ((app.models().active() != 1) || (app.models().data().radio.receiver.value != 0x10203040) ||
        (app.models().data().model.inputs.size() != 4) ||
        (app.output() != coretx::firmware::RadioOutput::off) || c.powered || c.internal_active)
        return false;
    if constexpr (coretx::firmware::features::a7105 || coretx::firmware::features::crsf) {
        press(Key::cancel_long);
        press(Key::ok_long);
        press(Key::down);
        press(Key::ok);
        press(Key::down);
        press(Key::down);
        press(Key::down);
        press(Key::ok);
        press(Key::ok);
        press(Key::ok);
        settle();
        if (app.output() != second.radio.output)
            return false;
        press(Key::cancel_long);
        press(Key::ok_long);
        press(Key::ok);
        press(Key::ok);
        press(Key::ok);
        settle();
        if (app.models().active() != (coretx::firmware::features::secure_ui ? 1U : 0U))
            return false;
        if constexpr (!coretx::firmware::features::secure_ui)
            if ((app.output() != coretx::firmware::RadioOutput::off) || c.powered ||
                c.internal_active)
                return false;
        press(Key::cancel_long);
        set_output(app, c, coretx::firmware::RadioOutput::off);
        settle();
    }
    press(Key::cancel_long);
    press(Key::ok_long);
    press(Key::ok);
    press(Key::ok);
    press(Key::ok);
    settle();
    if ((app.models().active() != 0) || (app.models().data().radio.receiver.value != 0x87654321) ||
        (app.models().data().radio.channel_mode != coretx::afhds2a::ChannelMode::standard14) ||
        (app.models().data().model.inputs[0].weight_percent.value() != 99))
        return false;
    struct RebootTestScenario {};
    using RebootTest = coretx::test::FakeApplication<RebootTestScenario, Context, BoardPolicy>;
    RebootTest::reset();
    auto &reboot = RebootTest::context;
    reboot.probe_state = x::StorageProbeState::found;
    reboot.eeprom = c.eeprom;
    x::Application<typename RebootTest::Board> restored{};
    (void)restored.initialize();
    for (unsigned i = 0; i < 1000; ++i)
        (void)step(restored, reboot, i);
    return (restored.models().active() == 0) &&
           (restored.models().data().model.inputs[0].weight_percent.value() == 99) &&
           (restored.models().data().model.inputs[0].source.index ==
            (coretx::firmware::features::extended_inputs ? 10 : 0)) &&
           (restored.models().data().radio.receiver.value == 0x87654321);
}

template <template <typename> typename BoardPolicy> bool calibration_reboot() {
    struct Scenario {};
    using Test = coretx::test::FakeApplication<Scenario, Context, BoardPolicy>;
    Test::reset();
    auto &c = Test::context;
    c.probe_state = x::StorageProbeState::found;
    c.eeprom.fill(0xFF);
    c.adc_event = x::ScanEvent::complete;
    c.samples.fill(2048);
    x::Application<typename Test::Board> app{};
    (void)app.initialize();
    settle_menu(app, c);
    press_key(app, c, Key::ok_long);
    press_key(app, c, Key::down);
    press_key(app, c, Key::down);
    press_key(app, c, Key::ok);
    press_key(app, c, Key::down);
    press_key(app, c, Key::ok);
    if (app.diagnostics().phase() != x::CalibrationPhase::center)
        return false;
    press_key(app, c, Key::ok);
    if (app.diagnostics().phase() != x::CalibrationPhase::range)
        return false;
    c.key = Key::none;
    for (const auto index : x::analog_indices)
        c.samples[index] = 400;
    (void)step(app, c, c.now + 1000U);
    for (const auto index : x::analog_indices)
        c.samples[index] = 3600;
    (void)step(app, c, c.now + 1000U);
    press_key(app, c, Key::ok);
    if (!app.diagnostics().calibrated() || !app.diagnostics().output_valid() ||
        (app.diagnostics().phase() != x::CalibrationPhase::idle))
        return false;
    settle_menu(app, c);
    struct RebootScenario {};
    using Reboot = coretx::test::FakeApplication<RebootScenario, Context, BoardPolicy>;
    Reboot::reset();
    auto &reboot = Reboot::context;
    reboot.probe_state = x::StorageProbeState::found;
    reboot.eeprom = c.eeprom;
    reboot.adc_event = x::ScanEvent::complete;
    reboot.samples.fill(2048);
    x::Application<typename Reboot::Board> restored{};
    (void)restored.initialize();
    settle_menu(restored, reboot);
    return restored.diagnostics().calibrated() && restored.diagnostics().output_valid() &&
           (restored.diagnostics().setup().calibration == app.diagnostics().setup().calibration) &&
           (restored.diagnostics().stick_mode() == app.diagnostics().stick_mode()) &&
           (restored.diagnostics().input_setup() == app.diagnostics().input_setup());
}

template <template <typename> typename BoardPolicy> bool stored_adc_extension_startup() {
    if constexpr (!coretx::firmware::features::extended_inputs)
        return true;
    struct Scenario {};
    using Test = coretx::test::FakeApplication<Scenario, Context, BoardPolicy>;
    Test::reset();
    auto &c = Test::context;
    c.eeprom.fill(0xFF);
    c.probe_state = x::StorageProbeState::found;
    c.adc_event = x::ScanEvent::complete;
    c.samples.fill(2048);
    c.adc_reconfigure_delay = 3;
    coretx::InputSetup setup{};
    setup.extra_analog = true;
    setup.modes[6] = coretx::InputMode::pot;
    const auto record = coretx::firmware::input_storage::encode(setup, 3);
    std::copy(record.begin(), record.end(),
              c.eeprom.begin() + coretx::firmware::input_storage::base_address);
    x::Application<typename Test::Board> app{};
    (void)app.initialize();
    for (unsigned i = 0; i < 1000; ++i)
        (void)step(app, c, c.now + 10U);
    return c.extra_analog && (c.adc_error == x::AdcError::none) &&
           (app.state() == x::ApplicationState::running) && app.diagnostics().valid() &&
           (app.diagnostics().input_setup() == setup);
}

bool expanded_input_reboot() {
    namespace fw = coretx::firmware;
    struct TestScenario {};
    using Test = coretx::test::FakeApplication<TestScenario, Context, TouchSenderBoard>;
    Test::reset();
    auto &c = Test::context;
    c.eeprom.fill(0xFF);
    c.probe_state = x::StorageProbeState::found;
    coretx::InputSetup setup{true};
    setup.extra_analog = setup.extra_digital = true;
    setup.modes[6] = coretx::InputMode::six_position;
    const auto record = fw::input_storage::encode(setup, 3);
    std::copy(record.begin(), record.end(), c.eeprom.begin() + fw::input_storage::base_address);
    fw::RadioSettings radio{};
    radio.output = fw::RadioOutput::external;
    const auto rf = fw::radio_storage::encode(radio, 3);
    std::copy(rf.begin(), rf.end(), c.eeprom.begin() + fw::radio_storage::base_address);
    x::Application<typename Test::Board> app{};
    (void)app.initialize();
    for (unsigned i = 0; i < 1000; ++i)
        (void)step(app, c, i);
    if ((app.diagnostics().input_setup() != setup) ||
        (c.extra_analog != fw::features::extended_inputs) ||
        (c.extra_digital != fw::features::extended_inputs))
        return false;
    if constexpr (fw::features::i6s && fw::features::extended_inputs && fw::features::crsf)
        if ((app.output() != fw::RadioOutput::off) || c.powered)
            return false;
    std::uint32_t now = 1000;
    const auto press = [&](const Key key) {
        c.key = key;
        (void)step(app, c, ++now);
    };
    press(Key::ok_long);
    if (!app.menu().visible())
        return false;
    if constexpr (fw::features::extended_inputs) {
        press(Key::down);
        press(Key::down);
        press(Key::ok);
        press(Key::down);
        press(Key::down);
        press(Key::ok);
        press(Key::ok);
        press(Key::down);
        press(Key::ok);
        if (app.output() == fw::RadioOutput::off) {
            if (c.extra_digital)
                return false;
            c.key = Key::none;
            for (unsigned i = 0; i < 100; ++i)
                (void)step(app, c, ++now);
            struct RebootTestScenario {};
            using RebootTest =
                coretx::test::FakeApplication<RebootTestScenario, Context, TouchSenderBoard>;
            RebootTest::reset();
            auto &reboot = RebootTest::context;
            reboot.eeprom = c.eeprom;
            reboot.probe_state = x::StorageProbeState::found;
            x::Application<RebootTest::Board> restored{};
            (void)restored.initialize();
            for (unsigned i = 0; i < 1000; ++i)
                (void)step(restored, reboot, i);
            if (reboot.extra_digital || !reboot.extra_analog)
                return false;
        }
    }
    return true;
}

template <template <typename> typename BoardPolicy> bool logical_model_lifecycle() {
    struct Scenario {};
    using Test = coretx::test::FakeApplication<Scenario, Context, BoardPolicy>;
    Test::reset();
    auto &c = Test::context;
    c.probe_state = x::StorageProbeState::found;
    c.eeprom.fill(0xFF);
    c.adc_event = x::ScanEvent::complete;
    c.samples.fill(0);
    coretx::storage::State calibration{x::diagnostic_model, {}};
    calibration.setup.calibration.fill({0, 2048, 4095});
    calibration.setup.mapping[0].reversed = calibration.setup.mapping[1].reversed = true;
    coretx::storage::Record record{};
    if (!coretx::storage::encode(calibration, 0, record))
        return false;
    std::copy(record.begin(), record.end(), c.eeprom.begin());
    x::models::Data data{};
    data.radio.transmitter = {0x12345678};
    data.radio.output = coretx::firmware::RadioOutput::off;
    auto &latch = data.model.logical_switches[0];
    latch.function = coretx::LogicalFunction::srff;
    latch.a = 0;
    latch.b = 3;
    latch.position_a = latch.position_b = coretx::SwitchPosition::down;
    x::models::Record model_record{};
    for (std::size_t i = 0; i < 2; ++i) {
        x::models::default_name(data.name, i);
        data.model.dashboard_channels[0] = i == 0 ? 16 : 9;
        if (!x::models::encode(data, i, true, 0, model_record))
            return false;
        std::copy(model_record.begin(), model_record.end(),
                  c.eeprom.begin() + x::models::base +
                      static_cast<std::ptrdiff_t>(i * 2 * x::models::record_size));
    }
    x::Application<typename Test::Board> app{};
    (void)app.initialize();
    const auto settle = [&] {
        c.key = Key::none;
        for (int i = 0; i < 1000; ++i)
            (void)step(app, c, c.now + 1000);
    };
    const auto press = [&](const Key key) {
        c.key = key;
        (void)step(app, c, c.now + 1000);
        c.key = Key::none;
    };
    const auto select = [&](const std::size_t index) {
        press(Key::cancel_long);
        press(Key::ok_long);
        press(Key::ok);
        for (std::size_t i = 0; i < index; ++i)
            press(Key::down);
        press(Key::ok);
        press(Key::ok);
        settle();
        press(Key::cancel_long);
    };
    settle();
    const auto active = [&] {
        return app.diagnostics().output_valid() &&
               ((app.diagnostics().evaluation().frame.logical.active & 1U) != 0);
    };
    if ((app.diagnostics().page() != 0) || active())
        return false;
    c.samples[4] = 3500;
    settle();
    c.samples[4] = 0;
    settle();
    if (!active())
        return false;
    press(Key::ok_long);
    press(Key::down);
    press(Key::ok);
    for (int i = 0; i < 4; ++i)
        press(Key::down);
    press(Key::ok);
    press(Key::ok);
    press(Key::ok); // Save an unchanged dashboard assignment.
    settle();
    if (!active())
        return false;
    select(0);
    if (!active())
        return false;
    press(Key::ok_long);
    press(Key::ok);
    press(Key::ok);
    for (int i = 0; i < 3; ++i)
        press(Key::down);
    press(Key::ok);
    press(Key::up);
    press(Key::ok_long); // Rename, without changing model identity.
    settle();
    if (!active())
        return false;
    select(1);
    if (active() || (app.diagnostics().model().dashboard_channels[0] != 9))
        return false;
    select(0);
    if (active() || (app.diagnostics().model().dashboard_channels[0] != 16))
        return false;
    c.samples[4] = 3500;
    settle();
    c.samples[4] = 0;
    settle();
    if (!active())
        return false;
    struct RebootScenario {};
    using Reboot = coretx::test::FakeApplication<RebootScenario, Context, BoardPolicy>;
    Reboot::reset();
    auto &reboot = Reboot::context;
    reboot.eeprom = c.eeprom;
    reboot.probe_state = x::StorageProbeState::found;
    reboot.samples.fill(0);
    reboot.adc_event = x::ScanEvent::complete;
    x::Application<typename Reboot::Board> restarted{};
    (void)restarted.initialize();
    for (int i = 0; i < 1000; ++i)
        (void)step(restarted, reboot, reboot.now + 1000);
    return restarted.diagnostics().output_valid() &&
           ((restarted.diagnostics().evaluation().frame.logical.active & 1U) == 0) &&
           (restarted.diagnostics().model().dashboard_channels[0] == 16);
}

bool dashboard_touch_configuration() {
    struct Scenario {};
    using Test = coretx::test::FakeApplication<Scenario, Context, TouchSenderBoard>;
    Test::reset();
    auto &c = Test::context;
    c.probe_state = x::StorageProbeState::found;
    c.eeprom.fill(0xFF);
    c.touch_ready = true;
    c.touch_event = s::TouchEvent::sample;
    x::Application<Test::Board, Test::Board::Input> app{};
    (void)app.initialize();
    const auto packet = [&] { (void)step(app, c, c.now + 20'000); };
    for (int i = 0; i < 1000; ++i)
        packet();
    const auto press = [&](const Key key) {
        c.touch_sample = {0, 0, 64, 32, 0, s::Gesture::none};
        packet();
        c.touch_sample.contacts = 1;
        packet();
        if (key == Key::ok_long) {
            for (int i = 0; i < 41; ++i)
                packet();
        } else {
            c.touch_sample.gesture = key == Key::down ? s::Gesture::down
                                     : key == Key::up ? s::Gesture::up
                                     : key == Key::ok ? s::Gesture::right
                                                      : s::Gesture::left;
            packet();
        }
        c.touch_sample.contacts = 0;
        c.touch_sample.gesture = s::Gesture::none;
        packet();
    };
    press(Key::ok_long);
    press(Key::down);
    press(Key::ok);
    for (int i = 0; i < 4; ++i)
        press(Key::down);
    press(Key::ok);
    if (app.menu().screen() != x::MenuScreen::dashboard_channels)
        return false;
    press(Key::ok);
    press(Key::down);
    press(Key::ok);
    for (int i = 0; i < 100; ++i)
        packet();
    if (app.models().data().model.dashboard_channels[0] != 0)
        return false;
    press(Key::cancel);
    for (int i = 0; i < 5; ++i)
        press(Key::down);
    press(Key::ok);
    press(Key::ok);
    press(Key::ok);
    press(Key::up); // Analog > threshold.
    press(Key::ok_long);
    for (int i = 0; i < 100; ++i)
        packet();
    return (app.menu().screen() == x::MenuScreen::logical_switches) &&
           (app.models().data().model.logical_switches[0].function ==
            coretx::LogicalFunction::greater) &&
           (c.key_calls == 0);
}

bool static_sender_isolation_and_reset() {
    struct FirstScenario {};
    struct SecondScenario {};
    using First = coretx::test::FakeApplication<FirstScenario, Context, SenderBoard>;
    using Second = coretx::test::FakeApplication<SecondScenario, Context, SenderBoard>;
    First::reset();
    Second::reset();
    auto &first = First::context;
    auto &second = Second::context;
    first.adc_event = second.adc_event = x::ScanEvent::complete;
    first.samples.fill(2000);
    second.samples.fill(2500);
    first.key = Key::down;
    first.eeprom[0] = 0x5A;
    x::Application<First::Board> first_app{};
    x::Application<Second::Board> second_app{};
    if ((first_app.initialize() != x::ApplicationState::starting) ||
        (second_app.initialize() != x::ApplicationState::starting) ||
        (step(first_app, first, 1) != x::ApplicationState::running) || (first.adc_calls != 1) ||
        (first.key_calls != 1) || (second.adc_calls != 0) || (second.key_calls != 0))
        return false;
    if ((step(second_app, second, 2) != x::ApplicationState::running) || (first.adc_calls != 1) ||
        (second.adc_calls != 1) || (first_app.diagnostics().page() != 1) ||
        (second_app.diagnostics().page() != 0))
        return false;
    first.stack_ok = false;
    if ((step(first_app, first, 3) != x::ApplicationState::fatal) ||
        (step(second_app, second, 3) != x::ApplicationState::running) || (first.adc_calls != 1) ||
        (second.adc_calls != 2))
        return false;
    first.extra_analog = first.extra_digital = true;
    First::reset();
    if (!first.stack_ok || (first.adc_calls != 0) || (first.key_calls != 0) ||
        (first.display_initializations != 0) || first.extra_analog || first.extra_digital ||
        (first.eeprom[0] != 0) || (second.adc_calls != 2))
        return false;
    x::Application<First::Board> restarted{};
    return (restarted.initialize() == x::ApplicationState::starting) &&
           (step(restarted, first, 4) == x::ApplicationState::starting) &&
           (first.display_initializations == 1) && (first.adc_calls == 1) &&
           (second.adc_calls == 2) && (second_app.state() == x::ApplicationState::running);
}

#if CORETX_CRSF
template <template <typename> typename BoardPolicy>
bool crsf_configuration_menu(const bool dashboard = false) {
    struct ConfigurationMenuScenario {};
    using Test = coretx::test::FakeApplication<ConfigurationMenuScenario, Context, BoardPolicy>;
    Test::reset();
    auto &c = Test::context;
    c.probe_state = x::StorageProbeState::found;
    c.eeprom.fill(0xFF);
    c.adc_event = x::ScanEvent::complete;
    c.samples.fill(2048);
    coretx::storage::State calibration{x::diagnostic_model, {}};
    calibration.setup.calibration.fill({0, 2048, 4095});
    calibration.setup.mapping[0].reversed = calibration.setup.mapping[1].reversed = true;
    coretx::storage::Record record{};
    if (!coretx::storage::encode(calibration, 0, record))
        return false;
    std::copy(record.begin(), record.end(), c.eeprom.begin());
    x::models::Data data{};
    data.radio.transmitter = {0x12345678};
    data.model.crsf_dashboard = dashboard;
    x::models::Record model{};
    if (!x::models::encode(data, 0, true, 0, model))
        return false;
    std::copy(model.begin(), model.end(), c.eeprom.begin() + x::models::base);
    x::Application<typename Test::Board> app{};
    (void)app.initialize();
    for (unsigned i = 0; i < 300; ++i)
        (void)step(app, c, c.now + 1000U);
    if (dashboard) {
        set_output(app, c, coretx::firmware::RadioOutput::external);
        press_key(app, c, Key::down);
        press_key(app, c, Key::down);
        if ((app.diagnostics().page() != x::Diagnostics::page_count) || !app.crsf_menu().visible())
            return false;
        press_key(app, c, Key::ok);
    } else {
        open_sender_setup(app, c);
        for (unsigned i = 0; i < 3; ++i)
            press_key(app, c, Key::down);
        press_key(app, c, Key::ok);
        if (app.crsf_menu().visible() || (app.output() != coretx::firmware::RadioOutput::off))
            return false;
        set_output(app, c, coretx::firmware::RadioOutput::external);
        open_sender_setup(app, c);
        for (unsigned i = 0; i < 3; ++i)
            press_key(app, c, Key::down);
        press_key(app, c, Key::ok);
    }
    if (!app.crsf_menu().visible() || (c.crsf_config.mode() != coretx::crsf::config::Mode::devices))
        return false;

    const auto draw_cycle = [&]() {
        c.key = Key::none;
        const auto calls = c.tx_calls;
        (void)step(app, c, c.now + 25'000U);
        for (unsigned i = 0; i < 8; ++i) {
            c.samples[1] = (i & 1U) != 0 ? 1000 : 3000;
            (void)step(app, c, c.now + 1000U);
            if (!c.tx_valid || (c.sampled_at != c.now) ||
                (c.tx_channels != app.diagnostics().evaluation().frame.channels))
                return false;
        }
        return c.tx_calls == (calls + 9);
    };
    if (!draw_cycle())
        return false;
    const auto discovery = c.display_bytes;
    const auto feed = [&](auto &frame) {
        frame.back() = coretx::crsf::crc8(std::span{frame}.subspan(2, frame.size() - 3));
        for (const auto byte : frame)
            c.telemetry.push(byte, coretx::TimestampUs{c.now}, c.crsf_config);
    };
    std::array<std::uint8_t, 23> info{0xEA, 21, 0x29, 0xEA, 0xEE, 'T', 'X', 0};
    info[20] = 1;
    feed(info);
    press_key(app, c, Key::ok);
    coretx::crsf::config::Frame outbound{};
    if (c.crsf_config.next_frame(coretx::TimestampUs{c.now}, outbound) != 8)
        return false;
    std::array<std::uint8_t, 14> root{0xEA, 12, 0x2B, 0xEA, 0xEE, 0, 0, 0, 11, 'R', 0, 1, 255};
    feed(root);
    if (!draw_cycle() || (c.display_bytes == discovery))
        return false;
    const auto pending = c.display_bytes;
    if (c.crsf_config.next_frame(coretx::TimestampUs{c.now}, outbound) != 8)
        return false;
    std::array<std::uint8_t, 17> value{0xEA, 15,  0x2B, 0xEA, 0xEE, 1,   0, 0,
                                       0,    'V', 0,    12,   0,    100, 0, 0};
    feed(value);
    if (!draw_cycle() || c.crsf_config.busy() || (c.display_bytes == pending))
        return false;
    coretx::ui::Framebuffer expected{};
    app.crsf_menu().render(c.crsf_config, expected, coretx::TimestampUs{c.now}, dashboard,
                           dashboard ? coretx::ui::tr::crsf_dashboard_active_help : nullptr);
    if (c.display_bytes != expected.bytes())
        return false;
    press_key(app, c, Key::cancel_long);
    if (dashboard) {
        if (!app.crsf_menu().visible() ||
            (c.crsf_config.mode() != coretx::crsf::config::Mode::parameters))
            return false;
        press_key(app, c, Key::down);
        if (app.crsf_menu().visible() ||
            (c.crsf_config.mode() != coretx::crsf::config::Mode::closed) ||
            (c.crsf_config.view_mode() != coretx::crsf::config::Mode::parameters))
            return false;
        press_key(app, c, Key::up);
        if (!app.crsf_menu().visible() || (c.crsf_config.folder() != 0) ||
            c.crsf_config.parameter(1))
            return false;
        if ((c.crsf_config.next_frame(coretx::TimestampUs{c.now}, outbound) != 8) ||
            (outbound[5] != 0))
            return false;
        feed(root);
        if ((c.crsf_config.next_frame(coretx::TimestampUs{c.now}, outbound) != 8) ||
            (outbound[5] != 1))
            return false;
        value[11] = 25;
        feed(value);
        return !c.crsf_config.busy() && (c.crsf_config.parameter(1)->value == 25);
    }
    return !app.crsf_menu().visible() &&
           (c.crsf_config.mode() == coretx::crsf::config::Mode::closed) &&
           (app.output() == coretx::firmware::RadioOutput::external) && c.powered;
}
#endif

template <template <typename> typename BoardPolicy> bool multiswitch_dashboard() {
#if !CORETX_MULTISWITCH
    return true;
#else
    struct Scenario {};
    using Test = coretx::test::FakeApplication<Scenario, Context, BoardPolicy>;
    Test::reset();
    auto &c = Test::context;
    c.probe_state = x::StorageProbeState::found;
    c.eeprom.fill(0xFF);
    c.adc_event = x::ScanEvent::complete;
    c.samples.fill(2048);
    c.touch_ready = true;
    c.touch_event = s::TouchEvent::sample;
    coretx::storage::State calibration{x::diagnostic_model, {}};
    calibration.setup.calibration.fill({0, 2048, 4095});
    calibration.setup.mapping[0].reversed = calibration.setup.mapping[1].reversed = true;
    coretx::storage::Record record{};
    if (!coretx::storage::encode(calibration, 0, record))
        return false;
    std::copy(record.begin(), record.end(), c.eeprom.begin());
    x::models::Data model{};
    model.radio.transmitter = {0x12345678};
    model.radio.output = coretx::firmware::RadioOutput::external;
    model.model.multiswitch[1].address = 42;
    model.model.multiswitch[1].momentary = 2;
    model.model.multiswitch[3].address = 255;
    model.model.logical_switches[0].function = coretx::LogicalFunction::greater;
    model.model.multiswitch[1].controls[6].set_condition(
        {true, coretx::switch_count, coretx::SwitchPosition::down});
    model.model.crsf_dashboard = true;
    x::models::Record model_record{};
    if (!x::models::encode(model, 0, true, 0, model_record))
        return false;
    std::copy(model_record.begin(), model_record.end(), c.eeprom.begin() + x::models::base);
    x::Application<typename Test::Board, typename Test::Board::Input> app{};
    (void)app.initialize();
    for (unsigned i = 0; i < 300; ++i)
        (void)step(app, c, c.now + 1000U);
    if (!app.diagnostics().output_valid() || !c.powered ||
        (app.diagnostics().dashboard_pages() != 5))
        return false;
    const auto touch = [&](const unsigned x, const unsigned y, const bool pressed,
                           const s::Gesture gesture = s::Gesture::none) {
        c.touch_sample = {0,
                          0,
                          static_cast<std::uint8_t>(x),
                          static_cast<std::uint8_t>(y),
                          static_cast<std::uint8_t>(pressed),
                          gesture};
        (void)step(app, c, c.now + 1000U);
    };
    const auto next = [&] {
        if constexpr (Test::Board::Input::touch) {
            touch(64, 32, true);
            touch(64, 32, true, s::Gesture::down);
            touch(64, 32, false);
        } else
            press_key(app, c, Key::down);
    };
    next();
    next();
    if (app.diagnostics().page() != 2)
        return false;
    if constexpr (Test::Board::Input::touch) {
        touch(0, 0, false); // Release the touch that navigated into this page.
        touch(4, 16, true);
        touch(4, 16, false);
        if (c.multiswitch_states[1] != 1)
            return false;
        touch(36, 16, true);
        if (c.multiswitch_states[1] != 3)
            return false;
        touch(36, 16, false);
        if (c.multiswitch_states[1] != 1)
            return false;
        touch(36, 16, true);
        c.touch_error = s::TouchError::nack;
        c.touch_ready = false;
        (void)step(app, c, c.now + 1000U);
        if (c.multiswitch_states[1] != 1)
            return false;
        c.touch_error = s::TouchError::none;
        c.touch_ready = true;
        touch(0, 0, false);
    } else {
        press_key(app, c, Key::ok); // Activate selection without toggling.
        if (c.multiswitch_states[1] != 0)
            return false;
        press_key(app, c, Key::ok);
        if (c.multiswitch_states[1] != 1)
            return false;
        press_key(app, c, Key::down);
        c.ok_held = true;
        press_key(app, c, Key::none);
        if (c.multiswitch_states[1] != 3)
            return false;
        press_key(app, c, Key::ok_long);
        if (app.menu().visible() || (c.multiswitch_states[1] != 3))
            return false;
        c.ok_held = false;
        press_key(app, c, Key::none);
        if (c.multiswitch_states[1] != 1)
            return false;
        press_key(app, c, Key::cancel);
    }
    next();
    if ((app.diagnostics().page() != 3) || (c.multiswitch_states[1] != 1) ||
        (c.multiswitch_states[3] != 0))
        return false;
    if constexpr (Test::Board::Input::touch) {
        touch(0, 0, false);
        touch(100, 34, true);
        touch(100, 34, false);
        if (c.multiswitch_states[3] != 0x80)
            return false;
    }
    next();
    if ((app.diagnostics().page() != 4) || !app.crsf_menu().visible())
        return false;
    if constexpr (Test::Board::Input::touch) {
        touch(64, 32, true);
        touch(64, 32, true, s::Gesture::right);
        touch(64, 32, false);
    } else
        press_key(app, c, Key::ok);
    next();
    if (app.diagnostics().page() != 4)
        return false; // UP/DOWN now belongs to the CRSF list.
    const auto hold = [&] {
        if constexpr (Test::Board::Input::touch) {
            touch(60, 56, true);
            for (unsigned i = 0; i < 81; ++i)
                (void)step(app, c, c.now + 10'000U);
            touch(60, 56, false);
        } else
            press_key(app, c, Key::cancel_long);
    };
    hold();
    next();
    if ((app.diagnostics().page() != 0) || app.crsf_menu().visible() ||
        (c.crsf_config.mode() != coretx::crsf::config::Mode::closed))
        return false;
    if constexpr (Test::Board::Input::touch) {
        touch(64, 32, true);
        touch(64, 32, true, s::Gesture::up);
        touch(64, 32, false);
    } else
        press_key(app, c, Key::up);
    if ((app.diagnostics().page() != 4) || !app.crsf_menu().visible())
        return false;
    if constexpr (Test::Board::Input::touch)
        hold();
    else
        press_key(app, c, Key::ok_long);
    if (!app.menu().visible() || (c.multiswitch_states[1] != 1) || !c.tx_valid)
        return false;
    c.key = Key::none;
    c.samples[0] = 0;
    (void)step(app, c, c.now + 1000U);
    if (c.multiswitch_states[1] != 0x41)
        return false; // A logical assignment remains active outside its dashboard.
    if constexpr (!Test::Board::Input::touch) {
        press_key(app, c, Key::down);
        press_key(app, c, Key::ok);
        for (unsigned i = 0; i < 6; ++i)
            press_key(app, c, Key::down);
        press_key(app, c, Key::ok);
        press_key(app, c, Key::down);
        press_key(app, c, Key::ok);
        press_key(app, c, Key::ok);
        press_key(app, c, Key::up);
        press_key(app, c, Key::ok); // Start saving a new page address.
        if (!app.models().busy() || (c.multiswitch_states[1] != 0x40) || !c.tx_valid) {
            std::fprintf(stderr, "MultiSwitch save: busy=%d state=%u screen=%u valid=%d\n",
                         app.models().busy(), c.multiswitch_states[1],
                         static_cast<unsigned>(app.menu().screen()), c.tx_valid);
            return false;
        }
        c.key = Key::none;
        c.samples[0] = 2048;
        (void)step(app, c, c.now + 1000U);
        if (!app.models().busy() || (c.multiswitch_states[1] != 0)) {
            std::fprintf(stderr, "MultiSwitch busy source: busy=%d state=%u\n", app.models().busy(),
                         c.multiswitch_states[1]);
            return false; // Sources must still follow inputs while EEPROM is being written.
        }
    }
    return c.tx_valid;
#endif
}

bool trim_controls() {
    coretx::Model model{};
    coretx::ControlSetup setup{};
    coretx::firmware::TrimKeys keys{};
    if (keys.update(model, setup, 0, coretx::TimestampUs{0}) ||
        !keys.update(model, setup, 1, coretx::TimestampUs{1000}) || (model.trims[0].value() != 1) ||
        keys.update(model, setup, 1, coretx::TimestampUs{400999}) ||
        !keys.update(model, setup, 1, coretx::TimestampUs{401000}) ||
        (model.trims[0].value() != 2) ||
        !keys.update(model, setup, 2, coretx::TimestampUs{402000}) ||
        (model.trims[0].value() != 1) || keys.update(model, setup, 3, coretx::TimestampUs{403000}))
        return false;
    (void)keys.update(model, setup, 0, coretx::TimestampUs{404000});
    setup.mapping[1].physical_index = 2;
    setup.mapping[2].physical_index = 1;
    if (!keys.update(model, setup, 4, coretx::TimestampUs{405000}) || (model.trims[2].value() != 1))
        return false;
    model.trim_switches = 2;
    (void)keys.update(model, setup, 0, coretx::TimestampUs{406000});
    if (keys.update(model, setup, 4, coretx::TimestampUs{407000}) ||
        keys.update(model, setup, 4, coretx::TimestampUs{807000}) || (model.trims[2].value() != 1))
        return false;
    keys.reset();
    if (keys.update(model, setup, 1, coretx::TimestampUs{808000}) ||
        keys.update(model, setup, 1, coretx::TimestampUs{1'808000}))
        return false;
    (void)keys.update(model, setup, 0, coretx::TimestampUs{1'809000});
    model.trims[0] = coretx::TrimSteps::constant<127>();
    return !keys.update(model, setup, 1, coretx::TimestampUs{1'810000}) &&
           (model.trims[0].value() == 127);
}

bool trim_application() {
    if constexpr (coretx::firmware::features::i6s)
        return true;
    for (const bool switches : {false, true}) {
        struct Scenario {};
        using Test = coretx::test::FakeApplication<Scenario, Context, SenderBoard>;
        Test::reset();
        auto &c = Test::context;
        c.probe_state = x::StorageProbeState::found;
        c.eeprom.fill(0xFF);
        c.adc_event = x::ScanEvent::complete;
        c.samples.fill(2048);
        coretx::storage::State calibration{x::diagnostic_model, {}};
        calibration.setup.calibration.fill({0, 2048, 4095});
        calibration.setup.mapping[0].reversed = calibration.setup.mapping[1].reversed = true;
        coretx::storage::Record record{};
        if (!coretx::storage::encode(calibration, 0, record))
            return false;
        std::copy(record.begin(), record.end(), c.eeprom.begin());
        x::models::Data data{};
        data.radio.transmitter = {0x12345678};
        data.model.inputs.resize<1>();
        data.model.inputs[0].source = switches ? coretx::Source{coretx::SourceKind::maximum, 0}
                                               : coretx::Source{coretx::SourceKind::analog, 1};
        data.model.inputs[0].trim = coretx::TrimSource::stick1;
        data.model.mixes.resize<1>();
        data.model.mixes[0].destination = 7;
        data.model.trim_switches = switches ? 1 : 0;
        if (switches) {
            if constexpr (coretx::firmware::features::crsf)
                data.radio.output = coretx::firmware::RadioOutput::external;
            data.model.trims[0] = coretx::TrimSteps::constant<10>();
            data.model.mixes[0].condition = {true, coretx::physical_switch_count,
                                             coretx::SwitchPosition::down};
        }
        x::models::Record model_record{};
        if (!x::models::encode(data, 0, true, 0, model_record))
            return false;
        std::copy(model_record.begin(), model_record.end(), c.eeprom.begin() + x::models::base);
        x::Application<Test::Board> app{};
        (void)app.initialize();
        for (unsigned i = 0; i < 300; ++i)
            (void)step(app, c, c.now + 1000U);
        if (!app.diagnostics().output_valid())
            return false;
        if constexpr (coretx::firmware::features::crsf)
            if (switches && (app.output() != coretx::firmware::RadioOutput::external))
                return false;
        const auto before = c.eeprom;
        c.trim_buttons = 1;
        (void)step(app, c, c.now + 1000U);
        if ((app.diagnostics().evaluation().frame.channels[7] != (switches ? 1024 : 2)) ||
            (c.eeprom != before))
            return false;
        c.trim_buttons = 0;
        (void)step(app, c, c.now + 1000U);
        for (unsigned i = 0; i < 300; ++i)
            (void)step(app, c, c.now + 1000U);
        if ((c.eeprom != before) ||
            (app.diagnostics().evaluation().frame.channels[7] != (switches ? 0 : 2)))
            return false;
        for (unsigned i = 0; i < 400; ++i)
            (void)step(app, c, c.now + 1000U);
        const auto offset =
            x::models::base + app.models().entries()[0].bank * x::models::record_size;
        std::copy_n(c.eeprom.begin() + static_cast<std::ptrdiff_t>(offset), model_record.size(),
                    model_record.begin());
        x::models::Info info{};
        if ((x::models::decode(model_record, 0, data.radio.transmitter, data, info) != 1) ||
            (data.model.trims[0].value() != (switches ? 10 : 1)) ||
            (switches && (c.eeprom != before)))
            return false;
        if (!switches) {
            const auto saved = c.eeprom;
            const auto press = [&](const Key key) {
                c.key = key;
                (void)step(app, c, c.now + 1000U);
            };
            press(Key::ok_long);
            press(Key::down);
            press(Key::down);
            press(Key::ok);
            press(Key::down);
            press(Key::ok);
            if (app.diagnostics().phase() == x::CalibrationPhase::idle)
                return false;
            c.trim_buttons = 1;
            for (unsigned i = 0; i < 6; ++i)
                (void)step(app, c, c.now + 100'000U);
            if ((app.models().data().model.trims[0].value() != 1) || (c.eeprom != saved))
                return false;
            press(Key::cancel);
            if ((app.diagnostics().phase() != x::CalibrationPhase::idle) ||
                (app.models().data().model.trims[0].value() != 1))
                return false;
            c.trim_buttons = 0;
            (void)step(app, c, c.now + 1000U);
            c.trim_buttons = 1;
            (void)step(app, c, c.now + 1000U);
            if (app.models().data().model.trims[0].value() != 2)
                return false;
        }
    }
    return true;
}

} // namespace

int main() {
    bool passed = true;
#define RUN(test)                                                                                  \
    if (!(test)) {                                                                                 \
        std::fprintf(stderr, "FAIL: %s\n", #test);                                                 \
        passed = false;                                                                            \
    }
    RUN(touch_lifecycle());
    RUN(trim_controls());
    RUN(trim_application());
    RUN(calibration_reboot<SenderBoard>());
    RUN(calibration_reboot<TouchSenderBoard>());
    RUN(startup_errors());
    RUN(sender_lifecycle());
    RUN(sender_fatal_priority<SenderBoard>());
    RUN(sender_fatal_priority<TouchSenderBoard>());
    RUN(shared_touch_sender());
    RUN(shared_touch_menus());
    RUN(multiswitch_dashboard<SenderBoard>());
    RUN(multiswitch_dashboard<TouchSenderBoard>());
    RUN(input_setup_recovery<SenderBoard>());
    RUN(input_setup_recovery<TouchSenderBoard>());
    RUN(display_refresh_interval());
    RUN(overview_keeps_channels_live());
    RUN(failed_input_storage_blocks_rf());
    RUN(exclusive_outputs());
    RUN(channel_mode_persistence());
    RUN(unified_sender_telemetry());
    RUN(transmitter_menu_models<SenderBoard>());
    RUN(transmitter_menu_models<TouchSenderBoard>());
    RUN(expanded_input_reboot());
    RUN(stored_adc_extension_startup<SenderBoard>());
    RUN(stored_adc_extension_startup<TouchSenderBoard>());
    RUN(logical_model_lifecycle<SenderBoard>());
    RUN(logical_model_lifecycle<TouchSenderBoard>());
    RUN(dashboard_touch_configuration());
#if CORETX_CRSF
    RUN(crsf_configuration_menu<SenderBoard>());
    RUN(crsf_configuration_menu<TouchSenderBoard>());
    RUN(crsf_configuration_menu<SenderBoard>(true));
    RUN(crsf_configuration_menu<TouchSenderBoard>(true));
#endif
    RUN(static_sender_isolation_and_reset());
#undef RUN
    return passed ? 0 : 1;
}
