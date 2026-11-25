// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/common/model_codec.hpp"
#include "boards/common/model_migration.hpp"
#include "boards/common/slot_transfer.hpp"

namespace coretx::firmware {

// Updating the native format requires explicit converter/fixture review.
inline constexpr std::uint8_t converter_target = 11;
static_assert(converter_target == storage::format_version,
              "Update the converter and its independent version fixtures");
static_assert(storage::oldest_format_version == 6,
              "Review supported source versions and converter fixtures");

template <typename Memory> class ModelConversion {
    static_assert(Memory::page_size == 64);
    using Transfer = SlotTransfer<Memory>;
    enum class Stage : std::uint8_t {
        marker,
        directory,
        scan,
        preview,
        mark,
        load,
        write,
        retire,
        clear,
        done,
        failed
    };
    struct Bank {
        models::Info info{};
        int status{};
        std::uint8_t version{};
        std::uint32_t raw_generation{};
    };

  public:
    constexpr ModelConversion(models::Record &record, models::Data &data) noexcept
        : record_{record}, data_{data} {}
    ModelConversion(const ModelConversion &) = delete;
    ModelConversion &operator=(const ModelConversion &) = delete;
    enum class State : std::uint8_t { reading, preview, converting, complete, failed };
    void initialize(const afhds2a::TxId transmitter) noexcept {
        auto &record = record_;
        auto &data = data_;
        std::destroy_at(this);
        std::construct_at(this, record, data);
        transmitter_ = transmitter;
        transfer_.read(model_migration::address, marker_);
    }
    [[nodiscard]] State state() const noexcept {
        if (stage_ == Stage::preview)
            return State::preview;
        if (stage_ == Stage::done)
            return State::complete;
        if (stage_ == Stage::failed)
            return State::failed;
        return writing_ ? State::converting : State::reading;
    }
    [[nodiscard]] const char *error() const noexcept { return error_; }
    [[nodiscard]] unsigned model_count() const noexcept { return model_count_; }
    [[nodiscard]] std::uint16_t versions() const noexcept { return versions_; }
    [[nodiscard]] bool resuming() const noexcept { return resume_; }
    [[nodiscard]] bool repairing_marker() const noexcept { return repair_marker_; }
    bool confirm() noexcept {
        if (stage_ != Stage::preview)
            return false;
        writing_ = true;
        slot_ = 0;
        if (resume_)
            next_slot();
        else {
            model_migration::encode(model_migration::Kind::conversion, converter_target, marker_);
            stage_ = Stage::mark;
            transfer_.write(model_migration::address, marker_, Memory::page_size, false);
        }
        return true;
    }
    void service(const TimestampUs now) noexcept {
        if (!transfer_.active())
            return;
        const auto result = transfer_.service(now);
        if (result == Transfer::Result::busy)
            return;
        if (result != Transfer::Result::done) {
            fail("EEPROM I/O / verify");
            return;
        }
        switch (stage_) {
        case Stage::marker: {
            model_migration::Info info{};
            const auto status = model_migration::decode(marker_, info);
            if (((status == model_migration::Status::invalid) && !repairable_marker()) ||
                ((status == model_migration::Status::valid) &&
                 ((info.kind != model_migration::Kind::conversion) ||
                  (info.target != converter_target)))) {
                fail("Marker: use recovery");
                return;
            }
            resume_ = status == model_migration::Status::valid;
            repair_marker_ = status == model_migration::Status::invalid;
            stage_ = Stage::directory;
            transfer_.read(models::directory_base, page());
            break;
        }
        case Stage::directory:
            if (!directory())
                return;
            if (++bank_ < 2)
                transfer_.read(static_cast<std::uint16_t>(models::directory_base + 64), page());
            else {
                if (!directory_valid_) {
                    fail("No native directory");
                    return;
                }
                bank_ = 0;
                stage_ = Stage::scan;
                read_bank();
            }
            break;
        case Stage::scan:
            if (!scan_bank())
                return;
            if (++bank_ < 2) {
                read_bank();
                break;
            }
            if (!select_slot())
                return;
            bank_ = 0;
            if (++slot_ < models::count) {
                read_bank();
                break;
            }
            if (!banks_[active_ * 2 + selected_[active_]].info.present) {
                fail("Active model missing");
                return;
            }
            if (final_scan_) {
                marker_.fill(0xFF);
                stage_ = Stage::clear;
                transfer_.write(model_migration::address, marker_, Memory::page_size, false);
            } else
                stage_ = Stage::preview;
            break;
        case Stage::mark:
            next_slot();
            break;
        case Stage::load: {
            models::Info info{};
            const auto &source = banks_[slot_ * 2 + selected_[slot_]];
            if ((models::decode(record_, slot_, transmitter_, data_, info) != 1) ||
                (info.checksum != source.info.checksum) ||
                !models::encode(data_, slot_, info.present, info.generation + 1U, record_)) {
                fail("Model changed / invalid");
                return;
            }
            stage_ = Stage::write;
            transfer_.write(address(slot_, 1U - selected_[slot_]), record_, 1);
            break;
        }
        case Stage::write: {
            models::Info info{};
            if ((record_[4] != converter_target) ||
                (models::decode(record_, slot_, transmitter_, data_, info) != 1)) {
                fail("Converted model invalid");
                return;
            }
            retire(selected_[slot_]);
            break;
        }
        case Stage::retire:
            ++slot_;
            next_slot();
            break;
        case Stage::clear:
            stage_ = Stage::done;
            break;
        default:
            break;
        }
    }

  private:
    [[nodiscard]] std::span<std::uint8_t> page() noexcept { return std::span{record_}.first(64); }
    [[nodiscard]] static std::uint16_t address(const std::size_t slot,
                                               const std::size_t bank) noexcept {
        return static_cast<std::uint16_t>(models::base + (slot * 2 + bank) * models::record_size);
    }
    static bool empty(const std::span<const std::uint8_t> bytes) noexcept {
        return std::all_of(bytes.begin(), bytes.end(), [](auto b) { return b == 0xFF; }) ||
               ((bytes[0] == 0) &&
                (((bytes[1] == 'I') && (bytes[2] == 'R') && (bytes[3] == '1')) ||
                 ((bytes[1] == 0xFF) && (bytes[2] == 0xFF) && (bytes[3] == 0xFF))));
    }
    void fail(const char *message) noexcept {
        error_ = message;
        stage_ = Stage::failed;
    }
    [[nodiscard]] bool repairable_marker() const noexcept {
        model_migration::Record expected{};
        model_migration::encode(model_migration::Kind::conversion, converter_target, expected);
        // Creation writes the marker into erased bytes; clearing erases those
        // same bytes. Anything besides expected bytes or FF is not our repair.
        for (std::size_t i = 0; i < marker_.size(); ++i)
            if ((marker_[i] != 0xFF) && (marker_[i] != expected[i]))
                return false;
        return true;
    }
    bool directory() noexcept {
        const auto bytes = page();
        if (empty(bytes))
            return true;
        storage::detail::Reader in{bytes};
        storage::detail::Reader crc{bytes.last(4)};
        std::uint32_t magic{}, generation{}, checksum{};
        std::uint8_t version{}, active{};
        in.word(magic);
        in.field(version);
        in.field(active);
        in.word(generation);
        crc.word(checksum);
        if ((magic != models::directory_magic) || (version != 1) || (active >= models::count) ||
            (checksum != storage::crc32(bytes.first(60))) ||
            !std::all_of(bytes.begin() + 10, bytes.end() - 4, [](auto b) { return b == 0xFF; })) {
            fail("Directory invalid");
            return false;
        }
        const auto delta = generation - directory_generation_;
        if (directory_valid_ && ((delta == 0x80000000U) || ((delta == 0) && (active != active_)))) {
            fail("Directory ambiguous");
            return false;
        }
        if (!directory_valid_ || ((delta != 0) && (delta < 0x80000000U))) {
            directory_generation_ = generation;
            active_ = active;
        }
        directory_valid_ = true;
        return true;
    }
    void read_bank() noexcept { transfer_.read(address(slot_, bank_), record_); }
    bool scan_bank() noexcept {
        auto &bank = banks_[slot_ * 2 + bank_];
        bank = {};
        bank.version = record_[4];
        storage::detail::Reader generation{std::span{record_}.subspan(8, 4)};
        generation.word(bank.raw_generation);
        bank.status = models::decode(record_, slot_, transmitter_, data_, bank.info);
        if (repair_marker_ && (bank.status == 2)) {
            fail("Marker repair: damaged model");
            return false;
        }
        // A page or the one-byte commit may tear arbitrarily. The conversion
        // marker proves preflight completed; a valid peer is still mandatory.
        if (resume_ && (bank.status == 2))
            bank.status = 5;
        if (final_scan_ && (bank.status != 0) && (bank.status != 1)) {
            fail("Final model verify failed");
            return false;
        }
        if ((bank.status == 3) || ((bank.status != 0) && (bank.status != 5) &&
                                   ((bank.version < 6) || (bank.version > converter_target)))) {
            fail("Unsupported version");
            return false;
        }
        if (bank.status == 4) {
            fail("Module unavailable");
            return false;
        }
        if (bank.status == 1) {
            versions_ |= static_cast<std::uint16_t>(1U << bank.version);
            // Preflight representability before even creating the migration marker.
            if (!models::encode(data_, slot_, bank.info.present, bank.info.generation, record_)) {
                fail("Unrepresentable model");
                return false;
            }
            if (final_scan_ && (bank.version != converter_target)) {
                fail("Old backup remains");
                return false;
            }
        }
        return true;
    }
    bool select_slot() noexcept {
        const auto &a = banks_[slot_ * 2];
        const auto &b = banks_[slot_ * 2 + 1];
        auto &chosen = selected_[slot_];
        chosen = a.status == 1 ? 0 : 1;
        if ((a.status == 1) && (b.status == 1)) {
            const auto delta = b.info.generation - a.info.generation;
            if ((delta == 0x80000000U) || ((delta == 0) && (a.info.checksum != b.info.checksum))) {
                fail("Model journal ambiguous");
                return false;
            }
            if ((delta != 0) && (delta < 0x80000000U))
                chosen = 1;
        }
        const auto &good = banks_[slot_ * 2 + chosen];
        const auto &other = banks_[slot_ * 2 + 1U - chosen];
        if (good.status != 1) {
            if ((a.status != 0) || (b.status != 0)) {
                fail("Model CRC / data error");
                return false;
            }
        } else {
            if (other.status == 2) {
                const auto delta = good.info.generation - other.raw_generation;
                if ((delta == 0) || (delta >= 0x80000000U)) {
                    fail("Damaged newer journal");
                    return false;
                }
            }
            if (!final_scan_ && good.info.present)
                ++model_count_;
        }
        return true;
    }
    void retire(const std::size_t bank) noexcept {
        record_[0] = 0;
        record_[1] = 'D';
        record_[2] = 'L';
        record_[3] = '1';
        stage_ = Stage::retire;
        transfer_.write(address(slot_, bank), std::span{record_}.first(4), 4, false);
    }
    void next_slot() noexcept {
        while (slot_ < models::count) {
            const auto &source = banks_[slot_ * 2 + selected_[slot_]];
            const auto &other = banks_[slot_ * 2 + 1U - selected_[slot_]];
            if ((source.status == 1) && (source.version != converter_target)) {
                stage_ = Stage::load;
                transfer_.read(address(slot_, selected_[slot_]), record_);
                return;
            }
            if ((source.status == 1) &&
                ((other.status == 2) || (other.status == 5) ||
                 ((other.status == 1) && (other.version != converter_target)))) {
                // A restart may find the target committed before source retirement.
                retire(1U - selected_[slot_]);
                return;
            }
            ++slot_;
        }
        final_scan_ = true;
        slot_ = bank_ = 0;
        stage_ = Stage::scan;
        read_bank();
    }
    models::Record &record_;
    model_migration::Record marker_{};
    models::Data &data_;
    std::array<Bank, models::count * 2> banks_{};
    std::array<std::uint8_t, models::count> selected_{};
    Transfer transfer_{};
    afhds2a::TxId transmitter_{};
    std::uint32_t directory_generation_{};
    std::uint16_t versions_{};
    std::size_t slot_{}, bank_{}, active_{};
    unsigned model_count_{};
    const char *error_ = "";
    Stage stage_{Stage::marker};
    bool resume_{}, repair_marker_{}, writing_{}, directory_valid_{}, final_scan_{};
};

template <typename Memory> class ModelConverter {
    models::Record record_{};
    models::Data data_{models::empty_model};
    ModelConversion<Memory> conversion_{record_, data_};

  public:
    using State = ModelConversion<Memory>::State;
    void initialize(const afhds2a::TxId transmitter) noexcept {
        conversion_.initialize(transmitter);
    }
    [[nodiscard]] State state() const noexcept { return conversion_.state(); }
    [[nodiscard]] const char *error() const noexcept { return conversion_.error(); }
    [[nodiscard]] unsigned model_count() const noexcept { return conversion_.model_count(); }
    [[nodiscard]] std::uint16_t versions() const noexcept { return conversion_.versions(); }
    [[nodiscard]] bool resuming() const noexcept { return conversion_.resuming(); }
    [[nodiscard]] bool repairing_marker() const noexcept { return conversion_.repairing_marker(); }
    bool confirm() noexcept { return conversion_.confirm(); }
    void service(const TimestampUs now) noexcept { conversion_.service(now); }
};
} // namespace coretx::firmware
