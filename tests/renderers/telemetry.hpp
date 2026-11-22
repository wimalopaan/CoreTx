// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Historical screens remain pure renderer fixtures, never dashboard routes.

#include "coretx/radio/telemetry.hpp"
#include "coretx/ui/ui.hpp"
#include "tests/translation.hpp"

namespace coretx::firmware {

inline constexpr std::size_t telemetry_rows = 4;

[[nodiscard]] constexpr std::size_t telemetry_pages(const telemetry::Pool &pool) noexcept {
    return std::max(std::size_t{1}, (pool.entries().size() + telemetry_rows - 1) / telemetry_rows);
}

[[nodiscard]] constexpr std::string_view
telemetry_label(const telemetry::Quantity quantity) noexcept {
    namespace tr = coretx::ui::tr;
    using enum telemetry::Quantity;
    switch (quantity) {
    case transmitter_voltage:
        return tr::telemetry_tx_battery;
    case receiver_voltage:
        return tr::telemetry_rx_battery;
    case flight_battery_voltage:
        return tr::telemetry_flight_battery;
    case current:
        return tr::telemetry_current;
    case consumption:
        return tr::telemetry_consumption;
    case temperature:
        return tr::telemetry_temperature;
    case rpm:
        return "RPM";
    case rssi:
        return "RSSI";
    case snr:
        return "SNR";
    case link_quality:
        return "LQ";
    case receiver_error:
        return tr::telemetry_rx_error;
    }
    return tr::unknown;
}

struct TelemetryFormat {
    std::string_view unit{};
    std::int32_t divisor{1};
    unsigned decimals{};
};

[[nodiscard]] constexpr TelemetryFormat telemetry_format(const telemetry::Unit unit) noexcept {
    using enum telemetry::Unit;
    switch (unit) {
    case millivolt:
        return {"V", 10, 2};
    case milliamp:
        return {"A", 10, 2};
    case milliamp_hour:
        return {"mAh"};
    case decicelsius:
        return {"C", 1, 1};
    case rpm:
        return {"rpm"};
    case dbm:
        return {"dBm"};
    case db:
        return {"dB"};
    case percent:
        return {"%"};
    case unknown:
        return {"?"};
    }
    return {"?"};
}

constexpr void render_telemetry_value(const telemetry::Entry &entry,
                                      coretx::ui::Framebuffer &display, const int y,
                                      const TimestampUs now) noexcept {
    if (!entry.fresh(now)) {
        const std::string_view stale = coretx::ui::tr::stale;
        display.text(128 - static_cast<int>(stale.size()) * display.character_width(), y, stale);
        return;
    }
    const auto unit = telemetry::unit(entry.key.quantity);
    if (unit == telemetry::Unit::unknown) {
        const std::string_view unknown = coretx::ui::tr::unknown;
        display.text(128 - static_cast<int>(unknown.size()) * display.character_width(), y,
                     unknown);
        return;
    }
    const auto format = telemetry_format(unit);
    const auto value = entry.value / format.divisor;
    auto magnitude =
        value < 0 ? 0U - static_cast<std::uint32_t>(value) : static_cast<std::uint32_t>(value);
    std::array<char, 16> buffer{};
    auto position = buffer.size();
    for (auto i = format.unit.size(); i > 0; --i)
        buffer[--position] = format.unit[i - 1];
    buffer[--position] = ' ';
    unsigned digits{};
    do {
        if ((format.decimals != 0) && (digits == format.decimals))
            buffer[--position] = '.';
        buffer[--position] = static_cast<char>('0' + magnitude % 10);
        magnitude /= 10;
        ++digits;
    } while ((magnitude != 0) || (digits <= format.decimals));
    if (entry.value < 0)
        buffer[--position] = '-';
    const auto length = buffer.size() - position;
    display.text(128 - static_cast<int>(length) * display.character_width(), y,
                 {buffer.data() + position, length});
}

constexpr void render_telemetry(const telemetry::Pool &pool, coretx::ui::Framebuffer &display,
                                const TimestampUs now, const std::size_t group,
                                const telemetry::Source active) noexcept {
    namespace tr = coretx::ui::tr;
    display.clear();
    const auto pages = telemetry_pages(pool);
    const auto selected = group % pages;
    display.text(0, 0, tr::telemetry_title);
    display.unsigned_number(110, 0, static_cast<std::uint32_t>(selected + 1));
    display.text(112, 0, "/");
    display.unsigned_number(127, 0, static_cast<std::uint32_t>(pages));
    display.small_text(0, 10,
                       pool.full()                            ? tr::telemetry_full
                       : active == telemetry::Source::afhds2a ? tr::telemetry_local_afhds2a
                       : active == telemetry::Source::crsf    ? tr::telemetry_local_crsf
                                                              : tr::telemetry_local);
    if (pool.entries().empty())
        display.text(0, 18, tr::no_data);
    const auto entries = pool.entries();
    for (std::size_t row = 0; row < telemetry_rows; ++row) {
        const auto index = selected * telemetry_rows + row;
        if (index >= entries.size())
            break;
        const auto &entry = entries[index];
        const auto y = 18 + static_cast<int>(row) * 10;
        display.text(0, y,
                     entry.key.source == telemetry::Source::local     ? "L"
                     : entry.key.source == telemetry::Source::afhds2a ? "A"
                                                                      : "C");
        const auto label = telemetry_label(entry.key.quantity);
        display.text(10, y, label);
        if (entry.key.instance != 0)
            display.unsigned_number(
                10 + static_cast<int>(label.size() + 1) * display.character_width() - 1, y,
                entry.key.instance);
        render_telemetry_value(entry, display, y, now);
    }
    display.small_text(0, 58, tr::telemetry_next);
}

} // namespace coretx::firmware
