// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "coretx/core/core.hpp"
#include "coretx/radio/crsf.hpp"

#include <limits>
#include <optional>

namespace coretx::multiswitch {

inline constexpr DurationUs repeat_interval{100'000};
inline constexpr std::size_t switch_count = multiswitch_function_count;
inline constexpr std::size_t maximum_addresses = multiswitch_page_count * switch_count;
inline constexpr unsigned timestamp_bits = 24;
inline constexpr std::uint32_t timestamp_mask = (1U << timestamp_bits) - 1U;
static_assert(maximum_addresses <= std::numeric_limits<std::uint32_t>::digits);
static_assert(repeat_interval.count() <= timestamp_mask);
inline constexpr std::int16_t disabled_address = MultiSwitchPage{}.address;
inline constexpr std::int16_t minimum_address = 0;
inline constexpr std::int16_t maximum_address = std::numeric_limits<std::uint8_t>::max();

namespace set4 {
inline constexpr std::uint8_t command_id = 0xA1;
inline constexpr std::uint8_t subcommand_id = 0x07;
inline constexpr std::uint8_t command_crc_polynomial = 0xBA;
inline constexpr unsigned bits_per_switch = 2;
// Command ID, SET4 ID, address, 16-bit switch state and inner CRC.
inline constexpr std::size_t payload_size = 6;
inline constexpr std::size_t frame_size =
    crsf::CRSF_FRAME_NOT_COUNTED_BYTES + crsf::CRSF_FRAME_LENGTH_EXT_TYPE_CRC + payload_size;
inline constexpr std::size_t frame_crc_index = frame_size - crsf::CRSF_FRAME_CRC_SIZE;
inline constexpr std::size_t command_crc_index = frame_crc_index - crsf::CRSF_FRAME_CRC_SIZE;
inline constexpr std::size_t address_index =
    command_crc_index - sizeof(std::uint16_t) - sizeof(std::uint8_t);
} // namespace set4

using Frame = std::array<std::uint8_t, set4::frame_size>;

[[nodiscard]] constexpr bool enabled(const std::int16_t address) noexcept {
    return (address >= minimum_address) && (address <= maximum_address);
}

[[nodiscard]] constexpr bool enabled(const MultiSwitchPage &page) noexcept {
    return enabled(page.address);
}

[[nodiscard]] constexpr std::size_t count(const Model &model) noexcept {
    std::size_t result{};
    for (const auto &page : model.multiswitch)
        result += enabled(page);
    return result;
}

// Bit 8 * page + button for every enabled page, matching Controls::multiswitch.
[[nodiscard]] constexpr std::uint32_t available(const Model &model) noexcept {
    std::uint32_t result{};
    for (std::size_t i = 0; i < model.multiswitch.size(); ++i)
        if (enabled(model.multiswitch[i]))
            result |= ((std::uint32_t{1} << multiswitch_buttons(model.multiswitch[i].layout)) - 1U)
                      << (i * switch_count);
    return result;
}

[[nodiscard]] constexpr std::optional<std::size_t> slot(const Model &model,
                                                        std::size_t ordinal) noexcept {
    for (std::size_t i = 0; i < model.multiswitch.size(); ++i)
        if (enabled(model.multiswitch[i])) {
            if (ordinal == 0)
                return i;
            --ordinal;
        }
    return std::nullopt;
}

[[nodiscard]] constexpr Frame encode(const std::uint8_t address,
                                     const std::uint8_t switches) noexcept {
    std::uint16_t bits{};
    for (unsigned i = 0; i < switch_count; ++i)
        if ((switches & (1U << i)) != 0)
            bits |= static_cast<std::uint16_t>(1U << (set4::bits_per_switch * i));
    Frame frame{crsf::CRSF_SYNC_BYTE,
                set4::frame_size - crsf::CRSF_FRAME_NOT_COUNTED_BYTES,
                crsf::CRSF_FRAMETYPE_COMMAND,
                crsf::CRSF_ADDRESS_BROADCAST,
                crsf::CRSF_ADDRESS_RADIO_TRANSMITTER,
                set4::command_id,
                set4::subcommand_id,
                address,
                static_cast<std::uint8_t>(bits >> crsf::byte_bits),
                static_cast<std::uint8_t>(bits),
                0,
                0};
    // SET4 protects the command separately from the enclosing CRSF frame.
    std::uint8_t command_crc{};
    for (const auto byte :
         std::span{frame}.subspan(crsf::CRSF_TELEMETRY_TYPE_INDEX,
                                  set4::command_crc_index - crsf::CRSF_TELEMETRY_TYPE_INDEX)) {
        command_crc ^= byte;
        for (unsigned bit = 0; bit < crsf::byte_bits; ++bit)
            command_crc = static_cast<std::uint8_t>(
                (std::uint32_t{command_crc} << 1) ^
                ((command_crc & crsf::crc_top_bit) != 0 ? set4::command_crc_polynomial : 0U));
    }
    frame[set4::command_crc_index] = command_crc;
    frame[set4::frame_crc_index] = crsf::crc8(std::span{frame}.subspan(
        crsf::CRSF_TELEMETRY_TYPE_INDEX, set4::frame_crc_index - crsf::CRSF_TELEMETRY_TYPE_INDEX));
    return frame;
}

class Pages {
  public:
    constexpr void configure(const Model &model) noexcept {
        model_ = &model;
        for (std::size_t i = 0; i < model.multiswitch.size(); ++i) {
            const auto &page = model.multiswitch[i];
            if (addresses_[i] != page.address) {
                manual_[i] = 0;
                states_[i] = 0;
                addresses_[i] = page.address;
            }
            const auto changed = static_cast<std::uint8_t>(momentary_[i] ^ page.momentary);
            manual_[i] &= static_cast<std::uint8_t>(~changed);
            states_[i] &= static_cast<std::uint8_t>(~changed);
            const auto mask =
                static_cast<std::uint8_t>((1U << multiswitch_buttons(page.layout)) - 1U);
            manual_[i] &= mask;
            states_[i] &= mask;
            momentary_[i] = page.momentary;
        }
        refresh_groups();
    }

    constexpr bool update(const std::optional<std::size_t> active,
                          const std::optional<std::size_t> held,
                          const std::optional<std::size_t> tapped,
                          const Controls *const controls = nullptr,
                          const LogicalFrame &logical = {}) noexcept {
        if (model_ == nullptr)
            return false;
        bool changed{};
        for (std::size_t i = 0; i < states_.size(); ++i) {
            const auto &page = model_->multiswitch[i];
            const auto count = multiswitch_buttons(page.layout);
            auto value = static_cast<std::uint8_t>(manual_[i] & ~page.momentary);
            if ((active == i) && enabled(page)) {
                if (held && (*held < count))
                    value |= static_cast<std::uint8_t>((1U << *held) & page.momentary);
                if (tapped && (*tapped < count) && ((page.momentary & (1U << *tapped)) == 0))
                    value ^= static_cast<std::uint8_t>(1U << *tapped);
            }
            manual_[i] = value;
            for (std::size_t button = 0; button < count; ++button) {
                const auto condition = page.controls[button].condition();
                if (!condition.enabled)
                    continue;
                const auto bit = static_cast<std::uint8_t>(1U << button);
                manual_[i] &= static_cast<std::uint8_t>(~bit);
                value &= static_cast<std::uint8_t>(~bit);
                if (enabled(page) && (controls != nullptr) &&
                    coretx::detail::matches(condition, *controls, logical))
                    value |= bit;
            }
            if (!enabled(page))
                value = manual_[i] = 0;
            changed = (states_[i] != value) || changed;
            states_[i] = value;
        }
        refresh_groups();
        return changed;
    }

    [[nodiscard]] constexpr std::optional<std::size_t> due(const TimestampUs now) const noexcept {
        if (valid_ == 0)
            return std::nullopt;
        for (const auto priority : {Priority::release, Priority::change, Priority::repeat}) {
            for (std::size_t i = 0; i < groups_.size(); ++i) {
                const auto index = (next_ + i) % groups_.size();
                const auto bit = std::uint32_t{1} << index;
                if ((valid_ & bit) == 0)
                    continue;
                if (priority == Priority::release) {
                    if ((pending_ & releasing_ & bit) != 0)
                        return index;
                } else if (priority == Priority::change) {
                    if ((pending_ & bit) != 0)
                        return index;
                } else {
                    const auto elapsed = (now.count() - groups_[index].sent_at()) & timestamp_mask;
                    if (elapsed >= repeat_interval.count())
                        return index;
                }
            }
        }
        return std::nullopt;
    }
    [[nodiscard]] constexpr Frame frame(const std::size_t index) const noexcept {
        return encode(groups_[index].address, groups_[index].switches);
    }
    constexpr void sent(const std::size_t index, const TimestampUs now) noexcept {
        const auto bit = std::uint32_t{1} << index;
        groups_[index].sent_at(now.count());
        pending_ &= ~bit;
        releasing_ &= ~bit;
        if (groups_[index].switches == 0)
            sent_on_ &= ~bit;
        else
            sent_on_ |= bit;
        if ((present_ & bit) == 0)
            valid_ &= ~bit;
        next_ = static_cast<std::uint8_t>((index + 1) % groups_.size());
    }
    [[nodiscard]] constexpr std::uint8_t switches(const std::size_t index) const noexcept {
        return states_[index];
    }
    [[nodiscard]] constexpr std::uint32_t states() const noexcept {
        std::uint32_t result{};
        for (std::size_t i = 0; i < states_.size(); ++i)
            result |= std::uint32_t{states_[i]} << (i * switch_count);
        return result;
    }
    [[nodiscard]] constexpr std::uint32_t available() const noexcept {
        return model_ != nullptr ? multiswitch::available(*model_) : 0;
    }

  private:
    enum class Priority : std::uint8_t { release, change, repeat };

    constexpr void refresh_groups() noexcept {
        if (model_ == nullptr)
            return;
        std::array<std::uint8_t, maximum_addresses> desired{};
        present_ = 0;
        for (std::size_t page_index = 0; page_index < model_->multiswitch.size(); ++page_index) {
            const auto &page = model_->multiswitch[page_index];
            if (!enabled(page))
                continue;
            for (std::size_t button = 0; button < multiswitch_buttons(page.layout); ++button) {
                const auto &control = page.controls[button];
                const auto address =
                    static_cast<std::uint8_t>(control.target_address(page.address));
                std::optional<std::size_t> index{};
                for (std::size_t i = 0; i < groups_.size(); ++i)
                    if (((valid_ & (std::uint32_t{1} << i)) != 0) &&
                        (groups_[i].address == address)) {
                        index = i;
                        break;
                    }
                if (!index) {
                    const auto preferred =
                        address == page.address ? page_index : multiswitch_page_count;
                    for (std::size_t i = 0; i < groups_.size(); ++i) {
                        const auto candidate = (preferred + i) % groups_.size();
                        const auto bit = std::uint32_t{1} << candidate;
                        if ((valid_ & bit) == 0) {
                            index = candidate;
                            groups_[candidate] = {};
                            groups_[candidate].address = address;
                            valid_ |= bit;
                            pending_ |= bit;
                            sent_on_ &= ~bit;
                            break;
                        }
                    }
                }
                // A retiring address occupies its slot until its final OFF packet starts.
                if (!index)
                    continue;
                present_ |= std::uint32_t{1} << *index;
                if ((states_[page_index] & (1U << button)) != 0)
                    desired[*index] |= static_cast<std::uint8_t>(1U << control.output());
            }
        }
        for (std::size_t i = 0; i < groups_.size(); ++i) {
            const auto bit = std::uint32_t{1} << i;
            if ((valid_ & bit) == 0)
                continue;
            if (((present_ & bit) == 0) && ((sent_on_ & bit) == 0)) {
                valid_ &= ~bit;
                pending_ &= ~bit;
                releasing_ &= ~bit;
                continue;
            }
            if ((groups_[i].switches & ~desired[i]) != 0)
                releasing_ |= bit;
            if (groups_[i].switches != desired[i]) {
                groups_[i].switches = desired[i];
                pending_ |= bit;
            }
        }
    }
    struct Group {
        std::array<std::uint8_t, timestamp_bits / crsf::byte_bits> timestamp{};
        std::uint8_t address{};
        std::uint8_t switches{};
        [[nodiscard]] constexpr std::uint32_t sent_at() const noexcept {
            std::uint32_t value{};
            for (std::size_t i = 0; i < timestamp.size(); ++i)
                value |= std::uint32_t{timestamp[i]} << (i * crsf::byte_bits);
            return value;
        }
        constexpr void sent_at(const std::uint32_t value) noexcept {
            for (std::size_t i = 0; i < timestamp.size(); ++i)
                timestamp[i] = static_cast<std::uint8_t>(value >> (i * crsf::byte_bits));
        }
    };
    const Model *model_{};
    std::array<Group, maximum_addresses> groups_{};
    std::array<std::uint8_t, multiswitch_page_count> states_{};
    std::array<std::uint8_t, multiswitch_page_count> manual_{};
    std::array<std::uint8_t, multiswitch_page_count> momentary_{};
    std::array<std::int16_t, multiswitch_page_count> addresses_{disabled_address, disabled_address,
                                                                disabled_address, disabled_address};
    std::uint32_t valid_{};
    std::uint32_t present_{};
    std::uint32_t pending_{};
    std::uint32_t releasing_{};
    std::uint32_t sent_on_{};
    std::uint8_t next_{};
};

} // namespace coretx::multiswitch
