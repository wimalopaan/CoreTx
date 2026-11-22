// SPDX-License-Identifier: GPL-2.0-or-later
#include "edgetx_pipeline.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

using mixsrc_t = std::int16_t;
using getvalue_t = std::int16_t;
using delayval_t = std::int16_t;
using bitfield_channels_t = std::uint32_t;

constexpr int MAX_EXPOS = static_cast<int>(coretx::max_input_lines);
constexpr int MAX_MIXERS = static_cast<int>(coretx::max_mix_lines);
constexpr int MAX_OUTPUT_CHANNELS = static_cast<int>(coretx::channel_count);
constexpr int RESX = coretx::resolution;
constexpr unsigned RESXu = RESX;
constexpr std::int32_t RESXl = RESX;
constexpr int RESX_SHIFT = 10;
constexpr int DELAY_POS_MARGIN = 3;
constexpr int TRIM_ON = 0;
constexpr int TRIM_OFF = 1;
constexpr int TRIM_MAX = 128;
constexpr int TRIM_EXTENDED_MAX = 512;
constexpr int IDLE_TRIM_SCALE = 2;
constexpr std::uint8_t e_perout_mode_normal = 0;
constexpr std::uint8_t e_perout_mode_inactive_flight_mode = 1;
constexpr std::uint8_t e_perout_mode_notrims = 4;
constexpr int CURVE_REF_DIFF = 0;
constexpr int CURVE_REF_EXPO = 1;
constexpr int CURVE_REF_CUSTOM = 3;
constexpr int MLTPX_ADD = 0;
constexpr int MLTPX_MUL = 1;
constexpr int MLTPX_REPL = 2;
constexpr int FUNCTION_TRAINER_CHANNELS = 0;

constexpr int MIXSRC_FIRST_INPUT = 1;
constexpr int MIXSRC_LAST_INPUT = MIXSRC_FIRST_INPUT + static_cast<int>(coretx::input_count) - 1;
constexpr int MIXSRC_FIRST_STICK = MIXSRC_LAST_INPUT + 1;
constexpr int MIXSRC_LAST_STICK = MIXSRC_FIRST_STICK + static_cast<int>(coretx::trim_count) - 1;
constexpr int first_switch_source = MIXSRC_FIRST_STICK + static_cast<int>(coretx::analog_count);
constexpr int maximum_source = first_switch_source + static_cast<int>(coretx::switch_count);
constexpr int MIXSRC_FIRST_TRAINER = maximum_source + 1;
constexpr int MIXSRC_LAST_TRAINER = MIXSRC_FIRST_TRAINER + 15;
constexpr int MIXSRC_FIRST_CH = MIXSRC_LAST_TRAINER + 1;
constexpr int MIXSRC_LAST_CH = MIXSRC_FIRST_CH + MAX_OUTPUT_CHANNELS - 1;
constexpr int MIXSRC_FIRST_TELEM = MIXSRC_LAST_CH + 1;
constexpr int MIXSRC_FIRST_TRIM = MIXSRC_FIRST_TELEM + 1;
constexpr int disabled_condition = static_cast<int>(coretx::switch_count) * 3 + 1;

template <typename T> T limit(const T low, const T value, const T high) {
    return std::clamp(value, low, high);
}
template <typename T> T max(const T left, const T right) { return std::max(left, right); }
void memclear(void *const data, const std::size_t size) { std::memset(data, 0, size); }

[[noreturn]] void unsupported(const char *const feature) {
    std::fprintf(stderr, "Unsupported EdgeTX reference path: %s\n", feature);
    std::abort();
}

struct CurveRef {
    int type{};
    int value{};
};
struct ExpoData {
    std::uint8_t chn{};
    mixsrc_t srcRaw{};
    std::int16_t swtch{};
    int mode{};
    int flightModes{};
    int scale{};
    int weight{100};
    int offset{};
    int trimSource{TRIM_ON};
    CurveRef curve{};
};
struct MixData {
    std::uint8_t destCh{};
    mixsrc_t srcRaw{};
    std::int16_t swtch{};
    int flightModes{};
    int carryTrim{};
    int mltpx{MLTPX_ADD};
    int weight{100};
    int offset{};
    int mixWarn{};
    int delayUp{};
    int delayDown{};
    int speedUp{};
    int speedDown{};
    bool delayPrec{};
    bool speedPrec{};
    CurveRef curve{};
};
struct LimitData {
    int minimum{-1000};
    int maximum{1000};
    int offset{};
    int curve{};
    bool revert{};
};
struct NativeModel {
    std::array<ExpoData, coretx::max_input_lines> inputs{};
    std::array<MixData, coretx::max_mix_lines> mixes{};
    std::array<LimitData, coretx::channel_count> outputs{};
    bool thrTrim{};
    bool throttleReversed{};
    bool extendedTrims{};

    int getThrottleStickTrimSource() const { return MIXSRC_FIRST_TRIM + 2; }
};
struct MixState {
    bool activeExpo{};
    bool activeMix{};
    delayval_t now{};
    delayval_t prev{};
    delayval_t delay{};
};

NativeModel g_model{};
coretx::Controls controls{};
std::int16_t anas[coretx::input_count]{};
std::int8_t virtualInputsTrims[coretx::input_count]{};
bool virtualInputsTrimsReversed[coretx::input_count]{};
std::int16_t trims[coretx::trim_count]{};
std::int32_t chans[coretx::channel_count]{};
std::int32_t act[coretx::max_mix_lines]{};
MixState mixState[coretx::max_mix_lines]{};
std::int16_t trainerInput[coretx::channel_count]{};
std::uint8_t mixerCurrentFlightMode{};
[[maybe_unused]] std::uint8_t mixWarning{};
bool s_mixer_first_run_done{};
int trimsCheckTimer{};
constexpr bitfield_channels_t all_channels_dirty = 0xFFFF;

ExpoData *expoAddress(const std::uint8_t index) { return &g_model.inputs[index]; }
MixData *mixAddress(const std::uint8_t index) { return &g_model.mixes[index]; }
LimitData *limitAddress(const std::uint8_t index) { return &g_model.outputs[index]; }
bool isTrainerValid() { return false; }
bool isFunctionActive(int) { return false; }
int keysGetMaxTrims() { return static_cast<int>(coretx::trim_count); }
int getTrimValue(std::uint8_t, const std::uint8_t index) { return controls.trims[index].value(); }
void evalLogicalSwitches(bool) {}
int convertTelemValue(int, int) { unsupported("telemetry"); }
int applyCustomCurve(int, int) { unsupported("custom curve"); }

bool getSwitch(const std::int16_t source) {
    if (source == 0) {
        return true;
    }
    if (source == disabled_condition) {
        return false;
    }
    const int absolute = std::abs(source);
    if (absolute > (static_cast<int>(coretx::switch_count) * 3)) {
        unsupported("switch condition");
    }
    const auto index = static_cast<std::size_t>((absolute - 1) / 3);
    const int position = (absolute - 1) % 3 - 1;
    return (static_cast<int>(controls.switches[index]) == position) != (source < 0);
}

getvalue_t getValue(const mixsrc_t source) {
    const int absolute = std::abs(source);
    int value{};
    if ((absolute >= MIXSRC_FIRST_INPUT) && (absolute <= MIXSRC_LAST_INPUT)) {
        value = anas[absolute - MIXSRC_FIRST_INPUT];
    } else if ((absolute >= MIXSRC_FIRST_STICK) && (absolute < first_switch_source)) {
        value = controls.analogs[static_cast<std::size_t>(absolute - MIXSRC_FIRST_STICK)];
    } else if ((absolute >= first_switch_source) && (absolute < maximum_source)) {
        value = static_cast<int>(
                    controls.switches[static_cast<std::size_t>(absolute - first_switch_source)]) *
                RESX;
    } else if (absolute == maximum_source) {
        value = RESX;
    } else {
        unsupported("source");
    }
    return static_cast<getvalue_t>(source < 0 ? -value : value);
}

int getSourceNumFieldValue(const int value, const int minimum, const int maximum) {
    return std::clamp(value * 10, minimum * 10, maximum * 10);
}
int expo(int value, int amount);
int applyCurve(const int value, CurveRef &curve) {
    if (curve.type != CURVE_REF_EXPO) {
        unsupported("non-expo curve");
    }
    return expo(value, curve.value);
}
void applyExpos(std::int16_t *values, std::uint8_t mode, std::int16_t override_index = 0,
                std::int16_t override_value = 0);
void evalTrims();
void evalInputs(const std::uint8_t mode) {
    // Board calibration is bypassed: Controls already contains normalized samples.
    applyExpos(anas, mode);
    evalTrims();
}

#define EXPO_VALID(ed) ((ed)->mode)
#define EXPO_MODE_ENABLE(ed, v)                                                                    \
    ((((v) < 0) && ((ed)->mode & 1)) || (((v) >= 0) && ((ed)->mode & 2)))
#define LIMIT_OFS_RESX(lim) calc1000toRESX((lim)->offset)
#define LIMIT_MIN_RESX(lim) calc1000toRESX((lim)->minimum)
#define LIMIT_MAX_RESX(lim) calc1000toRESX((lim)->maximum)
#define calc100to256_16Bits(x) calc100to256(x)
#define calc100toRESX_16Bits(x) calc100toRESX(x)

#include "upstream/mixer.inc"

constexpr mixsrc_t encode_source(const coretx::Source &source) {
    int encoded{};
    switch (source.kind) {
    case coretx::SourceKind::analog:
        encoded = MIXSRC_FIRST_STICK + source.index;
        break;
    case coretx::SourceKind::input:
        encoded = MIXSRC_FIRST_INPUT + source.index;
        break;
    case coretx::SourceKind::switch_position:
        encoded = first_switch_source + source.index;
        break;
    case coretx::SourceKind::maximum:
        encoded = maximum_source;
        break;
    }
    return static_cast<mixsrc_t>(source.inverted ? -encoded : encoded);
}

constexpr std::int16_t encode_condition(const coretx::Condition &condition) {
    if (!condition.enabled) {
        return 0;
    }
    const int encoded = 1 + condition.index * 3 + static_cast<int>(condition.position) + 1;
    return static_cast<std::int16_t>(condition.inverted ? -encoded : encoded);
}

void configure(const coretx::Model &model) {
    g_model = {};
    for (std::size_t i = 0; i < model.inputs.size(); ++i) {
        const auto &line = model.inputs[i];
        auto &native = g_model.inputs[i];
        native.chn = line.destination;
        native.srcRaw = encode_source(line.source);
        native.swtch = encode_condition(line.condition);
        switch (line.side) {
        case coretx::InputSide::both:
            native.mode = 3;
            break;
        case coretx::InputSide::negative:
            native.mode = 1;
            break;
        case coretx::InputSide::positive:
            native.mode = 2;
            break;
        }
        switch (line.trim) {
        case coretx::TrimSource::automatic:
            native.trimSource = TRIM_ON;
            break;
        case coretx::TrimSource::none:
            native.trimSource = TRIM_OFF;
            break;
        default:
            native.trimSource =
                -1 - (static_cast<int>(line.trim) - static_cast<int>(coretx::TrimSource::stick1));
            break;
        }
        native.weight = line.weight_percent.value();
        native.offset = line.offset_percent.value();
        native.curve = {CURVE_REF_EXPO, line.expo_percent.value()};
    }
    for (std::size_t i = 0; i < model.mixes.size(); ++i) {
        const auto &line = model.mixes[i];
        auto &native = g_model.mixes[i];
        native.destCh = line.destination;
        native.srcRaw = encode_source(line.source);
        native.swtch = encode_condition(line.condition);
        native.carryTrim = line.carry_trim ? 0 : 1;
        switch (line.multiplex) {
        case coretx::Multiplex::add:
            native.mltpx = MLTPX_ADD;
            break;
        case coretx::Multiplex::multiply:
            native.mltpx = MLTPX_MUL;
            break;
        case coretx::Multiplex::replace:
            native.mltpx = MLTPX_REPL;
            break;
        }
        native.weight = line.weight_percent.value();
        native.offset = line.offset_percent.value();
    }
    // Disabled tail lines keep the upstream activeMixes UI array initialized;
    // unlike a srcRaw=0 terminator, they do not break its initialization loop.
    for (std::size_t i = model.mixes.size(); i < coretx::max_mix_lines; ++i) {
        auto &native = g_model.mixes[i];
        native.destCh = model.mixes.size() ? model.mixes[model.mixes.size() - 1].destination : 0;
        native.srcRaw = maximum_source;
        native.swtch = disabled_condition;
    }
    for (std::size_t i = 0; i < coretx::channel_count; ++i) {
        const auto &output = model.outputs[i];
        g_model.outputs[i] = {output.minimum_permille.value(), output.maximum_permille.value(),
                              output.subtrim_permille.value(), 0, output.reversed};
    }
}

} // namespace

coretx::Frame edgetx_reference::Pipeline::evaluate(const coretx::Model &model,
                                                   const coretx::Controls &samples) {
    if (!coretx::validate(model)) {
        unsupported("invalid model");
    }
    configure(model);
    controls = samples;
    std::copy(inputs_.begin(), inputs_.end(), anas);
    std::copy(trim_origins_.begin(), trim_origins_.end(), virtualInputsTrims);
    std::copy(trim_reversed_.begin(), trim_reversed_.end(), virtualInputsTrimsReversed);
    std::fill(std::begin(mixState), std::end(mixState), MixState{});
    evalFlightModeMixes(e_perout_mode_normal, 1);
    std::copy(std::begin(anas), std::end(anas), inputs_.begin());
    std::copy(std::begin(virtualInputsTrims), std::end(virtualInputsTrims), trim_origins_.begin());
    std::copy(std::begin(virtualInputsTrimsReversed), std::end(virtualInputsTrimsReversed),
              trim_reversed_.begin());
    coretx::Frame frame{};
    frame.inputs = inputs_;
    for (std::size_t i = 0; i < coretx::channel_count; ++i) {
        frame.mixes[i] = coretx::MixValue::from_raw(chans[i]);
        frame.channels[i] = applyLimits(static_cast<std::uint8_t>(i), chans[i]);
    }
    return frame;
}
