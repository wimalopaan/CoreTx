// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "coretx/utility/time.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace coretx::telemetry {

enum class Source : std::uint8_t { local, afhds2a, crsf };
enum class Quantity : std::uint8_t {
    transmitter_voltage,
    receiver_voltage,
    flight_battery_voltage,
    current,
    consumption,
    temperature,
    rpm,
    rssi,
    snr,
    link_quality,
    receiver_error
};
enum class Unit : std::uint8_t {
    millivolt,
    milliamp,
    milliamp_hour,
    decicelsius,
    rpm,
    dbm,
    db,
    percent,
    unknown
};

struct Key {
    Source source{};
    Quantity quantity{};
    std::uint8_t instance{};
    constexpr bool operator==(const Key &) const noexcept = default;
};

[[nodiscard]] constexpr Unit unit(const Quantity quantity) noexcept {
    switch (quantity) {
    case Quantity::transmitter_voltage:
    case Quantity::receiver_voltage:
    case Quantity::flight_battery_voltage:
        return Unit::millivolt;
    case Quantity::current:
        return Unit::milliamp;
    case Quantity::consumption:
        return Unit::milliamp_hour;
    case Quantity::temperature:
        return Unit::decicelsius;
    case Quantity::rpm:
        return Unit::rpm;
    case Quantity::rssi:
        return Unit::dbm;
    case Quantity::snr:
        return Unit::db;
    case Quantity::link_quality:
    case Quantity::receiver_error:
        return Unit::percent;
    }
    return Unit::unknown;
}

[[nodiscard]] constexpr DurationUs freshness(const Source source) noexcept {
    return DurationUs{source == Source::local ? 500'000U : 2'000'000U};
}

struct Entry {
    std::int32_t value{};
    TimestampUs updated_at{};
    Key key{};
    bool valid{};

    [[nodiscard]] constexpr bool fresh(const TimestampUs now) const noexcept {
        return valid && ((now - updated_at) < freshness(key.source));
    }
};

class Pool {
  public:
    inline static constexpr std::size_t capacity = 16;

    constexpr void publish(const Key key, const std::int32_t value,
                           const TimestampUs sampled_at) noexcept {
        for (auto &entry : mutable_entries()) {
            if (entry.key == key) {
                // Re-reading a decoder snapshot must not rejuvenate an expired sample.
                if (entry.updated_at != sampled_at)
                    entry.valid = true;
                entry.value = value;
                entry.updated_at = sampled_at;
                return;
            }
        }
        if (size_ == capacity) {
            full_ = true;
            return;
        }
        values_[size_++] = {value, sampled_at, key, true};
    }

    constexpr void service(const TimestampUs now) noexcept {
        for (auto &entry : mutable_entries())
            if (!entry.fresh(now))
                entry.valid = false;
    }

    constexpr void invalidate(const Key key) noexcept {
        for (auto &entry : mutable_entries())
            if (entry.key == key)
                entry.valid = false;
    }

    constexpr void clear(const Source source) noexcept {
        std::uint8_t remaining{};
        for (const auto &entry : entries())
            if (entry.key.source != source)
                values_[remaining++] = entry;
        if (remaining < size_)
            full_ = false;
        size_ = remaining;
    }

    [[nodiscard]] constexpr std::span<const Entry> entries() const noexcept {
        return {values_.data(), size_};
    }
    [[nodiscard]] constexpr const Entry *find(const Key key) const noexcept {
        for (const auto &entry : entries())
            if (entry.key == key)
                return &entry;
        return nullptr;
    }
    [[nodiscard]] constexpr bool full() const noexcept { return full_; }

  private:
    [[nodiscard]] constexpr std::span<Entry> mutable_entries() noexcept {
        return {values_.data(), size_};
    }

    std::array<Entry, capacity> values_{};
    std::uint8_t size_{};
    bool full_{};
};

inline constexpr Key transmitter_battery{Source::local, Quantity::transmitter_voltage};

} // namespace coretx::telemetry
