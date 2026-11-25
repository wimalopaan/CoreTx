// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/common/model_codec.hpp"
#if CORETX_STORAGE_CONVERTER
#include "boards/common/model_converter.hpp"
#endif
#include "boards/common/model_migration.hpp"
#include "boards/common/radio_settings.hpp"
#include "boards/common/slot_transfer.hpp"
#include "coretx/ui/translation.hpp"
#include "platform/stm32f072/crsf.hpp"

#include <memory>
#include <optional>

namespace coretx::firmware {

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
        resetting_ = false;
#if CORETX_STORAGE_CONVERTER
        conversion_available_ = false;
#endif
        detected_version_.reset();
        expected_version_ = models::format_version;
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
    [[nodiscard]] constexpr const models::Data &data() const noexcept { return data_; }
    [[nodiscard]] constexpr const auto &entries() const noexcept { return entries_; }
    [[nodiscard]] constexpr std::size_t active() const noexcept { return active_; }
    [[nodiscard]] constexpr bool loaded() const noexcept { return loaded_; }
    [[nodiscard]] constexpr bool editable() const noexcept { return loaded_ && writable_; }
    [[nodiscard]] constexpr bool can_initialize() const noexcept { return resettable_; }
    [[nodiscard]] constexpr bool recovery_required() const noexcept { return resettable_; }
    [[nodiscard]] constexpr bool can_convert() const noexcept {
#if CORETX_STORAGE_CONVERTER
        return resettable_ && conversion_available_ && !busy();
#else
        return false;
#endif
    }
    bool convert_models() noexcept {
#if CORETX_STORAGE_CONVERTER
        if (!can_convert() || !conversion_.confirm())
            return false;
        conversion_available_ = resettable_ = loaded_ = writable_ = false;
        dirty_ = trim_save_pending_ = false;
        stage_ = Stage::converting;
        message_ = coretx::ui::tr::models_converting;
        return true;
#else
        return false;
#endif
    }
    [[nodiscard]] constexpr std::optional<std::uint8_t> detected_version() const noexcept {
        return detected_version_;
    }
    [[nodiscard]] constexpr std::uint8_t expected_version() const noexcept {
        return expected_version_;
    }
    constexpr bool initialize_models() noexcept {
        if (!resettable_ || busy())
            return false;
        models::four_channel_model(data_.model);
        data_.radio = {transmitter_, {}, RadioOutput::off, afhds2a::ChannelMode::extended16};
        data_.baud_index = static_cast<std::uint8_t>(crsf_default_baudrate_index);
        models::default_name(data_.name, 0);
        resettable_ = false;
#if CORETX_STORAGE_CONVERTER
        conversion_available_ = false;
#endif
        resetting_ = true;
        loaded_ = writable_ = changed_ = false;
        dirty_ = trim_save_pending_ = false;
        reset_index_ = 0;
        model_migration::Record marker{};
        model_migration::encode(model_migration::Kind::reset, models::format_version, marker);
        std::copy(marker.begin(), marker.end(), record_.begin());
        stage_ = Stage::reset_marker;
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
                           const Model &initial, const RadioSettings radio) noexcept {
#if CORETX_STORAGE_CONVERTER
        if ((stage_ == Stage::conversion_check) || (stage_ == Stage::converting)) {
            conversion_.service(now);
            using State = ModelConversion<Memory>::State;
            if (conversion_.state() == State::preview) {
                conversion_available_ = resettable_ = true;
                finish(coretx::ui::tr::models_version, false);
            } else if (conversion_.state() == State::failed) {
                conversion_available_ = false;
                resettable_ = true;
                finish(conversion_.error(), false);
            } else if (conversion_.state() == State::complete)
                initialize(transmitter_);
            return;
        }
#endif
        if (stage_ == Stage::wait) {
            const auto selection = select_memory<Memory>(probe);
            if (selection == ProbeSelection::wait)
                return;
            if (selection != ProbeSelection::selected) {
                finish(coretx::ui::tr::models_unique_eeprom, false);
                return;
            }
            if (!coretx::validate(initial)) {
                finish(coretx::ui::tr::models_legacy_invalid, false);
                return;
            }
            if (&initial != &data_.model)
                data_.model = initial;
            data_.radio = radio;
            record_.fill(0xFF);
            stage_ = Stage::migration_load;
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
#if CORETX_STORAGE_CONVERTER
        conversion_check,
        converting,
#endif
        wait,
        migration_load,
        directory_load,
        scan,
        active_scan,
        background_scan,
        ready,
        select,
        copy,
        rename,
        reset_marker,
        reset,
        reset_complete,
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
        case Stage::migration_load:
            transfer_.read(model_migration::address, directory_page());
            return true;
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
        case Stage::reset_marker:
        case Stage::reset_complete:
            transfer_.write(model_migration::address, directory_page(), Memory::page_size, false);
            return true;
        case Stage::reset: {
            // Erase the entire model area, retaining the reset marker until the new model commits.
            const auto address =
                static_cast<std::uint16_t>(models::base + reset_index_ * Memory::page_size);
            record_.fill(0xFF);
            transfer_.write(address, directory_page(), Memory::page_size, false);
            return true;
        }
        case Stage::wait:
        case Stage::ready:
#if CORETX_STORAGE_CONVERTER
        case Stage::conversion_check:
        case Stage::converting:
#endif
            break;
        }
        return false;
    }
    // Handles a completed transfer; true when the next stage needs another transfer.
    [[nodiscard]] constexpr bool complete() noexcept {
        switch (stage_) {
        case Stage::migration_load:
            if (!std::all_of(record_.begin(), record_.begin() + Memory::page_size,
                             [](const auto b) { return b == 0xFF; })) {
                resettable_ = true;
                finish(coretx::ui::tr::models_migration, false);
                return false;
            }
            stage_ = Stage::directory_load;
            return true;
        case Stage::reset_marker:
            stage_ = Stage::reset;
            return true;
        case Stage::reset_complete:
            resetting_ = false;
            changed_ = true;
            detected_version_.reset();
            expected_version_ = models::format_version;
            finish(coretx::ui::tr::models_saved, true);
            return false;
        case Stage::reset:
            ++reset_index_;
            if ((models::base + reset_index_ * Memory::page_size) == model_migration::address)
                ++reset_index_;
            if ((models::base + reset_index_ * Memory::page_size) < models::memory_size)
                return true;
            entries_.fill({});
            active_ = 0;
            directory_bank_ = 0;
            directory_generation_ = 0;
            directory_damaged_ = slot_damaged_ = false;
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
                changed_ = !resetting_;
            }
            switch_after_ = false;
            if (resetting_) {
                record_.fill(0xFF);
                stage_ = Stage::reset_complete;
                return true;
            }
            finish(coretx::ui::tr::models_saved, true);
            return false;
        case Stage::wait:
        case Stage::ready:
#if CORETX_STORAGE_CONVERTER
        case Stage::conversion_check:
        case Stage::converting:
#endif
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
            if (resetting_)
                resettable_ = true;
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
            resettable_ = true;
            detected_version_ = version;
            expected_version_ = 1;
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
                    resettable_ = true;
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
            detected_version_ = record_[4];
            resettable_ = true;
            finish(coretx::ui::tr::models_version, false);
            return false;
        }
        if ((status == 1) && (record_[4] != models::format_version)) {
            detected_version_ = record_[4];
#if CORETX_STORAGE_CONVERTER
            stage_ = Stage::conversion_check;
            message_ = coretx::ui::tr::models_loading;
            conversion_.initialize(transmitter_);
#else
            resettable_ = true;
            finish(coretx::ui::tr::models_version, false);
#endif
            return false;
        }
        if (status == 4) {
            resettable_ = true;
            finish(coretx::ui::tr::models_module_unavailable, false);
            return false;
        }
        scan_empty_ = scan_empty_ && (status == 0);
        slot_empty_ = slot_empty_ && (status == 0);
        auto &entry = entries_[target_];
        bool chosen = (status == 1) && !entry.valid;
        if ((status == 1) && entry.valid) {
            const auto delta = info.generation - entry.generation;
            if ((delta == 0x80000000U) || ((delta == 0) && (info.checksum != entry.checksum))) {
                resettable_ = true;
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
            // No model is exposed until every slot and both banks have been checked.
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
#if CORETX_STORAGE_CONVERTER
    ModelConversion<Memory> conversion_{record_, scratch_};
#endif
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
    bool resetting_{};
#if CORETX_STORAGE_CONVERTER
    bool conversion_available_{};
#endif
    std::optional<std::uint8_t> detected_version_{};
    std::uint8_t expected_version_{models::format_version};
};

} // namespace coretx::firmware
