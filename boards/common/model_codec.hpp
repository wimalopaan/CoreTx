// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/common/controls.hpp"
#include "boards/common/model_format.hpp"
#include "boards/common/radio_settings.hpp"
#include "platform/stm32f072/crsf.hpp"

#include <memory>

namespace coretx::firmware::models {
static_assert((directory_base + 128) <= 16'384);
// Curve references reuse line padding; packed labels and tank metadata retain the journal size.
static_assert((34 + 2 + (max_input_lines * coretx::storage::detail::packed_model::input_bytes) +
               (max_mix_lines * coretx::storage::detail::packed_model::mix_bytes) +
               (channel_count * coretx::storage::detail::packed_model::output_bytes) + 8 +
               (logical_switch_count * 7) + (multiswitch_page_count * 45) + 1 +
               (multiswitch_page_count * multiswitch_function_count * sizeof(MultiSwitchControl)) +
               trim_count + 1 + curve_count * 8 + (max_mix_lines + 7) / 8 + 5 +
               (max_mix_lines + 3) / 4) <= (record_size - 4));
using Record = std::array<std::uint8_t, record_size>;
using Name = std::array<char, 12>;

struct EmptyModel {};
inline constexpr EmptyModel empty_model{};

struct Data {
    constexpr Data() noexcept = default;
    // Avoids copying the flash-resident diagnostic_model; the owner fills the model at runtime.
    constexpr explicit Data(EmptyModel) noexcept : model{} {}

    Model model{diagnostic_model};
    RadioSettings radio{};
    Name name{'M', 'o', 'd', 'e', 'l', ' ', '0', '1'};
    std::uint8_t baud_index{static_cast<std::uint8_t>(crsf_default_baudrate_index)};
};

struct Info {
    Name name{};
    std::uint32_t generation{};
    std::uint32_t checksum{};
    std::uint8_t bank{};
    bool present{};
    bool valid{};
};

constexpr void default_name(Name &name, const std::size_t index) noexcept {
    name = {'M', 'o', 'd', 'e', 'l', ' ', '0', static_cast<char>('1' + index)};
}

constexpr void four_channel_model(Model &model) noexcept {
    // Reconstruct in caller-owned storage instead of assigning a full-model stack temporary.
    std::destroy_at(&model);
    std::construct_at(&model);
    model.inputs.resize<4>();
    model.mixes.resize<4>();
    for (std::size_t i = 0; i < 4; ++i) {
        model.inputs[i].destination = static_cast<std::uint8_t>(i);
        model.inputs[i].source.index = static_cast<std::uint8_t>(i);
        model.inputs[i].trim = TrimSource::automatic;
        model.mixes[i].destination = static_cast<std::uint8_t>(i);
        model.mixes[i].source = {SourceKind::input, static_cast<std::uint8_t>(i), false};
        model.mixes[i].carry_trim = true;
    }
}
constexpr Model four_channel_model() noexcept {
    Model model{};
    four_channel_model(model);
    return model;
}

constexpr bool valid_data(const Data &data, const afhds2a::TxId transmitter) noexcept {
    if (!coretx::validate(data.model) || (data.radio.transmitter != transmitter) ||
        !afhds2a::valid_id(transmitter.value) || (data.radio.output > RadioOutput::external) ||
        (data.radio.channel_mode > afhds2a::ChannelMode::extended16) ||
        (data.baud_index >= crsf_baudrates.size()) ||
        ((data.radio.receiver.value != 0) && !afhds2a::valid_id(data.radio.receiver.value)) ||
        (data.name[0] == 0) || (data.name.back() != 0))
        return false;
    bool ended{};
    for (const char ch : data.name) {
        if (ch == 0)
            ended = true;
        else if (ended || (ch < ' ') || (ch > '~'))
            return false;
    }
    return true;
}

template <typename Archive, typename Snapshot>
constexpr bool transfer_curves(Archive &archive, Snapshot &model, const bool enabled) noexcept {
    if (!coretx::storage::detail::transfer_curves(archive, model, enabled, false))
        return false;
    if (!enabled)
        return archive.good;
    // The low two mixer selection bits are stored in the packed line's spare bits.
    for (std::size_t group = 0; group < model.mixes.size(); group += 8) {
        std::uint8_t high{};
        if constexpr (std::is_const_v<Snapshot>)
            for (std::size_t i = group; (i < group + 8) && (i < model.mixes.size()); ++i)
                high |= static_cast<std::uint8_t>((model.mixes[i].curve >> 2) << (i - group));
        archive.field(high);
        if constexpr (!std::is_const_v<Snapshot>) {
            const auto remaining = std::min(std::size_t{8}, model.mixes.size() - group);
            if ((static_cast<unsigned>(high) >> remaining) != 0)
                archive.reject(archive.position - 1);
            for (std::size_t i = 0; i < remaining; ++i)
                model.mixes[group + i].curve |=
                    static_cast<std::uint8_t>(((static_cast<unsigned>(high) >> i) & 1U) << 2);
        }
    }
    return archive.good;
}

constexpr bool encode(const Data &data, const std::size_t index, const bool present,
                      const std::uint32_t generation, Record &record) noexcept {
    if ((index >= count) || !valid_data(data, data.radio.transmitter))
        return false;
    record.fill(0xFF);
    coretx::storage::detail::Writer out{record};
    out.word(magic);
    out.field(format_version);
    out.field(static_cast<std::uint8_t>(index));
    out.field(present);
    out.field(data.baud_index);
    out.word(generation);
    out.word(data.radio.transmitter.value);
    out.word(data.radio.receiver.value);
    out.field(data.radio.output);
    out.field(data.radio.channel_mode);
    for (const auto ch : data.name)
        out.field(static_cast<std::uint8_t>(ch));
    if (!coretx::storage::detail::transfer_model_counts(out, data.model) ||
        !coretx::storage::detail::transfer_model_lines(out, data.model, true) ||
        !coretx::storage::detail::transfer_model_extension(out, data.model) ||
        !coretx::storage::detail::transfer_multiswitch(out, data.model, true, true) ||
        !coretx::storage::detail::transfer_dashboard_options(out, data.model) ||
        !coretx::storage::detail::transfer_multiswitch_controls(out, data.model) ||
        !models::transfer_curves(out, data.model, true) ||
        !coretx::storage::detail::transfer_tank(out, data.model))
        return false;
    coretx::storage::detail::Writer crc{std::span{record}.last(4)};
    crc.word(coretx::storage::crc32(std::span{record}.first(record_size - 4)));
    return out.good && (out.position <= (record_size - 4));
}

// 0: erased, 1: valid, 2: damaged, 3: incompatible version, 4: unavailable module.
constexpr int decode(const Record &record, const std::size_t index, const afhds2a::TxId transmitter,
                     Data &data, Info &info) noexcept {
    if (std::all_of(record.begin(), record.end(), [](auto b) { return b == 0xFF; }))
        return 0;
    if ((record[0] == 0) && (((record[1] == 'D') && (record[2] == 'L') && (record[3] == '1')) ||
                             ((record[1] == 0xFF) && (record[2] == 0xFF) && (record[3] == 0xFF))))
        return 0; // Uncommitted journal bank.
    coretx::storage::detail::Reader crc{std::span{record}.last(4)};
    std::uint32_t checksum{};
    crc.word(checksum);
    if (checksum != coretx::storage::crc32(std::span{record}.first(record_size - 4)))
        return 2;
    coretx::storage::detail::Reader in{std::span{record}.first(record_size - 4)};
    std::uint32_t identifier{};
    std::uint8_t version{}, slot{};
    in.word(identifier);
    in.field(version);
    in.field(slot);
    if (identifier != magic)
        return 2;
    if (version > format_version)
        return 3;
    if (version < coretx::storage::oldest_format_version)
        return 3;
    if (slot != index)
        return 2;
    in.field(info.present);
    in.field(data.baud_index);
    in.word(info.generation);
    in.word(data.radio.transmitter.value);
    in.word(data.radio.receiver.value);
    in.field(data.radio.output);
    in.field(data.radio.channel_mode);
    for (auto &ch : data.name)
        ch = static_cast<char>(in.byte());
    if (!coretx::storage::detail::transfer_model_counts(in, data.model) ||
        !coretx::storage::detail::transfer_model_lines(in, data.model, version >= 8,
                                                       version >= 11) ||
        !coretx::storage::detail::transfer_model_extension(in, data.model) ||
        !coretx::storage::detail::transfer_multiswitch(in, data.model, version >= 8,
                                                       version >= 12) ||
        !coretx::storage::detail::transfer_dashboard_options(in, data.model, version >= 7) ||
        !coretx::storage::detail::transfer_multiswitch_controls(in, data.model) ||
        !models::transfer_curves(in, data.model, version >= 8) ||
        !coretx::storage::detail::transfer_tank(in, data.model, version >= 9, version >= 10) ||
        !std::all_of(record.begin() + static_cast<std::ptrdiff_t>(in.position), record.end() - 4,
                     [](auto b) { return b == 0xFF; }))
        return 2;
    if (coretx::validate(data.model).code == ErrorCode::tank_unavailable)
        return 4;
    if (!valid_data(data, transmitter))
        return 2;
    info.name = data.name;
    info.checksum = checksum;
    info.valid = true;
    return 1;
}
} // namespace coretx::firmware::models
