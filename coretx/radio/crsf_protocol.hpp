// SPDX-License-Identifier: GPL-3.0-only
// Constants adapted from ExpressLRS src/include/crsf_protocol.h:
// https://github.com/ExpressLRS/ExpressLRS/blob/f8700cab6f7bfd50fe076194abdcf0e26d787e82/src/include/crsf_protocol.h
// Copyright ExpressLRS contributors. See COPYING.GPL-3.0.
// Adapted 2026-11-20: typed constexpr values/functions, namespace, no target dependencies.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace coretx::crsf {

inline constexpr std::uint8_t CRSF_CRC_POLY = 0xD5;

inline constexpr std::uint16_t US_CHANNEL_VALUE_MIN = 476;
inline constexpr std::uint16_t US_CHANNEL_VALUE_EXT_MIN = 880;
inline constexpr std::uint16_t US_CHANNEL_VALUE_STD_MIN = 988;
inline constexpr std::uint16_t US_CHANNEL_VALUE_CENTER = 1500;
inline constexpr std::uint16_t US_CHANNEL_VALUE_STD_MAX = 2012;
inline constexpr std::uint16_t US_CHANNEL_VALUE_EXT_MAX = 2120;
inline constexpr std::uint16_t US_CHANNEL_VALUE_MAX = 2523;

inline constexpr std::uint16_t CRSF_CHANNEL_VALUE_EXT_MIN = 0;
inline constexpr std::uint16_t CRSF_CHANNEL_VALUE_STD_MIN = 172;
inline constexpr std::uint16_t CRSF_CHANNEL_VALUE_1000 = 191;
inline constexpr std::uint16_t CRSF_CHANNEL_VALUE_MID = 992;
inline constexpr std::uint16_t CRSF_CHANNEL_VALUE_2000 = 1792;
inline constexpr std::uint16_t CRSF_CHANNEL_VALUE_STD_MAX = 1811;
inline constexpr std::uint16_t CRSF_CHANNEL_VALUE_EXT_MAX = 1984;
inline constexpr std::uint16_t CRSF_CHANNEL_VALUE_UNSET = 0xFFFF;

inline constexpr std::size_t CRSF_MIN_PACKET_LEN = 4;
inline constexpr std::size_t CRSF_MAX_PACKET_LEN = 64;
inline constexpr std::uint8_t CRSF_SYNC_BYTE = 0xC8;
inline constexpr std::size_t CRSF_FRAME_NOT_COUNTED_BYTES = 2;
inline constexpr std::size_t CRSF_PAYLOAD_SIZE_MAX =
    CRSF_MAX_PACKET_LEN - CRSF_FRAME_NOT_COUNTED_BYTES;
[[nodiscard]] constexpr std::size_t CRSF_FRAME_SIZE(const std::size_t payload_size) noexcept {
    return payload_size + 2;
}
[[nodiscard]] constexpr std::size_t CRSF_EXT_FRAME_SIZE(const std::size_t payload_size) noexcept {
    return CRSF_FRAME_SIZE(payload_size) + 2;
}
inline constexpr std::size_t CRSF_FRAME_SIZE_MAX =
    CRSF_PAYLOAD_SIZE_MAX + CRSF_FRAME_NOT_COUNTED_BYTES;
inline constexpr std::size_t CRSF_FRAME_CRC_SIZE = 1;
inline constexpr std::size_t CRSF_FRAME_LENGTH_EXT_TYPE_CRC = 4;
inline constexpr std::size_t CRSF_TELEMETRY_LENGTH_INDEX = 1;
inline constexpr std::size_t CRSF_TELEMETRY_TYPE_INDEX = 2;
inline constexpr std::size_t CRSF_TELEMETRY_FIELD_ID_INDEX = 5;
inline constexpr std::size_t CRSF_TELEMETRY_FIELD_CHUNK_INDEX = 6;
inline constexpr std::size_t CRSF_TELEMETRY_CRC_LENGTH = 1;
[[nodiscard]] constexpr std::size_t
CRSF_TELEMETRY_TOTAL_SIZE(const std::size_t payload_size) noexcept {
    return payload_size + CRSF_FRAME_LENGTH_EXT_TYPE_CRC;
}

inline constexpr std::size_t CRSF_MSP_REQ_PAYLOAD_SIZE = 8;
inline constexpr std::size_t CRSF_MSP_RESP_PAYLOAD_SIZE = 58;
inline constexpr std::size_t CRSF_MSP_MAX_PAYLOAD_SIZE =
    CRSF_MSP_REQ_PAYLOAD_SIZE > CRSF_MSP_RESP_PAYLOAD_SIZE ? CRSF_MSP_REQ_PAYLOAD_SIZE
                                                           : CRSF_MSP_RESP_PAYLOAD_SIZE;

inline constexpr std::uint8_t CRSF_FRAMETYPE_GPS = 0x02;
inline constexpr std::uint8_t CRSF_FRAMETYPE_GPS_TIME = 0x03;
inline constexpr std::uint8_t CRSF_FRAMETYPE_VARIO = 0x07;
inline constexpr std::uint8_t CRSF_FRAMETYPE_BATTERY_SENSOR = 0x08;
inline constexpr std::uint8_t CRSF_FRAMETYPE_BARO_ALTITUDE = 0x09;
inline constexpr std::uint8_t CRSF_FRAMETYPE_AIRSPEED = 0x0A;
inline constexpr std::uint8_t CRSF_FRAMETYPE_HEARTBEAT = 0x0B;
inline constexpr std::uint8_t CRSF_FRAMETYPE_RPM = 0x0C;
inline constexpr std::uint8_t CRSF_FRAMETYPE_TEMP = 0x0D;
inline constexpr std::uint8_t CRSF_FRAMETYPE_CELLS = 0x0E;
inline constexpr std::uint8_t CRSF_FRAMETYPE_LINK_STATISTICS = 0x14;
inline constexpr std::uint8_t CRSF_FRAMETYPE_RC_CHANNELS_PACKED = 0x16;
inline constexpr std::uint8_t CRSF_FRAMETYPE_ATTITUDE = 0x1E;
inline constexpr std::uint8_t CRSF_FRAMETYPE_FLIGHT_MODE = 0x21;
inline constexpr std::uint8_t CRSF_FRAMETYPE_DEVICE_PING = 0x28;
inline constexpr std::uint8_t CRSF_FRAMETYPE_DEVICE_INFO = 0x29;
inline constexpr std::uint8_t CRSF_FRAMETYPE_PARAMETER_SETTINGS_ENTRY = 0x2B;
inline constexpr std::uint8_t CRSF_FRAMETYPE_PARAMETER_READ = 0x2C;
inline constexpr std::uint8_t CRSF_FRAMETYPE_PARAMETER_WRITE = 0x2D;
inline constexpr std::uint8_t CRSF_FRAMETYPE_ELRS_STATUS = 0x2E;
inline constexpr std::uint8_t CRSF_FRAMETYPE_COMMAND = 0x32;
inline constexpr std::uint8_t CRSF_FRAMETYPE_HANDSET = 0x3A;
inline constexpr std::uint8_t CRSF_FRAMETYPE_KISS_REQ = 0x78;
inline constexpr std::uint8_t CRSF_FRAMETYPE_KISS_RESP = 0x79;
inline constexpr std::uint8_t CRSF_FRAMETYPE_MSP_REQ = 0x7A;
inline constexpr std::uint8_t CRSF_FRAMETYPE_MSP_RESP = 0x7B;
inline constexpr std::uint8_t CRSF_FRAMETYPE_MSP_WRITE = 0x7C;
inline constexpr std::uint8_t CRSF_FRAMETYPE_ARDUPILOT_RESP = 0x80;

inline constexpr std::uint8_t CRSF_COMMAND_SUBCMD_RX = 0x10;
inline constexpr std::uint8_t CRSF_COMMAND_SUBCMD_RX_BIND = 0x01;
inline constexpr std::uint8_t CRSF_COMMAND_MODEL_SELECT_ID = 0x05;
inline constexpr std::uint8_t CRSF_HANDSET_SUBCMD_TIMING = 0x10;
inline constexpr std::size_t CRSF_FRAME_TX_MSP_FRAME_SIZE = 58;
inline constexpr std::size_t CRSF_FRAME_RX_MSP_FRAME_SIZE = 8;
inline constexpr std::size_t CRSF_FRAME_ORIGIN_DEST_SIZE = 2;

inline constexpr std::uint8_t CRSF_ADDRESS_BROADCAST = 0x00;
inline constexpr std::uint8_t CRSF_ADDRESS_USB = 0x10;
inline constexpr std::uint8_t CRSF_ADDRESS_BLUETOOTH_WIFI = 0x12;
inline constexpr std::uint8_t CRSF_ADDRESS_TBS_CORE_PNP_PRO = 0x80;
inline constexpr std::uint8_t CRSF_ADDRESS_RESERVED1 = 0x8A;
inline constexpr std::uint8_t CRSF_ADDRESS_CURRENT_SENSOR = 0xC0;
inline constexpr std::uint8_t CRSF_ADDRESS_GPS = 0xC2;
inline constexpr std::uint8_t CRSF_ADDRESS_TBS_BLACKBOX = 0xC4;
inline constexpr std::uint8_t CRSF_ADDRESS_FLIGHT_CONTROLLER = 0xC8;
inline constexpr std::uint8_t CRSF_ADDRESS_RESERVED2 = 0xCA;
inline constexpr std::uint8_t CRSF_ADDRESS_RACE_TAG = 0xCC;
inline constexpr std::uint8_t CRSF_ADDRESS_RADIO_TRANSMITTER = 0xEA;
inline constexpr std::uint8_t CRSF_ADDRESS_CRSF_RECEIVER = 0xEC;
inline constexpr std::uint8_t CRSF_ADDRESS_CRSF_TRANSMITTER = 0xEE;
inline constexpr std::uint8_t CRSF_ADDRESS_ELRS_LUA = 0xEF;

inline constexpr std::uint8_t CRSF_UINT8 = 0;
inline constexpr std::uint8_t CRSF_INT8 = 1;
inline constexpr std::uint8_t CRSF_UINT16 = 2;
inline constexpr std::uint8_t CRSF_INT16 = 3;
inline constexpr std::uint8_t CRSF_UINT32 = 4;
inline constexpr std::uint8_t CRSF_INT32 = 5;
inline constexpr std::uint8_t CRSF_UINT64 = 6;
inline constexpr std::uint8_t CRSF_INT64 = 7;
inline constexpr std::uint8_t CRSF_FLOAT = 8;
inline constexpr std::uint8_t CRSF_TEXT_SELECTION = 9;
inline constexpr std::uint8_t CRSF_STRING = 10;
inline constexpr std::uint8_t CRSF_FOLDER = 11;
inline constexpr std::uint8_t CRSF_INFO = 12;
inline constexpr std::uint8_t CRSF_COMMAND = 13;
inline constexpr std::uint8_t CRSF_VTX = 15;
inline constexpr std::uint8_t CRSF_OUT_OF_RANGE = 127;
inline constexpr std::uint8_t CRSF_FIELD_HIDDEN = 0x80;
inline constexpr std::uint8_t CRSF_FIELD_TYPE_MASK = 0x7F;

inline constexpr std::uint8_t CRSF_CHANNELS_STATUS_ARMED = 1U << 0;
inline constexpr std::uint8_t CRSF_CHANNELS_STATUS_ARMING_MODE_CH5 = 1U << 1;

// Upstream derives these lengths from packed wire structs and a local device_name.
inline constexpr std::size_t CRSF_HEADER_SIZE = 3;
inline constexpr std::size_t CRSF_EXT_HEADER_SIZE = CRSF_HEADER_SIZE + CRSF_FRAME_ORIGIN_DEST_SIZE;
inline constexpr std::size_t DEVICE_INFORMATION_FIXED_PAYLOAD_LENGTH = 14;
[[nodiscard]] constexpr std::size_t
DEVICE_INFORMATION_PAYLOAD_LENGTH(const std::string_view device_name) noexcept {
    return DEVICE_INFORMATION_FIXED_PAYLOAD_LENGTH + device_name.size() + 1;
}
[[nodiscard]] constexpr std::size_t
DEVICE_INFORMATION_LENGTH(const std::string_view device_name) noexcept {
    return CRSF_EXT_HEADER_SIZE + DEVICE_INFORMATION_PAYLOAD_LENGTH(device_name) +
           CRSF_FRAME_CRC_SIZE;
}
[[nodiscard]] constexpr std::size_t
DEVICE_INFORMATION_FRAME_SIZE(const std::string_view device_name) noexcept {
    return DEVICE_INFORMATION_PAYLOAD_LENGTH(device_name) + CRSF_FRAME_LENGTH_EXT_TYPE_CRC;
}
[[nodiscard]] constexpr std::size_t MSP_REQUEST_PAYLOAD_LENGTH(const std::size_t length) noexcept {
    return 7 + length;
}
[[nodiscard]] constexpr std::size_t MSP_REQUEST_LENGTH(const std::size_t length) noexcept {
    return CRSF_EXT_HEADER_SIZE + MSP_REQUEST_PAYLOAD_LENGTH(length) + CRSF_FRAME_CRC_SIZE;
}
[[nodiscard]] constexpr std::size_t MSP_REQUEST_FRAME_SIZE(const std::size_t length) noexcept {
    return MSP_REQUEST_PAYLOAD_LENGTH(length) + CRSF_FRAME_LENGTH_EXT_TYPE_CRC;
}
inline constexpr std::size_t MSP_SET_VTX_CONFIG_PAYLOAD_LENGTH = 15;
inline constexpr std::size_t MSP_SET_VTXTABLE_BAND_PAYLOAD_LENGTH = 29;
inline constexpr std::size_t MSP_SET_VTXTABLE_POWERLEVEL_PAYLOAD_LENGTH = 7;

// Wire-layout constants derived from the upstream channel/link-statistics structs.
inline constexpr std::size_t CRSF_PAYLOAD_OFFSET = CRSF_HEADER_SIZE;
inline constexpr std::size_t CRSF_FRAME_TYPE_CRC_SIZE =
    CRSF_MIN_PACKET_LEN - CRSF_FRAME_NOT_COUNTED_BYTES;
inline constexpr unsigned CRSF_CHANNEL_BITS = 11;
inline constexpr std::size_t CRSF_NUM_CHANNELS = 16;
inline constexpr std::size_t CRSF_RC_CHANNELS_PAYLOAD_SIZE =
    CRSF_NUM_CHANNELS * CRSF_CHANNEL_BITS / 8;
inline constexpr std::size_t CRSF_LINK_STATISTICS_PAYLOAD_SIZE = 10;
inline constexpr std::size_t CRSF_BATTERY_SENSOR_PAYLOAD_SIZE = 8;

} // namespace coretx::crsf
