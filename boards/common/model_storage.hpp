// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/common/controls.hpp"
#include "boards/common/radio_storage.hpp"
#include "boards/common/slot_transfer.hpp"
#include "coretx/ui/translation.hpp"
#include "platform/stm32f072/crsf.hpp"

#include <memory>

namespace coretx::firmware {

namespace models {
inline constexpr std::size_t count = 8;
inline constexpr std::size_t record_size = 768;
inline constexpr std::uint8_t format_version = coretx::storage::format_version;
inline constexpr std::uint16_t base = 2304;
inline constexpr std::uint16_t directory_base = base + count * 2 * record_size;
inline constexpr std::uint32_t magic = 0x314C444D;           // MDL1.
inline constexpr std::uint32_t directory_magic = 0x31524944; // DIR1.
static_assert((directory_base + 128) <= 16'384);
// Packed lines keep trim settings and per-function controls in the existing journals.
static_assert((34 + 2 + (max_input_lines * coretx::storage::detail::packed_model::input_bytes) +
               (max_mix_lines * coretx::storage::detail::packed_model::mix_bytes) +
               (channel_count * coretx::storage::detail::packed_model::output_bytes) + 8 +
               (logical_switch_count * 7) + (multiswitch_page_count * 51) + 1 +
               (multiswitch_page_count * multiswitch_function_count * sizeof(MultiSwitchControl)) +
               trim_count + 1) <= (record_size - 4));
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
        !coretx::storage::detail::transfer_model_lines(out, data.model) ||
        !coretx::storage::detail::transfer_model_extension(out, data.model) ||
        !coretx::storage::detail::transfer_multiswitch(out, data.model) ||
        !coretx::storage::detail::transfer_dashboard_options(out, data.model) ||
        !coretx::storage::detail::transfer_multiswitch_controls(out, data.model))
        return false;
    coretx::storage::detail::Writer crc{std::span{record}.last(4)};
    crc.word(coretx::storage::crc32(std::span{record}.first(record_size - 4)));
    return out.good && (out.position <= (record_size - 4));
}

// 0: erased, 1: valid, 2: damaged, 3: incompatible version.
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
        return 2;
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
        !coretx::storage::detail::transfer_model_lines(in, data.model) ||
        !coretx::storage::detail::transfer_model_extension(in, data.model) ||
        !coretx::storage::detail::transfer_multiswitch(in, data.model) ||
        !coretx::storage::detail::transfer_dashboard_options(in, data.model, version >= 7) ||
        !coretx::storage::detail::transfer_multiswitch_controls(in, data.model) ||
        !valid_data(data, transmitter) ||
        !std::all_of(record.begin() + static_cast<std::ptrdiff_t>(in.position), record.end() - 4,
                     [](auto b) { return b == 0xFF; }))
        return 2;
    info.name = data.name;
    info.checksum = checksum;
    info.valid = true;
    return 1;
}
} // namespace models

template <typename Memory> class ModelStorage {
    static_assert(Memory::page_size == 64);

  public:
    constexpr void initialize(const afhds2a::TxId transmitter) noexcept {
        stage_ = Stage::wait;
        transfer_ = {};
        loaded_ = writable_ = dirty_ = changed_ = switch_after_ = false;
        trim_save_pending_ = false;
        directory_valid_ = false;
        resettable_ = directory_damaged_ = slot_damaged_ = false;
        directory_empty_ = scan_empty_ = slot_empty_ = true;
        active_ = target_ = copy_target_ = 0;
        bank_ = directory_bank_ = 0;
        directory_generation_ = 0;
        entries_.fill({});
        message_ = coretx::ui::tr::models_loading;
        make_diagnostic_model(data_.model);
        data_.radio = {};
        data_.baud_index = 1;
        models::default_name(data_.name, 0);
        transmitter_ = transmitter;
        data_.radio.transmitter = transmitter;
    }
    [[nodiscard]] constexpr models::Data &data() noexcept { return data_; }
    // Scratch is unused before the first service and serves as calibration decode area.
    [[nodiscard]] constexpr Model &legacy_workspace() noexcept { return scratch_.model; }
    [[nodiscard]] constexpr const models::Data &data() const noexcept { return data_; }
    [[nodiscard]] constexpr const auto &entries() const noexcept { return entries_; }
    [[nodiscard]] constexpr std::size_t active() const noexcept { return active_; }
    [[nodiscard]] constexpr bool loaded() const noexcept { return loaded_; }
    [[nodiscard]] constexpr bool editable() const noexcept { return loaded_ && writable_; }
    [[nodiscard]] constexpr bool can_initialize() const noexcept { return resettable_; }
    constexpr bool initialize_models(const Model &model, const RadioSettings radio) noexcept {
        if (!resettable_ || busy())
            return false;
        if (!coretx::validate(model)) {
            message_ = coretx::ui::tr::models_error;
            return false;
        }
        data_.model = model;
        data_.radio = radio;
        data_.radio.output = RadioOutput::off;
        data_.baud_index = static_cast<std::uint8_t>(crsf_default_baudrate_index);
        models::default_name(data_.name, 0);
        resettable_ = false;
        reset_index_ = 0;
        stage_ = Stage::reset;
        message_ = coretx::ui::tr::models_saving;
        return true;
    }
    [[nodiscard]] constexpr bool busy() const noexcept {
        return (stage_ != Stage::ready) || dirty_;
    }
    [[nodiscard]] constexpr bool background_loading() const noexcept {
        return loaded_ && (stage_ == Stage::background_scan);
    }
    [[nodiscard]] constexpr bool owns_memory() const noexcept {
        return (stage_ != Stage::wait) && (stage_ != Stage::ready);
    }
    [[nodiscard]] constexpr const char *message() const noexcept { return message_; }
    [[nodiscard]] constexpr bool take_changed() noexcept {
        const bool changed = changed_;
        changed_ = false;
        return changed;
    }
    constexpr bool save() noexcept {
        if (!loaded_ || !writable_)
            return false;
        dirty_ = true;
        trim_save_pending_ = false;
        return true;
    }
    inline static constexpr DurationUs trim_save_delay{500'000};
    constexpr bool save_trims(const TimestampUs now) noexcept {
        if (!save())
            return false;
        trim_save_pending_ = true;
        trim_changed_at_ = now;
        message_ = coretx::ui::tr::models_saving;
        return true;
    }
    constexpr bool select(const std::size_t index) noexcept {
        if (!available())
            return false;
        if ((index >= models::count) || !entries_[index].present) {
            message_ = coretx::ui::tr::models_none;
            return false;
        }
        if (index == active_)
            return true;
        target_ = index;
        bank_ = entries_[index].bank;
        stage_ = Stage::select;
        message_ = coretx::ui::tr::models_loading_one;
        return true;
    }
    constexpr bool create(const bool copy, const std::size_t source = models::count) noexcept {
        if (!available())
            return false;
        if (source > models::count) {
            message_ = coretx::ui::tr::models_none;
            return false;
        }
        std::size_t index =
            !copy && (source < models::count) && !entries_[source].present ? source : 0;
        while ((index < models::count) && entries_[index].present)
            ++index;
        if (index == models::count) {
            message_ = coretx::ui::tr::models_full;
            return false;
        }
        if (copy && (source < models::count) && (source != active_)) {
            if (!entries_[source].present) {
                message_ = coretx::ui::tr::models_none;
                return false;
            }
            copy_target_ = index;
            target_ = source;
            bank_ = entries_[source].bank;
            stage_ = Stage::copy;
            return true;
        }
        scratch_ = data_;
        if (!copy) {
            models::four_channel_model(scratch_.model);
            scratch_.radio = {transmitter_, {}, RadioOutput::off, afhds2a::ChannelMode::extended16};
            scratch_.baud_index = 1;
        }
        models::default_name(scratch_.name, index);
        return begin_save(index, true, true);
    }
    constexpr bool rename(const std::size_t index, const models::Name &name) noexcept {
        if (!available())
            return false;
        if ((index >= models::count) || !entries_[index].present) {
            message_ = coretx::ui::tr::models_none;
            return false;
        }
        if (index == active_) {
            const auto old = data_.name;
            data_.name = name;
            if (!models::valid_data(data_, transmitter_)) {
                data_.name = old;
                message_ = coretx::ui::tr::models_invalid_name;
                return false;
            }
            return save();
        }
        pending_name_ = name;
        target_ = index;
        bank_ = entries_[index].bank;
        stage_ = Stage::rename;
        return true;
    }
    constexpr bool erase(const std::size_t index) noexcept {
        if (!available())
            return false;
        if ((index >= models::count) || !entries_[index].present) {
            message_ = coretx::ui::tr::models_none;
            return false;
        }
        if (index == active_) {
            message_ = coretx::ui::tr::models_active_delete;
            return false;
        }
        scratch_ = data_;
        models::default_name(scratch_.name, index);
        return begin_save(index, false, false);
    }
    constexpr void service(const TimestampUs now, const StorageProbeResult probe,
                           const Model &legacy, const RadioSettings radio) noexcept {
        if (stage_ == Stage::wait) {
            const auto selection = select_memory<Memory>(probe);
            if (selection == ProbeSelection::wait)
                return;
            if (selection != ProbeSelection::selected) {
                finish(coretx::ui::tr::models_unique_eeprom, false);
                return;
            }
            if (!coretx::validate(legacy)) {
                finish(coretx::ui::tr::models_legacy_invalid, false);
                return;
            }
            if (&legacy != &data_.model)
                data_.model = legacy;
            data_.radio = radio;
            record_.fill(0xFF);
            stage_ = Stage::directory_load;
        }
        if (stage_ == Stage::ready) {
            if (!dirty_ || !writable_)
                return;
            if (trim_save_pending_ && ((now - trim_changed_at_) < trim_save_delay))
                return;
            trim_save_pending_ = false;
            dirty_ = false;
            scratch_ = data_;
            if (!begin_save(active_, true, false))
                return;
        }
        for (;;) {
            if (!transfer_.active() && !start())
                return;
            switch (transfer_.service(now)) {
            case Transfer::Result::busy:
                return;
            case Transfer::Result::failed:
                finish(Memory::error() == I2cError::none ? coretx::ui::tr::bad_request
                                                         : error_message(Memory::error()),
                       false);
                return;
            case Transfer::Result::mismatch:
                finish(coretx::ui::tr::models_error, false);
                return;
            case Transfer::Result::done:
                break;
            }
            if (!complete())
                return;
        }
    }

  private:
    enum class Stage : std::uint8_t {
        wait,
        directory_load,
        scan,
        active_scan,
        background_scan,
        ready,
        select,
        copy,
        rename,
        reset,
        write,
        directory
    };
    using Transfer = SlotTransfer<Memory>;
    [[nodiscard]] constexpr std::uint16_t record_address() const noexcept {
        return static_cast<std::uint16_t>(models::base +
                                          (target_ * 2 + bank_) * models::record_size);
    }
    [[nodiscard]] constexpr std::span<std::uint8_t> directory_page() noexcept {
        return std::span{record_}.first(Memory::page_size);
    }
    [[nodiscard]] constexpr std::uint16_t directory_address() const noexcept {
        return static_cast<std::uint16_t>(models::directory_base + bank_ * Memory::page_size);
    }
    // Starts the EEPROM transfer of the current stage; false when the stage has none.
    constexpr bool start() noexcept {
        switch (stage_) {
        case Stage::directory_load:
            transfer_.read(directory_address(), directory_page());
            return true;
        case Stage::scan:
        case Stage::active_scan:
        case Stage::background_scan:
        case Stage::select:
        case Stage::copy:
        case Stage::rename:
            transfer_.read(record_address(), record_);
            return true;
        case Stage::write:
            // The first page holds the header; writing it last commits the record.
            transfer_.write(record_address(), record_, Memory::page_size);
            return true;
        case Stage::directory:
            transfer_.write(directory_address(), directory_page(), Memory::page_size);
            return true;
        case Stage::reset: {
            // Reset markers make every slot and directory bank explicitly invalid.
            const bool record = reset_index_ < (models::count * 2);
            const auto address = static_cast<std::uint16_t>(
                record ? models::base + reset_index_ * models::record_size
                       : models::directory_base +
                             (reset_index_ - models::count * 2) * Memory::page_size);
            const std::array<std::uint8_t, 4> marker{
                0, static_cast<std::uint8_t>(record ? 'D' : 'I'),
                static_cast<std::uint8_t>(record ? 'L' : 'R'), '1'};
            std::copy(marker.begin(), marker.end(), record_.begin());
            transfer_.write(address, std::span{record_}.first(marker.size()), marker.size(), false);
            return true;
        }
        case Stage::wait:
        case Stage::ready:
            break;
        }
        return false;
    }
    // Handles a completed transfer; true when the next stage needs another transfer.
    [[nodiscard]] constexpr bool complete() noexcept {
        switch (stage_) {
        case Stage::reset:
            if (++reset_index_ != ((models::count * 2) + 2))
                return true;
            entries_.fill({});
            active_ = 0;
            directory_bank_ = 0;
            directory_generation_ = 0;
            directory_damaged_ = slot_damaged_ = false;
            writable_ = true;
            scratch_ = data_;
            return begin_save(0, true, true);
        case Stage::directory_load:
            return load_directory();
        case Stage::scan:
        case Stage::active_scan:
        case Stage::background_scan:
            return scan_record();
        case Stage::select:
        case Stage::copy:
        case Stage::rename: {
            models::Info info{};
            if ((models::decode(record_, target_, transmitter_, scratch_, info) != 1) ||
                !info.present || (info.checksum != entries_[target_].checksum)) {
                finish(coretx::ui::tr::models_error, false);
                return false;
            }
            if (stage_ == Stage::copy) {
                models::default_name(scratch_.name, copy_target_);
                return begin_save(copy_target_, true, true);
            }
            if (stage_ == Stage::rename) {
                scratch_.name = pending_name_;
                return begin_save(target_, true, false);
            }
            switch_after_ = true;
            start_directory();
            return true;
        }
        case Stage::write: {
            models::Info info{};
            if (models::decode(record_, target_, transmitter_, scratch_, info) != 1) {
                finish(coretx::ui::tr::models_error, false);
                return false;
            }
            info.bank = bank_;
            entries_[target_] = info;
            if (switch_after_) {
                start_directory();
                return true;
            }
            finish(coretx::ui::tr::models_saved, true);
            return false;
        }
        case Stage::directory:
            directory_bank_ = bank_;
            ++directory_generation_;
            if (switch_after_) {
                data_ = scratch_;
                active_ = target_;
                changed_ = true;
            }
            switch_after_ = false;
            finish(coretx::ui::tr::models_saved, true);
            return false;
        case Stage::wait:
        case Stage::ready:
            break;
        }
        return false;
    }
    [[nodiscard]] constexpr bool scanning() const noexcept {
        return (stage_ == Stage::scan) || (stage_ == Stage::active_scan) ||
               (stage_ == Stage::background_scan);
    }
    constexpr bool available() noexcept {
        if (!loaded_ || !writable_)
            return false;
        if (busy() || dirty_) {
            message_ = coretx::ui::tr::models_busy;
            return false;
        }
        return true;
    }
    constexpr void finish(const char *message, const bool writable) noexcept {
        message_ = message;
        writable_ = writable;
        stage_ = Stage::ready;
        transfer_ = {};
        loaded_ = true;
        if (!writable) {
            dirty_ = false;
            switch_after_ = false;
        }
    }
    constexpr bool begin_save(const std::size_t target, const bool present,
                              const bool switch_after) noexcept {
        target_ = target;
        bank_ = entries_[target].valid ? static_cast<std::uint8_t>(1U - entries_[target].bank) : 0;
        const auto generation = entries_[target].valid ? entries_[target].generation + 1U : 0U;
        if (!models::encode(scratch_, target, present, generation, record_)) {
            finish(coretx::ui::tr::models_error, false);
            return false;
        }
        switch_after_ = switch_after;
        stage_ = Stage::write;
        message_ = coretx::ui::tr::models_saving;
        return true;
    }
    constexpr void start_directory() noexcept {
        bank_ = static_cast<std::uint8_t>(1U - directory_bank_);
        record_.fill(0xFF);
        coretx::storage::detail::Writer out{std::span{record_}.first(Memory::page_size)};
        out.word(models::directory_magic);
        out.field(std::uint8_t{1});
        out.field(static_cast<std::uint8_t>(target_));
        out.word(directory_generation_ + 1U);
        coretx::storage::detail::Writer crc{std::span{record_}.subspan(Memory::page_size - 4, 4)};
        crc.word(coretx::storage::crc32(std::span{record_}.first(Memory::page_size - 4)));
        stage_ = Stage::directory;
    }
    constexpr bool load_directory() noexcept {
        const auto bytes = std::span{record_}.first(Memory::page_size);
        const bool empty =
            std::all_of(bytes.begin(), bytes.end(), [](auto b) { return b == 0xFF; }) ||
            ((bytes[0] == 0) && (((bytes[1] == 'I') && (bytes[2] == 'R') && (bytes[3] == '1')) ||
                                 ((bytes[1] == 0xFF) && (bytes[2] == 0xFF) && (bytes[3] == 0xFF))));
        coretx::storage::detail::Reader in{bytes};
        coretx::storage::detail::Reader crc{bytes.last(4)};
        std::uint32_t magic{}, generation{}, checksum{};
        std::uint8_t version{}, index{};
        in.word(magic);
        in.field(version);
        in.field(index);
        in.word(generation);
        crc.word(checksum);
        const bool recognized =
            !empty && (magic == models::directory_magic) &&
            (checksum == coretx::storage::crc32(bytes.first(Memory::page_size - 4)));
        if (recognized && (version != 1)) {
            finish(coretx::ui::tr::models_directory_version, false);
            return false;
        }
        const bool valid =
            recognized && (index < models::count) &&
            std::all_of(bytes.begin() + 10, bytes.end() - 4, [](auto b) { return b == 0xFF; });
        if (valid) {
            if (directory_valid_) {
                const auto delta = generation - directory_generation_;
                if ((delta == 0x80000000U) || ((delta == 0) && (index != active_))) {
                    finish(coretx::ui::tr::models_directory_ambiguous, false);
                    return false;
                }
                if ((delta != 0) && (delta < 0x80000000U)) {
                    active_ = index;
                    directory_generation_ = generation;
                    directory_bank_ = bank_;
                }
            } else {
                active_ = index;
                directory_generation_ = generation;
                directory_bank_ = bank_;
            }
            directory_valid_ = true;
        }
        directory_empty_ = directory_empty_ && empty;
        if (bank_ == 0) {
            bank_ = 1;
            return true;
        }
        if (!directory_valid_ && !directory_empty_) {
            directory_damaged_ = true;
        }
        bank_ = 0;
        target_ = directory_damaged_ ? 0 : active_;
        stage_ = directory_damaged_ ? Stage::scan : Stage::active_scan;
        return true;
    }
    constexpr bool scan_record() noexcept {
        models::Info info{};
        const auto status = models::decode(record_, target_, transmitter_, scratch_, info);
        if (status == 3) {
            finish(coretx::ui::tr::models_version, false);
            return false;
        }
        scan_empty_ = scan_empty_ && (status == 0);
        slot_empty_ = slot_empty_ && (status == 0);
        auto &entry = entries_[target_];
        bool chosen = (status == 1) && !entry.valid;
        if ((status == 1) && entry.valid) {
            const auto delta = info.generation - entry.generation;
            if ((delta == 0x80000000U) || ((delta == 0) && (info.checksum != entry.checksum))) {
                finish(coretx::ui::tr::models_ambiguous, false);
                return false;
            }
            chosen = (delta != 0) && (delta < 0x80000000U);
        }
        if (chosen) {
            info.bank = bank_;
            entry = info;
            if ((target_ == active_) && info.present)
                data_ = scratch_;
        }
        if (bank_ == 0) {
            bank_ = 1;
            return true;
        }
        if (!entry.valid && !slot_empty_) {
            slot_damaged_ = true;
        }
        slot_empty_ = true;
        bank_ = 0;
        if (stage_ == Stage::active_scan) {
            // Resolve both active banks before exposing a model to the RC pipeline.
            if (entry.valid && entry.present) {
                loaded_ = writable_ = changed_ = true;
            }
            stage_ = Stage::background_scan;
            target_ = active_ == 0 ? 1 : 0;
            return true;
        }
        ++target_;
        if ((stage_ == Stage::background_scan) && (target_ == active_))
            ++target_;
        if (target_ != models::count)
            return true;
        if (directory_damaged_ || slot_damaged_) {
            resettable_ = true;
            finish(directory_damaged_ ? coretx::ui::tr::models_directory_damaged
                                      : coretx::ui::tr::models_error,
                   false);
            return false;
        }
        if (scan_empty_ && directory_empty_) {
            writable_ = loaded_ = true;
            scratch_ = data_;
            models::default_name(scratch_.name, 0);
            return begin_save(0, true, true);
        }
        if (!entries_[active_].valid || !entries_[active_].present) {
            resettable_ = true;
            finish(coretx::ui::tr::models_active_missing, false);
            return false;
        }
        if (!loaded_)
            changed_ = true;
        finish(coretx::ui::tr::models_loaded, true);
        return false;
    }
    models::Data data_{models::empty_model};
    models::Data scratch_{models::empty_model};
    models::Name pending_name_{};
    models::Record record_{};
    std::array<models::Info, models::count> entries_{};
    Transfer transfer_{};
    afhds2a::TxId transmitter_{};
    const char *message_{coretx::ui::tr::models_loading};
    std::uint32_t directory_generation_{};
    std::size_t active_{}, target_{}, copy_target_{};
    std::size_t reset_index_{};
    std::uint8_t bank_{}, directory_bank_{};
    Stage stage_{Stage::wait};
    TimestampUs trim_changed_at_{};
    bool trim_save_pending_{};
    bool loaded_{}, writable_{}, dirty_{}, changed_{}, switch_after_{};
    bool directory_valid_{}, directory_empty_{true}, scan_empty_{true};
    bool slot_empty_{true};
    bool resettable_{}, directory_damaged_{}, slot_damaged_{};
};

} // namespace coretx::firmware
