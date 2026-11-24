// SPDX-License-Identifier: GPL-2.0-or-later
// RC arithmetic derived from EdgeTX, radio/src/mixer.cpp and edgetx_helpers.h.
#pragma once

#include "coretx/core/mix_value.hpp"
#include "coretx/utility/bounded_value.hpp"
#include "coretx/utility/fixed_vector.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace coretx {

inline constexpr std::int16_t resolution = 1024;
inline constexpr std::size_t analog_count = 16;
inline constexpr std::size_t trim_count = 4;
inline constexpr std::size_t physical_switch_count = 16;
inline constexpr std::size_t switch_count = physical_switch_count + trim_count;
inline constexpr std::size_t input_count = 8;
inline constexpr std::size_t channel_count = 16;
inline constexpr std::size_t max_input_lines = 16;
inline constexpr std::size_t max_mix_lines = 24;
inline constexpr std::size_t logical_switch_count = 8;
inline constexpr std::size_t curve_count = 4;

struct Curve {
    // The outer x coordinates are fixed at -100 and +100 percent.
    std::array<std::int8_t, 3> x{-50, 0, 50};
    std::array<std::int8_t, 5> y{-100, -50, 0, 50, 100};
    constexpr bool operator==(const Curve &) const noexcept = default;
};

struct InputWeightTag;
struct MixWeightTag;
struct InputOffsetTag;
struct MixOffsetTag;
struct ExpoPercentTag;
struct TravelPermilleTag;
struct SubtrimPermilleTag;
struct TrimStepsTag;

using InputWeight = BoundedValue<InputWeightTag, std::int8_t, -100, 100>;
using MixWeight = BoundedValue<MixWeightTag, std::int16_t, -500, 500>;
using InputOffset = BoundedValue<InputOffsetTag, std::int8_t, -100, 100>;
using MixOffset = BoundedValue<MixOffsetTag, std::int16_t, -500, 500>;
using ExpoPercent = BoundedValue<ExpoPercentTag, std::int8_t, -100, 100>;
using TravelPermille = BoundedValue<TravelPermilleTag, std::int16_t, -1500, 1500>;
using SubtrimPermille = BoundedValue<SubtrimPermilleTag, std::int16_t, -1000, 1000>;
using TrimSteps = BoundedValue<TrimStepsTag, std::int8_t, -127, 127>;

static_assert(sizeof(InputWeight) == sizeof(std::int8_t));
static_assert(sizeof(MixWeight) == sizeof(std::int16_t));
static_assert(sizeof(InputOffset) == sizeof(std::int8_t));
static_assert(sizeof(MixOffset) == sizeof(std::int16_t));
static_assert(sizeof(ExpoPercent) == sizeof(std::int8_t));
static_assert(sizeof(TravelPermille) == sizeof(std::int16_t));
static_assert(sizeof(SubtrimPermille) == sizeof(std::int16_t));
static_assert(sizeof(TrimSteps) == sizeof(std::int8_t));

enum class SourceKind : std::uint8_t { analog, input, switch_position, maximum };

struct Source {
    SourceKind kind{SourceKind::analog};
    std::uint8_t index{};
    bool inverted{};
};

enum class SwitchPosition : std::int8_t {
    up = -1,
    middle = 0,
    down = 1,
    pos1 = 2,
    pos2,
    pos3,
    pos4,
    pos5,
    pos6
};

[[nodiscard]] constexpr bool valid_switch_position(const SwitchPosition position) noexcept {
    return (position >= SwitchPosition::up) && (position <= SwitchPosition::pos6);
}

[[nodiscard]] constexpr std::int16_t switch_value(const SwitchPosition position) noexcept {
    if (position <= SwitchPosition::down)
        return static_cast<std::int16_t>(static_cast<int>(position) * resolution);
    constexpr std::array<std::int16_t, 6> values{-1024, -614, -205, 205, 614, 1024};
    return valid_switch_position(position) ? values[static_cast<std::size_t>(position) - 2] : 0;
}

struct Condition {
    bool enabled{};
    // Physical switches, then trim rockers, then L1..L8 (down=true, up=false).
    std::uint8_t index{};
    SwitchPosition position{SwitchPosition::up};
    bool inverted{};
};

enum class LogicalFunction : std::uint8_t {
    disabled,
    greater,
    less,
    equal,
    not_equal,
    greater_equal,
    less_equal,
    and_,
    or_,
    xor_,
    srff
};

struct LogicalSwitch {
    LogicalFunction function{};
    // Comparisons: analogs followed by physical switches/trim rockers.
    // Boolean functions: physical switches/trim rockers followed by logical switches.
    std::uint8_t a{};
    std::uint8_t b{};
    SwitchPosition position_a{SwitchPosition::up};
    SwitchPosition position_b{SwitchPosition::up};
    std::int16_t threshold{};
    bool inverted_a{};
    bool inverted_b{};
    bool inverted{};
};

struct LogicalState {
    std::uint8_t latches{};
#if CORETX_MODUL_TANKMIX
    bool tank_reverse{};
#endif
};

struct LogicalFrame {
    std::uint8_t available{};
    std::uint8_t active{};
};

enum class InputSide : std::uint8_t { both, negative, positive };
enum class TrimSource : std::uint8_t { automatic, none, stick1, stick2, stick3, stick4 };
enum class Multiplex : std::uint8_t { add, multiply, replace };
enum class MixRole : std::uint8_t { manual, tank_drive, tank_forward, tank_reverse };

struct TankMixer {
    // Retain storage metadata in module-free builds for lossless inactive-model roundtrips.
    bool enabled{};
    std::uint8_t drive_input{0}, steering_input{1}, left_channel{0}, right_channel{1},
        steering_percent{100}, hysteresis_percent{3};
    bool reverse_steering{true};
    constexpr bool operator==(const TankMixer &) const noexcept = default;
};

struct InputLine {
    std::uint8_t destination{};
    Source source{};
    Condition condition{};
    InputSide side{InputSide::both};
    TrimSource trim{TrimSource::automatic};
    InputWeight weight_percent{InputWeight::constant<100>()};
    InputOffset offset_percent{};
    ExpoPercent expo_percent{};
    // 0 disables the curve; 1..4 select a model curve.
    std::uint8_t curve{};
};

struct MixLine {
    std::uint8_t destination{};
    Source source{SourceKind::input, 0, false};
    Condition condition{};
    Multiplex multiplex{Multiplex::add};
    bool carry_trim{true};
    MixWeight weight_percent{MixWeight::constant<100>()};
    MixOffset offset_percent{};
    std::uint8_t curve{};
    MixRole role{MixRole::manual};
};

struct Output {
    TravelPermille minimum_permille{TravelPermille::constant<-1000>()};
    TravelPermille maximum_permille{TravelPermille::constant<1000>()};
    SubtrimPermille subtrim_permille{};
    bool reversed{};
};

inline constexpr std::size_t multiswitch_function_count = 8;
namespace multiswitch_control {
inline constexpr unsigned byte_bits = std::numeric_limits<std::uint8_t>::digits;
inline constexpr unsigned source_bits = 5;
inline constexpr unsigned position_bits = 4;
inline constexpr unsigned output_bits = 3;
inline constexpr unsigned position_shift = source_bits;
inline constexpr unsigned output_shift = position_shift + position_bits;
inline constexpr std::uint16_t source_mask = (1U << source_bits) - 1U;
inline constexpr std::uint8_t no_source = static_cast<std::uint8_t>(source_mask);
inline constexpr std::uint16_t position_mask = ((1U << position_bits) - 1U) << position_shift;
inline constexpr std::uint16_t output_mask = ((1U << output_bits) - 1U) << output_shift;
inline constexpr std::uint16_t address_override = 1U << (output_shift + output_bits);
inline constexpr std::uint16_t inverted = address_override << 1;
inline constexpr std::uint16_t valid_mask = (inverted << 1) - 1U;
} // namespace multiswitch_control

struct MultiSwitchControl {
    std::array<std::uint8_t, sizeof(std::uint16_t) + sizeof(std::uint8_t)> bytes{
        multiswitch_control::no_source, 0, 0};

    [[nodiscard]] constexpr std::uint16_t flags() const noexcept {
        return static_cast<std::uint16_t>(
            bytes[0] | (std::uint16_t{bytes[1]} << multiswitch_control::byte_bits));
    }
    [[nodiscard]] constexpr std::uint8_t output() const noexcept {
        return static_cast<std::uint8_t>((flags() & multiswitch_control::output_mask) >>
                                         multiswitch_control::output_shift);
    }
    constexpr bool set_output(const std::uint8_t value) noexcept {
        if (value >= multiswitch_function_count)
            return false;
        set_flags(static_cast<std::uint16_t>(
            (flags() & ~multiswitch_control::output_mask) |
            (std::uint16_t{value} << multiswitch_control::output_shift)));
        return true;
    }
    [[nodiscard]] constexpr std::int16_t address() const noexcept {
        return (flags() & multiswitch_control::address_override) != 0 ? bytes.back() : -1;
    }
    constexpr bool set_address(const int value) noexcept {
        if ((value < -1) || (value > std::numeric_limits<std::uint8_t>::max()))
            return false;
        bytes.back() = static_cast<std::uint8_t>(value < 0 ? 0 : value);
        set_flags(static_cast<std::uint16_t>(
            (std::uint32_t{flags()} & ~std::uint32_t{multiswitch_control::address_override}) |
            (value < 0 ? 0U : multiswitch_control::address_override)));
        return true;
    }
    [[nodiscard]] constexpr std::int16_t target_address(const std::int16_t page) const noexcept {
        return address() < 0 ? page : address();
    }
    [[nodiscard]] constexpr Condition condition() const noexcept {
        const auto source = static_cast<std::uint8_t>(flags() & multiswitch_control::source_mask);
        const auto position = static_cast<int>((flags() & multiswitch_control::position_mask) >>
                                               multiswitch_control::position_shift);
        return {source != multiswitch_control::no_source,
                static_cast<std::uint8_t>(source == multiswitch_control::no_source ? 0 : source),
                static_cast<SwitchPosition>(position - 1),
                (flags() & multiswitch_control::inverted) != 0};
    }
    constexpr bool set_condition(const Condition &value) noexcept {
        if (!valid_switch_position(value.position) ||
            (value.enabled &&
             ((value.index >= (switch_count + logical_switch_count)) ||
              ((value.index >= switch_count) && (value.position != SwitchPosition::up) &&
               (value.position != SwitchPosition::down)))))
            return false;
        set_flags(static_cast<std::uint16_t>(
            (std::uint32_t{flags()} &
             ~std::uint32_t{multiswitch_control::source_mask | multiswitch_control::position_mask |
                            multiswitch_control::inverted}) |
            (value.enabled ? value.index : multiswitch_control::no_source) |
            (static_cast<unsigned>(static_cast<int>(value.position) + 1)
             << multiswitch_control::position_shift) |
            (value.inverted ? multiswitch_control::inverted : 0U)));
        return true;
    }
    constexpr bool operator==(const MultiSwitchControl &) const noexcept = default;

  private:
    constexpr void set_flags(const std::uint16_t value) noexcept {
        bytes[0] = static_cast<std::uint8_t>(value);
        bytes[1] = static_cast<std::uint8_t>(value >> multiswitch_control::byte_bits);
    }
};

struct MultiSwitchPage {
    // -1 disables a page; 0..255 address the SET4 module.
    std::int16_t address{-1};
    std::uint8_t momentary{};
    std::array<std::array<char, 6>, multiswitch_function_count> labels{};
    std::array<MultiSwitchControl, multiswitch_function_count> controls{};
    constexpr MultiSwitchPage() noexcept {
        for (std::size_t i = 0; i < controls.size(); ++i)
            controls[i].set_output(static_cast<std::uint8_t>(i));
    }
    constexpr bool operator==(const MultiSwitchPage &) const noexcept = default;
};
inline constexpr std::size_t multiswitch_page_count = 4;

struct Model {
    // Active lines must be grouped in ascending destination order.
    FixedVector<InputLine, max_input_lines> inputs{};
    FixedVector<MixLine, max_mix_lines> mixes{};
    // Explicit elements avoid partial runtime aggregate initialization with GCC 16.
    std::array<Output, channel_count> outputs{
        Output{}, Output{}, Output{}, Output{}, Output{}, Output{}, Output{}, Output{},
        Output{}, Output{}, Output{}, Output{}, Output{}, Output{}, Output{}, Output{}};
    std::array<LogicalSwitch, logical_switch_count> logical_switches{};
    // Explicit elements avoid partial runtime aggregate initialization with GCC 16.
    std::array<Curve, curve_count> curves{Curve{}, Curve{}, Curve{}, Curve{}};
    // 0 disables a slot; 1..16 select a channel.
    std::array<std::uint8_t, 8> dashboard_channels{1, 2, 3, 4, 5, 6, 7, 8};
    std::array<MultiSwitchPage, multiswitch_page_count> multiswitch{};
    bool crsf_dashboard : 1 {};
    bool multiswitch_beep : 1 {true};
    std::array<TrimSteps, trim_count> trims{};
    std::uint8_t trim_switches{};
    TankMixer tank{};
};

struct Controls {
    // Already calibrated and mapped to logical stick order, nominal +/-1024.
    std::array<std::int16_t, analog_count> analogs{};
    std::array<SwitchPosition, switch_count> switches{};
    // Reduced symmetric trim range; one step is two channel units.
    std::array<TrimSteps, trim_count> trims{};
    std::uint16_t active_analogs{0xFFFF};
    std::uint32_t active_switches{(1U << switch_count) - 1U};
};

struct Frame {
    std::array<std::int16_t, input_count> inputs{};
    std::array<MixValue, channel_count> mixes{};
    std::array<std::int16_t, channel_count> channels{};
    LogicalFrame logical{};
#if CORETX_MODUL_TANKMIX
    bool tank_reverse{};
#endif
};

enum class ErrorCode : std::uint8_t {
    none,
    line_count,
    destination,
    line_order,
    source,
    condition,
    input_settings,
    mix_settings,
    output_settings,
    control_value,
    arithmetic_overflow,
    logical_settings,
    logical_cycle,
    dashboard_settings,
    multiswitch_settings,
    multiswitch_duplicate,
    curve_settings,
    tank_settings,
    tank_conflict,
    capacity,
    tank_unavailable
};

enum class Stage : std::uint8_t { model, inputs, mixes, outputs, controls };

struct Status {
    ErrorCode code{ErrorCode::none};
    Stage stage{Stage::model};
    std::size_t index{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return code == ErrorCode::none;
    }
};

struct Evaluation {
    Status status{};
    Frame frame{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return static_cast<bool>(status);
    }
};

[[nodiscard]] constexpr Status validate(const Model &model) noexcept;
#if CORETX_MODUL_TANKMIX
// Atomically replaces only module-owned lines; manual lines on enabled tank outputs conflict.
[[nodiscard]] constexpr Status configure_tank(Model &model, const TankMixer &tank) noexcept;
[[nodiscard]] constexpr bool tank_mix_active(MixRole role, const Frame &frame) noexcept;
#endif
// Percent coordinates are rounded to resolution units; values outside the endpoints saturate.
[[nodiscard]] constexpr std::int32_t evaluate_curve(const Curve &curve,
                                                    std::int32_t input) noexcept;
// No output frame may be published unless status reports success.
[[nodiscard]] constexpr Evaluation evaluate(const Model &model, const Controls &controls) noexcept;
[[nodiscard]] constexpr Evaluation evaluate(const Model &model, const Controls &controls,
                                            LogicalState &state) noexcept;
[[nodiscard]] constexpr const char *error_message(const ErrorCode &code) noexcept;
[[nodiscard]] constexpr const char *stage_name(const Stage &stage) noexcept;

} // namespace coretx

#include "coretx/core/detail/core_impl.hpp"
