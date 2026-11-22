// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace coretx::ui::tr {
inline constexpr auto tx_channels = text<21>("TX channels");
inline constexpr auto no_data = text<21>("No data");
inline constexpr auto stale = text<21>("Stale");
inline constexpr auto live = text<21>("Live");
inline constexpr auto crsf_baud = text<21>("CRSF / OK: baud");
inline constexpr auto actual = text<13>("Actual");
inline constexpr auto rx_errors = text<13>("RX err/s");
inline constexpr auto telemetry_title = text<21>("Telemetry");
inline constexpr auto telemetry_full = text<21>("Sensor pool full");
inline constexpr auto telemetry_local = text<21>("Local");
inline constexpr auto telemetry_local_afhds2a = text<21>("Local / AFHDS2A");
inline constexpr auto telemetry_local_crsf = text<21>("Local / CRSF");
inline constexpr auto telemetry_next = text<21>("OK: next values");
inline constexpr auto telemetry_tx_battery = text<10>("TX bat");
inline constexpr auto telemetry_rx_battery = text<10>("RX bat");
inline constexpr auto telemetry_flight_battery = text<10>("Flight bat");
inline constexpr auto telemetry_current = text<10>("Current");
inline constexpr auto telemetry_consumption = text<10>("Used");
inline constexpr auto telemetry_temperature = text<10>("Temp");
inline constexpr auto telemetry_rx_error = text<10>("RX error");
inline constexpr auto radio_internal = text<21>("Internal AFHDS 2A");
inline constexpr auto radio_external = text<21>("External CRSF");
inline constexpr auto radio_change = text<21>("OK: change output");
inline constexpr auto radio_saved = text<21>("RF: saved");
inline constexpr auto radio_empty = text<21>("RF: empty / RAM only");
inline constexpr auto radio_saving = text<21>("RF: saving");
inline constexpr auto radio_loading = text<21>("RF: loading");
inline constexpr auto radio_missing = text<21>("RF: no EEPROM");
inline constexpr auto radio_multiple = text<21>("RF: multiple devices");
inline constexpr auto radio_invalid = text<21>("RF: invalid data");
inline constexpr auto radio_version = text<21>("RF: bad version");
inline constexpr auto radio_verify = text<21>("RF: verify error");
inline constexpr auto radio_waiting_rx = text<21>("Waiting for RX");
inline constexpr auto radio_unbound = text<21>("Not bound");
inline constexpr auto radio_bind = text<21>("OK: bind/cancel");
inline constexpr auto radio_center_failsafe = text<21>("Failsafe: all center");
inline constexpr auto radio_format = text<21>("OK: change format");
inline constexpr auto radio_channels_standard = text<21>("14 RC");
inline constexpr auto radio_channels_extended = text<21>("16 RC + LQI");
inline constexpr auto a7105_start = text<21>("Checking...");
inline constexpr auto raw = text<4>("Raw");
inline constexpr auto touch_start = text<21>("Touch start");
inline constexpr auto touch_ready = text<21>("Touch ready");
inline constexpr auto touch_contact = text<21>("Contact");
inline constexpr auto touch_released = text<21>("Released");
inline constexpr auto touch_multi = text<21>("Multi: ignored");
inline constexpr auto key = text<5>("Key");
inline constexpr auto key_none = text<9>("None");
inline constexpr auto key_up = text<9>("Up");
inline constexpr auto key_down = text<9>("Down");
inline constexpr auto key_cancel = text<9>("Back");

} // namespace coretx::ui::tr
