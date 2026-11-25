// SPDX-License-Identifier: GPL-2.0-or-later
#include "boards/common/application.hpp"
#include "boards/i6s/power.hpp"
#include "boards/i6s/setup_controls.hpp"
#include "boards/i6s/touch_input.hpp"
#include "platform/stm32f072/crsf.hpp"
#include "static_fixture.hpp"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

namespace s = coretx::firmware::i6s;
namespace fw = coretx::firmware;
using coretx::firmware::CrsfError;
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
    std::uint8_t rear_pressed{};
    bool setup_leds{true};
    bool setup_active{};
    fw::AdcError adc_error{};
    fw::ScanEvent adc_event{fw::ScanEvent::pending};
    std::array<std::uint16_t, fw::sender_inputs.size()> samples{};
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
    bool power_button{};
    unsigned power_releases{};
    std::vector<std::uint32_t> farewell_hz{};
    unsigned memory_delay{};
    unsigned memory_remaining{};
    bool memory_fail_writes{};
    fw::I2cError memory_error{};
    bool receiver_changed{};
    bool tx_valid{};
    bool extra_analog{};
    std::uint8_t extra_digital{};
    std::uint8_t digital_pressed{};
    unsigned center_clicks{};
    unsigned multiswitch_clicks{};
    std::uint32_t sampled_at{};
    coretx::afhds2a::Channels radio_channels{};
    coretx::TimestampUs radio_sampled_at{};
    bool radio_valid{};
    fw::StorageProbeState probe_state{fw::StorageProbeState::not_found};
    coretx::crsf::Telemetry telemetry{};
    coretx::firmware::CrsfClient crsf_config{};
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
    static void configure(std::uint8_t enabled) {
        auto &c = Test::context;
        c.extra_digital = enabled;
    }
    static std::uint8_t read() {
        auto &c = Test::context;
        return c.digital_pressed;
    }
    static void service(coretx::TimestampUs, bool click,
                        fw::ClickTone tone = fw::ClickTone::control) {
        auto &c = Test::context;
        if (click) {
            if (tone == fw::ClickTone::multiswitch)
                ++c.multiswitch_clicks;
            else
                ++c.center_clicks;
        }
    }
    static void play(coretx::TimestampUs, std::uint32_t hz, coretx::DurationUs) {
        Test::context.farewell_hz.push_back(hz);
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
    static fw::AdcError initialize() {
        auto &c = Test::context;
        return c.adc_error;
    }
    static fw::AdcError error() {
        auto &c = Test::context;
        return c.adc_error;
    }
    static fw::ScanEvent service(coretx::TimestampUs now) {
        auto &c = Test::context;
        ++c.adc_calls;
        if (c.adc_enabling) {
            const auto elapsed = now - coretx::TimestampUs{c.adc_started_at};
            if (elapsed >= coretx::DurationUs{10'000}) {
                c.adc_error = fw::AdcError::ready;
                return fw::ScanEvent::failed;
            }
            if (elapsed >= coretx::DurationUs{2})
                c.adc_enabling = false;
            return fw::ScanEvent::pending;
        }
        return c.adc_event;
    }
    static const auto &samples() {
        auto &c = Test::context;
        return c.samples;
    }
    static fw::AdcError set_extended(bool enabled) {
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
    static fw::StorageProbeState state() {
        auto &c = Test::context;
        return c.probe_state;
    }
    static fw::I2cError error() {
        auto &c = Test::context;
        return c.probe_state == fw::StorageProbeState::failed ? fw::I2cError::bus
                                                              : fw::I2cError::none;
    }
    static std::uint8_t acknowledged() { return 0; }
    static fw::StorageProbeResult result() {
        auto &c = Test::context;
        return {state(), error(),
                static_cast<std::uint8_t>(c.probe_state == fw::StorageProbeState::found ? 1 : 0)};
    }
};

template <typename Test> struct Memory {
    inline static constexpr std::size_t page_size = 64;
    static void select(std::uint8_t address) {
        if (address != 0x50)
            std::abort();
    }
    static void service(coretx::TimestampUs) {
        auto &c = Test::context;
        if (c.memory_remaining != 0)
            --c.memory_remaining;
    }
    static bool busy() { return Test::context.memory_remaining != 0; }
    static fw::I2cError error() { return Test::context.memory_error; }
    static bool read(std::uint16_t address, std::span<std::uint8_t> bytes) {
        auto &c = Test::context;
        c.memory_error = fw::I2cError::none;
        c.memory_remaining = c.memory_delay;
        if ((address + bytes.size()) > c.eeprom.size())
            std::abort();
        std::copy_n(c.eeprom.begin() + address, bytes.size(), bytes.begin());
        return true;
    }
    static bool write(std::uint16_t address, std::span<const std::uint8_t> bytes) {
        auto &c = Test::context;
        c.memory_remaining = c.memory_delay;
        c.memory_error = c.memory_fail_writes ? fw::I2cError::timeout : fw::I2cError::none;
        if (c.memory_fail_writes)
            return true;
        if ((address + bytes.size()) > c.eeprom.size())
            std::abort();
        std::copy(bytes.begin(), bytes.end(), c.eeprom.begin() + address);
        return true;
    }
};

template <typename Test> struct ExternalRFModule {
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
        if (index >= coretx::firmware::crsf_baudrates.size())
            return false;
        c.baud_index = index;
        return true;
    }
    static CrsfError service(coretx::TimestampUs now, const coretx::crsf::Channels &channels,
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
        return CrsfError::none;
    }
    static const coretx::crsf::Telemetry &telemetry() {
        auto &c = Test::context;
        return c.telemetry;
    }
    static coretx::firmware::CrsfClient &config() { return Test::context.crsf_config; }
    static CrsfError error() { return CrsfError::none; }
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
    using ExternalRFModule = std::conditional_t<fw::features::crsf, ::ExternalRFModule<Test>, void>;
    using InternalRFModule =
        std::conditional_t<fw::features::a7105, typename Platform<Test>::Internal, void>;
    using Radio = std::conditional_t<fw::features::a7105, typename Platform<Test>::Radio, void>;
    using Input = coretx::firmware::PhysicalInput<Keys>;
    static void reset() noexcept {}
};
template <typename Test> struct TouchSenderBoard : SenderBoard<Test> {
    using Input = coretx::firmware::TouchInput<::Touch<Test>>;
    using Power = coretx::firmware::i6s::PowerButton;
    inline static constexpr std::string_view name = "I6S";
    inline static constexpr bool sb_three_position = true;
    static bool power_button_pressed() { return Test::context.power_button; }
    static void release_power() { ++Test::context.power_releases; }
};

template <typename Test> struct SetupControls : s::SetupNavigation {
    void initialize() {
        static_cast<s::SetupNavigation &>(*this) = {};
        Test::context.setup_leds = true;
        Test::context.setup_active = false;
    }
    void set_mode(const coretx::TimestampUs now, const bool active) {
        s::SetupNavigation::set_mode(now, active);
        Test::context.setup_leds = led_on();
        Test::context.setup_active = active;
    }
    Key service(const coretx::TimestampUs now, const bool enabled, const std::int16_t vra,
                const std::int16_t vrb, const bool jog) {
        const auto key =
            s::SetupNavigation::service(now, Test::context.rear_pressed, enabled, vra, vrb, jog);
        Test::context.setup_leds = led_on();
        return key;
    }
};

template <typename Test> struct SetupSenderBoard : TouchSenderBoard<Test> {
    using Input = fw::TouchInput<::Touch<Test>, ::SetupControls<Test>>;
};

template <typename Test> struct DisabledModuleBoard : SenderBoard<Test> {
    using InternalRFModule = void;
    using ExternalRFModule = void;
    using Radio = void;
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

bool disabled_module_types() {
    struct Scenario {};
    using Test = coretx::test::FakeApplication<Scenario, Context, DisabledModuleBoard>;
    Test::reset();
    auto &c = Test::context;
    c.eeprom.fill(0xFF);
    c.samples.fill(2000);
    c.adc_event = fw::ScanEvent::complete;
    fw::Application<Test::Board> app{};
    if (app.initialize() != fw::ApplicationState::starting)
        return false;
    for (unsigned i = 0; i < 600; ++i)
        if (step(app, c, c.now + 1000U) != fw::ApplicationState::running)
            return false;
    if (!app.models().loaded())
        return false;
    if constexpr (fw::features::a7105)
        set_output(app, c, fw::RadioOutput::internal);
    if constexpr (fw::features::crsf)
        set_output(app, c, fw::RadioOutput::external);
    return (app.output() == fw::RadioOutput::off) && !c.powered && !c.internal_active &&
           (c.radio_initializations == 0) && (c.radio_calls == 0) && (c.tx_calls == 0);
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
        c.adc_event = fw::ScanEvent::complete;
        c.samples.fill(2000);
        fw::Application<Test::Board, Test::Board::Input> app{};
        if ((app.initialize() != fw::ApplicationState::starting) || (c.touch_initializations != 1))
            return false;
        c.touch_ready = true;
        c.touch_event = s::TouchEvent::sample;
        if ((step(app, c, base) != fw::ApplicationState::running) ||
            (app.diagnostics().phase() != fw::CalibrationPhase::idle) || (c.touch_calls != 1) ||
            (c.display_calls != 1))
            return false;
        c.finish_frame = true;
        // Touch received while the first display frame was busy must not arm a gesture.
        c.touch_event = s::TouchEvent::none;
        if (step(app, c, base + 1U) != fw::ApplicationState::running)
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
        if ((app.diagnostics().phase() != fw::CalibrationPhase::idle) || app.menu().visible())
            return false;
        (void)step(app, c, base + 4U);
        if (app.diagnostics().phase() != fw::CalibrationPhase::idle)
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
        if ((app.diagnostics().phase() != fw::CalibrationPhase::center) || app.menu().visible())
            return false;
        const auto draws = c.draws;
        c.touch_error = s::TouchError::nack;
        c.touch_ready = false;
        c.touch_event = s::TouchEvent::failed;
        if (step(app, c, c.now + 1U) != fw::ApplicationState::running)
            return false;
        (void)step(app, c, c.now + 50'000U);
        if ((app.diagnostics().phase() != fw::CalibrationPhase::center) ||
            (c.draws != (draws + 1)) || (c.touch_calls != c.display_calls) || (c.key_calls != 0) ||
            c.powered || c.internal_active)
            return false;

        c.touch_error = s::TouchError::none;
        c.touch_ready = true;
        c.touch_event = s::TouchEvent::sample;
        c.touch_sample.gesture = s::Gesture::right;
        (void)step(app, c, c.now + 1U);
        if (app.diagnostics().phase() != fw::CalibrationPhase::center)
            return false;
        c.touch_sample.contacts = 0;
        c.touch_sample.gesture = s::Gesture::none;
        (void)step(app, c, c.now + 1U);
        c.touch_sample = {0, 0, 64, 32, 1, s::Gesture::none};
        (void)step(app, c, c.now + 1U);
        c.touch_sample.x = 80;
        (void)step(app, c, c.now + 1U);
        if ((app.diagnostics().phase() != fw::CalibrationPhase::range) || (c.key_calls != 0))
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
    touch.adc_event = fw::ScanEvent::complete;
    touch.samples.fill(2000);
    fw::Application<TouchTest::Board, TouchTest::Board::Input> touch_app{};
    if (touch_app.initialize() != fw::ApplicationState::starting)
        return false;
    const auto initial_draws = touch.draws;
    touch.touch_error = s::TouchError::timeout;
    touch.touch_event = s::TouchEvent::failed;
    if ((step(touch_app, touch, 1) != fw::ApplicationState::running) ||
        (touch.draws != initial_draws))
        return false;
    touch.finish_frame = true;
    if ((step(touch_app, touch, 50'001) != fw::ApplicationState::running) ||
        (touch.draws != (initial_draws + 1)) ||
        (touch_app.diagnostics().phase() != fw::CalibrationPhase::idle) || (touch.key_calls != 0) ||
        touch.powered || touch.internal_active)
        return false;

    struct SenderTestScenario {};
    using SenderTest = coretx::test::FakeApplication<SenderTestScenario, Context, SenderBoard>;
    SenderTest::reset();
    auto &sender = SenderTest::context;
    sender.adc_error = fw::AdcError::calibration;
    fw::Application<SenderTest::Board> sender_app{};
    if (sender_app.initialize() != fw::ApplicationState::input_error)
        return false;
    sender.key = Key::down;
    return (step(sender_app, sender, 1) == fw::ApplicationState::input_error) && !sender.powered &&
           !sender.tx_valid && (sender.tx_calls == 0) && (sender_app.diagnostics().page() == 1);
}

bool sender_lifecycle() {
    struct TestScenario {};
    using Test = coretx::test::FakeApplication<TestScenario, Context, SenderBoard>;
    Test::reset();
    auto &c = Test::context;
    c.probe_state = fw::StorageProbeState::found;
    c.eeprom.fill(0xFF);
    c.samples.fill(2000);
    for (const auto index : fw::switch_indices)
        c.samples[index] = 0;
    fw::Application<typename Test::Board> app{};
    if (app.initialize() != fw::ApplicationState::starting)
        return false;
    (void)step(app, c, 0);
    if ((app.state() != fw::ApplicationState::starting) || c.tx_valid)
        return false;
    c.adc_event = fw::ScanEvent::complete;
    if ((step(app, c, 5'000) != fw::ApplicationState::running) || c.tx_valid)
        return false;
    settle_menu(app, c);
    open_sender_setup(app, c);
    press_key(app, c, Key::down);
    press_key(app, c, Key::ok);
    c.key = Key::ok;
    (void)step(app, c, 10'000);
    (void)step(app, c, 15'000);
    c.key = Key::none;
    for (const auto index : fw::analog_indices)
        c.samples[index] = 400;
    (void)step(app, c, 20'000);
    for (const auto index : fw::analog_indices)
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
    c.adc_error = fw::AdcError::overrun;
    c.adc_event = fw::ScanEvent::failed;
    c.key = Key::down;
    if ((step(app, c, 35'000) != fw::ApplicationState::input_error) || c.tx_valid ||
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
        fw::Application<typename Test::Board, typename Test::Board::Input> app{};
        if (kind == 0)
            c.hardware_error = HardwareError::pll_ready;
        if (kind == 1)
            c.lcd_init_error = LcdError::frame_busy;
        const auto initialized = app.initialize();
        if (kind < 2) {
            if ((initialized != fw::ApplicationState::fatal) || c.powered ||
                (c.touch_initializations != 0) ||
                (app.failure() !=
                 (kind == 0 ? fw::ApplicationFailure::hardware : fw::ApplicationFailure::lcd)))
                return false;
            continue;
        }
        c.key = Key::ok;
        c.touch_ready = true;
        c.touch_event = s::TouchEvent::sample;
        c.touch_sample = {0, 0, 80, 32, 1, s::Gesture::right};
        c.adc_event = fw::ScanEvent::complete;
        if (kind == 2)
            c.lcd_error = LcdError::frame_busy;
        if (kind == 3)
            c.stack_ok = false;
        if (kind == 4)
            c.break_stack_on_display = true;
        if ((step(app, c, 1) != fw::ApplicationState::fatal) || c.powered || c.internal_active ||
            (c.key_calls != 0) || (c.touch_calls != 0) || (c.adc_calls != 0) || (c.tx_calls != 0) ||
            (app.diagnostics().phase() != fw::CalibrationPhase::idle) ||
            (app.failure() !=
             (kind == 2 ? fw::ApplicationFailure::lcd : fw::ApplicationFailure::stack)))
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
    c.adc_event = fw::ScanEvent::complete;
    c.samples.fill(2000);
    fw::Application<Test::Board, Test::Board::Input> app{};
    if ((app.initialize() == fw::ApplicationState::fatal) ||
        (step(app, c, 0) != fw::ApplicationState::running) ||
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
    // Each recognized stroke is confirmed audibly while the trim beep is enabled.
    if ((app.diagnostics().phase() != fw::CalibrationPhase::idle) ||
        (app.diagnostics().page() != fw::Diagnostics::system_page) || (c.center_clicks != 2))
        return false;
    c.touch_error = s::TouchError::nack;
    c.touch_ready = false;
    (void)step(app, c, 50'001);
    return (c.key_calls == 0) && (c.touch_calls == 8) && (c.adc_calls == 8) && !c.powered &&
           (app.diagnostics().phase() == fw::CalibrationPhase::idle);
}

bool shared_touch_menus() {
    struct TestScenario {};
    using Test = coretx::test::FakeApplication<TestScenario, Context, TouchSenderBoard>;
    Test::reset();
    auto &c = Test::context;
    c.touch_ready = true;
    c.touch_event = s::TouchEvent::sample;
    c.touch_sample = {0, 0, 64, 32, 1, s::Gesture::none};
    fw::Application<Test::Board, Test::Board::Input> app{};
    (void)app.initialize();
    for (unsigned now = 0; now <= 800000; now += 2000) {
        c.touch_event = now % 242000 == 0 ? s::TouchEvent::sample : s::TouchEvent::none;
        (void)step(app, c, now);
    }
    if (!app.menu().visible() || (app.menu().screen() != fw::MenuScreen::root) ||
        (app.diagnostics().phase() != fw::CalibrationPhase::idle))
        return false;
    c.touch_sample.contacts = 0;
    c.touch_event = s::TouchEvent::sample;
    (void)step(app, c, 820000);
    c.touch_sample.contacts = 1;
    c.touch_sample.x = 80;
    (void)step(app, c, 840000);
    c.touch_sample.x = 64;
    (void)step(app, c, 860000);
    return !app.menu().visible() && (c.key_calls == 0);
}

bool safe_power_off() {
    for (const bool failure : {false, true}) {
        struct Scenario {};
        using Test = coretx::test::FakeApplication<Scenario, Context, TouchSenderBoard>;
        Test::reset();
        auto &c = Test::context;
        c.eeprom.fill(0xFF);
        c.probe_state = fw::StorageProbeState::found;
        c.power_button = true;
        c.adc_event = fw::ScanEvent::complete;
        c.samples.fill(2000);
        fw::Application<typename Test::Board> app{};
        if (app.initialize() == fw::ApplicationState::fatal)
            return false;
        settle_menu(app, c);
        if (app.models().busy() || !app.models().editable())
            return false;
        if constexpr (fw::features::crsf)
            set_output(app, c, fw::RadioOutput::external);
        else if constexpr (fw::features::a7105)
            set_output(app, c, fw::RadioOutput::internal);
        (void)step(app, c, c.now + 2'000'000U);
        if (c.power_releases != 0)
            return false;
        c.power_button = false;
        (void)step(app, c, c.now + 1U);
        (void)step(app, c, c.now + 30'000U);
        if constexpr (!fw::features::secure_ui) {
            press_key(app, c, Key::ok_long);
            press_key(app, c, Key::down);
            press_key(app, c, Key::ok);
            press_key(app, c, Key::ok);
            press_key(app, c, Key::ok);
            press_key(app, c, Key::down);
            press_key(app, c, Key::ok);
            press_key(app, c, Key::down);
            c.key = Key::none;
            if (!app.menu().pending_model_edit() ||
                (app.models().data().model.inputs[0].weight_percent.value() != 99))
                return false;
        }
        c.power_button = true;
        (void)step(app, c, c.now + 1U);
        (void)step(app, c, c.now + 1'499'999U);
        if (c.power_releases != 0)
            return false;
        c.trim_buttons = 1;
        (void)step(app, c, c.now);
        c.trim_buttons = 0;
        if (!app.models().busy() || (app.models().data().model.trims[0].value() != 1))
            return false;
        c.memory_delay = 2;
        c.memory_fail_writes = failure;
        (void)step(app, c, c.now + 1U);
        if ((c.power_releases != 0) || c.powered || c.internal_active ||
            (app.output() != fw::RadioOutput::off) || app.menu().pending_model_edit() ||
            (app.models().data().model.inputs[0].weight_percent.value() != 100))
            return false;
        for (unsigned i = 0; i < 1000; ++i)
            (void)step(app, c, c.now + 1000U);
        if (failure) {
            if ((c.power_releases != 0) || (app.state() == fw::ApplicationState::powered_off) ||
                (app.output() != fw::RadioOutput::off))
                return false;
        } else {
            if ((c.power_releases != 1) || (app.state() != fw::ApplicationState::powered_off) ||
                app.models().busy())
                return false;
            const auto index = app.models().active();
            const auto bank = app.models().entries()[index].bank;
            fw::models::Record record{};
            std::copy_n(c.eeprom.begin() + fw::models::base +
                            (index * 2 + bank) * fw::models::record_size,
                        record.size(), record.begin());
            fw::models::Data restored{};
            fw::models::Info info{};
            if ((fw::models::decode(record, index, Test::Board::transmitter_id(), restored, info) !=
                 1) ||
                (restored.model.trims[0].value() != 1) ||
                (restored.model.inputs[0].weight_percent.value() != 100))
                return false;
            const auto saved_output = fw::features::crsf    ? fw::RadioOutput::external
                                      : fw::features::a7105 ? fw::RadioOutput::internal
                                                            : fw::RadioOutput::off;
            if (restored.radio.output != saved_output)
                return false;
            const auto adc_calls = c.adc_calls;
            (void)step(app, c, c.now + 2'000'000U);
            if ((c.power_releases != 1) || (c.adc_calls != adc_calls))
                return false;
        }
    }
    return true;
}

bool power_off_during_load() {
    struct Scenario {};
    using Test = coretx::test::FakeApplication<Scenario, Context, TouchSenderBoard>;
    Test::reset();
    auto &c = Test::context;
    c.eeprom.fill(0xFF);
    c.probe_state = fw::StorageProbeState::scanning;
    c.adc_event = fw::ScanEvent::complete;
    c.samples.fill(2048);
    c.touch_ready = true;
    c.touch_event = s::TouchEvent::sample;
    coretx::ControlSetup calibration{};
    calibration.calibration.fill({0, 2048, 4095});
    calibration.mapping[0].reversed = calibration.mapping[1].reversed = true;
    const auto calibration_record = coretx::firmware::calibration_storage::encode(calibration, 0);
    std::copy(calibration_record.begin(), calibration_record.end(),
              c.eeprom.begin() + coretx::firmware::calibration_storage::base_address);
    fw::models::Data data{};
    data.radio.transmitter = {0x12345678};
    data.radio.output = fw::features::crsf    ? fw::RadioOutput::external
                        : fw::features::a7105 ? fw::RadioOutput::internal
                                              : fw::RadioOutput::off;
    fw::models::Record record{};
    if (!fw::models::encode(data, 0, true, 0, record))
        return false;
    std::copy(record.begin(), record.end(), c.eeprom.begin() + fw::models::base);
    fw::Application<typename Test::Board, typename Test::Board::Input> app{};
    (void)app.initialize();
    (void)step(app, c, 1);
    (void)step(app, c, 30'001);
    c.power_button = true;
    (void)step(app, c, 30'002);
    (void)step(app, c, 1'530'002);
    if (c.power_releases != 0)
        return false;
    c.probe_state = fw::StorageProbeState::found;
    for (unsigned i = 0; i < 1000; ++i) {
        (void)step(app, c, c.now + 1000U);
        // The farewell tones and the collapse animation finish before the supply is released.
        if ((c.power_releases != 0) && (i < 790)) {
            std::fprintf(stderr, "Shutdown released before animation: iteration=%u\n", i);
            return false;
        }
        if (c.powered || c.internal_active || (app.output() != fw::RadioOutput::off)) {
            std::fprintf(stderr, "Shutdown load RF: iteration=%u output=%u\n", i,
                         static_cast<unsigned>(app.output()));
            return false;
        }
    }
    return (c.power_releases == 1) && (app.state() == fw::ApplicationState::powered_off) &&
           (app.models().data().radio.output == data.radio.output) &&
           (c.farewell_hz == std::vector<std::uint32_t>{1'760, 1'320, 880});
}

template <template <typename> typename BoardPolicy> bool input_setup_recovery() {
    struct Scenario {};
    using Test = coretx::test::FakeApplication<Scenario, Context, BoardPolicy>;
    using Board = typename Test::Board;
    Test::reset();
    auto &c = Test::context;
    c.probe_state = fw::StorageProbeState::found;
    c.eeprom.fill(0xFF);
    const auto base = coretx::firmware::input_storage::base_address;
    std::fill_n(c.eeprom.begin() + base, 256, 0);
    coretx::ControlSetup legacy{};
    for (auto &axis : legacy.calibration)
        axis = {0, 2048, 4095};
    legacy.calibration[4] = {100, 1000, 3500};
    legacy.mapping[0].reversed = legacy.mapping[1].reversed = true;
    const auto calibration = coretx::firmware::calibration_storage::encode(legacy, 0);
    std::copy(calibration.begin(), calibration.end(),
              c.eeprom.begin() + coretx::firmware::calibration_storage::base_address);
    fw::Application<Board> app{};
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
               ? app.diagnostics().input_setup().extra_analog
               : app.diagnostics().input_setup().modes[0] == coretx::InputMode::three_position;
}

bool display_refresh_interval() {
    struct Scenario {};
    using Test = coretx::test::FakeApplication<Scenario, Context, SenderBoard>;
    Test::reset();
    auto &c = Test::context;
    fw::Application<typename Test::Board> app{};
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
    c.probe_state = fw::StorageProbeState::found;
    c.eeprom.fill(0xFF);
    c.adc_event = fw::ScanEvent::complete;
    c.samples.fill(2048);
    coretx::ControlSetup calibration{};
    calibration.calibration.fill({0, 2048, 4095});
    calibration.mapping[0].reversed = calibration.mapping[1].reversed = true;
    const auto record = coretx::firmware::calibration_storage::encode(calibration, 0);
    std::copy(record.begin(), record.end(),
              c.eeprom.begin() + coretx::firmware::calibration_storage::base_address);
    fw::models::Data model{};
    model.radio.transmitter = {0x12345678};
    model.radio.receiver = {0x87654321};
    model.radio.output = coretx::firmware::RadioOutput::internal;
    fw::models::Record model_record{};
    if (!fw::models::encode(model, 0, true, 0, model_record))
        return false;
    std::copy(model_record.begin(), model_record.end(), c.eeprom.begin() + fw::models::base);
    fw::Application<typename Test::Board> app{};
    (void)app.initialize();
    for (unsigned i = 0; i < 300; ++i)
        (void)step(app, c, c.now + 1000U);
    if (!app.diagnostics().output_valid() || !c.internal_active)
        return false;
    for (unsigned i = 0; i < 2; ++i) {
        press_key(app, c, Key::ok);
        if ((app.output() != coretx::firmware::RadioOutput::internal) || c.internal_binding ||
            (app.diagnostics().phase() != fw::CalibrationPhase::idle) ||
            (c.internal_mode != coretx::afhds2a::ChannelMode::standard14) || (c.baud_index != 1))
            return false;
        press_key(app, c, Key::down);
    }
    if (app.diagnostics().page() != fw::Diagnostics::overview_page)
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
    c.probe_state = fw::StorageProbeState::failed;
    fw::Application<typename Test::Board> app{};
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
    c.probe_state = fw::StorageProbeState::found;
    c.eeprom.fill(0xFF);
    fw::Application<typename Test::Board> app{};
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
    c.probe_state = fw::StorageProbeState::found;
    c.eeprom.fill(0xFF);
    fw::Application<typename Test::Board> app{};
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
    reboot.probe_state = fw::StorageProbeState::found;
    reboot.eeprom = c.eeprom;
    fw::Application<RebootTest::Board> restored{};
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
    fw::ModelStorage<RebootTest::Board::Memory> journal{};
    journal.initialize({0x12345678});
    for (unsigned i = 0; i < 1000; ++i)
        journal.service(coretx::TimestampUs{i}, RebootTest::Board::Probe::result(),
                        fw::diagnostic_model, coretx::firmware::RadioSettings{{0x12345678}});
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
    c.probe_state = fw::StorageProbeState::found;
    c.eeprom.fill(0xFF);
    c.samples.fill(2000);
    c.samples[fw::battery_index] = 2526;
    c.adc_event = fw::ScanEvent::complete;
    fw::Application<typename Test::Board> app{};
    (void)app.initialize();
    (void)step(app, c, 100);
    const auto *battery = app.telemetry().find(tm::transmitter_battery);
    if ((battery == nullptr) || (battery->value != 6200) ||
        (battery->updated_at != coretx::TimestampUs{100}))
        return false;
    c.adc_event = fw::ScanEvent::pending;
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
    if (app.diagnostics().phase() != fw::CalibrationPhase::idle)
        return false;
    c.adc_error = fw::AdcError::overrun;
    c.adc_event = fw::ScanEvent::failed;
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
    c.probe_state = fw::StorageProbeState::found;
    c.eeprom.fill(0xFF);
    c.adc_event = fw::ScanEvent::complete;
    c.samples.fill(2048);
    c.samples[0] = 0;
    coretx::ControlSetup legacy{};
    for (auto &axis : legacy.calibration)
        axis = {0, 2048, 4095};
    legacy.mapping[0].reversed = legacy.mapping[1].reversed = true;
    const auto calibration = coretx::firmware::calibration_storage::encode(legacy, 0);
    std::copy(calibration.begin(), calibration.end(),
              c.eeprom.begin() + coretx::firmware::calibration_storage::base_address);
    fw::models::Data first{}, second{};
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
    second.model = fw::models::four_channel_model();
    second.radio.receiver = {0x10203040};
    second.radio.channel_mode = coretx::afhds2a::ChannelMode::extended16;
    second.radio.output =
        coretx::firmware::features::a7105  ? coretx::firmware::RadioOutput::internal
        : coretx::firmware::features::crsf ? coretx::firmware::RadioOutput::external
                                           : coretx::firmware::RadioOutput::off;
    fw::models::default_name(second.name, 1);
    fw::models::Record record{};
    if (!fw::models::encode(first, 0, true, 0, record))
        return false;
    std::copy(record.begin(), record.end(), c.eeprom.begin() + fw::models::base);
    if (!fw::models::encode(second, 1, true, 0, record))
        return false;
    std::copy(record.begin(), record.end(),
              c.eeprom.begin() + fw::models::base + 2 * fw::models::record_size);
    fw::Application<Board> app{};
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
    if (!app.menu().visible() || (app.diagnostics().phase() != fw::CalibrationPhase::idle))
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
    for (std::size_t i = 0; i < fw::menu_detail::trim_row; ++i)
        press(Key::down);
    press(Key::ok);
    if (app.menu().screen() != fw::MenuScreen::trims)
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
    reboot.probe_state = fw::StorageProbeState::found;
    reboot.eeprom = c.eeprom;
    fw::Application<typename RebootTest::Board> restored{};
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
    c.probe_state = fw::StorageProbeState::found;
    c.eeprom.fill(0xFF);
    c.adc_event = fw::ScanEvent::complete;
    c.samples.fill(2048);
    fw::Application<typename Test::Board> app{};
    (void)app.initialize();
    settle_menu(app, c);
    press_key(app, c, Key::ok_long);
    press_key(app, c, Key::down);
    press_key(app, c, Key::down);
    press_key(app, c, Key::ok);
    press_key(app, c, Key::down);
    press_key(app, c, Key::ok);
    if (app.diagnostics().phase() != fw::CalibrationPhase::center)
        return false;
    press_key(app, c, Key::ok);
    if (app.diagnostics().phase() != fw::CalibrationPhase::range)
        return false;
    c.key = Key::none;
    for (const auto index : fw::analog_indices)
        c.samples[index] = 400;
    (void)step(app, c, c.now + 1000U);
    for (const auto index : fw::analog_indices)
        c.samples[index] = 3600;
    (void)step(app, c, c.now + 1000U);
    press_key(app, c, Key::ok);
    if (!app.diagnostics().calibrated() || !app.diagnostics().output_valid() ||
        (app.diagnostics().phase() != fw::CalibrationPhase::idle))
        return false;
    settle_menu(app, c);
    struct RebootScenario {};
    using Reboot = coretx::test::FakeApplication<RebootScenario, Context, BoardPolicy>;
    Reboot::reset();
    auto &reboot = Reboot::context;
    reboot.probe_state = fw::StorageProbeState::found;
    reboot.eeprom = c.eeprom;
    reboot.adc_event = fw::ScanEvent::complete;
    reboot.samples.fill(2048);
    fw::Application<typename Reboot::Board> restored{};
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
    c.probe_state = fw::StorageProbeState::found;
    c.adc_event = fw::ScanEvent::complete;
    c.samples.fill(2048);
    c.adc_reconfigure_delay = 3;
    coretx::InputSetup setup{};
    setup.extra_analog = true;
    setup.modes[6] = coretx::InputMode::pot;
    const auto record = coretx::firmware::input_storage::encode(setup, 3);
    std::copy(record.begin(), record.end(),
              c.eeprom.begin() + coretx::firmware::input_storage::base_address);
    fw::Application<typename Test::Board> app{};
    (void)app.initialize();
    for (unsigned i = 0; i < 1000; ++i)
        (void)step(app, c, c.now + 10U);
    return c.extra_analog && (c.adc_error == fw::AdcError::none) &&
           (app.state() == fw::ApplicationState::running) && app.diagnostics().valid() &&
           (app.diagnostics().input_setup() == setup);
}

bool expanded_input_reboot() {
    namespace fw = coretx::firmware;
    struct TestScenario {};
    using Test = coretx::test::FakeApplication<TestScenario, Context, TouchSenderBoard>;
    Test::reset();
    auto &c = Test::context;
    c.eeprom.fill(0xFF);
    c.probe_state = fw::StorageProbeState::found;
    coretx::InputSetup setup{true};
    setup.extra_analog = true;
    setup.set_digital_mode(0, coretx::DigitalInputMode::two_position);
    setup.set_digital_mode(1, coretx::DigitalInputMode::two_position);
    setup.modes[6] = coretx::InputMode::six_position;
    const auto record = fw::input_storage::encode(setup, 3);
    std::copy(record.begin(), record.end(), c.eeprom.begin() + fw::input_storage::base_address);
    fw::Application<typename Test::Board> app{};
    (void)app.initialize();
    for (unsigned i = 0; i < 1000; ++i)
        (void)step(app, c, i);
    if ((app.diagnostics().input_setup() != setup) ||
        (c.extra_analog != fw::features::extended_inputs) ||
        (c.extra_digital != (fw::features::extended_inputs ? 3 : 0)))
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
            if (c.extra_analog)
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
            reboot.probe_state = fw::StorageProbeState::found;
            fw::Application<RebootTest::Board> restored{};
            (void)restored.initialize();
            for (unsigned i = 0; i < 1000; ++i)
                (void)step(restored, reboot, i);
            if (reboot.extra_analog || (reboot.extra_digital != 3))
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
    c.probe_state = fw::StorageProbeState::found;
    c.eeprom.fill(0xFF);
    c.adc_event = fw::ScanEvent::complete;
    c.samples.fill(0);
    coretx::ControlSetup calibration{};
    calibration.calibration.fill({0, 2048, 4095});
    calibration.mapping[0].reversed = calibration.mapping[1].reversed = true;
    const auto record = coretx::firmware::calibration_storage::encode(calibration, 0);
    std::copy(record.begin(), record.end(),
              c.eeprom.begin() + coretx::firmware::calibration_storage::base_address);
    fw::models::Data data{};
    data.radio.transmitter = {0x12345678};
    data.radio.output = coretx::firmware::RadioOutput::off;
    auto &latch = data.model.logical_switches[0];
    latch.function = coretx::LogicalFunction::srff;
    latch.a = 0;
    latch.b = 3;
    latch.position_a = latch.position_b = coretx::SwitchPosition::down;
    fw::models::Record model_record{};
    for (std::size_t i = 0; i < 2; ++i) {
        fw::models::default_name(data.name, i);
        data.model.dashboard_channels[0] = i == 0 ? 16 : 9;
        if (!fw::models::encode(data, i, true, 0, model_record))
            return false;
        std::copy(model_record.begin(), model_record.end(),
                  c.eeprom.begin() + fw::models::base +
                      static_cast<std::ptrdiff_t>(i * 2 * fw::models::record_size));
    }
    fw::Application<typename Test::Board> app{};
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
    reboot.probe_state = fw::StorageProbeState::found;
    reboot.samples.fill(0);
    reboot.adc_event = fw::ScanEvent::complete;
    fw::Application<typename Reboot::Board> restarted{};
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
    c.probe_state = fw::StorageProbeState::found;
    c.eeprom.fill(0xFF);
    c.touch_ready = true;
    c.touch_event = s::TouchEvent::sample;
    fw::Application<Test::Board, Test::Board::Input> app{};
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
    if (app.menu().screen() != fw::MenuScreen::dashboard_channels)
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
    return (app.menu().screen() == fw::MenuScreen::logical_switches) &&
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
    first.adc_event = second.adc_event = fw::ScanEvent::complete;
    first.samples.fill(2000);
    second.samples.fill(2500);
    first.key = Key::down;
    first.eeprom[0] = 0x5A;
    fw::Application<First::Board> first_app{};
    fw::Application<Second::Board> second_app{};
    if ((first_app.initialize() != fw::ApplicationState::starting) ||
        (second_app.initialize() != fw::ApplicationState::starting) ||
        (step(first_app, first, 1) != fw::ApplicationState::running) || (first.adc_calls != 1) ||
        (first.key_calls != 1) || (second.adc_calls != 0) || (second.key_calls != 0))
        return false;
    if ((step(second_app, second, 2) != fw::ApplicationState::running) || (first.adc_calls != 1) ||
        (second.adc_calls != 1) || (first_app.diagnostics().page() != 1) ||
        (second_app.diagnostics().page() != 0))
        return false;
    first.stack_ok = false;
    if ((step(first_app, first, 3) != fw::ApplicationState::fatal) ||
        (step(second_app, second, 3) != fw::ApplicationState::running) || (first.adc_calls != 1) ||
        (second.adc_calls != 2))
        return false;
    first.extra_analog = true;
    first.extra_digital = 1;
    First::reset();
    if (!first.stack_ok || (first.adc_calls != 0) || (first.key_calls != 0) ||
        (first.display_initializations != 0) || first.extra_analog || first.extra_digital ||
        (first.eeprom[0] != 0) || (second.adc_calls != 2))
        return false;
    fw::Application<First::Board> restarted{};
    return (restarted.initialize() == fw::ApplicationState::starting) &&
           (step(restarted, first, 4) == fw::ApplicationState::starting) &&
           (first.display_initializations == 1) && (first.adc_calls == 1) &&
           (second.adc_calls == 2) && (second_app.state() == fw::ApplicationState::running);
}

#if CORETX_CRSF
template <template <typename> typename BoardPolicy>
bool crsf_configuration_menu(const bool dashboard = false) {
    struct ConfigurationMenuScenario {};
    using Test = coretx::test::FakeApplication<ConfigurationMenuScenario, Context, BoardPolicy>;
    Test::reset();
    auto &c = Test::context;
    c.probe_state = fw::StorageProbeState::found;
    c.eeprom.fill(0xFF);
    c.adc_event = fw::ScanEvent::complete;
    c.samples.fill(2048);
    coretx::ControlSetup calibration{};
    calibration.calibration.fill({0, 2048, 4095});
    calibration.mapping[0].reversed = calibration.mapping[1].reversed = true;
    const auto record = coretx::firmware::calibration_storage::encode(calibration, 0);
    std::copy(record.begin(), record.end(),
              c.eeprom.begin() + coretx::firmware::calibration_storage::base_address);
    fw::models::Data data{};
    data.radio.transmitter = {0x12345678};
    data.model.crsf_dashboard = dashboard;
    fw::models::Record model{};
    if (!fw::models::encode(data, 0, true, 0, model))
        return false;
    std::copy(model.begin(), model.end(), c.eeprom.begin() + fw::models::base);
    fw::Application<typename Test::Board> app{};
    (void)app.initialize();
    for (unsigned i = 0; i < 300; ++i)
        (void)step(app, c, c.now + 1000U);
    if (dashboard) {
        set_output(app, c, coretx::firmware::RadioOutput::external);
        press_key(app, c, Key::down);
        press_key(app, c, Key::down);
        if ((app.diagnostics().page() != fw::Diagnostics::page_count) || !app.crsf_menu().visible())
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
    c.probe_state = fw::StorageProbeState::found;
    c.eeprom.fill(0xFF);
    c.adc_event = fw::ScanEvent::complete;
    c.samples.fill(2048);
    c.touch_ready = true;
    c.touch_event = s::TouchEvent::sample;
    coretx::ControlSetup calibration{};
    calibration.calibration.fill({0, 2048, 4095});
    calibration.mapping[0].reversed = calibration.mapping[1].reversed = true;
    const auto record = coretx::firmware::calibration_storage::encode(calibration, 0);
    std::copy(record.begin(), record.end(),
              c.eeprom.begin() + coretx::firmware::calibration_storage::base_address);
    fw::models::Data model{};
    model.radio.transmitter = {0x12345678};
    model.radio.output = coretx::firmware::RadioOutput::external;
    model.model.multiswitch[1].address = 42;
    model.model.multiswitch[1].momentary = 2;
    model.model.multiswitch[3].address = 255;
    model.model.logical_switches[0].function = coretx::LogicalFunction::greater;
    model.model.multiswitch[1].controls[6].set_condition(
        {true, coretx::switch_count, coretx::SwitchPosition::down});
    model.model.crsf_dashboard = true;
    // The last diagnostic mix follows M21, the first button of page 2.
    auto &m21 = model.model.mixes[model.model.mixes.size() - 1];
    m21.source = {coretx::SourceKind::switch_position,
                  static_cast<std::uint8_t>(coretx::multiswitch_switch_base + 8), false};
    const auto m21_channel = m21.destination;
    fw::models::Record model_record{};
    if (!fw::models::encode(model, 0, true, 0, model_record))
        return false;
    std::copy(model_record.begin(), model_record.end(), c.eeprom.begin() + fw::models::base);
    fw::Application<typename Test::Board, typename Test::Board::Input> app{};
    (void)app.initialize();
    for (unsigned i = 0; i < 300; ++i)
        (void)step(app, c, c.now + 1000U);
    if (!app.diagnostics().output_valid() || !c.powered ||
        (app.diagnostics().dashboard_pages() != 5) || (c.tx_channels[m21_channel] >= 0))
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
            touch(64, 32, true, s::Gesture::left);
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
    if (c.tx_channels[m21_channel] <= 0)
        return false;
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
        touch(64, 32, true, s::Gesture::down);
        touch(64, 32, false);
    } else {
        press_key(app, c, Key::ok);
        next();
    }
    if (app.diagnostics().page() != 4)
        return false; // UP/DOWN now belongs to the CRSF list.
    if constexpr (Test::Board::Input::touch) {
        const auto tap = [&](const unsigned x, const unsigned y) {
            touch(x, y, true);
            touch(x, y, false);
        };
        const auto feed = [&](auto &frame) {
            frame.back() = coretx::crsf::crc8(std::span{frame}.subspan(2, frame.size() - 3));
            for (const auto byte : frame)
                c.telemetry.push(byte, coretx::TimestampUs{c.now}, c.crsf_config);
        };
        std::array<std::uint8_t, 23> info{0xEA, 21, 0x29, 0xEA, 0xEE, 'T', 'X', 0};
        info[20] = 2;
        feed(info);
        tap(12, 50);
        if (c.crsf_config.mode() != coretx::crsf::config::Mode::parameters)
            return false;
        coretx::crsf::config::Frame outbound{};
        if (c.crsf_config.next_frame(coretx::TimestampUs{c.now}, outbound) != 8)
            return false;
        std::array<std::uint8_t, 15> root{0xEA, 13, 0x2B, 0xEA, 0xEE, 0, 0,
                                          0,    11, 'R',  0,    1,    2, 255};
        feed(root);
        std::array<std::uint8_t, 17> value{0xEA, 15,  0x2B, 0xEA, 0xEE, 1,   0, 0,
                                           0,    'V', 0,    25,   0,    100, 0, 0};
        for (std::uint8_t id = 1; id <= 2; ++id) {
            if (c.crsf_config.next_frame(coretx::TimestampUs{c.now}, outbound) != 8)
                return false;
            value[5] = id;
            feed(value);
        }
        const auto change = [&] {
            tap(12, 50);
            touch(64, 32, true);
            touch(64, 32, true, s::Gesture::up);
            touch(64, 32, false);
        };
        const auto confirm = [&] {
            touch(12, 50, true);
            for (unsigned i = 0; i < 81; ++i)
                (void)step(app, c, c.now + 10'000U);
            touch(12, 50, false);
        };
        touch(64, 32, true);
        touch(64, 32, true, s::Gesture::down);
        touch(64, 32, false);
        change();
        if (!app.crsf_menu().editing() || (app.diagnostics().page() != 4))
            return false;
        tap(90, 50);
        if (app.crsf_menu().editing() || c.crsf_config.pending_write())
            return false;
        change();
        confirm();
        if (!c.crsf_config.pending_write() || app.crsf_menu().editing() || !c.tx_valid ||
            (c.crsf_config.next_frame(coretx::TimestampUs{c.now}, outbound) != 8) ||
            (outbound[5] != 2) || (outbound[6] != 26))
            return false;
        std::array<std::uint8_t, 8> ack{0xEA, 6, 0x2D, 0xEA, 0xEE, 2, 26};
        feed(ack);
    }
    const auto hold = [&] {
        if constexpr (Test::Board::Input::touch) {
            touch(60, 56, true);
            for (unsigned i = 0; i < 81; ++i)
                (void)step(app, c, c.now + 10'000U);
            touch(60, 56, false);
        } else
            press_key(app, c, Key::cancel_long);
    };
    if constexpr (!Test::Board::Input::touch)
        hold();
    if constexpr (Test::Board::Input::touch) {
        // Swipe left is Back: it first leaves the CRSF parameter list, then the page changes.
        if (c.crsf_config.mode() == coretx::crsf::config::Mode::parameters) {
            touch(64, 32, true);
            touch(64, 32, true, s::Gesture::left);
            touch(64, 32, false);
        }
    }
    next();
    if ((app.diagnostics().page() != 0) || app.crsf_menu().visible() ||
        (c.crsf_config.mode() != coretx::crsf::config::Mode::closed))
        return false;
    if constexpr (Test::Board::Input::touch) {
        touch(64, 32, true);
        touch(64, 32, true, s::Gesture::right);
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
    if constexpr (Test::Board::Input::touch) {
        const auto swipe = [&](const s::Gesture gesture) {
            touch(64, 32, true);
            touch(64, 32, true, gesture);
            touch(64, 32, false);
        };
        swipe(s::Gesture::down);
        swipe(s::Gesture::down);
        swipe(s::Gesture::right);
        for (unsigned i = 0; i < 3; ++i)
            swipe(s::Gesture::down);
        swipe(s::Gesture::right);
        if (!app.crsf_menu().visible() ||
            (c.crsf_config.mode() != coretx::crsf::config::Mode::devices))
            return false;
    }
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

template <template <typename> typename BoardPolicy> bool active_model_background_loading() {
    if constexpr (!fw::features::a7105 && !fw::features::crsf)
        return true;
    for (int failure = 0; failure < 3; ++failure) {
        struct Scenario {};
        using Test = coretx::test::FakeApplication<Scenario, Context, BoardPolicy>;
        Test::reset();
        auto &c = Test::context;
        c.probe_state = fw::StorageProbeState::found;
        c.eeprom.fill(0xFF);
        c.adc_event = fw::ScanEvent::complete;
        c.samples.fill(2048);
        c.samples[0] = 0;
        c.touch_ready = true;
        c.touch_event = s::TouchEvent::sample;
        coretx::ControlSetup calibration{};
        calibration.calibration.fill({0, 2048, 4095});
        calibration.mapping[0].reversed = calibration.mapping[1].reversed = true;
        const auto calibration_record =
            coretx::firmware::calibration_storage::encode(calibration, 0);
        std::copy(calibration_record.begin(), calibration_record.end(),
                  c.eeprom.begin() + coretx::firmware::calibration_storage::base_address);
        fw::models::Data data{};
        data.radio.transmitter = {0x12345678};
        data.radio.output =
            fw::features::crsf ? fw::RadioOutput::external : fw::RadioOutput::internal;
        data.model.mixes[0].weight_percent = coretx::MixWeight::constant<37>();
        fw::models::Record record{};
        if (!fw::models::encode(data, 0, true, 9, record))
            return false;
        std::copy(record.begin(), record.end(), c.eeprom.begin() + fw::models::base);
        data.model.mixes[0].weight_percent = coretx::MixWeight::constant<61>();
        if (!fw::models::encode(data, 0, true, 10, record))
            return false;
        std::copy(record.begin(), record.end(),
                  c.eeprom.begin() + fw::models::base + fw::models::record_size);
        if (failure != 0) {
            if (!fw::models::encode(data, 7, true, 0, record))
                return false;
            if (failure == 1)
                record[700] ^= 1;
            else {
                record[4] = fw::models::format_version + 1;
                coretx::storage::detail::Writer crc{std::span{record}.last(4)};
                crc.word(
                    coretx::storage::crc32(std::span{record}.first(fw::models::record_size - 4)));
            }
            std::copy(record.begin(), record.end(),
                      c.eeprom.begin() + fw::models::base + 14 * fw::models::record_size);
        }
        fw::Application<typename Test::Board, typename Test::Board::Input> app{};
        (void)app.initialize();
        const auto before = c.eeprom;
        for (unsigned i = 0; !app.models().loaded() && (i < 1000); ++i) {
            (void)step(app, c, c.now + 1000U);
            if (!app.models().loaded() &&
                ((app.output() != fw::RadioOutput::off) || c.powered || c.internal_active))
                return false;
        }
        if (!app.models().loaded() ||
            (app.models().data().model.mixes[0].weight_percent.value() != 61))
            return false;
        if (app.models().busy() || (c.eeprom != before) ||
            (app.models().editable() != (failure == 0)) ||
            (app.output() != (failure == 0 ? data.radio.output : fw::RadioOutput::off)) ||
            (c.powered != ((failure == 0) && fw::features::crsf)) ||
            (c.internal_active != ((failure == 0) && !fw::features::crsf)))
            return false;
        if ((failure == 1) && (app.models().message() != coretx::ui::tr::models_error))
            return false;
        if ((failure == 2) && (app.models().message() != coretx::ui::tr::models_version))
            return false;
    }
    return true;
}

template <template <typename> typename BoardPolicy> bool model_recovery_dialog() {
    for (unsigned action = 0; action < (fw::features::storage_converter ? 8U : 6U); ++action) {
        const bool old_format = action >= 3;
        const auto choice = action % 3;
        const bool reset = choice != 0;
        struct Scenario {};
        using Test = coretx::test::FakeApplication<Scenario, Context, BoardPolicy>;
        using Board = typename Test::Board;
        Test::reset();
        auto &c = Test::context;
        c.probe_state = fw::StorageProbeState::found;
        c.eeprom.fill(0xFF);
        c.adc_event = fw::ScanEvent::complete;
        c.samples.fill(2048);
        c.touch_ready = true;
        c.touch_event = s::TouchEvent::sample;
        fw::models::Data model{};
        model.radio = {{0x12345678},
                       {0x87654321},
                       fw::RadioOutput::internal,
                       coretx::afhds2a::ChannelMode::standard14};
        model.model.inputs[0].weight_percent = coretx::InputWeight::constant<61>();
        fw::models::Record record{};
        if (!fw::models::encode(model, 0, true, 1, record))
            return false;
        if (old_format) {
            namespace codec = coretx::storage::detail;
            const auto &snapshot = model.model;
            codec::Writer out{std::span{record}.first(record.size() - 4), 34};
            if (!codec::transfer_model_counts(out, snapshot) ||
                !codec::transfer_model_lines(out, snapshot, true, false) ||
                !codec::transfer_model_extension(out, snapshot) ||
                !codec::transfer_multiswitch(out, snapshot, true) ||
                !codec::transfer_dashboard_options(out, snapshot) ||
                !codec::transfer_multiswitch_controls(out, snapshot) ||
                !fw::models::transfer_curves(out, snapshot, true) ||
                !codec::transfer_tank(out, snapshot, true, false))
                return false;
            std::fill(record.begin() + static_cast<std::ptrdiff_t>(out.position), record.end() - 4,
                      0xFF);
        }
        const auto version =
            old_format ? std::uint8_t{9} : std::uint8_t{fw::models::format_version + 1};
        record[4] = version;
        coretx::storage::detail::Writer crc{std::span{record}.last(4)};
        crc.word(coretx::storage::crc32(std::span{record}.first(record.size() - 4)));
        if (old_format) {
            fw::models::Data decoded{};
            fw::models::Info info{};
            if (fw::models::decode(record, 0, model.radio.transmitter, decoded, info) != 1)
                return false;
        }
        std::copy(record.begin(), record.end(), c.eeprom.begin() + fw::models::base);
        std::array<std::uint8_t, 64> directory{};
        directory.fill(0xFF);
        coretx::storage::detail::Writer out{directory};
        out.word(fw::models::directory_magic);
        out.field(std::uint8_t{1});
        out.field(std::uint8_t{0});
        out.word(std::uint32_t{1});
        coretx::storage::detail::Writer directory_crc{std::span{directory}.last(4)};
        directory_crc.word(coretx::storage::crc32(std::span{directory}.first(60)));
        std::copy(directory.begin(), directory.end(),
                  c.eeprom.begin() + fw::models::directory_base);
        const auto before = c.eeprom;
        fw::Application<Board, typename Board::Input> app{};
        (void)app.initialize();
        settle_menu(app, c);
        if (!app.models().recovery_required() || app.models().editable() || (c.eeprom != before) ||
            c.powered || c.internal_active ||
            (app.models().message() != coretx::ui::tr::models_version) ||
            (app.models().can_convert() != (old_format && fw::features::storage_converter)) ||
            (app.models().detected_version() != version) ||
            (app.models().expected_version() != fw::models::format_version))
            return false;
        const auto navigate = [&](const Key key) {
            if constexpr (Board::Input::touch) {
                c.touch_sample = {0,  0, 60,
                                  32, 1, key == Key::down ? s::Gesture::down : s::Gesture::up};
                (void)step(app, c, c.now + 1000U);
                c.touch_sample.contacts = 0;
                (void)step(app, c, c.now + 1000U);
            } else
                press_key(app, c, key);
        };
        const auto choose = [&](const unsigned selection) {
            navigate(Key::up);
            navigate(Key::up);
            for (unsigned i = 0; i < selection; ++i)
                navigate(Key::down);
            if constexpr (Board::Input::touch) {
                c.touch_sample = {0, 0, 30, 50, 1, s::Gesture::none};
                (void)step(app, c, c.now + 1000U);
                c.touch_sample.contacts = 0;
                (void)step(app, c, c.now + 1000U);
            } else {
                press_key(app, c, Key::ok);
            }
        };
        if (action >= 6) {
            c.memory_fail_writes = action == 7;
            choose(2);
            for (unsigned i = 0; !app.models().loaded() && (i < 3000); ++i) {
                if (c.powered || c.internal_active || (app.output() != fw::RadioOutput::off))
                    return false;
                (void)step(app, c, c.now + 1000U);
            }
            if (action == 7) {
                if (!app.models().recovery_required() || app.models().editable() ||
                    app.models().can_convert() || (c.eeprom != before) ||
                    (app.output() != fw::RadioOutput::off) || c.powered || c.internal_active)
                    return false;
                continue;
            }
            if (!app.models().editable() || app.models().recovery_required() ||
                (app.models().data().radio != model.radio) ||
                (app.models().data().name != model.name) ||
                (app.models().data().model.inputs[0].weight_percent.value() != 61) ||
                !std::equal(c.eeprom.begin(), c.eeprom.begin() + fw::models::base, before.begin()))
                return false;
            const auto converted = c.eeprom;
            fw::Application<Board, typename Board::Input> reboot{};
            (void)reboot.initialize();
            settle_menu(reboot, c);
            if (!reboot.models().editable() || reboot.models().recovery_required() ||
                (c.eeprom != converted) ||
                (reboot.models().data().model.inputs[0].weight_percent.value() != 61))
                return false;
            continue;
        }
        if (reset) {
            choose(true);
            if ((c.eeprom != before) || app.models().busy())
                return false;
            choose(false); // The confirmation defaults to cancellation, not deletion.
            if ((c.eeprom != before) || app.models().busy())
                return false;
            choose(true);
            c.memory_fail_writes = choice == 2;
            choose(true);
            settle_menu(app, c);
            if (choice == 2) {
                if (!app.models().recovery_required() || app.models().editable() ||
                    (c.eeprom != before))
                    return false;
                choose(false);
                settle_menu(app, c);
                if ((app.state() != fw::ApplicationState::powered_off) ||
                    (c.power_releases != (Board::Input::touch ? 1U : 0U)))
                    return false;
                continue;
            }
            if (!app.models().editable() || app.models().recovery_required() ||
                (app.models().data().model.mixes.size() != 4) ||
                (app.models().data().radio.receiver.value != 0) ||
                (app.output() != fw::RadioOutput::off) || c.powered || c.internal_active ||
                !std::equal(c.eeprom.begin(), c.eeprom.begin() + fw::models::base, before.begin()))
                return false;
            for (std::size_t i = 1; i < fw::models::count; ++i)
                if (app.models().entries()[i].present)
                    return false;
        } else {
            if constexpr (Board::Input::touch)
                choose(false);
            else
                press_key(app, c, Key::ok); // First/default choice is power off.
            settle_menu(app, c);
            if ((app.state() != fw::ApplicationState::powered_off) || (c.eeprom != before) ||
                (c.power_releases != (Board::Input::touch ? 1U : 0U)))
                return false;
        }
    }
    return true;
}

bool trim_application() {
    if constexpr (coretx::firmware::features::i6s)
        return true;
    for (int mode = 0; mode < 12; ++mode) {
        const bool trim_sound = ((mode / 3) % 2) == 0;
        const bool multiswitch_sound = mode < 6;
        const bool switches = (mode % 3) != 0;
        const bool bound = (mode % 3) == 2;
        if (bound && !fw::features::multiswitch)
            continue;
        struct Scenario {};
        using Test = coretx::test::FakeApplication<Scenario, Context, SenderBoard>;
        Test::reset();
        auto &c = Test::context;
        c.probe_state = fw::StorageProbeState::found;
        c.eeprom.fill(0xFF);
        c.adc_event = fw::ScanEvent::complete;
        c.samples.fill(2048);
        coretx::ControlSetup calibration{};
        calibration.calibration.fill({0, 2048, 4095});
        calibration.mapping[0].reversed = calibration.mapping[1].reversed = true;
        const auto record = coretx::firmware::calibration_storage::encode(calibration, 0);
        std::copy(record.begin(), record.end(),
                  c.eeprom.begin() + coretx::firmware::calibration_storage::base_address);
        coretx::InputSetup inputs{};
        inputs.trim_beep = trim_sound;
        const auto input_record = fw::input_storage::encode(inputs, 0);
        std::copy(input_record.begin(), input_record.end(),
                  c.eeprom.begin() + fw::input_storage::base_address);
        fw::models::Data data{};
        data.model.multiswitch_beep = multiswitch_sound;
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
        if (bound) {
            data.model.multiswitch[0].address = 42;
            data.model.multiswitch[0].controls[0].set_condition(
                {true, coretx::physical_switch_count, coretx::SwitchPosition::down});
        }
        fw::models::Record model_record{};
        if (!fw::models::encode(data, 0, true, 0, model_record))
            return false;
        std::copy(model_record.begin(), model_record.end(), c.eeprom.begin() + fw::models::base);
        fw::Application<Test::Board> app{};
        (void)app.initialize();
        for (unsigned i = 0; i < 300; ++i)
            (void)step(app, c, c.now + 1000U);
        if (!app.diagnostics().output_valid())
            return false;
        if constexpr (coretx::firmware::features::crsf)
            if (switches && (app.output() != coretx::firmware::RadioOutput::external))
                return false;
        const auto before = c.eeprom;
        const auto control_clicks = c.center_clicks;
        const auto multiswitch_clicks = c.multiswitch_clicks;
        c.trim_buttons = 1;
        (void)step(app, c, c.now + 1000U);
        if ((app.diagnostics().evaluation().frame.channels[7] != (switches ? 1024 : 2)) ||
            (c.eeprom != before) ||
            (c.center_clicks != (control_clicks + (!switches && trim_sound ? 1U : 0U))) ||
            (c.multiswitch_clicks !=
             (multiswitch_clicks + (bound && multiswitch_sound ? 1U : 0U))) ||
            (c.multiswitch_states[0] != (bound ? 1 : 0)))
            return false;
        c.trim_buttons = 0;
        (void)step(app, c, c.now + 1000U);
        for (unsigned i = 0; i < 300; ++i)
            (void)step(app, c, c.now + 1000U);
        if ((c.eeprom != before) ||
            (app.diagnostics().evaluation().frame.channels[7] != (switches ? 0 : 2)) ||
            (c.center_clicks != (control_clicks + (!switches && trim_sound ? 1U : 0U))) ||
            (c.multiswitch_clicks !=
             (multiswitch_clicks + (bound && multiswitch_sound ? 2U : 0U))) ||
            (c.multiswitch_states[0] != 0))
            return false;
        for (unsigned i = 0; i < 400; ++i)
            (void)step(app, c, c.now + 1000U);
        const auto offset =
            fw::models::base + app.models().entries()[0].bank * fw::models::record_size;
        std::copy_n(c.eeprom.begin() + static_cast<std::ptrdiff_t>(offset), model_record.size(),
                    model_record.begin());
        fw::models::Info info{};
        if ((fw::models::decode(model_record, 0, data.radio.transmitter, data, info) != 1) ||
            (data.model.trims[0].value() != (switches ? 10 : 1)) ||
            (switches && (c.eeprom != before)))
            return false;
        if (!switches) {
            const auto press = [&](const Key key) {
                c.key = key;
                (void)step(app, c, c.now + 1000U);
                c.key = Key::none;
            };
            const auto channels = app.diagnostics().evaluation().frame.channels;
            const auto adc_started_at = c.adc_started_at;
            press(Key::ok_long);
            press(Key::down);
            press(Key::down);
            press(Key::ok);
            for (unsigned i = 0; i < fw::menu_detail::trim_beep_row; ++i)
                press(Key::down);
            press(Key::ok);
            if ((app.diagnostics().input_setup().trim_beep == trim_sound) ||
                !app.diagnostics().output_valid() ||
                (app.diagnostics().evaluation().frame.channels != channels) ||
                (c.adc_started_at != adc_started_at))
                return false;
            press(Key::ok); // Input journal is busy; leave the setting unchanged.
            if (app.diagnostics().input_setup().trim_beep == trim_sound)
                return false;
            for (unsigned i = 0; i < 300; ++i)
                (void)step(app, c, c.now + 1000U);
            fw::input_storage::Record saved_inputs{};
            std::copy_n(c.eeprom.begin() + fw::input_storage::base_address +
                            fw::input_storage::record_size,
                        saved_inputs.size(), saved_inputs.begin());
            auto expected_inputs = inputs;
            expected_inputs.trim_beep = !trim_sound;
            const auto decoded_inputs = fw::input_storage::decode(saved_inputs);
            if (!decoded_inputs.valid() || (decoded_inputs.setup != expected_inputs))
                return false;
            press(Key::ok);
            if (app.diagnostics().input_setup().trim_beep != trim_sound)
                return false;
            for (unsigned i = 0; i < 300; ++i)
                (void)step(app, c, c.now + 1000U);
            press(Key::cancel_long);
            const auto saved = c.eeprom;
            press(Key::ok_long);
            press(Key::down);
            press(Key::down);
            press(Key::ok);
            press(Key::down);
            press(Key::ok);
            if (app.diagnostics().phase() == fw::CalibrationPhase::idle)
                return false;
            c.trim_buttons = 1;
            for (unsigned i = 0; i < 6; ++i)
                (void)step(app, c, c.now + 100'000U);
            if ((app.models().data().model.trims[0].value() != 1) || (c.eeprom != saved))
                return false;
            press(Key::cancel);
            if ((app.diagnostics().phase() != fw::CalibrationPhase::idle) ||
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

bool external_module_inputs() {
    if constexpr (!fw::features::i6s || !fw::features::extended_inputs || !fw::features::crsf)
        return true;
    struct Scenario {};
    using Test = coretx::test::FakeApplication<Scenario, Context, TouchSenderBoard>;
    Test::reset();
    auto &c = Test::context;
    c.probe_state = fw::StorageProbeState::found;
    c.eeprom.fill(0xFF);
    c.adc_event = fw::ScanEvent::complete;
    c.samples.fill(2048);
    coretx::ControlSetup calibration{};
    calibration.calibration.fill({0, 2048, 4095});
    calibration.mapping[0].reversed = calibration.mapping[1].reversed = true;
    const auto calibration_record = coretx::firmware::calibration_storage::encode(calibration, 0);
    std::copy(calibration_record.begin(), calibration_record.end(),
              c.eeprom.begin() + coretx::firmware::calibration_storage::base_address);
    coretx::InputSetup setup{true};
    setup.external_module = false;
    setup.set_digital_mode(2, coretx::DigitalInputMode::three_position);
    setup.set_digital_mode(4, coretx::DigitalInputMode::two_position);
    const auto inputs = fw::input_storage::encode(setup, 0);
    std::copy(inputs.begin(), inputs.end(), c.eeprom.begin() + fw::input_storage::base_address);
    fw::models::Data model{};
    model.radio.transmitter = {0x12345678};
    model.radio.output = fw::RadioOutput::external;
    fw::models::Record record{};
    if (!fw::models::encode(model, 0, true, 0, record))
        return false;
    std::copy(record.begin(), record.end(), c.eeprom.begin() + fw::models::base);
    fw::Application<Test::Board> app{};
    (void)app.initialize();
    for (unsigned i = 0; i < 1000; ++i)
        (void)step(app, c, c.now + 1000U);
    if ((app.output() != fw::RadioOutput::off) || c.powered || (c.extra_digital != 0x1C))
        return false;
    const auto open_setting = [&](auto &application, auto &context) {
        const auto press = [&](const Key key) {
            context.key = key;
            (void)step(application, context, context.now + 1000U);
        };
        press(Key::ok_long);
        press(Key::down);
        press(Key::down);
        press(Key::ok);
        for (std::size_t i = 0; i < fw::menu_detail::external_module_row; ++i)
            press(Key::down);
        press(Key::ok);
    };
    open_setting(app, c);
    if (!app.diagnostics().input_setup().external_module || (c.extra_digital != 0x10) ||
        (app.diagnostics().input_setup().digital_mode(2) !=
         coretx::DigitalInputMode::three_position))
        return false;
    c.key = Key::none;
    for (unsigned i = 0; i < 1000; ++i)
        (void)step(app, c, c.now + 1000U);
    struct RebootScenario {};
    using Reboot = coretx::test::FakeApplication<RebootScenario, Context, TouchSenderBoard>;
    Reboot::reset();
    auto &reboot = Reboot::context;
    reboot.eeprom = c.eeprom;
    reboot.probe_state = fw::StorageProbeState::found;
    fw::Application<Reboot::Board> restored{};
    (void)restored.initialize();
    for (unsigned i = 0; i < 1000; ++i)
        (void)step(restored, reboot, reboot.now + 1000U);
    if (!restored.diagnostics().input_setup().external_module ||
        (restored.output() != fw::RadioOutput::external) || !reboot.powered ||
        (reboot.extra_digital != 0x10))
        return false;
    open_setting(restored, reboot);
    return restored.diagnostics().input_setup().external_module &&
           (restored.output() == fw::RadioOutput::external) && reboot.powered &&
           (reboot.extra_digital == 0x10);
}

bool setup_controls() {
    struct Scenario {};
    using Test = coretx::test::FakeApplication<Scenario, Context, SetupSenderBoard>;
    Test::reset();
    auto &c = Test::context;
    c.probe_state = fw::StorageProbeState::found;
    c.eeprom.fill(0xFF);
    c.adc_event = fw::ScanEvent::complete;
    c.samples.fill(2048);
    c.touch_ready = true;
    c.touch_event = s::TouchEvent::sample;
    coretx::ControlSetup calibration{};
    calibration.calibration.fill({0, 2048, 4095});
    calibration.mapping[0].reversed = calibration.mapping[1].reversed = true;
    const auto record = coretx::firmware::calibration_storage::encode(calibration, 0);
    std::copy(record.begin(), record.end(),
              c.eeprom.begin() + coretx::firmware::calibration_storage::base_address);
    fw::models::Data model{};
    model.radio.transmitter = {0x12345678};
    model.radio.output = fw::features::crsf ? fw::RadioOutput::external : fw::RadioOutput::off;
    model.model = fw::diagnostic_model;
    model.model.mixes.resize<12>();
    for (std::size_t i = 0; i < 2; ++i) {
        model.model.mixes[10 + i].destination = static_cast<std::uint8_t>(10 + i);
        model.model.mixes[10 + i].source = {
            coretx::SourceKind::switch_position,
            static_cast<std::uint8_t>(coretx::physical_switch_count + i), false};
    }
    model.model.crsf_dashboard = fw::features::crsf;
    fw::models::Record model_record{};
    if (!fw::models::encode(model, 0, true, 0, model_record))
        return false;
    std::copy(model_record.begin(), model_record.end(), c.eeprom.begin() + fw::models::base);
    fw::Application<Test::Board, Test::Board::Input> app{};
    (void)app.initialize();
    for (unsigned i = 0; i < 300; ++i)
        (void)step(app, c, c.now + 1000U);
    if (!app.diagnostics().output_valid() || c.setup_active || !c.setup_leds)
        return false;
    c.samples[6] = 4095;
    c.samples[7] = 0;
    c.rear_pressed = 1;
    (void)step(app, c, c.now + 1000U);
    (void)step(app, c, c.now + 20'000U);
    const auto before = app.diagnostics().evaluation().frame.channels;
    if ((before[4] != coretx::resolution) || (before[5] != -coretx::resolution) ||
        (before[10] != coretx::resolution) || (before[11] != -coretx::resolution) ||
        app.menu().visible())
        return false;
    c.touch_sample = {0, 0, 64, 32, 1, s::Gesture::none};
    (void)step(app, c, c.now + 1000U);
    for (unsigned i = 0; i < 4; ++i)
        (void)step(app, c, c.now + 200'000U);
    if ((app.menu().screen() != fw::MenuScreen::root) || !c.setup_active)
        return false;
    c.touch_sample.contacts = 0;
    c.rear_pressed = 0;
    c.samples[0] = 0;
    c.samples[6] = 2048;
    c.samples[7] = 4095;
    (void)step(app, c, c.now + 1000U);
    (void)step(app, c, c.now + 20'000U);
    const auto held = app.diagnostics().evaluation().frame.channels;
    if ((held[4] != before[4]) || (held[5] != before[5]) || (held[10] != before[10]) ||
        (held[11] != before[11]) || (held[0] == before[0]))
        return false;
    c.samples[7] = 2048;
    (void)step(app, c, c.now + 1000U);
    c.samples[6] = 4095;
    c.samples[7] = 4095;
    (void)step(app, c, c.now + 1000U); // VRA selects the next root item.
    c.touch_error = s::TouchError::bus;
    c.touch_event = s::TouchEvent::failed;
    c.rear_pressed = 1;
    (void)step(app, c, c.now + 1000U);
    (void)step(app, c, c.now + 20'000U);
    c.rear_pressed = 0;
    (void)step(app, c, c.now + 1000U);
    (void)step(app, c, c.now + 20'000U);
    if (app.menu().screen() != fw::MenuScreen::setup)
        return false;
    (void)step(app, c, c.now + 250'000U);
    if (c.setup_leds)
        return false;
    (void)step(app, c, c.now + 250'000U);
    if (!c.setup_leds)
        return false;
    c.rear_pressed = 2;
    (void)step(app, c, c.now + 1000U);
    (void)step(app, c, c.now + 20'000U);
    (void)step(app, c, c.now + 800'000U);
    if (app.menu().visible() || c.setup_active || !c.setup_leds ||
        (app.diagnostics().evaluation().frame.channels[5] != coretx::resolution))
        return false;
    c.rear_pressed = 0;
    (void)step(app, c, c.now + 1000U);
    (void)step(app, c, c.now + 20'000U);
    c.samples[6] = 0;
    (void)step(app, c, c.now + 1000U);
    if (app.menu().visible() || c.setup_active ||
        (app.diagnostics().evaluation().frame.channels[4] != -coretx::resolution) ||
        (app.diagnostics().evaluation().frame.channels[10] != -coretx::resolution))
        return false;
    if constexpr (fw::features::crsf) {
        c.touch_error = s::TouchError::none;
        c.touch_event = s::TouchEvent::sample;
        c.touch_sample = {};
        (void)step(app, c, c.now + 1000U);
        for (unsigned i = 0; i < 2; ++i) {
            c.touch_sample = {0, 0, 64, 32, 1, s::Gesture::left};
            (void)step(app, c, c.now + 1000U);
            c.touch_sample = {};
            (void)step(app, c, c.now + 1000U);
        }
        c.samples[6] = 4095;
        c.rear_pressed = 2;
        (void)step(app, c, c.now + 1000U);
        (void)step(app, c, c.now + 20'000U);
        if ((app.diagnostics().page() != fw::Diagnostics::page_count) || c.setup_active ||
            !c.setup_leds ||
            (app.diagnostics().evaluation().frame.channels[4] != coretx::resolution) ||
            (app.diagnostics().evaluation().frame.channels[10] != -coretx::resolution))
            return false;
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
    RUN(disabled_module_types());
    RUN(trim_controls());
    RUN(active_model_background_loading<SenderBoard>());
    RUN(active_model_background_loading<TouchSenderBoard>());
    RUN(model_recovery_dialog<SenderBoard>());
    RUN(model_recovery_dialog<TouchSenderBoard>());
    RUN(trim_application());
    RUN(calibration_reboot<SenderBoard>());
    RUN(calibration_reboot<TouchSenderBoard>());
    RUN(startup_errors());
    RUN(sender_lifecycle());
    RUN(sender_fatal_priority<SenderBoard>());
    RUN(sender_fatal_priority<TouchSenderBoard>());
    RUN(shared_touch_sender());
    RUN(shared_touch_menus());
    RUN(setup_controls());
    RUN(external_module_inputs());
    RUN(safe_power_off());
    RUN(power_off_during_load());
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
