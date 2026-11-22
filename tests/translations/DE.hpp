// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace coretx::ui::tr {
inline constexpr auto tx_channels = text<21>("Sende Kanaele");
inline constexpr auto no_data = text<21>("Keine Daten");
inline constexpr auto stale = text<21>("Veraltet");
inline constexpr auto live = text<21>("Aktuell");
inline constexpr auto crsf_baud = text<21>("CRSF / OK: Baud");
inline constexpr auto actual = text<13>("Ist");
inline constexpr auto rx_errors = text<13>("RX Feh/s");
inline constexpr auto telemetry_title = text<21>("Telemetrie");
inline constexpr auto telemetry_full = text<21>("Sensorpool voll");
inline constexpr auto telemetry_local = text<21>("Lokal");
inline constexpr auto telemetry_local_afhds2a = text<21>("Lokal / AFHDS2A");
inline constexpr auto telemetry_local_crsf = text<21>("Lokal / CRSF");
inline constexpr auto telemetry_next = text<21>("OK: weitere Werte");
inline constexpr auto telemetry_tx_battery = text<10>("TX Akku");
inline constexpr auto telemetry_rx_battery = text<10>("RX Akku");
inline constexpr auto telemetry_flight_battery = text<10>("Flugakku");
inline constexpr auto telemetry_current = text<10>("Strom");
inline constexpr auto telemetry_consumption = text<10>("Verbrauch");
inline constexpr auto telemetry_temperature = text<10>("Temp");
inline constexpr auto telemetry_rx_error = text<10>("RX Fehler");
inline constexpr auto radio_internal = text<21>("Intern AFHDS 2A");
inline constexpr auto radio_external = text<21>("Extern CRSF");
inline constexpr auto radio_change = text<21>("OK: Ausgang wechseln");
inline constexpr auto radio_saved = text<21>("HF: gespeichert");
inline constexpr auto radio_empty = text<21>("HF: leer / nur RAM");
inline constexpr auto radio_saving = text<21>("HF: speichert");
inline constexpr auto radio_loading = text<21>("HF: laedt");
inline constexpr auto radio_missing = text<21>("HF: kein EEPROM");
inline constexpr auto radio_multiple = text<21>("HF: mehrere EEPROM");
inline constexpr auto radio_invalid = text<21>("HF: ungueltige Daten");
inline constexpr auto radio_version = text<21>("HF: falsche Version");
inline constexpr auto radio_verify = text<21>("HF: Prueffehler");
inline constexpr auto radio_waiting_rx = text<21>("Warte auf Empfaenger");
inline constexpr auto radio_unbound = text<21>("Nicht gebunden");
inline constexpr auto radio_bind = text<21>("OK: Bind/Abbruch");
inline constexpr auto radio_center_failsafe = text<21>("Failsafe: alle Mitte");
inline constexpr auto radio_format = text<21>("OK: Format wechseln");
inline constexpr auto radio_channels_standard = text<21>("14 RC");
inline constexpr auto radio_channels_extended = text<21>("16 RC + LQI");
inline constexpr auto a7105_start = text<21>("Pruefung...");
inline constexpr auto raw = text<4>("Roh");
inline constexpr auto touch_start = text<21>("Touch Start");
inline constexpr auto touch_ready = text<21>("Touch bereit");
inline constexpr auto touch_contact = text<21>("Kontakt");
inline constexpr auto touch_released = text<21>("Losgelassen");
inline constexpr auto touch_multi = text<21>("Multi: ignoriert");
inline constexpr auto key = text<5>("Taste");
inline constexpr auto key_none = text<9>("Keine");
inline constexpr auto key_up = text<9>("Auf");
inline constexpr auto key_down = text<9>("Ab");
inline constexpr auto key_cancel = text<9>("Zurueck");

} // namespace coretx::ui::tr
