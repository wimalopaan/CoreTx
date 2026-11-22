// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "coretx/core/core.hpp"
#include "coretx/utility/time.hpp"

#include <algorithm>
#include <array>
#include <span>

namespace coretx::afhds2a {

using namespace coretx::literals;
inline constexpr std::size_t tx_size = 38;
inline constexpr std::size_t rx_size = 37;
inline constexpr std::size_t hop_count = 16;
inline constexpr auto frame_period = 3'850_us;
inline constexpr auto input_freshness = 10_ms;
inline constexpr auto bind_timeout = 60'000_ms;
inline constexpr auto link_freshness = 200_ms;
inline constexpr std::uint32_t sync_id = 0x5475C52A;
inline constexpr std::uint32_t unbound_id = 0;
inline constexpr std::uint32_t broadcast_id = 0xFFFFFFFFU;
inline constexpr std::uint8_t sticks_type = 0x58;
inline constexpr std::uint8_t settings_type = 0xAA;
inline constexpr std::uint8_t failsafe_type = 0x56;
inline constexpr std::uint8_t bind_first_type = 0xBB;
inline constexpr std::uint8_t bind_next_type = 0xBC;
inline constexpr std::uint8_t telemetry_type = 0xAC;
inline constexpr std::uint8_t settings_request = 0xFC;
inline constexpr std::uint8_t failsafe_request = 0xFD;
inline constexpr std::uint16_t center_us = 1500;
inline constexpr std::uint16_t minimum_us = 988;
inline constexpr std::uint16_t maximum_us = 2012;
inline constexpr std::size_t base_channels = 14;
inline constexpr std::size_t extended_channels = 16;
inline constexpr std::size_t lqi_channel = extended_channels;
inline constexpr std::uint8_t rx_voltage_sensor = 0x00;
inline constexpr std::uint8_t rx_error_sensor = 0xFE;
inline constexpr std::uint8_t maximum_rx_error_percent = 100;
inline constexpr std::uint16_t lqi_maximum_us = 2000;
inline constexpr std::uint16_t lqi_us_per_percent = 10;
inline constexpr std::size_t telemetry_sensor_bytes = 4;
inline constexpr std::size_t telemetry_value_offset = 2;
inline constexpr std::size_t channels_offset = 9;
inline constexpr std::size_t channel_slot_bytes = sizeof(std::uint16_t);
inline constexpr unsigned channel_extension_bits = 4;
inline constexpr std::uint8_t channel_high_mask = 0x0F;
inline constexpr std::uint8_t extension_nibble_mask = 0xF0;
inline constexpr std::size_t extension_slots = 3;
inline constexpr std::int32_t normalized_units_per_us = 2;
inline constexpr std::uint8_t unused_byte = 0xFF;
inline constexpr std::uint8_t bind_confirmations = 4;
inline constexpr std::uint8_t bind_search_stage = 0;
inline constexpr std::uint8_t bind_offer_stage = 1;
inline constexpr std::uint8_t bind_confirm_stage = 2;
inline constexpr std::uint8_t bind_mode_marker = 0x80;
inline constexpr std::uint8_t bind_reserved_marker = 0x01;
inline constexpr std::array<std::uint8_t, 2> bind_channels{0x0D, 0x8C};
inline constexpr std::uint8_t hop_marker = 0x80;
inline constexpr std::uint8_t hop_wrap_marker = 0x90;
inline constexpr std::uint8_t first_hop_after_bind = 1;
inline constexpr std::uint32_t failsafe_interval_frames = 1569;
inline constexpr std::uint16_t receiver_pwm_rate_hz = 50;
inline constexpr std::uint8_t settings_format = 0xFD;
inline constexpr std::uint8_t settings_output_tag = 0x05;
inline constexpr std::uint8_t settings_ibus_output = 0xDE;

namespace offset {
inline constexpr std::size_t type = 0;
inline constexpr std::size_t tx_id = 1;
inline constexpr std::size_t rx_id = tx_id + sizeof(std::uint32_t);
inline constexpr std::size_t payload = rx_id + sizeof(std::uint32_t);
inline constexpr std::size_t trailer = tx_size - 1;
inline constexpr std::size_t bind_stage = payload;
inline constexpr std::size_t bind_hops = 11;
inline constexpr std::size_t bind_reserved = bind_hops + hop_count;
inline constexpr std::size_t bind_mode = bind_reserved + 1;
inline constexpr std::size_t settings_format = payload;
inline constexpr std::size_t settings_reserved = 10;
inline constexpr std::size_t settings_pwm_rate = 11;
inline constexpr std::size_t settings_unused = 15;
inline constexpr std::size_t settings_center_high = 18;
inline constexpr std::size_t settings_center_low = 19;
inline constexpr std::size_t settings_output_tag = 20;
inline constexpr std::size_t settings_output = 21;
inline constexpr std::size_t next_hop = 34;
inline constexpr std::size_t hop_marker = 36;
inline constexpr std::size_t request = payload;
} // namespace offset

namespace hopping {
inline constexpr std::uint32_t lcg_multiplier = 0x0019660D;
inline constexpr std::uint32_t lcg_increment = 0x3C6EF35F;
inline constexpr unsigned generation_limit = 16'384;
inline constexpr unsigned band_count = 4;
inline constexpr unsigned channels_per_band = 41;
inline constexpr unsigned first_channel = 1;
inline constexpr std::uint8_t minimum_separation = 5;
inline constexpr unsigned tx_id_band_shift = 24;
} // namespace hopping
using Packet = std::array<std::uint8_t, tx_size>;
using Received = std::array<std::uint8_t, rx_size>;
using Hops = std::array<std::uint8_t, hop_count>;
using Channels = std::array<std::int16_t, channel_count>;
static_assert(channel_count <= extended_channels);

enum class ChannelMode : std::uint8_t { standard14, extended16 };
[[nodiscard]] constexpr std::size_t control_channels(const ChannelMode mode) noexcept {
    return mode == ChannelMode::extended16 ? extended_channels : base_channels;
}

struct TxId {
    std::uint32_t value{};
    friend constexpr bool operator==(const TxId &, const TxId &) = default;
};
struct RxId {
    std::uint32_t value{};
    friend constexpr bool operator==(const RxId &, const RxId &) = default;
};
[[nodiscard]] constexpr bool valid_id(const std::uint32_t id) noexcept {
    return (id != unbound_id) && (id != broadcast_id);
}

[[nodiscard]] constexpr bool make_hops(const TxId id, Hops &hops) noexcept {
    std::uint32_t random = id.value;
    std::size_t index{};
    for (unsigned attempt = 0; (index < hops.size()) && (attempt < hopping::generation_limit);
         ++attempt) {
        const auto band =
            (((index << 1) | ((index >> 1) & 1U)) + (id.value >> hopping::tx_id_band_shift)) %
            hopping::band_count;
        random = random * hopping::lcg_multiplier + hopping::lcg_increment;
        const auto candidate =
            static_cast<std::uint8_t>(band * hopping::channels_per_band + hopping::first_channel +
                                      ((random >> index) % hopping::channels_per_band));
        bool separated = true;
        for (std::size_t i = 0; i < index; ++i) {
            const auto distance = candidate > hops[i] ? candidate - hops[i] : hops[i] - candidate;
            if (distance < hopping::minimum_separation)
                separated = false;
        }
        if (separated)
            hops[index++] = candidate;
    }
    return index == hops.size();
}

constexpr void put_id(Packet &packet, const std::size_t offset, const std::uint32_t id) noexcept {
    for (std::size_t i = 0; i < sizeof(id); ++i)
        packet[offset + i] = static_cast<std::uint8_t>(id >> (8 * i));
}
[[nodiscard]] constexpr std::uint32_t read_id(const Received &packet,
                                              const std::size_t offset) noexcept {
    std::uint32_t id{};
    for (std::size_t i = 0; i < sizeof(id); ++i)
        id |= std::uint32_t{packet[offset + i]} << (8 * i);
    return id;
}
[[nodiscard]] constexpr Packet header(const std::uint8_t type, const TxId tx,
                                      const RxId rx) noexcept {
    Packet packet{};
    packet[offset::type] = type;
    put_id(packet, offset::tx_id, tx.value);
    put_id(packet, offset::rx_id, rx.value);
    return packet;
}
constexpr void put_channel(Packet &packet, const std::size_t index,
                           const std::uint16_t value) noexcept {
    if (index < base_channels) {
        packet[channels_offset + index * channel_slot_bytes] = static_cast<std::uint8_t>(value);
        packet[channels_offset + index * channel_slot_bytes + 1] |=
            static_cast<std::uint8_t>((value >> 8) & channel_high_mask);
    } else {
        const auto first =
            channels_offset + 1 + (index - base_channels) * extension_slots * channel_slot_bytes;
        packet[first] |=
            static_cast<std::uint8_t>((value & channel_high_mask) << channel_extension_bits);
        packet[first + channel_slot_bytes] |=
            static_cast<std::uint8_t>(value & extension_nibble_mask);
        packet[first + 2 * channel_slot_bytes] |=
            static_cast<std::uint8_t>((value >> channel_extension_bits) & extension_nibble_mask);
    }
}
[[nodiscard]] constexpr Packet
sticks(const TxId tx, const RxId rx, const Channels &channels, const std::uint8_t next_hop,
       const ChannelMode mode = ChannelMode::standard14,
       const std::uint8_t rx_error_percent = maximum_rx_error_percent) noexcept {
    auto packet = header(sticks_type, tx, rx);
    for (std::size_t i = 0; i < control_channels(mode); ++i) {
        const auto value = i < channels.size()
                               ? std::clamp<std::int32_t>(center_us + std::int32_t{channels[i]} /
                                                                          normalized_units_per_us,
                                                          minimum_us, maximum_us)
                               : center_us;
        put_channel(packet, i, static_cast<std::uint16_t>(value));
    }
    if (mode == ChannelMode::extended16)
        put_channel(packet, lqi_channel,
                    static_cast<std::uint16_t>(
                        lqi_maximum_us -
                        lqi_us_per_percent * std::min(rx_error_percent, maximum_rx_error_percent)));
    packet[offset::next_hop] |= static_cast<std::uint8_t>(next_hop << channel_extension_bits);
    packet[offset::hop_marker] |= next_hop == 0 ? hop_wrap_marker : hop_marker;
    return packet;
}
[[nodiscard]] constexpr Packet failsafe(const TxId tx, const RxId rx,
                                        const ChannelMode mode = ChannelMode::standard14) noexcept {
    auto packet = header(failsafe_type, tx, rx);
    for (std::size_t i = 0; i < control_channels(mode); ++i)
        put_channel(packet, i, center_us);
    return packet;
}
[[nodiscard]] constexpr Packet settings(const TxId tx, const RxId rx) noexcept {
    auto packet = header(settings_type, tx, rx);
    std::fill(packet.begin() + offset::settings_unused, packet.begin() + offset::trailer,
              unused_byte);
    packet[offset::settings_format] = settings_format;
    packet[offset::settings_reserved] = unused_byte;
    packet[offset::settings_pwm_rate] = static_cast<std::uint8_t>(receiver_pwm_rate_hz);
    packet[offset::settings_pwm_rate + 1] = static_cast<std::uint8_t>(receiver_pwm_rate_hz >> 8);
    packet[offset::settings_center_high] = static_cast<std::uint8_t>(center_us >> 8);
    packet[offset::settings_center_low] = static_cast<std::uint8_t>(center_us);
    packet[offset::settings_output_tag] = settings_output_tag;
    packet[offset::settings_output] = settings_ibus_output;
    return packet;
}
enum class BindPhase : std::uint8_t { first, second, third, confirm };
[[nodiscard]] constexpr Packet bind_packet(const TxId tx, const RxId rx, const BindPhase phase,
                                           const Hops &hops) noexcept {
    auto packet = header(phase == BindPhase::first ? bind_first_type : bind_next_type, tx,
                         phase == BindPhase::confirm ? rx : RxId{broadcast_id});
    packet[offset::bind_stage] = (phase == BindPhase::first) || (phase == BindPhase::third)
                                     ? bind_offer_stage
                                 : phase == BindPhase::confirm ? bind_confirm_stage
                                                               : bind_search_stage;
    if (phase == BindPhase::confirm)
        std::fill(packet.begin() + offset::bind_hops, packet.begin() + offset::bind_reserved,
                  unused_byte);
    else
        std::copy(hops.begin(), hops.end(), packet.begin() + offset::bind_hops);
    std::fill(packet.begin() + offset::bind_reserved, packet.begin() + offset::trailer,
              unused_byte);
    if (phase != BindPhase::first) {
        packet[offset::bind_reserved] = bind_reserved_marker;
        packet[offset::bind_mode] = bind_mode_marker;
    }
    return packet;
}

enum class PacketKind : std::uint8_t { channels, settings, failsafe, startup };
struct Plan {
    Packet packet{};
    std::uint8_t channel{};
};
struct ReceiverTelemetry {
    struct Voltage {
        TimestampUs updated_at{};
        std::uint16_t centivolts{};
        std::uint8_t instance{};
        bool received{};
    };
    TimestampUs updated_at{};
    std::uint8_t error_percent{};
    bool received{};
    Voltage voltage{};
};
class Session {
  public:
    [[nodiscard]] constexpr bool initialize(const TxId tx, const RxId rx) noexcept {
        *this = {};
        tx_ = tx;
        receiver_ = rx;
        if (bound())
            next_ = PacketKind::startup;
        return valid_id(tx.value) && ((rx.value == unbound_id) || valid_id(rx.value)) &&
               make_hops(tx, hops_);
    }
    constexpr void set_channel_mode(const ChannelMode mode) noexcept { channel_mode_ = mode; }
    [[nodiscard]] constexpr ChannelMode channel_mode() const noexcept { return channel_mode_; }
    constexpr void start_bind(const TimestampUs now) noexcept {
        previous_receiver_ = receiver_;
        receiver_ = {};
        binding_ = true;
        phase_ = BindPhase::first;
        confirmations_ = 0;
        bind_started_ = now;
        invalidate_telemetry();
        rx_error_percent_ = maximum_rx_error_percent;
    }
    constexpr void cancel_bind() noexcept {
        if (binding_)
            receiver_ = previous_receiver_;
        binding_ = false;
    }
    constexpr void invalidate_telemetry() noexcept {
        received_ = false;
        rx_error_received_ = false;
        rx_voltage_.received = false;
    }
    [[nodiscard]] constexpr bool bind_expired(const TimestampUs now) const noexcept {
        return binding_ && ((now - bind_started_) >= bind_timeout);
    }
    [[nodiscard]] constexpr Plan prepare(const Channels &channels) noexcept {
        ++attempts_;
        if (binding_)
            return {bind_packet(tx_, receiver_, phase_, hops_),
                    bind_channels[(attempts_ - 1U) % bind_channels.size()]};
        const auto channel = hops_[hop_];
        hop_ = static_cast<std::uint8_t>((hop_ + 1) % hops_.size());
        Packet packet{};
        const auto kind = next_;
        next_ = kind == PacketKind::startup ? PacketKind::settings : PacketKind::channels;
        if (kind == PacketKind::settings) {
            packet = settings(tx_, receiver_);
            next_ = PacketKind::failsafe;
        } else if ((kind == PacketKind::failsafe) ||
                   ((kind == PacketKind::channels) &&
                    ((attempts_ % failsafe_interval_frames) == 0)))
            packet = failsafe(tx_, receiver_, channel_mode_);
        else
            packet = sticks(tx_, receiver_, channels, hop_, channel_mode_, rx_error_percent_);
        return {packet, channel};
    }
    constexpr void transmitted() noexcept {
        ++sent_;
        if (!binding_)
            return;
        if (phase_ == BindPhase::confirm) {
            if (++confirmations_ == bind_confirmations) {
                binding_ = false;
                receiver_changed_ = true;
                next_ = PacketKind::startup;
                hop_ = first_hop_after_bind;
            }
        } else {
            phase_ = phase_ == BindPhase::first    ? BindPhase::second
                     : phase_ == BindPhase::second ? BindPhase::third
                                                   : BindPhase::first;
        }
    }
    [[nodiscard]] constexpr bool receive(const Received &packet, const TimestampUs now) noexcept {
        if (read_id(packet, offset::tx_id) != tx_.value)
            return false;
        const RxId rx{read_id(packet, offset::rx_id)};
        if (!valid_id(rx.value))
            return false;
        if (binding_) {
            if ((packet[offset::type] != bind_next_type) ||
                (packet[offset::bind_stage] != bind_offer_stage) || (phase_ == BindPhase::confirm))
                return false;
            receiver_ = rx;
            phase_ = BindPhase::confirm;
            confirmations_ = 0;
            return true;
        }
        if ((rx != receiver_) ||
            ((packet[offset::type] != settings_type) && (packet[offset::type] != telemetry_type)))
            return false;
        received_at_ = now;
        received_ = true;
        if ((packet[offset::type] == settings_type) &&
            (packet[offset::request] == settings_request))
            next_ = PacketKind::settings;
        if ((packet[offset::type] == settings_type) &&
            (packet[offset::request] == failsafe_request))
            next_ = PacketKind::failsafe;
        if ((packet[offset::type] == settings_type) &&
            (packet[offset::request] != settings_request) &&
            (packet[offset::request] != failsafe_request)) {
            for (std::size_t i = offset::payload; (i + telemetry_sensor_bytes) <= packet.size();
                 i += telemetry_sensor_bytes) {
                if (packet[i] == unused_byte)
                    break;
                if (packet[i] == rx_voltage_sensor) {
                    const auto centivolts = static_cast<std::uint16_t>(
                        std::uint32_t{packet[i + telemetry_value_offset]} |
                        (std::uint32_t{packet[i + telemetry_value_offset + 1]} << 8));
                    rx_voltage_ = {now, centivolts, packet[i + 1], true};
                } else if ((packet[i] == rx_error_sensor) &&
                           (packet[i + telemetry_value_offset] <= maximum_rx_error_percent)) {
                    rx_error_percent_ = packet[i + telemetry_value_offset];
                    rx_error_updated_at_ = now;
                    rx_error_received_ = true;
                }
            }
        }
        return true;
    }
    [[nodiscard]] constexpr bool binding() const noexcept { return binding_; }
    [[nodiscard]] constexpr RxId receiver() const noexcept { return receiver_; }
    [[nodiscard]] constexpr bool bound() const noexcept { return valid_id(receiver_.value); }
    [[nodiscard]] constexpr std::uint32_t frames() const noexcept { return sent_; }
    [[nodiscard]] constexpr bool linked(const TimestampUs now) const noexcept {
        return received_ && ((now - received_at_) < link_freshness);
    }
    [[nodiscard]] constexpr ReceiverTelemetry telemetry() const noexcept {
        return {rx_error_updated_at_, rx_error_percent_, rx_error_received_, rx_voltage_};
    }
    [[nodiscard]] constexpr bool take_receiver_changed() noexcept {
        const auto changed = receiver_changed_;
        receiver_changed_ = false;
        return changed;
    }

  private:
    TxId tx_{};
    RxId receiver_{};
    RxId previous_receiver_{};
    Hops hops_{};
    TimestampUs bind_started_{};
    TimestampUs received_at_{};
    TimestampUs rx_error_updated_at_{};
    ReceiverTelemetry::Voltage rx_voltage_{};
    std::uint32_t attempts_{};
    std::uint32_t sent_{};
    std::uint8_t hop_{};
    std::uint8_t confirmations_{};
    BindPhase phase_{};
    PacketKind next_{};
    ChannelMode channel_mode_{};
    std::uint8_t rx_error_percent_{maximum_rx_error_percent};
    bool binding_{};
    bool receiver_changed_{};
    bool received_{};
    bool rx_error_received_{};
};

} // namespace coretx::afhds2a
