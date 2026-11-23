// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <bit>
#include <type_traits>

namespace coretx::storage {
namespace detail {

inline constexpr std::uint32_t magic = 0x53434352; // Little-endian "RCCS".
inline constexpr std::size_t checksum_offset = 12;
struct Writer {
    std::span<std::uint8_t> bytes;
    std::size_t position{};
    bool good{true};

    constexpr void field(const std::uint8_t value) noexcept {
        if (position == bytes.size()) {
            good = false;
        } else {
            bytes[position++] = value;
        }
    }
    constexpr void field(const std::uint16_t value) noexcept {
        field(static_cast<std::uint8_t>(value & 0xFFU));
        field(static_cast<std::uint8_t>(value >> 8));
    }
    constexpr void word(const std::uint32_t value) noexcept {
        field(static_cast<std::uint16_t>(value & 0xFFFFU));
        field(static_cast<std::uint16_t>(value >> 16));
    }
    constexpr void field(const bool value) noexcept { field(std::uint8_t{value}); }

    template <typename Enum>
        requires std::is_enum_v<Enum>
    constexpr void field(const Enum &value) noexcept {
        field(static_cast<std::uint8_t>(value));
    }
    template <typename Value>
        requires requires { Value::minimum; }
    constexpr void field(const Value value) noexcept {
        static_assert(sizeof(value.value()) <= 2);
        if constexpr (sizeof(value.value()) == 1) {
            field(static_cast<std::uint8_t>(value.value()));
        } else {
            field(static_cast<std::uint16_t>(value.value()));
        }
    }
};

struct Reader {
    std::span<const std::uint8_t> bytes;
    std::size_t position{};
    bool good{true};
    std::size_t error_offset{};

    constexpr void reject(const std::size_t offset) noexcept {
        if (good) {
            error_offset = offset;
            good = false;
        }
    }
    [[nodiscard]] constexpr std::uint8_t byte() noexcept {
        if (position == bytes.size()) {
            reject(position);
            return 0;
        }
        return bytes[position++];
    }
    constexpr void field(std::uint8_t &value) noexcept { value = byte(); }
    constexpr void field(std::uint16_t &value) noexcept {
        const auto low = byte();
        const auto high = byte();
        value = static_cast<std::uint16_t>(low | (static_cast<std::uint16_t>(high) << 8));
    }
    constexpr void word(std::uint32_t &value) noexcept {
        std::uint16_t low{};
        std::uint16_t high{};
        field(low);
        field(high);
        value = low | (static_cast<std::uint32_t>(high) << 16);
    }
    constexpr void field(bool &value) noexcept {
        const auto raw = byte();
        if (raw > 1) {
            reject(position - 1);
        }
        value = raw != 0;
    }
    template <typename Enum>
        requires std::is_enum_v<Enum>
    constexpr void field(Enum &value) noexcept {
        const auto raw = byte();
        if constexpr (std::is_signed_v<std::underlying_type_t<Enum>>) {
            const auto signed_value =
                raw < 128 ? static_cast<std::int32_t>(raw) : static_cast<std::int32_t>(raw) - 256;
            value = static_cast<Enum>(signed_value);
        } else {
            value = static_cast<Enum>(raw);
        }
    }
    template <typename Value>
        requires requires { Value::minimum; }
    constexpr void field(Value &value) noexcept {
        static_assert(sizeof(value.value()) <= 2);
        const auto offset = position;
        std::uint16_t raw{};
        if constexpr (sizeof(value.value()) == 1) {
            raw = byte();
        } else {
            field(raw);
        }
        constexpr std::int32_t modulus = sizeof(value.value()) == 1 ? 256 : 65536;
        const auto signed_value = raw < (modulus / 2) ? static_cast<std::int32_t>(raw)
                                                      : static_cast<std::int32_t>(raw) - modulus;
        if (!value.try_set(signed_value)) {
            reject(offset);
        }
    }
};

namespace packed_model {
inline constexpr std::size_t input_bytes = 7;
inline constexpr std::size_t mix_bytes = 6;
inline constexpr std::size_t output_bytes = 5;
inline constexpr unsigned flag_bits = 1;
inline constexpr unsigned source_kind_bits = 2;
inline constexpr unsigned source_index_bits = 5;
inline constexpr unsigned condition_index_bits = 5;
inline constexpr unsigned position_bits = 4;
inline constexpr unsigned input_destination_bits = 3;
inline constexpr unsigned output_destination_bits = 4;
inline constexpr unsigned side_bits = 2;
inline constexpr unsigned trim_bits = 3;
inline constexpr unsigned multiplex_bits = 2;
inline constexpr unsigned byte_bits = std::numeric_limits<std::uint8_t>::digits;
} // namespace packed_model
inline constexpr std::size_t maximum_payload =
    2 + calibration_count * 8 + max_input_lines * packed_model::input_bytes +
    max_mix_lines * packed_model::mix_bytes + channel_count * packed_model::output_bytes + 64 +
    multiswitch_page_count * 51 + 1 +
    multiswitch_page_count * multiswitch_function_count * sizeof(MultiSwitchControl) + trim_count +
    1;
static_assert((header_size + maximum_payload) <= slot_size);

template <typename Archive> class PackedFields {
  public:
    constexpr PackedFields(Archive &archive, const std::size_t bytes) noexcept
        : archive_(archive), start_(archive.position), bytes_(bytes) {
        if constexpr (std::is_same_v<Archive, Reader>)
            for (std::size_t i = 0; i < bytes_; ++i)
                word_ |= std::uint64_t{archive_.byte()} << (i * packed_model::byte_bits);
    }
    template <typename Value> constexpr void field(const unsigned width, Value &value) noexcept {
        using Type = std::remove_cv_t<Value>;
        const auto mask = (std::uint64_t{1} << width) - 1;
        if constexpr (std::is_same_v<Archive, Reader>) {
            const auto raw = static_cast<std::uint32_t>((word_ >> used_) & mask);
            if constexpr (requires { Type::minimum; }) {
                if (!value.try_set(Type::minimum + static_cast<std::int32_t>(raw)))
                    archive_.reject(start_);
            } else
                value = static_cast<Type>(raw);
        } else {
            const auto raw = [&] constexpr {
                if constexpr (requires { Type::minimum; })
                    return static_cast<std::uint64_t>(value.value() - Type::minimum);
                else
                    return static_cast<std::uint64_t>(value);
            }();
            if (raw > mask)
                archive_.good = false;
            word_ |= (raw & mask) << used_;
        }
        used_ += width;
    }
    template <typename Value>
        requires requires { std::remove_cv_t<Value>::minimum; }
    constexpr void field(Value &value) noexcept {
        using Type = std::remove_cv_t<Value>;
        constexpr auto range = static_cast<std::uint32_t>(Type::maximum - Type::minimum);
        field(std::bit_width(range), value);
    }
    constexpr bool finish() noexcept {
        if constexpr (std::is_same_v<Archive, Reader>) {
            if ((word_ >> used_) != 0)
                archive_.reject(start_);
        } else
            for (std::size_t i = 0; i < bytes_; ++i)
                archive_.field(static_cast<std::uint8_t>(word_ >> (i * packed_model::byte_bits)));
        return archive_.good;
    }

  private:
    Archive &archive_;
    const std::size_t start_;
    const std::size_t bytes_;
    std::uint64_t word_{};
    unsigned used_{};
};

template <typename Archive, typename ModelType>
constexpr bool transfer_model_lines(Archive &archive, ModelType &model) noexcept {
    namespace bits = packed_model;
    const auto common = [&](auto &fields, auto &line, const unsigned destination_bits) constexpr {
        fields.field(destination_bits, line.destination);
        fields.field(bits::source_kind_bits, line.source.kind);
        fields.field(bits::source_index_bits, line.source.index);
        fields.field(bits::flag_bits, line.source.inverted);
        fields.field(bits::flag_bits, line.condition.enabled);
        fields.field(bits::condition_index_bits, line.condition.index);
        auto position = static_cast<std::uint8_t>(static_cast<int>(line.condition.position) + 1);
        fields.field(bits::position_bits, position);
        fields.field(bits::flag_bits, line.condition.inverted);
        if constexpr (!std::is_const_v<ModelType>)
            line.condition.position = static_cast<SwitchPosition>(static_cast<int>(position) - 1);
    };
    for (auto &line : model.inputs) {
        PackedFields fields{archive, bits::input_bytes};
        common(fields, line, bits::input_destination_bits);
        fields.field(bits::side_bits, line.side);
        fields.field(bits::trim_bits, line.trim);
        fields.field(line.weight_percent);
        fields.field(line.offset_percent);
        fields.field(line.expo_percent);
        if (!fields.finish())
            return false;
    }
    for (auto &line : model.mixes) {
        PackedFields fields{archive, bits::mix_bytes};
        common(fields, line, bits::output_destination_bits);
        fields.field(bits::multiplex_bits, line.multiplex);
        fields.field(bits::flag_bits, line.carry_trim);
        fields.field(line.weight_percent);
        fields.field(line.offset_percent);
        if (!fields.finish())
            return false;
    }
    for (auto &output : model.outputs) {
        PackedFields fields{archive, bits::output_bytes};
        fields.field(output.minimum_permille);
        fields.field(output.maximum_permille);
        fields.field(output.subtrim_permille);
        fields.field(bits::flag_bits, output.reversed);
        if (!fields.finish())
            return false;
    }
    return archive.good;
}

template <typename Archive, typename Snapshot>
constexpr bool transfer_multiswitch(Archive &archive, Snapshot &model) noexcept {
    for (auto &page : model.multiswitch) {
        auto address = static_cast<std::uint16_t>(page.address);
        archive.field(address);
        archive.field(page.momentary);
        for (auto &label : page.labels)
            for (auto &character : label) {
                auto byte = static_cast<std::uint8_t>(character);
                archive.field(byte);
                if constexpr (!std::is_const_v<Snapshot>)
                    character = static_cast<char>(byte);
            }
        if constexpr (!std::is_const_v<Snapshot>)
            page.address = static_cast<std::int16_t>(address);
    }
    return archive.good;
}

template <typename Archive, typename ModelType>
constexpr bool transfer_model_counts(Archive &archive, ModelType &model) noexcept {
    auto inputs = static_cast<std::uint8_t>(model.inputs.size());
    auto mixes = static_cast<std::uint8_t>(model.mixes.size());
    archive.field(inputs);
    archive.field(mixes);
    if constexpr (!std::is_const_v<ModelType>) {
        if (!model.inputs.try_resize(inputs) || !model.mixes.try_resize(mixes)) {
            archive.reject(0);
        }
    }
    return archive.good;
}

template <typename Archive, typename Snapshot>
constexpr bool transfer_model_extension(Archive &archive, Snapshot &model) noexcept {
    for (auto &channel : model.dashboard_channels)
        archive.field(channel);
    for (auto &line : model.logical_switches) {
        auto function = static_cast<std::uint8_t>(static_cast<unsigned>(line.function) |
                                                  (line.inverted ? 0x80U : 0U));
        auto flags_a =
            static_cast<std::uint8_t>(static_cast<unsigned>(static_cast<int>(line.position_a) + 1) |
                                      (line.inverted_a ? 0x10U : 0U));
        auto flags_b =
            static_cast<std::uint8_t>(static_cast<unsigned>(static_cast<int>(line.position_b) + 1) |
                                      (line.inverted_b ? 0x10U : 0U));
        archive.field(function);
        archive.field(line.a);
        archive.field(flags_a);
        archive.field(line.b);
        archive.field(flags_b);
        auto threshold = static_cast<std::uint16_t>(line.threshold);
        archive.field(threshold);
        if constexpr (!std::is_const_v<Snapshot>) {
            if (((function & 0x7FU) > static_cast<unsigned>(LogicalFunction::srff)) ||
                ((flags_a & 0xEFU) > 8U) || ((flags_b & 0xEFU) > 8U))
                archive.reject(archive.position - 7);
            line.function = static_cast<LogicalFunction>(function & 0x7FU);
            line.inverted = (function & 0x80U) != 0;
            line.position_a = static_cast<SwitchPosition>(static_cast<int>(flags_a & 0xFU) - 1);
            line.position_b = static_cast<SwitchPosition>(static_cast<int>(flags_b & 0xFU) - 1);
            line.inverted_a = (flags_a & 0x10U) != 0;
            line.inverted_b = (flags_b & 0x10U) != 0;
            line.threshold = static_cast<std::int16_t>(threshold);
        }
    }
    return archive.good;
}

template <typename Archive, typename Snapshot>
constexpr bool transfer_multiswitch_controls(Archive &archive, Snapshot &model) noexcept {
    for (auto &page : model.multiswitch)
        for (auto &control : page.controls)
            for (auto &byte : control.bytes)
                archive.field(byte);
    for (auto &trim : model.trims)
        archive.field(trim);
    archive.field(model.trim_switches);
    return archive.good;
}

template <typename Archive, typename Snapshot>
constexpr bool transfer_dashboard_options(Archive &archive, Snapshot &model,
                                          const bool sound_options = true) noexcept {
    // Format 6 has no mute bit, so its feedback remains enabled.
    auto flags = static_cast<std::uint8_t>((model.crsf_dashboard ? 1U : 0U) |
                                           (model.multiswitch_beep ? 0U : 2U));
    archive.field(flags);
    if constexpr (!std::is_const_v<Snapshot>) {
        if (flags > (sound_options ? 3U : 1U))
            archive.reject(archive.position - 1);
        model.crsf_dashboard = (flags & 1U) != 0;
        model.multiswitch_beep = (flags & 2U) == 0;
    }
    return archive.good;
}

template <typename Archive, typename Snapshot>
constexpr bool transfer(Archive &archive, Snapshot &state,
                        const bool sound_options = true) noexcept {
    if (!transfer_model_counts(archive, state.model))
        return false;
    for (auto &calibration : state.setup.calibration) {
        archive.field(calibration.minimum);
        archive.field(calibration.center);
        archive.field(calibration.maximum);
    }
    for (auto &mapping : state.setup.mapping) {
        archive.field(mapping.physical_index);
        archive.field(mapping.reversed);
    }
    return transfer_model_lines(archive, state.model) &&
           transfer_model_extension(archive, state.model) &&
           transfer_multiswitch(archive, state.model) &&
           transfer_dashboard_options(archive, state.model, sound_options) &&
           transfer_multiswitch_controls(archive, state.model);
}

[[nodiscard]] constexpr std::uint32_t crc_byte(std::uint32_t crc,
                                               const std::uint8_t byte) noexcept {
    crc ^= byte;
    for (unsigned bit = 0; bit < 8; ++bit) {
        crc = (crc >> 1) ^ ((crc & 1U) != 0 ? 0xEDB88320U : 0U);
    }
    return crc;
}

[[nodiscard]] constexpr std::uint32_t
record_crc(const std::span<const std::uint8_t> &record) noexcept {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t i = 0; i < record.size(); ++i) {
        if ((i < checksum_offset) || (i >= (checksum_offset + 4))) {
            crc = crc_byte(crc, record[i]);
        }
    }
    return ~crc;
}

[[nodiscard]] constexpr SlotInfo decode_into(const std::span<const std::uint8_t> &record,
                                             State &state) noexcept {
    if (record.empty()) {
        return {{Error::empty}, 0};
    }
    if (record.size() != slot_size) {
        return {{Error::truncated}, 0};
    }
    if (std::all_of(record.begin(), record.end(), [](const auto value) { return value == 0xFF; })) {
        return {{Error::empty}, 0};
    }
    Reader header{record.first(header_size)};
    std::uint32_t identifier{};
    std::uint16_t version{};
    std::uint16_t length{};
    std::uint32_t generation{};
    std::uint32_t checksum{};
    header.word(identifier);
    header.field(version);
    header.field(length);
    header.word(generation);
    header.word(checksum);
    if (identifier != magic) {
        return {{Error::magic}, 0};
    }
    if (checksum != record_crc(record)) {
        return {{Error::checksum, checksum_offset}, 0};
    }
    if (version > format_version) {
        return {{Error::version, 4}, 0};
    }
    if (version < oldest_format_version)
        return {{Error::obsolete, 4}, 0};
    if ((length > maximum_payload) ||
        !std::all_of(record.begin() + header_size + length, record.end(),
                     [](const auto value) { return value == 0xFF; })) {
        return {{Error::length, 6}, 0};
    }
    Reader payload{record.subspan(header_size, length)};
    if (!transfer(payload, state, version >= 7)) {
        return {{Error::field, header_size + payload.error_offset}, 0};
    }
    if (payload.position != length) {
        return {{Error::length, 6}, 0};
    }
    return {validate(state), generation};
}

} // namespace detail

constexpr Status validate(const State &state) noexcept {
    if (!coretx::validate(state.model)) {
        return {Error::model};
    }
    if (!coretx::validate(state.setup)) {
        return {Error::setup};
    }
    return {};
}

constexpr std::uint32_t crc32(const std::span<const std::uint8_t> &data) noexcept {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const auto byte : data) {
        crc = detail::crc_byte(crc, byte);
    }
    return ~crc;
}

constexpr Status encode(const State &state, const std::uint32_t generation,
                        Record &record) noexcept {
    if (const auto status = validate(state); !status) {
        return status;
    }
    record.fill(0xFF);
    detail::Writer payload{std::span{record}.subspan(header_size)};
    if (!detail::transfer(payload, state)) {
        return {Error::length};
    }
    detail::Writer header{std::span{record}.first(header_size)};
    header.word(detail::magic);
    header.field(format_version);
    header.field(static_cast<std::uint16_t>(payload.position));
    header.word(generation);
    header.word(0);
    detail::Writer checksum{std::span{record}.subspan(detail::checksum_offset, 4)};
    checksum.word(detail::record_crc(record));
    return {};
}

constexpr SlotInfo decode(const std::span<const std::uint8_t> &record, State &state) noexcept {
    State candidate{};
    const auto info = detail::decode_into(record, candidate);
    if (info) {
        state = candidate;
    }
    return info;
}

constexpr SlotInfo inspect(const std::span<const std::uint8_t> &record) noexcept {
    State candidate{};
    return detail::decode_into(record, candidate);
}

namespace detail {
constexpr Selection select_records(const std::span<const std::uint8_t> &first,
                                   const std::span<const std::uint8_t> &second, const SlotInfo a,
                                   const SlotInfo b) noexcept {
    Selection result{{}, {a.status, b.status}, 0, a.generation};
    // Never overwrite an intact record from a newer, unsupported format.
    if ((a.status.code == Error::version) || (b.status.code == Error::version)) {
        result.status = {Error::version};
    } else if (!a && !b) {
        result.status = {(a.status.code == Error::empty) && (b.status.code == Error::empty)
                             ? Error::empty
                             : Error::no_valid_slot};
    } else if (!a) {
        result.slot = 1;
        result.generation = b.generation;
    } else if (b) {
        const std::uint32_t distance = b.generation - a.generation;
        if ((distance == 0x80000000U) ||
            ((distance == 0) && !std::equal(first.begin(), first.end(), second.begin()))) {
            result.status = {Error::generation};
        } else if ((distance != 0) && (distance < 0x80000000U)) {
            result.slot = 1;
            result.generation = b.generation;
        }
    }
    return result;
}
} // namespace detail

constexpr Selection select(const std::span<const std::uint8_t> &first,
                           const std::span<const std::uint8_t> &second) noexcept {
    const auto a = inspect(first);
    const auto b = inspect(second);
    return detail::select_records(first, second, a, b);
}

constexpr Selection select_into(const std::span<const std::uint8_t> &first,
                                const std::span<const std::uint8_t> &second,
                                State &scratch) noexcept {
    const auto a = detail::decode_into(first, scratch);
    const auto b = detail::decode_into(second, scratch);
    auto result = detail::select_records(first, second, a, b);
    if (result && (result.slot == 0))
        result.status = detail::decode_into(first, scratch).status;
    return result;
}

constexpr const char *error_message(const Error &error) noexcept {
    switch (error) {
    case Error::none:
        return "no error";
    case Error::empty:
        return "no stored state";
    case Error::truncated:
        return "incomplete slot";
    case Error::magic:
        return "invalid storage identifier";
    case Error::version:
        return "unsupported storage version";
    case Error::obsolete:
        return "obsolete storage version";
    case Error::length:
        return "invalid payload length or reserved bytes";
    case Error::checksum:
        return "CRC mismatch";
    case Error::field:
        return "invalid serialized field";
    case Error::model:
        return "invalid stored model";
    case Error::setup:
        return "invalid stored calibration or mapping";
    case Error::no_valid_slot:
        return "neither storage slot is valid";
    case Error::generation:
        return "ambiguous storage generations";
    }
    return "unknown storage error";
}

} // namespace coretx::storage
