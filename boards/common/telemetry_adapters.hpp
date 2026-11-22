// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "coretx/radio/afhds2a.hpp"
#include "coretx/radio/crsf.hpp"
#include "coretx/radio/telemetry.hpp"

namespace coretx::firmware {

constexpr void publish_telemetry(const crsf::Telemetry &decoder, telemetry::Pool &pool,
                                 const TimestampUs now) noexcept {
    using enum telemetry::Quantity;
    using telemetry::Source;
    const auto &battery = decoder.battery();
    const telemetry::Key voltage{Source::crsf, flight_battery_voltage, battery.instance};
    if (decoder.battery_fresh(now))
        pool.publish(voltage, std::int32_t{battery.decivolts} * 100, battery.updated_at);
    else
        pool.invalidate(voltage);
    if (!decoder.fresh(now)) {
        pool.invalidate({Source::crsf, rssi, 1});
        pool.invalidate({Source::crsf, rssi, 2});
        pool.invalidate({Source::crsf, snr});
        pool.invalidate({Source::crsf, link_quality});
        return;
    }
    const auto &link = decoder.link();
    const auto sampled_at = decoder.updated_at();
    pool.publish({Source::crsf, rssi, 1}, link.rssi1_dbm, sampled_at);
    pool.publish({Source::crsf, rssi, 2}, link.rssi2_dbm, sampled_at);
    pool.publish({Source::crsf, snr}, link.snr_db, sampled_at);
    pool.publish({Source::crsf, link_quality}, link.quality, sampled_at);
}

constexpr void publish_telemetry(const afhds2a::ReceiverTelemetry &reading,
                                 telemetry::Pool &pool) noexcept {
    if (reading.received)
        pool.publish({telemetry::Source::afhds2a, telemetry::Quantity::receiver_error},
                     reading.error_percent, reading.updated_at);
    else
        pool.invalidate({telemetry::Source::afhds2a, telemetry::Quantity::receiver_error});
    const auto &voltage = reading.voltage;
    const telemetry::Key key{telemetry::Source::afhds2a, telemetry::Quantity::receiver_voltage,
                             voltage.instance};
    if (voltage.received)
        pool.publish(key, std::int32_t{voltage.centivolts} * 10, voltage.updated_at);
    else
        pool.invalidate(key);
}

} // namespace coretx::firmware
