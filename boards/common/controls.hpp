// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/common/hardware.hpp"
#include "boards/common/trims.hpp"
#include "coretx/core/controls.hpp"
#include "coretx/core/input_setup.hpp"
#include "coretx/features.hpp"
#include "coretx/ui/input.hpp"
#include "platform/stm32f072/adc.hpp"

#include <memory>
#include <span>

namespace coretx::firmware {

using StockSamples = std::array<std::uint16_t, stock_inputs.size()>;
inline constexpr std::array<std::size_t, 6> analog_indices{0, 1, 2, 3, 6, 7};
inline constexpr std::array<std::size_t, 4> switch_indices{4, 5, 8, 9};
inline constexpr std::array<std::string_view, 10> control_names{"RH",  "RV", "LV", "LH", "VRA",
                                                                "VRB", "SA", "SB", "SC", "SD"};
static_assert((analog_indices.size() <= calibration_count) &&
              (switch_indices.size() <= switch_count));

[[nodiscard]] constexpr SwitchPosition decode_switch(const std::uint16_t raw,
                                                     const bool three_position) noexcept {
    if (three_position) {
        return raw < 1365   ? SwitchPosition::up
               : raw < 2730 ? SwitchPosition::middle
                            : SwitchPosition::down;
    }
    return raw < 2048 ? SwitchPosition::up : SwitchPosition::down;
}

// Builds the diagnostic model in caller-owned storage. Firmware code uses this instead of
// diagnostic_model so that the mostly empty 1 KB model image is not kept in flash.
constexpr void make_diagnostic_model(Model &model) noexcept {
    std::destroy_at(&model);
    std::construct_at(&model);
    model.inputs.resize<analog_indices.size()>();
    model.mixes.resize<control_names.size()>();
    for (std::size_t i = 0; i < model.inputs.size(); ++i) {
        auto &line = model.inputs[i];
        line.destination = static_cast<std::uint8_t>(i);
        line.source.index = static_cast<std::uint8_t>(i);
        line.trim = TrimSource::automatic;
    }
    for (std::size_t i = 0; i < model.mixes.size(); ++i) {
        auto &line = model.mixes[i];
        line.destination = static_cast<std::uint8_t>(i);
        line.source = i < analog_indices.size()
                          ? Source{SourceKind::input, static_cast<std::uint8_t>(i), false}
                          : Source{SourceKind::switch_position,
                                   static_cast<std::uint8_t>(i - analog_indices.size()), false};
        line.carry_trim = true;
    }
}
// Compile-time reference for tests and host tools; odr-using it in firmware costs flash.
inline constexpr Model diagnostic_model = [] {
    Model model{};
    make_diagnostic_model(model);
    return model;
}();
static_assert(static_cast<bool>(validate(diagnostic_model)));

enum class CalibrationPhase : std::uint8_t { idle, center, range };
enum class CalibrationError : std::uint8_t { none, no_sample, range };

class Diagnostics {
  public:
    inline static constexpr std::uint16_t minimum_half_span = 128;
    inline static constexpr std::size_t overview_page = 0;
    inline static constexpr std::size_t system_page = 1;
    inline static constexpr std::size_t page_count = 2;
    inline static constexpr std::size_t calibration_rows = 6;
    struct CalibrationRow {
        std::string_view name{};
        std::uint16_t sample{};
        AnalogCalibration range{};
    };

    constexpr explicit Diagnostics(const bool sb_three_position = false) noexcept
        : Diagnostics(sb_three_position, diagnostic_model) {}
    // The firmware passes the model storage's active model, which starts as the diagnostic model.
    constexpr Diagnostics(const bool sb_three_position, const Model &model) noexcept
        : input_setup_(sb_three_position), model_(&model) {
        setup_.mapping[0].reversed = true;
        setup_.mapping[1].reversed = true;
        setup_.calibration[4] = {0, 2048, 4095};
        setup_.calibration[5] = {0, 2048, 4095};
        // The two unconnected core analog slots are explicitly neutral, never ADC channels.
        for (auto i = analog_indices.size(); i < calibration_count; ++i) {
            setup_.calibration[i] = {0, 2048, 4095};
            raw_.analogs[i] = 2048;
        }
    }

    constexpr void observe(const std::span<const std::uint16_t> samples,
                           const TimestampUs now) noexcept {
        if (samples.size() < stock_inputs.size()) {
            invalidate();
            return;
        }
        std::copy_n(samples.begin(), samples_.size(), samples_.begin());
        valid_ = true;
        for (std::size_t i = 0; i < analog_indices.size(); ++i) {
            raw_.analogs[i] = samples[analog_indices[i]];
            if (phase_ == CalibrationPhase::range) {
                auto &axis = draft_[i];
                axis.minimum = std::min(axis.minimum, raw_.analogs[i]);
                axis.maximum = std::max(axis.maximum, raw_.analogs[i]);
            }
        }
        constexpr std::array<std::size_t, auxiliary_count> indices{4,  5,  8,  9,  6, 7,
                                                                   11, 12, 13, 14, 15};
        for (std::size_t i = 0; i < auxiliary_count; ++i) {
            if (indices[i] < samples.size())
                auxiliary_samples_[i] = samples[indices[i]];
            const auto mode = input_setup_.modes[i];
            if (auxiliary_enabled(i) && is_pot(mode) && (phase_ == CalibrationPhase::range)) {
                auto &axis = input_draft_[i];
                axis.minimum = std::min(axis.minimum, auxiliary_samples_[i]);
                axis.maximum = std::max(axis.maximum, auxiliary_samples_[i]);
            }
            const auto position =
                auxiliary_enabled(i) && (mode >= InputMode::two_position)
                    ? decode_input_switch(auxiliary_samples_[i], mode, input_setup_.calibration[i])
                    : SwitchPosition::middle;
            if (!setup_frozen_ || ((i != 4) && (i != 5)))
                debounce(i, position, now);
            if (auxiliary_enabled(i) && (mode == InputMode::pot_detent) &&
                (phase_ == CalibrationPhase::idle) && calibrated_) {
                const auto value =
                    decode_pot(auxiliary_samples_[i], input_setup_.calibration[i], true);
                const int side = value < -20 ? -1 : value > 20 ? 1 : 0;
                if (side != 0) {
                    if ((detent_side_[i] != 0) && (side != detent_side_[i]) &&
                        ((now - last_click_) >= 100'000_us)) {
                        click_ = true;
                        last_click_ = now;
                    }
                    detent_side_[i] = static_cast<std::int8_t>(side);
                }
            }
        }
        switches_initialized_ = true;
        refresh(true);
    }

    constexpr void invalidate() noexcept {
        valid_ = false;
        output_valid_ = false;
    }

    [[nodiscard]] static constexpr bool valid_setup(const ControlSetup &setup) noexcept {
        if (!coretx::validate(setup)) {
            return false;
        }
        for (std::size_t i = 0; i < calibration_count; ++i) {
            const auto physical = setup.mapping[i].physical_index;
            if ((i < 4 ? physical >= 4 : physical != i) ||
                (setup.mapping[i].reversed != (physical < 2))) {
                return false;
            }
            const auto &axis = setup.calibration[i];
            if ((axis.maximum > 4095) || ((i < analog_indices.size()) &&
                                          (((axis.center - axis.minimum) < minimum_half_span) ||
                                           ((axis.maximum - axis.center) < minimum_half_span)))) {
                return false;
            }
        }
        return stick_mode(setup) != 0;
    }

    [[nodiscard]] constexpr bool restore_calibration(const ControlSetup &setup) noexcept {
        if ((phase_ != CalibrationPhase::idle) || !valid_setup(setup)) {
            return false;
        }
        setup_ = setup;
        if (!custom_inputs_) {
            input_setup_.calibration[4] = setup.calibration[4];
            input_setup_.calibration[5] = setup.calibration[5];
        }
        calibrated_ = true;
        calibration_error_ = CalibrationError::none;
        refresh();
        return true;
    }

    constexpr void handle(const Key key) noexcept {
        if (key == Key::none) {
            return;
        }
        if (key == Key::cancel) {
            phase_ = CalibrationPhase::idle;
            calibration_error_ = CalibrationError::none;
            refresh();
            return;
        }
        if (phase_ == CalibrationPhase::idle) {
            if ((key == Key::up) || (key == Key::down)) {
                const auto count = dashboard_pages();
                page_ = (page_ + (key == Key::down ? 1U : count - 1U)) % count;
            }
            return;
        }
        if ((phase_ == CalibrationPhase::range) && ((key == Key::up) || (key == Key::down))) {
            const auto pages = calibration_pages();
            calibration_page_ = static_cast<std::uint8_t>(
                (calibration_page_ + (key == Key::down ? 1U : pages - 1U)) % pages);
            return;
        }
        if (key != Key::ok) {
            return;
        }
        if (!valid_) {
            calibration_error_ = CalibrationError::no_sample;
            return;
        }
        if (phase_ == CalibrationPhase::center) {
            draft_ = setup_.calibration;
            input_draft_ = input_setup_.calibration;
            for (std::size_t i = 0; i < analog_indices.size(); ++i) {
                const auto sample = raw_.analogs[i];
                draft_[i] = {sample, sample, sample};
            }
            for (std::size_t i = 0; i < auxiliary_count; ++i)
                if (auxiliary_enabled(i) && is_pot(input_setup_.modes[i])) {
                    const auto sample = auxiliary_samples_[i];
                    input_draft_[i] = {sample, sample, sample};
                }
            phase_ = CalibrationPhase::range;
            calibration_error_ = CalibrationError::none;
            return;
        }
        auto candidate = setup_;
        candidate.calibration = draft_;
        for (std::size_t i = 0; i < analog_indices.size(); ++i) {
            auto &axis = candidate.calibration[i];
            if (i >= 4) {
                if (!is_pot(input_setup_.modes[i])) {
                    axis = setup_.calibration[i];
                    continue;
                }
                if (input_setup_.modes[i] == InputMode::pot_detent)
                    continue;
                axis.center =
                    static_cast<std::uint16_t>((std::uint32_t{axis.minimum} + axis.maximum) / 2U);
            }
            if ((axis.center < axis.minimum) || (axis.maximum < axis.center) ||
                ((axis.center - axis.minimum) < minimum_half_span) ||
                ((axis.maximum - axis.center) < minimum_half_span)) {
                calibration_error_ = CalibrationError::range;
                bad_axis_ = i;
                show_calibration_axis(bad_axis_);
                return;
            }
        }
        for (std::size_t i = 0; i < auxiliary_count; ++i) {
            if (!auxiliary_enabled(i) || !is_pot(input_setup_.modes[i]))
                continue;
            auto &axis = input_draft_[i];
            if (input_setup_.modes[i] == InputMode::pot)
                axis.center =
                    static_cast<std::uint16_t>((std::uint32_t{axis.minimum} + axis.maximum) / 2U);
            if ((axis.minimum >= axis.center) || (axis.center >= axis.maximum) ||
                ((axis.center - axis.minimum) < minimum_half_span) ||
                ((axis.maximum - axis.center) < minimum_half_span)) {
                calibration_error_ = CalibrationError::range;
                bad_axis_ = auxiliary_analog_indices[i];
                show_calibration_axis(bad_axis_);
                return;
            }
            if ((i == 4) || (i == 5))
                candidate.calibration[i] = axis;
        }
        setup_ = candidate;
        input_setup_.calibration = input_draft_;
        custom_inputs_ = true;
        detent_side_ = {};
        calibrated_ = true;
        phase_ = CalibrationPhase::idle;
        calibration_error_ = CalibrationError::none;
        refresh();
    }

    [[nodiscard]] constexpr bool valid() const noexcept { return valid_; }
    [[nodiscard]] constexpr bool output_valid() const noexcept { return output_valid_; }
    [[nodiscard]] constexpr bool calibrated() const noexcept { return calibrated_; }
    [[nodiscard]] constexpr CalibrationPhase phase() const noexcept { return phase_; }
    [[nodiscard]] constexpr CalibrationError calibration_error() const noexcept {
        return calibration_error_;
    }
    [[nodiscard]] constexpr std::size_t bad_axis() const noexcept { return bad_axis_; }
    [[nodiscard]] constexpr std::size_t page() const noexcept { return page_; }
    [[nodiscard]] constexpr std::size_t dashboard_pages() const noexcept {
        std::size_t count = page_count;
        if constexpr (features::multiswitch) {
            for (const auto &page : model_->multiswitch)
                count += (page.address >= 0) && (page.address <= 255);
        }
        if constexpr (features::crsf)
            count += model_->crsf_dashboard;
        return count;
    }
    constexpr void constrain_page() noexcept {
        if (page_ >= dashboard_pages())
            page_ = 0;
    }
    [[nodiscard]] constexpr const StockSamples &samples() const noexcept { return samples_; }
    [[nodiscard]] constexpr const RawControls &raw() const noexcept { return raw_; }
    constexpr void observe_setup_controls(const bool active, const std::uint8_t buttons) noexcept {
        // The i6S has no trim keys: their first two reserved source slots hold stock SE/SF.
        bool changed = !rear_inputs_ || (setup_frozen_ != active);
        rear_inputs_ = true;
        if (!setup_frozen_ || !active) {
            for (std::size_t i = 0; i < 2; ++i) {
                const auto position =
                    (buttons & (1U << i)) != 0 ? SwitchPosition::down : SwitchPosition::up;
                auto &previous = raw_.switches[physical_switch_count + i];
                changed = changed || (previous != position);
                previous = position;
            }
        }
        if (active && !setup_frozen_)
            frozen_pots_ = {auxiliary_samples_[4], auxiliary_samples_[5]};
        setup_frozen_ = active;
        if (changed)
            refresh(true);
    }
    [[nodiscard]] constexpr const ControlSetup &setup() const noexcept { return setup_; }
    [[nodiscard]] constexpr const PreparedControls &prepared() const noexcept { return prepared_; }
    [[nodiscard]] constexpr const Evaluation &evaluation() const noexcept { return evaluation_; }
    constexpr void use_model(const Model &model) noexcept {
        logical_state_ = {};
        model_ = &model;
        constrain_page();
        refresh();
    }
    constexpr void allow_logical_commit(const bool enabled) noexcept {
        logical_commit_enabled_ = enabled;
    }
    constexpr void refresh_model() noexcept { refresh(); }
    constexpr void defer_refresh() noexcept { refresh_deferred_ = true; }
    constexpr void flush_refresh() noexcept {
        refresh_deferred_ = false;
        if (!refresh_pending_)
            return;
        const bool commit = refresh_commit_pending_;
        refresh_pending_ = false;
        refresh_commit_pending_ = false;
        refresh(commit);
    }
    [[nodiscard]] constexpr const Model &model() const noexcept { return *model_; }
    inline static constexpr std::array<std::array<std::uint8_t, 4>, 4> stick_mappings{
        std::array<std::uint8_t, 4>{0, 2, 1, 3}, {0, 1, 2, 3}, {3, 1, 2, 0}, {3, 2, 1, 0}};
    [[nodiscard]] constexpr std::size_t stick_mode() const noexcept { return stick_mode(setup_); }
    [[nodiscard]] static constexpr std::size_t stick_mode(const ControlSetup &setup) noexcept {
        for (std::size_t m = 0; m < stick_mappings.size(); ++m) {
            bool match = true;
            for (std::size_t i = 0; i < 4; ++i)
                match = match && (setup.mapping[i].physical_index == stick_mappings[m][i]);
            if (match)
                return m + 1;
        }
        return 0;
    }
    [[nodiscard]] constexpr bool set_stick_mode(const std::size_t mode) noexcept {
        if ((mode < 1) || (mode > 4) || (phase_ != CalibrationPhase::idle))
            return false;
        for (std::size_t i = 0; i < 4; ++i) {
            const auto physical = stick_mappings[mode - 1][i];
            setup_.mapping[i] = {physical, physical < 2};
        }
        refresh();
        return true;
    }
    constexpr void begin_calibration() noexcept {
        page_ = 0;
        phase_ = CalibrationPhase::center;
        calibration_page_ = 0;
        calibration_error_ = CalibrationError::none;
        output_valid_ = false;
    }
    [[nodiscard]] constexpr const InputSetup &input_setup() const noexcept { return input_setup_; }
    constexpr void set_trim_beep(const bool enabled) noexcept { input_setup_.trim_beep = enabled; }
    [[nodiscard]] constexpr std::size_t calibration_pages() const noexcept {
        std::size_t count = 4;
        for (std::size_t i = 0; i < auxiliary_count; ++i)
            count += auxiliary_enabled(i) && is_pot(input_setup_.modes[i]) ? 1U : 0U;
        return (count + calibration_rows - 1U) / calibration_rows;
    }
    [[nodiscard]] constexpr std::size_t calibration_first() const noexcept {
        return std::size_t{calibration_page_} * calibration_rows;
    }
    [[nodiscard]] constexpr std::optional<CalibrationRow>
    calibration_row(std::size_t row) const noexcept {
        if (row < 4)
            return CalibrationRow{control_names[row], raw_.analogs[row], draft_[row]};
        row -= 4;
        for (std::size_t i = 0; i < auxiliary_count; ++i) {
            if (!auxiliary_enabled(i) || !is_pot(input_setup_.modes[i]))
                continue;
            if (row == 0)
                return CalibrationRow{auxiliary_names[i], auxiliary_samples_[i], input_draft_[i]};
            --row;
        }
        return {};
    }
    [[nodiscard]] constexpr bool auxiliary_enabled(const std::size_t index) const noexcept {
        return (index < 6) || (features::extended_inputs && input_setup_.extra_analog &&
                               (index < (6 + extra_adc_count)));
    }
    [[nodiscard]] constexpr bool extra_digital_enabled() const noexcept {
        return digital_pin_mask() != 0;
    }
    [[nodiscard]] constexpr bool digital_enabled(const std::size_t index) const noexcept {
        return features::extended_inputs && (index < board::digital_inputs) &&
               (input_setup_.digital_mode(index) != DigitalInputMode::off) &&
               !(board::external_module_input(index) && input_setup_.external_module);
    }
    [[nodiscard]] constexpr std::uint8_t digital_pin_mask() const noexcept {
        std::uint8_t result{};
        for (std::size_t i = 0; i < digital_input_count; ++i)
            if (digital_enabled(i))
                result |= static_cast<std::uint8_t>(
                    (input_setup_.digital_mode(i) == DigitalInputMode::three_position ? 3U : 1U)
                    << i);
        return result;
    }
    [[nodiscard]] constexpr bool three_position(const std::size_t index) const noexcept {
        return index < auxiliary_count ? input_setup_.modes[index] == InputMode::three_position
               : index < physical_switch_count
                   ? input_setup_.digital_mode(index - auxiliary_count) ==
                         DigitalInputMode::three_position
                   : false;
    }
    [[nodiscard]] constexpr bool set_input_setup(const InputSetup &setup,
                                                 const bool custom = true) noexcept {
        if ((phase_ != CalibrationPhase::idle) || !valid_input_setup(setup))
            return false;
        input_setup_ = setup;
        custom_inputs_ = custom;
        if (!custom_inputs_ && calibrated_) {
            input_setup_.calibration[4] = setup_.calibration[4];
            input_setup_.calibration[5] = setup_.calibration[5];
        }
        switches_initialized_ = false;
        digital_initialized_ = false;
        detent_side_ = {};
        click_ = false;
        invalidate();
        return true;
    }
    constexpr void observe_digital(const std::uint8_t pressed, const TimestampUs now) noexcept {
        bool changed{};
        for (std::size_t i = 0; i < board::digital_inputs; ++i) {
            const auto previous = raw_.switches[11 + i];
            auto position = SwitchPosition::middle;
            if (digital_enabled(i)) {
                if (input_setup_.digital_mode(i) == DigitalInputMode::three_position) {
                    const auto contacts = (pressed >> i) & 3U;
                    // Contradictory contacts go through the normal control-error/RF inhibit path.
                    position = contacts == 0   ? SwitchPosition::middle
                               : contacts == 1 ? SwitchPosition::up
                               : contacts == 2 ? SwitchPosition::down
                                               : static_cast<SwitchPosition>(8);
                } else
                    position =
                        (pressed & (1U << i)) != 0 ? SwitchPosition::down : SwitchPosition::up;
            }
            debounce(11 + i, position, now, digital_initialized_);
            changed = changed || (previous != raw_.switches[11 + i]);
        }
        digital_initialized_ = true;
        if (changed)
            refresh(true);
    }
    constexpr void observe_trims(const std::uint8_t buttons) noexcept {
        bool changed{};
        for (std::size_t i = 0; i < trim_count; ++i) {
            const auto position = trim_position(buttons, i);
            changed = changed || (raw_.switches[physical_switch_count + i] != position);
            raw_.switches[physical_switch_count + i] = position;
        }
        if (changed)
            refresh(true);
    }
    [[nodiscard]] constexpr bool take_click() noexcept {
        const bool click = click_;
        click_ = false;
        return click;
    }
    [[nodiscard]] constexpr std::uint16_t analog_mask() const noexcept {
        std::uint16_t result{0xF};
        for (std::size_t i = 0; i < auxiliary_count; ++i)
            if (auxiliary_enabled(i) && is_pot(input_setup_.modes[i]))
                result |= static_cast<std::uint16_t>(1U << auxiliary_analog_indices[i]);
        return result;
    }
    [[nodiscard]] constexpr std::uint32_t switch_mask() const noexcept {
        std::uint32_t result{};
        for (std::size_t i = 0; i < auxiliary_count; ++i)
            if (auxiliary_enabled(i) && (input_setup_.modes[i] >= InputMode::two_position))
                result |= static_cast<std::uint16_t>(1U << i);
        for (std::size_t i = 0; i < digital_input_count; ++i)
            if (digital_enabled(i))
                result |= 1U << (auxiliary_count + i);
        if (rear_inputs_)
            result |= 3U << physical_switch_count;
        if constexpr (board::trim_buttons)
            result |= std::uint32_t{model_->trim_switches} << physical_switch_count;
        return result;
    }
    [[nodiscard]] constexpr std::uint16_t six_mask() const noexcept {
        std::uint16_t result{};
        for (std::size_t i = 0; i < auxiliary_count; ++i)
            if (auxiliary_enabled(i) && (input_setup_.modes[i] == InputMode::six_position))
                result |= static_cast<std::uint16_t>(1U << i);
        return result;
    }
    [[nodiscard]] constexpr std::string_view bad_axis_name() const noexcept {
        if (bad_axis_ < 4)
            return control_names[bad_axis_];
        for (std::size_t i = 0; i < auxiliary_count; ++i)
            if (auxiliary_analog_indices[i] == bad_axis_)
                return auxiliary_names[i];
        return "?";
    }

  private:
    constexpr void debounce(const std::size_t index, const SwitchPosition position,
                            const TimestampUs now, const bool initialized) noexcept {
        if (!initialized) {
            candidate_[index] = position;
            raw_.switches[index] = position;
            changed_[index] = now;
        } else {
            if (candidate_[index] != position) {
                candidate_[index] = position;
                changed_[index] = now;
            }
            if ((now - changed_[index]) >= 20'000_us)
                raw_.switches[index] = position;
        }
    }
    constexpr void debounce(const std::size_t index, const SwitchPosition position,
                            const TimestampUs now) noexcept {
        debounce(index, position, now, switches_initialized_);
    }
    constexpr void refresh(const bool commit = false) noexcept {
        output_valid_ = false;
        if (refresh_deferred_) {
            refresh_pending_ = true;
            refresh_commit_pending_ = refresh_commit_pending_ || commit;
            return;
        }
        evaluate_controls(commit);
    }
    constexpr void evaluate_controls(const bool commit) noexcept {
        if (!valid_ || !calibrated_ || (phase_ != CalibrationPhase::idle)) {
            return;
        }
        prepared_ = prepare_controls(setup_, raw_);
        if (!prepared_) {
            return;
        }
        auto &controls = prepared_.controls;
        controls.active_analogs = 0xF;
        controls.active_switches = 0;
        for (std::size_t i = 0; i < auxiliary_count; ++i) {
            const auto index = auxiliary_analog_indices[i];
            controls.analogs[index] = 0;
            if (!auxiliary_enabled(i))
                continue;
            const auto mode = input_setup_.modes[i];
            if (is_pot(mode)) {
                controls.analogs[index] =
                    decode_pot(setup_frozen_ && ((i == 4) || (i == 5)) ? frozen_pots_[i - 4]
                                                                       : auxiliary_samples_[i],
                               input_setup_.calibration[i], mode == InputMode::pot_detent);
                controls.active_analogs |= static_cast<std::uint16_t>(1U << index);
            } else if (mode >= InputMode::two_position)
                controls.active_switches |= static_cast<std::uint16_t>(1U << i);
        }
        for (std::size_t i = 0; i < digital_input_count; ++i)
            if (digital_enabled(i))
                controls.active_switches |= 1U << (auxiliary_count + i);
        if (rear_inputs_)
            controls.active_switches |= 3U << physical_switch_count;
        controls.trims = model_->trims;
        if constexpr (board::trim_buttons) {
            controls.active_switches |= std::uint32_t{model_->trim_switches}
                                        << physical_switch_count;
            for (std::size_t i = 0; i < trim_count; ++i)
                if ((model_->trim_switches & (1U << setup_.mapping[i].physical_index)) != 0)
                    controls.trims[i] = {};
        }
        auto next = logical_state_;
        evaluation_ = evaluate(*model_, prepared_.controls, next);
        output_valid_ = static_cast<bool>(evaluation_);
        if (commit && logical_commit_enabled_ && output_valid_)
            logical_state_ = next;
    }

    constexpr void show_calibration_axis(const std::size_t axis) noexcept {
        std::size_t row = axis;
        if (axis >= 4) {
            row = 4;
            for (std::size_t i = 0; i < auxiliary_count; ++i) {
                if (!auxiliary_enabled(i) || !is_pot(input_setup_.modes[i]))
                    continue;
                if (auxiliary_analog_indices[i] == axis)
                    break;
                ++row;
            }
        }
        calibration_page_ = static_cast<std::uint8_t>(row / calibration_rows);
    }

    ControlSetup setup_{};
    // Only calibration values change while calibrating.
    std::array<AnalogCalibration, calibration_count> draft_{};
    StockSamples samples_{};
    RawControls raw_{};
    PreparedControls prepared_{};
    Evaluation evaluation_{};
    LogicalState logical_state_{};
    bool logical_commit_enabled_{true};
    std::array<SwitchPosition, physical_switch_count> candidate_{};
    std::array<TimestampUs, physical_switch_count> changed_{};
    std::size_t page_{};
    std::size_t bad_axis_{};
    std::uint8_t calibration_page_{};
    CalibrationPhase phase_{};
    CalibrationError calibration_error_{};
    bool calibrated_{};
    bool switches_initialized_{};
    bool valid_{};
    bool output_valid_{};
    InputSetup input_setup_{};
    std::array<AnalogCalibration, auxiliary_count> input_draft_{};
    std::array<std::uint16_t, auxiliary_count> auxiliary_samples_{};
    std::array<std::uint16_t, 2> frozen_pots_{};
    std::array<std::int8_t, auxiliary_count> detent_side_{};
    TimestampUs last_click_{};
    bool custom_inputs_{};
    bool rear_inputs_{}, setup_frozen_{};
    bool digital_initialized_{};
    bool click_{};
    bool refresh_deferred_{};
    bool refresh_pending_{};
    bool refresh_commit_pending_{};
    const Model *model_{};
};

} // namespace coretx::firmware
