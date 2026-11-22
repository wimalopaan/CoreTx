// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "coretx/radio/crsf_protocol.hpp"
#include "coretx/utility/time.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>

namespace coretx::crsf {

using literals::operator""_us;

inline constexpr unsigned byte_bits = std::numeric_limits<std::uint8_t>::digits;
inline constexpr std::uint32_t byte_mask = std::numeric_limits<std::uint8_t>::max();
inline constexpr std::uint32_t crc_top_bit = 1U << (byte_bits - 1);
inline constexpr std::int32_t channel_scale_numerator = 4;
inline constexpr std::int32_t channel_scale_denominator = 5;
inline constexpr std::uint8_t maximum_link_quality = 100;
namespace link_statistics {
inline constexpr std::size_t uplink_rssi_1 = CRSF_PAYLOAD_OFFSET;
inline constexpr std::size_t uplink_rssi_2 = uplink_rssi_1 + 1;
inline constexpr std::size_t uplink_link_quality = uplink_rssi_2 + 1;
inline constexpr std::size_t uplink_snr = uplink_link_quality + 1;
} // namespace link_statistics

using Channels = std::array<std::int16_t, CRSF_NUM_CHANNELS>;
using ChannelsFrame = std::array<std::uint8_t, CRSF_RC_CHANNELS_PAYLOAD_SIZE + CRSF_MIN_PACKET_LEN>;

[[nodiscard]] constexpr std::uint8_t crc8_byte(const std::uint8_t previous,
                                               const std::uint8_t byte) noexcept {
    std::uint32_t crc = previous ^ byte;
    for (unsigned bit = 0; bit < byte_bits; ++bit) {
        crc = ((crc << 1) ^ ((crc & crc_top_bit) != 0 ? CRSF_CRC_POLY : 0U)) & byte_mask;
    }
    return static_cast<std::uint8_t>(crc);
}

[[nodiscard]] constexpr std::uint8_t crc8(const std::span<const std::uint8_t> bytes) noexcept {
    std::uint8_t crc{};
    for (const auto byte : bytes) {
        crc = crc8_byte(crc, byte);
    }
    return crc;
}

[[nodiscard]] constexpr ChannelsFrame encode(const Channels &channels) noexcept {
    ChannelsFrame frame{CRSF_ADDRESS_CRSF_TRANSMITTER,
                        CRSF_RC_CHANNELS_PAYLOAD_SIZE + CRSF_FRAME_TYPE_CRC_SIZE,
                        CRSF_FRAMETYPE_RC_CHANNELS_PACKED};
    std::uint32_t packed{};
    unsigned bits{};
    std::size_t position = CRSF_PAYLOAD_OFFSET;
    for (const auto channel : channels) {
        // Match EdgeTX's signed integer scaling, including extended travel.
        const auto value = std::clamp<std::int32_t>(
            CRSF_CHANNEL_VALUE_MID +
                (std::int32_t{channel} * channel_scale_numerator) / channel_scale_denominator,
            CRSF_CHANNEL_VALUE_EXT_MIN, CRSF_CHANNEL_VALUE_EXT_MAX);
        packed |= static_cast<std::uint32_t>(value) << bits;
        bits += CRSF_CHANNEL_BITS;
        while (bits >= byte_bits) {
            frame[position++] = static_cast<std::uint8_t>(packed);
            packed >>= byte_bits;
            bits -= byte_bits;
        }
    }
    frame.back() = crc8(std::span{frame}.subspan(
        CRSF_TELEMETRY_TYPE_INDEX, frame.size() - CRSF_TELEMETRY_TYPE_INDEX - CRSF_FRAME_CRC_SIZE));
    return frame;
}

struct LinkStatistics {
    std::int16_t rssi1_dbm{};
    std::int16_t rssi2_dbm{};
    std::int8_t snr_db{};
    std::uint8_t quality{};
};

struct BatteryVoltage {
    TimestampUs updated_at{};
    std::uint16_t decivolts{};
    std::uint8_t instance{};
    bool valid{};
};

struct NoFrameReceiver {
    constexpr void on_frame(std::span<const std::uint8_t>, TimestampUs) noexcept {}
};

class Telemetry {
  public:
    inline static constexpr DurationUs stale_us = 2'000'000_us;
    inline static constexpr DurationUs partial_timeout_us = 10'000_us;

    constexpr void push(std::uint8_t byte, TimestampUs now) noexcept;

    template <typename Receiver>
    constexpr void push(const std::uint8_t byte, const TimestampUs now,
                        Receiver &receiver) noexcept {
        service(now, receiver);
        bytes_[(head_ + size_) % bytes_.size()] = byte;
        ++size_;
        last_byte_ = now;
        parse(now, false, receiver);
    }

    constexpr void service(TimestampUs now) noexcept;

    template <typename Receiver>
    constexpr void service(const TimestampUs now, Receiver &receiver) noexcept {
        if ((size_ != 0) && ((now - last_byte_) >= partial_timeout_us)) {
            // Buffered suffixes may contain a good frame behind a truncated prefix.
            parse(last_byte_, true, receiver);
        }
        if (valid_ && ((now - updated_) >= stale_us)) {
            valid_ = false;
        }
        if (battery_.valid && ((now - battery_.updated_at) >= stale_us))
            battery_.valid = false;
    }

    constexpr void reset_stream() noexcept {
        head_ = 0;
        size_ = 0;
    }

    constexpr void invalidate() noexcept {
        reset_stream();
        valid_ = false;
        battery_.valid = false;
    }

    [[nodiscard]] constexpr const LinkStatistics &link() const noexcept { return link_; }
    [[nodiscard]] constexpr const BatteryVoltage &battery() const noexcept { return battery_; }
    [[nodiscard]] constexpr bool battery_fresh(const TimestampUs now) const noexcept {
        return battery_.valid && ((now - battery_.updated_at) < stale_us);
    }
    [[nodiscard]] constexpr bool received() const noexcept { return received_; }
    [[nodiscard]] constexpr TimestampUs updated_at() const noexcept { return updated_; }
    [[nodiscard]] constexpr bool fresh(const TimestampUs now) const noexcept {
        return valid_ && ((now - updated_) < stale_us);
    }
    [[nodiscard]] constexpr std::uint32_t frames() const noexcept { return frames_; }
    [[nodiscard]] constexpr std::uint32_t crc_errors() const noexcept { return crc_errors_; }
    [[nodiscard]] constexpr std::uint32_t framing_errors() const noexcept {
        return framing_errors_;
    }

  private:
    [[nodiscard]] constexpr std::uint8_t at(const std::size_t offset) const noexcept {
        return bytes_[(head_ + offset) % bytes_.size()];
    }

    constexpr void discard(const std::size_t count) noexcept {
        head_ = (head_ + count) % bytes_.size();
        size_ -= count;
    }

    template <typename Receiver>
    constexpr void parse(const TimestampUs now, const bool expired, Receiver &receiver) noexcept {
        while (size_ != 0) {
            const auto address = at(0);
            if (((address != CRSF_ADDRESS_BROADCAST) ||
                 std::is_same_v<Receiver, NoFrameReceiver>) &&
                (address != CRSF_ADDRESS_FLIGHT_CONTROLLER) &&
                (address != CRSF_ADDRESS_RADIO_TRANSMITTER) &&
                (address != CRSF_ADDRESS_CRSF_RECEIVER) &&
                (address != CRSF_ADDRESS_CRSF_TRANSMITTER)) {
                discard(1);
                continue;
            }
            if (size_ < CRSF_FRAME_NOT_COUNTED_BYTES) {
                if (expired) {
                    discard(1);
                }
                return;
            }
            const auto length = at(CRSF_TELEMETRY_LENGTH_INDEX);
            if ((length < CRSF_FRAME_TYPE_CRC_SIZE) ||
                (length > (bytes_.size() - CRSF_FRAME_NOT_COUNTED_BYTES))) {
                ++framing_errors_;
                discard(1);
                continue;
            }
            const auto total = std::size_t{length} + CRSF_FRAME_NOT_COUNTED_BYTES;
            if (size_ < total) {
                if (expired) {
                    ++framing_errors_;
                    discard(1);
                    continue;
                }
                return;
            }
            std::uint8_t crc{};
            for (std::size_t i = CRSF_TELEMETRY_TYPE_INDEX; i < (total - CRSF_FRAME_CRC_SIZE);
                 ++i) {
                crc = crc8_byte(crc, at(i));
            }
            if (crc != at(total - CRSF_FRAME_CRC_SIZE)) {
                ++crc_errors_;
                discard(1);
                continue;
            }
            ++frames_;
            if (at(CRSF_TELEMETRY_TYPE_INDEX) == CRSF_FRAMETYPE_LINK_STATISTICS) {
                if ((length != (CRSF_LINK_STATISTICS_PAYLOAD_SIZE + CRSF_FRAME_TYPE_CRC_SIZE)) ||
                    (at(link_statistics::uplink_link_quality) > maximum_link_quality)) {
                    ++framing_errors_;
                } else {
                    link_ = {std::bit_cast<std::int8_t>(at(link_statistics::uplink_rssi_1)),
                             std::bit_cast<std::int8_t>(at(link_statistics::uplink_rssi_2)),
                             std::bit_cast<std::int8_t>(at(link_statistics::uplink_snr)),
                             at(link_statistics::uplink_link_quality)};
                    updated_ = now;
                    received_ = true;
                    valid_ = true;
                }
            }
            if (at(CRSF_TELEMETRY_TYPE_INDEX) == CRSF_FRAMETYPE_BATTERY_SENSOR) {
                constexpr auto minimum_length =
                    CRSF_BATTERY_SENSOR_PAYLOAD_SIZE + CRSF_FRAME_TYPE_CRC_SIZE;
                if ((length != minimum_length) && (length != (minimum_length + 1))) {
                    ++framing_errors_;
                } else {
                    const auto voltage = static_cast<std::uint16_t>(
                        (std::uint16_t{at(CRSF_PAYLOAD_OFFSET)} << byte_bits) |
                        at(CRSF_PAYLOAD_OFFSET + 1));
                    const auto instance =
                        length == minimum_length
                            ? std::uint8_t{0}
                            : at(CRSF_PAYLOAD_OFFSET + CRSF_BATTERY_SENSOR_PAYLOAD_SIZE);
                    battery_ = {now, voltage, instance, voltage != 0xFFFF};
                }
            }
            if constexpr (!std::is_same_v<Receiver, NoFrameReceiver>) {
                if ((at(2) == CRSF_FRAMETYPE_DEVICE_INFO) ||
                    (at(2) == CRSF_FRAMETYPE_PARAMETER_SETTINGS_ENTRY) ||
                    (at(2) == CRSF_FRAMETYPE_PARAMETER_WRITE)) {
                    std::array<std::uint8_t, CRSF_MAX_PACKET_LEN> frame{};
                    for (std::size_t i = 0; i < total; ++i)
                        frame[i] = at(i);
                    receiver.on_frame(std::span{frame}.first(total), now);
                }
            }
            discard(total);
        }
    }

    std::array<std::uint8_t, CRSF_MAX_PACKET_LEN> bytes_{};
    std::size_t head_{};
    std::size_t size_{};
    LinkStatistics link_{};
    BatteryVoltage battery_{};
    TimestampUs last_byte_{};
    TimestampUs updated_{};
    std::uint32_t frames_{};
    std::uint32_t crc_errors_{};
    std::uint32_t framing_errors_{};
    bool received_{};
    bool valid_{};
};

constexpr void Telemetry::push(const std::uint8_t byte, const TimestampUs now) noexcept {
    NoFrameReceiver receiver{};
    push(byte, now, receiver);
}

constexpr void Telemetry::service(const TimestampUs now) noexcept {
    NoFrameReceiver receiver{};
    service(now, receiver);
}

} // namespace coretx::crsf
