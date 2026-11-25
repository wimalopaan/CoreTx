// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/common/controls.hpp"
#include "boards/common/radio_settings.hpp"
#include "boards/common/switch_names.hpp"
#include "coretx/radio/telemetry.hpp"
#include "coretx/ui/translation.hpp"
#include "coretx/ui/ui.hpp"
#include "coretx/version.hpp"
#include "platform/stm32f072/i2c.hpp"

namespace coretx::firmware {

inline constexpr auto overview_channel_count = Model{}.dashboard_channels.size();

struct OverviewLayout {
    static constexpr int left = 0;
    static constexpr int right = coretx::ui::Framebuffer::width - 1;
    static constexpr int character_width = 4;
    static constexpr int character_height = 6;
    static constexpr int voltage_y = 0;
    static constexpr int rx_label_x = 48;
    static constexpr int tx_voltage_right = rx_label_x - character_width - 1;
    static constexpr int rx_voltage_right = 92;
    static constexpr int unavailable_width = 2 * character_width;
    static constexpr int voltage_fraction_width = 3 * character_width;
    static constexpr int voltage_decimal_offset = voltage_fraction_width + character_width;
    static constexpr int channel_heading_y = voltage_y + character_height + 1;
    static constexpr int model_y = channel_heading_y;
    static constexpr std::size_t model_name_characters = 12;
    static constexpr int signal_label_x = 96;
    static constexpr int percent_x = right - character_width;
    static constexpr int channel_spacing =
        coretx::ui::Framebuffer::width / static_cast<int>(overview_channel_count);
    static constexpr int bar_left = 4;
    static constexpr int bar_top = channel_heading_y + character_height + 3;
    static constexpr int bar_width = 8;
    static constexpr int bar_height = 28;
    static constexpr int border_width = 1;
    static constexpr int bar_right = bar_left + bar_width - border_width;
    static constexpr int bar_bottom = bar_top + bar_height - border_width;
    static constexpr int zero_y = bar_top + bar_height / 2;
    static constexpr int zero_padding = 2;
    static constexpr int zero_left = bar_left - zero_padding;
    static constexpr int zero_width = bar_width + 2 * zero_padding;
    static constexpr int fill_padding = 2;
    static constexpr int fill_left = bar_left + fill_padding;
    static constexpr int fill_width = bar_width - 2 * fill_padding;
    static constexpr int negative_fill_y = zero_y + border_width;
    static constexpr int maximum_fill_height = bar_bottom - negative_fill_y;
    static constexpr int channel_label_y = bar_bottom + 2;
    static constexpr int switches_y = coretx::ui::Framebuffer::height - 2 * character_height;
    static constexpr int switch_spacing =
        coretx::ui::Framebuffer::width / static_cast<int>(physical_switch_count);
    static constexpr int logical_switches_y = switches_y + character_height;
    static constexpr int logical_switch_spacing =
        coretx::ui::Framebuffer::width / static_cast<int>(logical_switch_count);
    static constexpr int message_y = 24;
};

static_assert((OverviewLayout::channel_label_y + OverviewLayout::character_height) <=
              OverviewLayout::switches_y);

struct SystemLayout {
    static constexpr int label_x = 0;
    static constexpr int title_y = 0;
    static constexpr int row_spacing = 8;
    static constexpr int sender_y = title_y + row_spacing;
    static constexpr int storage_y = sender_y + row_spacing;
    static constexpr int value_x = 32;
    static constexpr int address_columns = 4;
    static constexpr int address_spacing = coretx::ui::Framebuffer::width / address_columns;
    static constexpr int address_y = storage_y + row_spacing;
    static constexpr int address_row_spacing = 7;
    static constexpr int board_name_x = coretx::ui::Framebuffer::width - address_spacing;
    static constexpr int calibration_y = 39;
    static constexpr int calibration_value_x = 48;
    static constexpr int input_y = calibration_y + row_spacing;
    static constexpr int watchdog_y = input_y + row_spacing;
    static constexpr int number_right = coretx::ui::Framebuffer::width - 1;
};

struct MessageLayout {
    static constexpr int input_x = 0;
    static constexpr int left = 3;
    static constexpr int title_y = 3;
    static constexpr int detail_y = 20;
};

struct CalibrationLayout {
    static constexpr int left = MessageLayout::left;
    static constexpr int title_y = MessageLayout::title_y;
    static constexpr int instructions_y = 18;
    static constexpr int row_spacing = 10;
    static constexpr int throttle_y = instructions_y + row_spacing;
    static constexpr int capture_y = 43;
    static constexpr int cancel_y = capture_y + row_spacing;
    static constexpr int range_label_x = 0;
    static constexpr int range_axis_x = coretx::ui::Framebuffer::width / 2;
    static constexpr int range_first_y = 14;
    static constexpr int range_row_spacing = 8;
    static constexpr int sample_right = 53;
    static constexpr int minimum_right = 90;
    static constexpr int maximum_right = coretx::ui::Framebuffer::width - 1;
    static constexpr int page_separator_x = maximum_right - 2 * OverviewLayout::character_width;
};
static_assert((CalibrationLayout::range_first_y +
               (Diagnostics::calibration_rows - 1) * CalibrationLayout::range_row_spacing +
               OverviewLayout::character_height) <= coretx::ui::Framebuffer::height);

struct SystemInfo {
    StorageProbeState storage_state{};
    I2cError storage_error{};
    std::uint8_t storage_addresses{};
    const char *calibration_label{};
    std::uint32_t watchdog_resets{};
    const char *input_label{};
    std::string_view board_name{"I6X"};
    std::uint32_t watchdog_fault{};
};

constexpr void render_system(const SystemInfo &system, coretx::ui::Framebuffer &display) noexcept {
    using Layout = SystemLayout;
    display.compact();
    display.text(Layout::label_x, Layout::title_y, coretx::ui::tr::system_info);
    display.text(Layout::board_name_x, Layout::title_y, "FW");
    display.unsigned_number(coretx::ui::Framebuffer::width - 1, Layout::title_y,
                            coretx::firmware_version);
    display.text(Layout::label_x, Layout::sender_y, coretx::ui::tr::sender);
    if (system.watchdog_fault != 0) {
        constexpr std::string_view digits{"0123456789ABCDEF"};
        const auto fault = system.watchdog_fault;
        const std::array<char, 6> code{'F', digits[(fault >> 12) & 15U], digits[(fault >> 8) & 15U],
                                       ':', digits[(fault >> 4) & 15U],  digits[fault & 15U]};
        display.text(Layout::board_name_x, Layout::sender_y, {code.data(), code.size()});
    } else
        display.text(Layout::board_name_x, Layout::sender_y, system.board_name);
    display.text(Layout::label_x, Layout::storage_y, "EEPROM");
    switch (system.storage_state) {
    case StorageProbeState::idle:
        display.text(Layout::value_x, Layout::storage_y, coretx::ui::tr::not_started);
        break;
    case StorageProbeState::scanning:
        display.text(Layout::value_x, Layout::storage_y, coretx::ui::tr::scanning);
        break;
    case StorageProbeState::not_found:
        display.text(Layout::value_x, Layout::storage_y, coretx::ui::tr::not_found);
        break;
    case StorageProbeState::failed:
        display.text(Layout::value_x, Layout::storage_y, coretx::ui::tr::i2c_error);
        display.text(Layout::label_x, Layout::address_y, error_message(system.storage_error));
        break;
    case StorageProbeState::found:
        display.text(Layout::value_x, Layout::storage_y, coretx::ui::tr::eeprom_found);
        for (std::uint8_t i = 0; i < 8; ++i) {
            if ((system.storage_addresses & (1U << i)) != 0) {
                const std::array<char, 4> address{'0', 'x', '5', static_cast<char>('0' + i)};
                display.text(static_cast<int>(i % Layout::address_columns) *
                                 Layout::address_spacing,
                             Layout::address_y + static_cast<int>(i / Layout::address_columns) *
                                                     Layout::address_row_spacing,
                             std::string_view{address.data(), address.size()});
            }
        }
        break;
    }
    display.text(Layout::label_x, Layout::calibration_y, coretx::ui::tr::calibration_status);
    display.text(Layout::calibration_value_x, Layout::calibration_y,
                 system.calibration_label == nullptr ? coretx::ui::tr::need_cal
                                                     : system.calibration_label);
    display.text(Layout::label_x, Layout::input_y, coretx::ui::tr::input_status);
    display.text(Layout::value_x, Layout::input_y,
                 system.input_label == nullptr ? coretx::ui::tr::status_ok : system.input_label);
    display.text(Layout::label_x, Layout::watchdog_y, coretx::ui::tr::watchdog_resets);
    display.unsigned_number(Layout::number_right, Layout::watchdog_y, system.watchdog_resets);
    display.compact(false);
}

constexpr void overview_voltage(const telemetry::Pool &pool, const telemetry::Key key,
                                coretx::ui::Framebuffer &display, const int right,
                                const TimestampUs now) noexcept {
    using Layout = OverviewLayout;
    const auto entry = [&]() constexpr {
        const telemetry::Entry *selected = pool.find(key);
        if ((key.quantity == telemetry::Quantity::receiver_voltage) ||
            (key.quantity == telemetry::Quantity::flight_battery_voltage)) {
            for (const auto &candidate : pool.entries()) {
                if ((candidate.key.source == key.source) &&
                    (candidate.key.quantity == key.quantity) &&
                    ((selected == nullptr) || (candidate.key.instance < selected->key.instance)))
                    selected = &candidate;
            }
        }
        return selected;
    }();
    if ((entry == nullptr) || !entry->fresh(now)) {
        display.text(right - Layout::unavailable_width, Layout::voltage_y, "--");
        return;
    }
    const auto value = std::clamp(entry->value, std::int32_t{0}, std::int32_t{99'999}) / 10;
    display.number(right - Layout::voltage_decimal_offset, Layout::voltage_y, value / 100);
    display.text(right - Layout::voltage_decimal_offset, Layout::voltage_y, ".");
    const std::array<char, 3> tail{static_cast<char>('0' + ((value / 10) % 10)),
                                   static_cast<char>('0' + (value % 10)), 'V'};
    display.text(right - Layout::voltage_fraction_width, Layout::voltage_y,
                 {tail.data(), tail.size()});
}

constexpr void render_physical_switches(coretx::ui::Framebuffer &display,
                                        const std::array<SwitchPosition, switch_count> &positions,
                                        const std::uint32_t active,
                                        const std::uint32_t available) noexcept {
    using Layout = OverviewLayout;
    const auto spacing = std::popcount(active) > static_cast<int>(physical_switch_count)
                             ? coretx::ui::Framebuffer::width / std::popcount(active)
                             : Layout::switch_spacing;
    std::size_t cell{};
    for (std::size_t i = 0; i < switch_count; ++i) {
        if ((active & (1U << i)) == 0)
            continue;
        const auto position = positions[i];
        // Use the mixer switch names, with lowercase letters for potentiometers (VRA -> a).
        const auto full = switch_names[i];
        const auto name = i < 4                    ? static_cast<char>('A' + i)
                          : full.starts_with("VR") ? static_cast<char>(full.back() - 'A' + 'a')
                                                   : full.back();
        const std::array<char, 2> label{
            name, ((available & (1U << i)) == 0)     ? '?'
                  : position == SwitchPosition::up   ? '^'
                  : position == SwitchPosition::down ? 'v'
                  : position == SwitchPosition::middle
                      ? '-'
                      : static_cast<char>('1' + (static_cast<int>(position) - 2))};
        const int x = static_cast<int>(cell) * spacing;
        if (spacing < Layout::switch_spacing) {
            // More than sixteen switches: a single glyph plus a vertical position marker.
            display.text(x, Layout::switches_y, {label.data(), 1});
            const int marker =
                position == SwitchPosition::up       ? 0
                : position == SwitchPosition::middle ? Layout::character_height / 2
                : position == SwitchPosition::down
                    ? Layout::character_height - 1
                    : (static_cast<int>(position) - static_cast<int>(SwitchPosition::pos1)) *
                          (Layout::character_height - 1) /
                          (static_cast<int>(SwitchPosition::pos6) -
                           static_cast<int>(SwitchPosition::pos1));
            if (label[1] == '?')
                display.rectangle(x + spacing - 1, Layout::switches_y, 1, Layout::character_height);
            else
                display.rectangle(x + spacing - 1, Layout::switches_y + marker, 1, 1);
        } else
            display.text(x, Layout::switches_y, {label.data(), label.size()});
        ++cell;
    }
}

enum class OverviewSection : std::uint8_t { all, header, channel, footer };

constexpr void render_overview(const Diagnostics &state, coretx::ui::Framebuffer &display,
                               const telemetry::Pool &pool, const RadioOutput output,
                               const TimestampUs now, const std::string_view model_name,
                               const char *const model_error,
                               const OverviewSection section = OverviewSection::all,
                               const std::size_t selected_slot = 0) noexcept {
    using Layout = OverviewLayout;
    display.compact();
    if ((section == OverviewSection::all) || (section == OverviewSection::header)) {
        display.text(Layout::left, Layout::voltage_y, "TX");
        overview_voltage(pool, telemetry::transmitter_battery, display, Layout::tx_voltage_right,
                         now);
        display.text(Layout::rx_label_x, Layout::voltage_y, "RX");
        const auto source = output == RadioOutput::internal   ? telemetry::Source::afhds2a
                            : output == RadioOutput::external ? telemetry::Source::crsf
                                                              : telemetry::Source::local;
        if (output == RadioOutput::off)
            display.text(Layout::rx_voltage_right - Layout::unavailable_width, Layout::voltage_y,
                         "--");
        else
            overview_voltage(pool,
                             {source,
                              source == telemetry::Source::crsf
                                  ? telemetry::Quantity::flight_battery_voltage
                                  : telemetry::Quantity::receiver_voltage,
                              0},
                             display, Layout::rx_voltage_right, now);
        const bool quality = output == RadioOutput::internal;
        display.text(Layout::signal_label_x, Layout::voltage_y, quality ? "LQ" : "RSSI");
        const auto signal =
            output == RadioOutput::off
                ? nullptr
                : pool.find(
                      {source,
                       quality ? telemetry::Quantity::receiver_error : telemetry::Quantity::rssi,
                       source == telemetry::Source::crsf ? std::uint8_t{1} : std::uint8_t{0}});
        if ((signal == nullptr) || !signal->fresh(now))
            display.text(Layout::right - Layout::unavailable_width, Layout::voltage_y, "--");
        else if (quality) {
            display.number(Layout::percent_x, Layout::voltage_y, 100 - signal->value);
            display.text(Layout::percent_x, Layout::voltage_y, "%");
        } else
            display.number(Layout::right, Layout::voltage_y, signal->value);
        display.text(Layout::left, Layout::channel_heading_y, coretx::ui::tr::output_channels);
        const std::string_view name{model_name.data(),
                                    std::min(model_name.size(), Layout::model_name_characters)};
        display.text(coretx::ui::Framebuffer::width -
                         static_cast<int>(name.size()) * Layout::character_width,
                     Layout::model_y, name);
    }
    if ((section == OverviewSection::all) || (section == OverviewSection::channel)) {
        if (model_error != nullptr)
            display.text(Layout::left, Layout::message_y, model_error);
        else if (!state.calibrated())
            display.text(Layout::left, Layout::message_y, coretx::ui::tr::need_cal);
        else if (!state.output_valid())
            display.text(Layout::left, Layout::message_y,
                         !state.valid()      ? coretx::ui::tr::wait_for_adc
                         : !state.prepared() ? coretx::ui::tr::control_error
                                             : error_message(state.evaluation().status.code));
        else {
            for (std::size_t slot = 0; slot < overview_channel_count; ++slot) {
                if ((section == OverviewSection::channel) && (slot != selected_slot))
                    continue;
                const auto channel = state.model().dashboard_channels[slot];
                if (channel == 0)
                    continue;
                const auto x = static_cast<int>(slot) * Layout::channel_spacing;
                display.rectangle(x + Layout::bar_left, Layout::bar_top, Layout::bar_width,
                                  Layout::bar_height, false);
                display.rectangle(x + Layout::bar_left, Layout::bar_top, Layout::bar_width,
                                  Layout::border_width);
                display.rectangle(x + Layout::bar_left, Layout::bar_bottom, Layout::bar_width,
                                  Layout::border_width);
                display.rectangle(x + Layout::bar_left, Layout::bar_top, Layout::border_width,
                                  Layout::bar_height);
                display.rectangle(x + Layout::bar_right, Layout::bar_top, Layout::border_width,
                                  Layout::bar_height);
                display.rectangle(x + Layout::zero_left, Layout::zero_y, Layout::zero_width,
                                  Layout::border_width);
                const auto value = std::clamp(state.evaluation().frame.channels[channel - 1],
                                              std::int16_t{-resolution}, resolution);
                const int height =
                    ((value < 0 ? -static_cast<int>(value) : value) * Layout::maximum_fill_height) /
                    resolution;
                if (height != 0)
                    display.rectangle(x + Layout::fill_left,
                                      value > 0 ? Layout::zero_y - height : Layout::negative_fill_y,
                                      Layout::fill_width, height);
                display.number(x + Layout::bar_right, Layout::channel_label_y, channel);
            }
        }
    }
    if ((section == OverviewSection::all) || (section == OverviewSection::footer)) {
        const auto active =
            state.output_valid() ? state.prepared().controls.active_switches : state.switch_mask();
        render_physical_switches(display,
                                 state.output_valid() ? state.prepared().controls.switches
                                                      : state.raw().switches,
                                 active, state.valid() ? active : std::uint16_t{0});
        for (std::size_t i = 0; i < logical_switch_count; ++i) {
            const auto bit = 1U << i;
            const auto logical = state.evaluation().frame.logical;
            const bool disabled =
                state.model().logical_switches[i].function == LogicalFunction::disabled;
            const std::array<char, 3> label{
                'L', static_cast<char>('1' + i),
                disabled                                                    ? '-'
                : !state.output_valid() || ((logical.available & bit) == 0) ? '?'
                : ((logical.active & bit) != 0)                             ? '1'
                                                                            : '0'};
            display.text(static_cast<int>(i) * Layout::logical_switch_spacing,
                         Layout::logical_switches_y, {label.data(), label.size()});
        }
    }
    display.compact(false);
}
constexpr void render_dashboard(const Diagnostics &state, const char *const adc_error,
                                coretx::ui::Framebuffer &display, const SystemInfo &system = {},
                                const telemetry::Pool &telemetry = {},
                                const RadioOutput output = RadioOutput::off,
                                const char *const input_error = nullptr, const TimestampUs now = {},
                                const std::string_view model_name = "DIAG",
                                const char *const model_error = nullptr) noexcept {
    using Layout = CalibrationLayout;
    display.clear();
    if ((state.phase() == CalibrationPhase::idle) && (state.page() == Diagnostics::system_page)) {
        render_system(system, display);
        return;
    }
    if (input_error != nullptr) {
        display.text(MessageLayout::input_x, MessageLayout::title_y, coretx::ui::tr::input_error);
        display.text(MessageLayout::input_x, MessageLayout::detail_y, input_error);
        return;
    }
    if (adc_error != nullptr) {
        display.text(MessageLayout::left, MessageLayout::title_y, coretx::ui::tr::adc_error);
        display.text(MessageLayout::left, MessageLayout::detail_y, adc_error);
        return;
    }
    if (state.phase() == CalibrationPhase::center) {
        display.text(Layout::left, Layout::title_y, coretx::ui::tr::cal_center);
        display.text(Layout::left, Layout::instructions_y, coretx::ui::tr::sticks_to_mid);
        display.text(Layout::left, Layout::throttle_y, coretx::ui::tr::gas_also_mid);
        display.text(Layout::left, Layout::capture_y,
                     state.calibration_error() == CalibrationError::no_sample
                         ? coretx::ui::tr::wait_for_adc
                         : coretx::ui::tr::ok_capture);
        display.text(Layout::left, Layout::cancel_y, coretx::ui::tr::cancel_back);
        return;
    }
    if (state.phase() == CalibrationPhase::range) {
        display.compact();
        if (state.calibration_error() == CalibrationError::range) {
            display.text(Layout::range_label_x, Layout::title_y, coretx::ui::tr::bad_range);
            display.text(Layout::range_axis_x, Layout::title_y, state.bad_axis_name());
        } else {
            display.text(Layout::range_label_x, Layout::title_y,
                         state.calibration_error() == CalibrationError::no_sample
                             ? coretx::ui::tr::wait_for_adc
                             : coretx::ui::tr::move_apply);
        }
        if (state.calibration_pages() > 1) {
            display.number(Layout::page_separator_x, Layout::title_y,
                           static_cast<std::int32_t>(
                               state.calibration_first() / Diagnostics::calibration_rows + 1));
            display.text(Layout::page_separator_x, Layout::title_y, "/");
            display.number(Layout::maximum_right, Layout::title_y,
                           static_cast<std::int32_t>(state.calibration_pages()));
        }
        for (std::size_t i = 0; i < Diagnostics::calibration_rows; ++i) {
            const auto row = state.calibration_row(state.calibration_first() + i);
            if (!row)
                break;
            const auto y = Layout::range_first_y + static_cast<int>(i) * Layout::range_row_spacing;
            display.text(Layout::range_label_x, y, row->name);
            display.number(Layout::sample_right, y, row->sample);
            display.number(Layout::minimum_right, y, row->range.minimum);
            display.number(Layout::maximum_right, y, row->range.maximum);
        }
        display.compact(false);
        return;
    }
    render_overview(state, display, telemetry, output, now, model_name, model_error);
}

} // namespace coretx::firmware
