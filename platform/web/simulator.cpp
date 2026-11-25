// SPDX-License-Identifier: GPL-2.0-or-later
#include "platform/web/simulator.hpp"

#include "boards/common/application.hpp"
#include "boards/i6s/setup_controls.hpp"
#include "boards/i6s/touch_input.hpp"
#include "platform/stm32f072/a7105.hpp"

#include <algorithm>
#include <array>
#include <memory>

namespace {
namespace fw = coretx::firmware;
using coretx::TimestampUs;

struct Peripherals {
    std::array<std::uint8_t, fw::models::memory_size> eeprom{};
    std::array<std::uint16_t, fw::sender_inputs.size()> adc{};
    std::array<std::uint8_t, 128 * 64> lcd{};
    coretx::firmware::CrsfClient crsf{};
    coretx::crsf::Telemetry telemetry{};
    fw::i6s::TouchSample contact{};
    std::array<fw::i6s::TouchSample, 64> touch_events{};
    std::uint32_t now{}, revision{}, touch_polled{};
    std::uint32_t repeat_started{}, repeated_at{};
    int repeat_key{};
    std::array<bool, 7> keys{};
    std::array<std::uint32_t, 2> long_started{};
    std::array<bool, 2> long_sent{};
    std::array<fw::Key, 32> events{};
    std::size_t event_read{}, event_write{};
    std::size_t touch_read{}, touch_write{};
    std::uint8_t trims{}, digital{}, rear{};
    fw::I2cError memory_error{};
    bool extended{}, display_busy{}, started{};
};
Peripherals io{};
const char *bridge_error{};

void queue(const fw::Key key) {
    const auto next = (io.event_write + 1) % io.events.size();
    if (next != io.event_read) {
        io.events[io.event_write] = key;
        io.event_write = next;
    } else
        bridge_error = "Sender key event queue overflow.";
}

struct Keys {
    static void initialize() {}
    static fw::Key service(TimestampUs) {
        if (io.event_read != io.event_write) {
            const auto key = io.events[io.event_read];
            io.event_read = (io.event_read + 1) % io.events.size();
            return key;
        }
        for (std::size_t key = 5; key <= 6; ++key) {
            const auto index = key - 5;
            if (io.keys[key] && !io.long_sent[index] &&
                (io.now - io.long_started[index] >= 800'000)) {
                io.long_sent[index] = true;
                return key == 5 ? fw::Key::ok_long : fw::Key::cancel_long;
            }
        }
        if ((io.repeat_key != 0) && io.keys[static_cast<std::size_t>(io.repeat_key)] &&
            (io.now - io.repeat_started >= 400'000) && (io.now - io.repeated_at >= 100'000)) {
            io.repeated_at = io.now;
            return io.repeat_key == 1 ? fw::Key::up : fw::Key::down;
        }
        return fw::Key::none;
    }
    static bool ok_held() { return io.keys[5]; }
    static std::uint8_t trim_buttons() { return io.trims; }
};

struct Touch {
    static void initialize() {}
    static fw::i6s::TouchEvent service(TimestampUs) {
        if (io.touch_read != io.touch_write) {
            io.contact = io.touch_events[io.touch_read];
            io.touch_read = (io.touch_read + 1) % io.touch_events.size();
        } else if (io.now - io.touch_polled < 10'000)
            return fw::i6s::TouchEvent::none;
        io.touch_polled = io.now;
        return fw::i6s::TouchEvent::sample;
    }
    static bool ready() { return true; }
    static fw::i6s::TouchError error() { return {}; }
    static const auto &sample() { return io.contact; }
};

struct SetupControls {
    void initialize() { navigation_ = {}; }
    fw::Key service(TimestampUs now, bool enabled, std::int16_t vra, std::int16_t vrb, bool jog) {
        const auto physical = navigation_.service(now, io.rear, enabled, vra, vrb, jog);
        const auto keyboard = Keys::service(now);
        return keyboard != fw::Key::none ? keyboard : physical;
    }
    void set_mode(TimestampUs now, bool active) { navigation_.set_mode(now, active); }
    std::uint8_t buttons() const { return navigation_.buttons(); }

  private:
    fw::i6s::SetupNavigation navigation_{};
};

struct Adc {
    static fw::AdcError initialize() { return {}; }
    static fw::AdcError error() { return {}; }
    static fw::ScanEvent service(TimestampUs) { return fw::ScanEvent::complete; }
    static auto samples() {
        auto values = io.adc;
        if (!io.extended)
            std::fill(values.begin() + 11, values.end(), std::uint16_t{2048});
        return values;
    }
    static fw::AdcError set_extended(bool enabled) {
        io.extended = enabled;
        return {};
    }
};

struct Display {
    static fw::LcdError initialize() { return {}; }
    static fw::LcdError service(const coretx::ui::Framebuffer &frame) {
        if (!io.display_busy)
            return {};
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 128; ++x)
                io.lcd[static_cast<std::size_t>(y * 128 + x)] = frame.pixel_at(x, y) ? 1 : 0;
        io.display_busy = false;
        return {};
    }
    static bool busy() { return io.display_busy; }
    static void begin_frame() { io.display_busy = true; }
};

struct Inputs {
    static void initialize() {}
    static void configure(std::uint8_t) {}
    static std::uint8_t read() { return io.digital; }
    static void service(TimestampUs, bool, fw::ClickTone = fw::ClickTone::control) {}
    static void play(TimestampUs, std::uint32_t, coretx::DurationUs) {}
    static void stop() {}
};

struct Probe {
    static void initialize() {}
    static void service(TimestampUs) {}
    static fw::StorageProbeState state() { return fw::StorageProbeState::found; }
    static fw::I2cError error() { return {}; }
    static std::uint8_t acknowledged() { return 1; }
    static fw::StorageProbeResult result() { return {state(), fw::I2cError::none, 1}; }
};

struct Memory {
    inline static constexpr std::size_t page_size = 64;
    static void select(std::uint8_t) {}
    static void service(TimestampUs) {}
    static bool busy() { return false; }
    static fw::I2cError error() { return io.memory_error; }
    static bool read(std::uint16_t address, std::span<std::uint8_t> bytes) {
        if (address + bytes.size() > io.eeprom.size()) {
            io.memory_error = fw::I2cError::bus;
            return false;
        }
        std::copy_n(io.eeprom.begin() + address, bytes.size(), bytes.begin());
        return true;
    }
    static bool write(std::uint16_t address, std::span<const std::uint8_t> bytes) {
        if (address + bytes.size() > io.eeprom.size()) {
            io.memory_error = fw::I2cError::bus;
            return false;
        }
        if (!std::equal(bytes.begin(), bytes.end(), io.eeprom.begin() + address)) {
            std::copy(bytes.begin(), bytes.end(), io.eeprom.begin() + address);
            ++io.revision;
        }
        return true;
    }
};

// Protocol sessions are real, but these adapters have no transport or receiver.
struct Radio {
    static void initialize(TimestampUs) {}
    static void service(TimestampUs) {}
    static fw::A7105Info info() { return {fw::A7105State::ready, fw::A7105Error::none}; }
};
class Internal {
  public:
    void initialize(coretx::afhds2a::TxId tx, coretx::afhds2a::RxId rx, TimestampUs) {
        (void)session_.initialize(tx, rx);
    }
    void activate() { active_ = true; }
    void deactivate() {
        active_ = false;
        session_.cancel_bind();
    }
    void set_channel_mode(coretx::afhds2a::ChannelMode mode) { session_.set_channel_mode(mode); }
    void service(TimestampUs, const coretx::afhds2a::Channels &, bool, TimestampUs) {}
    bool bind(TimestampUs now) {
        if (!active_)
            return false;
        if (session_.binding())
            session_.cancel_bind();
        else
            session_.start_bind(now);
        return true;
    }
    bool ready() const { return true; }
    fw::Afhds2aError error() const { return {}; }
    const auto &session() const { return session_; }
    auto telemetry() const { return session_.telemetry(); }
    bool take_receiver_changed() { return false; }

  private:
    coretx::afhds2a::Session session_{};
    bool active_{};
};

struct External {
    static void power_off() {}
    static void initialize(TimestampUs) {}
    static bool set_baud(std::size_t index, TimestampUs) {
        if (index >= fw::crsf_baudrates.size())
            return false;
        baud_ = index;
        return true;
    }
    static void next_baud(TimestampUs now) {
        (void)set_baud((baud_ + 1) % fw::crsf_baudrates.size(), now);
    }
    static fw::CrsfError service(TimestampUs now, const coretx::crsf::Channels &, bool, TimestampUs,
                                 coretx::multiswitch::Pages * = nullptr) {
        io.crsf.service(now);
        return {};
    }
    static const auto &telemetry() { return io.telemetry; }
    static auto &config() { return io.crsf; }
    static fw::CrsfError error() { return {}; }
    static std::uint32_t baudrate() { return fw::crsf_baudrates[baud_].rate; }
    static std::uint32_t actual_baudrate() { return baudrate(); }
    static std::uint32_t frames() { return 0; }
    static std::uint32_t receive_errors_per_second() { return 0; }
    static std::uint32_t receive_overruns() { return 0; }
    inline static std::size_t baud_{fw::crsf_default_baudrate_index};
};

struct Board {
    inline static constexpr auto name = fw::Hardware::name;
    inline static constexpr bool sb_three_position = fw::Hardware::sb_three_position;
    using Adc = ::Adc;
    using Display = ::Display;
    using Inputs = ::Inputs;
    using Probe = ::Probe;
    using Memory = ::Memory;
    using Radio = ::Radio;
    using InternalRFModule = std::conditional_t<fw::features::a7105, Internal, void>;
    using ExternalRFModule = std::conditional_t<fw::features::crsf, External, void>;
    using Input = std::conditional_t<fw::features::i6s, fw::TouchInput<Touch, SetupControls>,
                                     fw::PhysicalInput<Keys>>;
    struct Watchdog {
        static std::uint32_t resets() { return 0; }
        static std::uint32_t last_fault() { return 0; }
        static void mark(std::uint8_t) {}
    };
    static fw::stm32f072::HardwareError initialize() {
        External::baud_ = fw::crsf_default_baudrate_index;
        return {};
    }
    static TimestampUs now_us() { return TimestampUs{io.now}; }
    static bool stack_intact() { return true; }
    static void power_off() {}
    static void reset() {}
    static coretx::afhds2a::TxId transmitter_id() { return {0x12345678}; }
};

using Application = fw::Application<Board, Board::Input>;
Application app{};
std::array<std::int16_t, 16> channels{};

bool defaults() {
    coretx::ControlSetup setup{};
    setup.calibration.fill({0, 2048, 4095});
    setup.mapping[0].reversed = setup.mapping[1].reversed = true;
    const auto calibration = fw::calibration_storage::encode(setup, 0);
    std::copy(calibration.begin(), calibration.end(),
              io.eeprom.begin() + fw::calibration_storage::base_address);
    fw::models::Data data{};
    fw::models::four_channel_model(data.model);
    data.radio = {Board::transmitter_id(),
                  {},
                  fw::RadioOutput::off,
                  coretx::afhds2a::ChannelMode::extended16};
    data.baud_index = static_cast<std::uint8_t>(fw::crsf_default_baudrate_index);
    fw::models::default_name(data.name, 0);
    fw::models::Record model{};
    if (!fw::models::encode(data, 0, true, 0, model))
        return false;
    std::copy(model.begin(), model.end(), io.eeprom.begin() + fw::models::base);
    const auto inputs = fw::input_storage::encode(coretx::InputSetup{Board::sb_three_position}, 0);
    std::copy(inputs.begin(), inputs.end(), io.eeprom.begin() + fw::input_storage::base_address);
    ++io.revision;
    return true;
}
} // namespace

extern "C" {
void sim_init() {
    io.adc.fill(2048);
    for (const auto index : fw::switch_indices)
        io.adc[index] = 0;
    io.adc[fw::battery_index] = 3200;
    io.now = 0;
    io.touch_polled = 0;
    io.repeat_key = 0;
    io.repeat_started = io.repeated_at = 0;
    io.keys.fill(false);
    io.long_started.fill(0);
    io.long_sent.fill(false);
    io.trims = io.digital = io.rear = 0;
    io.event_read = io.event_write = 0;
    io.touch_read = io.touch_write = 0;
    io.contact = {};
    io.crsf = {};
    io.telemetry = {};
    io.memory_error = {};
    io.extended = io.display_busy = false;
    io.started = true;
    io.lcd.fill(0);
    channels.fill(0);
    bridge_error = nullptr;
    if (std::all_of(io.eeprom.begin(), io.eeprom.end(), [](auto byte) { return byte == 0xFF; }) &&
        !defaults())
        bridge_error = "Simulator defaults could not be encoded.";
    std::destroy_at(&app);
    std::construct_at(&app);
    (void)app.initialize();
}
int sim_step(int elapsed_ms) {
    if (io.started && bridge_error == nullptr) {
        for (int ms = 0; ms < std::clamp(elapsed_ms, 0, 100); ++ms) {
            io.now += 1000;
            (void)app.service(TimestampUs{io.now});
        }
        if ((sim_status() & 4) != 0)
            channels = app.diagnostics().evaluation().frame.channels;
        else
            channels.fill(0);
    }
    return sim_status();
}
// Analog indices: sticks 0..3, VRA/VRB 4..5, then the additional inputs VRC.. of the board.
int sim_analog_count() { return static_cast<int>(6 + fw::extra_adc_count); }
void sim_set_analog(int index, int raw) {
    if ((index < 0) || (index >= sim_analog_count()) || (raw < 0) || (raw > 4095))
        return;
    const auto analog = static_cast<std::size_t>(index);
    io.adc[analog < 4   ? analog
           : analog < 6 ? analog + 2
                        : fw::battery_index + 1 + (analog - 6)] = static_cast<std::uint16_t>(raw);
}
void sim_set_switch(int index, int position) {
    if ((index < 0) || (index >= 16) || (position < 0) || (position > 2))
        return;
    if (index < 4)
        io.adc[fw::switch_indices[static_cast<std::size_t>(index)]] =
            static_cast<std::uint16_t>(position == 0   ? 0
                                       : position == 1 ? 2048
                                                       : 4095);
    else if (index < 9) {
        const auto bit = static_cast<std::uint8_t>(1U << (index - 4));
        io.digital = static_cast<std::uint8_t>((io.digital & ~bit) | (position == 2 ? bit : 0));
    } else if (index >= 14) {
        const auto bit = static_cast<std::uint8_t>(1U << (index - 14));
        io.rear = static_cast<std::uint8_t>((io.rear & ~bit) | (position == 2 ? bit : 0));
    }
}
int sim_digital_inputs() {
    const auto &diagnostics = app.diagnostics();
    int result{};
    for (std::size_t i = 0; i < coretx::digital_input_count; ++i)
        if (diagnostics.digital_enabled(i))
            result |= static_cast<int>(diagnostics.input_setup().digital_mode(i)) << (2 * i);
    return result;
}
void sim_key(int key, int down) {
    if constexpr (fw::features::i6s)
        return;
    if ((key != 1) && (key != 2) && (key != 5) && (key != 6))
        return;
    const auto index = static_cast<std::size_t>(key);
    if ((down != 0) == io.keys[index])
        return;
    if (key <= 2) {
        if (down) {
            queue(key == 1 ? fw::Key::up : fw::Key::down);
            io.repeat_key = key;
            io.repeat_started = io.repeated_at = io.now;
        }
    } else {
        const auto long_index = index - 5;
        if (down) {
            io.long_started[long_index] = io.now;
            io.long_sent[long_index] = false;
        } else if (!io.long_sent[long_index])
            queue(key == 5 ? fw::Key::ok : fw::Key::cancel);
    }
    io.keys[index] = down != 0;
}
void sim_trim(int button, int down) {
    if ((button < 0) || (button >= 8))
        return;
    const auto bit = static_cast<std::uint8_t>(1U << button);
    io.trims = static_cast<std::uint8_t>((io.trims & ~bit) | (down ? bit : 0));
}
void sim_touch(int x, int y, int down) {
    const auto next = (io.touch_write + 1) % io.touch_events.size();
    if (next == io.touch_read) {
        bridge_error = "Sender touch event queue overflow.";
        return;
    }
    auto &sample = io.touch_events[io.touch_write];
    sample = {};
    sample.x = static_cast<std::uint8_t>(std::clamp(x, 0, 127));
    sample.y = static_cast<std::uint8_t>(std::clamp(y, 0, 63));
    sample.contacts = down < 0 ? 2 : down ? 1 : 0;
    sample.position_valid = true;
    io.touch_write = next;
    if (down < 0)
        sim_touch(x, y, 0);
}
std::uint8_t *sim_eeprom() { return io.eeprom.data(); }
std::uint32_t sim_revision() { return io.revision; }
int sim_busy() { return !io.started || app.storage_pending(); }
int sim_status() {
    if (!io.started)
        return 0;
    const bool fatal = bridge_error != nullptr || app.state() == fw::ApplicationState::fatal;
    const bool valid = !fatal && app.models().editable() && app.diagnostics().output_valid() &&
                       app.state() == fw::ApplicationState::running;
    return (app.models().loaded() ? 1 : 0) | (app.models().editable() ? 2 : 0) | (valid ? 4 : 0) |
           (sim_busy() ? 8 : 0) | (app.models().recovery_required() ? 16 : 0) |
           (app.state() == fw::ApplicationState::powered_off ? 32 : 0) | (fatal ? 64 : 0);
}
const std::uint8_t *sim_lcd() { return io.lcd.data(); }
const std::int16_t *sim_channels() { return channels.data(); }
const char *sim_error() {
    if (bridge_error)
        return bridge_error;
    if (app.state() == fw::ApplicationState::fatal)
        return "Virtual sender application failed.";
    if (app.models().recovery_required())
        return app.models().message();
    if (!app.models().loaded())
        return "Loading EEPROM";
    if (!app.diagnostics().calibrated())
        return "Calibration required in sender settings";
    if (!app.diagnostics().output_valid())
        return "Invalid control or model data";
    return "";
}
int sim_output() { return static_cast<int>(app.output()); }
}
