// SPDX-License-Identifier: GPL-2.0-or-later
#include "boards/common/model_converter.hpp"

#include <cstdio>
#include <cstdlib>

namespace {
namespace fw = coretx::firmware;
namespace md = fw::models;
constexpr coretx::afhds2a::TxId tx{0x12345678};
constexpr auto fixture_versions = std::to_array<std::uint8_t>({6, 7, 8, 9, 10});
static_assert(fixture_versions.front() == coretx::storage::oldest_format_version);
static_assert(fixture_versions.back() + 1 == fw::converter_target &&
                  fixture_versions.size() ==
                      fw::converter_target - coretx::storage::oldest_format_version,
              "Add an independent prior-current format fixture before raising the target");
static_assert(
    [] {
        for (std::size_t i = 0; i < fixture_versions.size(); ++i)
            if (fixture_versions[i] != coretx::storage::oldest_format_version + i)
                return false;
        return true;
    }(),
    "Historical fixture versions must be contiguous and unique");
void check(bool value, int line) {
    if (!value) {
        std::fprintf(stderr, "converter FAIL %d\n", line);
        std::exit(1);
    }
}
#define CHECK(x) check(static_cast<bool>(x), __LINE__)

void seal(std::span<std::uint8_t> bytes) {
    coretx::storage::detail::Writer out{bytes.last(4)};
    out.word(coretx::storage::crc32(bytes.first(bytes.size() - 4)));
}
void word(md::Record &record, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        record[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
}

// Independent native MDL1 fixtures: fixed historical field bytes, not the current
// encoder or its transfer helpers. v6/7 use 51-byte pages; v8 packs labels to 45.
md::Record fixture(std::uint8_t version, std::uint8_t slot = 0, std::uint32_t generation = 10,
                   bool present = true) {
    md::Record r{};
    r.fill(0xFF);
    constexpr std::array<std::uint8_t, 34> header{
        'M',  'D',  'L',  '1', 6, 0,   1,   1,   10,  0,   0,   0,   0x78, 0x56, 0x34, 0x12, 0x21,
        0x43, 0x65, 0x87, 1,   1, 'F', 'i', 'x', 't', 'u', 'r', 'e', 0,    0,    0,    0,    0};
    std::copy(header.begin(), header.end(), r.begin());
    r[4] = version;
    r[5] = slot;
    r[6] = static_cast<std::uint8_t>(present);
    word(r, 8, generation);
    r[34] = r[35] = 1;
    std::size_t p = 36;
    // Historical packed lines: analog 2 inverted, -73/+37/-29, trim stick 3;
    // input 0 -> output 3, replace, carry trim, -321/+187.
    constexpr std::array<std::uint8_t, 7> old_input{0x40, 0x04, 0x42, 0xDC, 0x48, 0x3C, 0x02};
    constexpr std::array<std::uint8_t, 7> curve_input{0x40, 0x04, 0x42, 0xDC, 0x48, 0x3C, 0x1A};
    constexpr std::array<std::uint8_t, 6> mixer{0x13, 0x00, 0x04, 0xCF, 0xF2, 0x2A};
    for (const auto byte : version >= 8 ? curve_input : old_input)
        r[p++] = byte;
    for (const auto byte : mixer)
        r[p++] = byte;
    for (unsigned i = 0; i < 16; ++i)
        for (auto byte : {0xF4, 0x41, 0x9C, 0xE8, 0x03})
            r[p++] = static_cast<std::uint8_t>(byte); // -1000/+1000/0 output.
    for (unsigned i = 1; i <= 8; ++i)
        r[p++] = static_cast<std::uint8_t>(i);
    for (unsigned i = 0; i < 56; ++i)
        r[p++] = 0; // Disabled logical switches.
    for (unsigned i = 0; i < 4; ++i) {
        r[p++] = 0xFF;
        r[p++] = 0xFF;
        r[p++] = 0; // Disabled SET4 page.
        r[p++] = 'A';
        for (unsigned j = 1; j < (version >= 8 ? 42U : 48U); ++j)
            r[p++] = 0;
    }
    r[p++] = version == 6 ? 1 : 3; // Dashboard; v7 introduces mute.
    for (unsigned i = 0; i < 96; ++i)
        r[p++] = 0;
    for (auto byte : {0xDB, 42, 0x81, 127, 10})
        r[p++] = static_cast<std::uint8_t>(byte);
    if (version >= 8) {
        for (unsigned i = 0; i < 4; ++i)
            for (auto byte : {0xB5, 0, 60, 0x9C, 0xE7, 12, 50, 100})
                r[p++] = static_cast<std::uint8_t>(byte);
        r[p++] = 1; // Mixer curve 4: high selection bit, stored after curve coordinates.
    }
    if (version >= 9) {
        // Historical v9 tank defaults, reversal disabled; v10 adds third motor channel 3.
        for (auto byte : {0x10, 0x08, 0xF2, 0x08})
            r[p++] = static_cast<std::uint8_t>(byte);
        if (version >= 10)
            r[p++] = 0x02;
        r[p++] = 0x00; // One manual mixer role.
    }
    seal(r);
    return r;
}
void fixture_lines(const md::Data &data, const std::uint8_t source_version) {
    CHECK(data.model.inputs.size() == 1 && data.model.mixes.size() == 1);
    const auto &input = data.model.inputs[0];
    CHECK(input.destination == 0 && input.source.kind == coretx::SourceKind::analog);
    CHECK(input.source.index == 2 && input.source.inverted);
    CHECK(input.weight_percent.value() == -73 && input.offset_percent.value() == 37);
    CHECK(input.expo_percent.value() == -29 && input.side == coretx::InputSide::negative);
    CHECK(input.trim == coretx::TrimSource::stick3);
    const auto &mix = data.model.mixes[0];
    CHECK(mix.destination == 3 && mix.source.kind == coretx::SourceKind::input);
    CHECK(mix.source.index == 0 && !mix.source.inverted);
    CHECK(mix.weight_percent.value() == -321 && mix.offset_percent.value() == 187);
    CHECK(mix.multiplex == coretx::Multiplex::replace && mix.carry_trim);
    CHECK(input.curve == (source_version >= 8 ? 3 : 0));
    CHECK(mix.curve == (source_version >= 8 ? 4 : 0));
}

struct Memory {
    inline static constexpr std::size_t page_size = 64;
    inline static std::array<std::uint8_t, 16384> bytes{};
    inline static unsigned writes{}, cut{}, tear{};
    inline static fw::I2cError failure{};
    static void service(coretx::TimestampUs) {}
    static bool busy() { return false; }
    static fw::I2cError error() { return failure; }
    static bool read(std::uint16_t address, std::span<std::uint8_t> out) {
        std::copy_n(bytes.begin() + address, out.size(), out.begin());
        return true;
    }
    static bool write(std::uint16_t address, std::span<const std::uint8_t> in) {
        CHECK(in.size() <= 64 && address % 64 + in.size() <= 64);
        if ((address >= md::base) && (address < md::directory_base)) {
            fw::model_migration::Record marker{};
            std::copy_n(bytes.begin() + fw::model_migration::address, marker.size(),
                        marker.begin());
            fw::model_migration::Info info{};
            CHECK(fw::model_migration::decode(marker, info) == fw::model_migration::Status::valid);
            CHECK(info.kind == fw::model_migration::Kind::conversion && info.target == 11);
        }
        ++writes;
        if (cut && writes == cut) {
            std::copy_n(in.begin(), std::min<std::size_t>(tear, in.size()),
                        bytes.begin() + address);
            if (tear == 65)
                bytes[address + in.size() / 2] ^= 0x55;
            failure = fw::I2cError::timeout;
            return false;
        }
        std::copy(in.begin(), in.end(), bytes.begin() + address);
        return true;
    }
    static void reset() {
        bytes.fill(0xFF);
        writes = cut = tear = 0;
        failure = fw::I2cError::none;
    }
};
using Converter = fw::ModelConverter<Memory>;
using State = Converter::State;
void put(const md::Record &r, unsigned slot, unsigned bank = 0) {
    std::copy(r.begin(), r.end(),
              Memory::bytes.begin() + md::base + (slot * 2 + bank) * md::record_size);
}
void directory(unsigned bank, unsigned active, std::uint32_t generation) {
    std::array<std::uint8_t, 64> r{};
    r.fill(0xFF);
    coretx::storage::detail::Writer out{r};
    out.word(md::directory_magic);
    out.field(std::uint8_t{1});
    out.field(static_cast<std::uint8_t>(active));
    out.word(generation);
    seal(r);
    std::copy(r.begin(), r.end(), Memory::bytes.begin() + md::directory_base + bank * 64);
}
State settle(Converter &c) {
    for (unsigned i = 0; i < 10000; ++i) {
        c.service(coretx::TimestampUs{i * 1000});
        if (c.state() == State::preview || c.state() == State::complete ||
            c.state() == State::failed)
            return c.state();
    }
    CHECK(false);
    return State::failed;
}
void seed() {
    Memory::reset();
    put(fixture(6, 0), 0);
    put(fixture(7, 1, 0xFFFFFFFF), 1);
    put(fixture(8, 2, 1, false), 2);
    directory(0, 0, 0xFFFFFFFF);
    directory(1, 1, 0);
    for (unsigned i = 0; i < md::base; ++i)
        Memory::bytes[i] = static_cast<std::uint8_t>(i * 13);
}
void converted(const std::array<std::uint8_t, 16384> &original) {
    CHECK(std::equal(original.begin(), original.begin() + md::base, Memory::bytes.begin()));
    CHECK(std::equal(original.begin() + md::directory_base,
                     original.begin() + fw::model_migration::address,
                     Memory::bytes.begin() + md::directory_base));
    for (unsigned slot = 0; slot < 3; ++slot) {
        md::Record r{};
        std::copy_n(Memory::bytes.begin() + md::base + (slot * 2 + 1) * md::record_size, r.size(),
                    r.begin());
        md::Data data{md::empty_model};
        md::Info info{};
        CHECK(md::decode(r, slot, tx, data, info) == 1);
        CHECK(r[4] == 11 && info.present == (slot != 2));
        CHECK(info.generation == (slot == 0 ? 11U : slot == 1 ? 0U : 2U));
        CHECK(data.radio.receiver.value == 0x87654321 && data.radio.transmitter == tx);
        CHECK(data.radio.output == fw::RadioOutput::internal && data.baud_index == 1);
        CHECK(data.name[0] == 'F' && data.model.trims[0].value() == -37);
        CHECK(data.model.trims[1].value() == 42 && data.model.trim_switches == 10);
        fixture_lines(data, static_cast<std::uint8_t>(6 + slot));
        CHECK(data.model.crsf_dashboard && data.model.multiswitch_beep == (slot == 0));
        CHECK(data.model.multiswitch[0].labels[0][0] == 'A');
        if (slot == 2) {
            const coretx::Curve curve{{-75, 0, 60}, {-100, -25, 12, 50, 100}};
            CHECK(data.model.curves[0] == curve);
        } else
            CHECK(data.model.curves[0] == coretx::Curve{});
        CHECK(Memory::bytes[md::base + slot * 2 * md::record_size] == 0);
    }
    CHECK(std::all_of(Memory::bytes.begin() + fw::model_migration::address,
                      Memory::bytes.begin() + fw::model_migration::address + 64,
                      [](auto b) { return b == 0xFF; }));
}
void basic() {
    seed();
    const auto original = Memory::bytes;
    Converter c;
    c.initialize(tx);
    CHECK(settle(c) == State::preview && Memory::writes == 0);
    CHECK(c.model_count() == 2 && c.versions() == ((1 << 6) | (1 << 7) | (1 << 8)));
    for (unsigned i = 0; i < 20; ++i)
        c.service(coretx::TimestampUs{i});
    CHECK(Memory::bytes == original && Memory::writes == 0);
    CHECK(c.confirm() && settle(c) == State::complete);
    converted(original);
}
void refusals() {
    for (unsigned which = 0; which < 9; ++which) {
        seed();
        auto r = fixture(6);
        if (which == 0 || which == 1) {
            r[4] = which == 0 ? 11 : 5;
            seal(r);
            put(r, 0);
        } else if (which == 2) {
            r[200] ^= 1;
            put(r, 0);
        } else if (which == 3 || which == 4) {
            r = fixture(7, 0, which == 3 ? 10U : 0x8000000AU);
            put(r, 0, 1);
        } else if (which == 5)
            directory(1, 1, 0x7FFFFFFFU);
        else if (which == 6) {
            fw::model_migration::Record marker{};
            fw::model_migration::encode(fw::model_migration::Kind::reset, 10, marker);
            std::copy(marker.begin(), marker.end(),
                      Memory::bytes.begin() + fw::model_migration::address);
        } else if (which == 7)
            Memory::bytes[fw::model_migration::address] = 0;
        else
            Memory::bytes[md::directory_base + 30] ^= 1;
        const auto original = Memory::bytes;
        Converter c;
        c.initialize(tx);
        CHECK(settle(c) == State::failed);
        CHECK(Memory::writes == 0 && Memory::bytes == original);
    }
}
void interruptions() {
    seed();
    const auto original = Memory::bytes;
    Converter count;
    count.initialize(tx);
    CHECK(settle(count) == State::preview);
    CHECK(count.confirm() && settle(count) == State::complete);
    const auto write_count = Memory::writes;
    // Every write operation, including marker creation/clearing, invalidation,
    // body pages and commit: cut before, within, or after the bytes took effect.
    for (unsigned cut = 1; cut <= write_count; ++cut) {
        for (unsigned tear : {0U, 1U, 17U, 63U, 64U, 65U}) {
            Memory::bytes = original;
            Memory::writes = 0;
            Memory::cut = cut;
            Memory::tear = tear;
            Memory::failure = fw::I2cError::none;
            Converter first;
            first.initialize(tx);
            CHECK(settle(first) == State::preview);
            CHECK(first.confirm() && settle(first) == State::failed);
            Memory::cut = 0;
            Memory::failure = fw::I2cError::none;
            fw::model_migration::Record marker{};
            std::copy_n(Memory::bytes.begin() + fw::model_migration::address, 64, marker.begin());
            fw::model_migration::Info info{};
            const auto marker_status = fw::model_migration::decode(marker, info);
            Converter resumed;
            resumed.initialize(tx);
            const auto before = Memory::bytes;
            const auto writes = Memory::writes;
            const auto state = settle(resumed);
            if ((marker_status == fw::model_migration::Status::invalid) && (tear == 65)) {
                CHECK(state == State::failed && before == Memory::bytes &&
                      writes == Memory::writes);
            } else {
                if (state != State::preview)
                    std::fprintf(stderr, "cut=%u tear=%u error=%s\n", cut, tear, resumed.error());
                CHECK(state == State::preview);
                CHECK(resumed.repairing_marker() ==
                      (marker_status == fw::model_migration::Status::invalid));
                CHECK(resumed.resuming() == (marker_status == fw::model_migration::Status::valid));
                CHECK(before == Memory::bytes && writes == Memory::writes);
                CHECK(resumed.confirm() && settle(resumed) == State::complete);
                converted(original);
            }
        }
    }
}
void mixed_target_and_tank() {
    seed();
    md::Data data{md::empty_model};
    md::Info info{};
    CHECK(md::decode(fixture(8, 3), 3, tx, data, info) == 1);
    data.model.curves[0] = {};
    auto tank = data.model.tank;
    tank.enabled = true;
    tank.steering_percent = 73;
    tank.reverse_steering = false;
    tank.third_motor = true;
    tank.third_channel = 15;
    CHECK(coretx::configure_tank(data.model, tank));
    data.model.curves[data.model.tank.third_curve - 1].y.back() = 80;
    md::Record target{};
    CHECK(md::encode(data, 3, true, 1, target));
    put(target, 3, 1);
    put(fixture(6, 3, 0), 3);
    const auto original = Memory::bytes;
    Converter c;
    c.initialize(tx);
    CHECK(settle(c) == State::preview);
    CHECK(c.confirm() && settle(c) == State::complete);
    md::Record actual{};
    std::copy_n(Memory::bytes.begin() + md::base + 7 * md::record_size, actual.size(),
                actual.begin());
    CHECK(actual == target); // Active tank and managed roles survive main option OFF.
    converted(original);
}
void damaged_inactive() {
    seed();
    auto stale = fixture(6, 0, 9);
    stale[200] ^= 1;
    put(stale, 0, 1);
    Converter c;
    c.initialize(tx);
    CHECK(settle(c) == State::preview && Memory::writes == 0);
    CHECK(c.confirm() && settle(c) == State::complete);
}
void newest_wrap() {
    seed();
    put(fixture(6, 0, 0xFFFFFFFE), 0);
    auto latest = fixture(7, 0, 1);
    latest[22] = 'N';
    seal(latest);
    put(latest, 0, 1);
    Converter c;
    c.initialize(tx);
    CHECK(settle(c) == State::preview);
    CHECK(c.confirm() && settle(c) == State::complete);
    md::Record actual{};
    std::copy_n(Memory::bytes.begin() + md::base, actual.size(), actual.begin());
    md::Data data{md::empty_model};
    md::Info info{};
    CHECK(md::decode(actual, 0, tx, data, info) == 1);
    CHECK(actual[4] == 11 && info.generation == 2 && data.name[0] == 'N');
    CHECK(!data.model.multiswitch_beep);
}
void marker_prefix_repairs() {
    seed();
    const auto original = Memory::bytes;
    Converter completed;
    completed.initialize(tx);
    CHECK(settle(completed) == State::preview);
    CHECK(completed.confirm() && settle(completed) == State::complete);
    const auto targets = Memory::bytes;
    fw::model_migration::Record expected{};
    fw::model_migration::encode(fw::model_migration::Kind::conversion, fw::converter_target,
                                expected);
    for (const bool clearing : {false, true}) {
        for (std::size_t prefix = 1; prefix < expected.size(); ++prefix) {
            Memory::reset();
            Memory::bytes = clearing ? targets : original;
            auto marker = expected;
            if (clearing)
                std::fill_n(marker.begin(), prefix, 0xFF);
            else
                std::fill(marker.begin() + static_cast<std::ptrdiff_t>(prefix), marker.end(), 0xFF);
            std::copy(marker.begin(), marker.end(),
                      Memory::bytes.begin() + fw::model_migration::address);
            const auto before = Memory::bytes;
            Converter repair;
            repair.initialize(tx);
            CHECK(settle(repair) == State::preview);
            fw::model_migration::Info info{};
            const auto status = fw::model_migration::decode(marker, info);
            CHECK(repair.repairing_marker() == (status == fw::model_migration::Status::invalid));
            CHECK(!repair.repairing_marker() || !repair.resuming());
            CHECK(Memory::writes == 0 && Memory::bytes == before);
            CHECK(repair.confirm() && settle(repair) == State::complete);
            converted(original);
        }
    }
}
void marker_repair_refusals() {
    for (unsigned which = 0; which < 5; ++which) {
        seed();
        fw::model_migration::Record marker{};
        fw::model_migration::encode(fw::model_migration::Kind::conversion, fw::converter_target,
                                    marker);
        if (which == 0) {
            fw::model_migration::encode(fw::model_migration::Kind::conversion,
                                        fw::converter_target + 1, marker);
        } else if (which == 1) {
            marker[4] = 2; // An unknown future marker schema is never repairable.
            seal(marker);
        } else {
            marker[0] = 0xFF; // Recognizable partial marker clear.
            if (which == 2) {
                auto damaged = fixture(6);
                damaged[200] ^= 1;
                put(damaged, 0);
            } else if (which == 3) {
                // Ordinary migration tolerates this older damaged inactive bank;
                // marker repair deliberately does not.
                auto damaged = fixture(6, 0, 9);
                damaged[200] ^= 1;
                put(damaged, 0, 1);
            } else
                Memory::bytes[md::directory_base + 30] ^= 1;
        }
        std::copy(marker.begin(), marker.end(),
                  Memory::bytes.begin() + fw::model_migration::address);
        const auto before = Memory::bytes;
        Converter repair;
        repair.initialize(tx);
        CHECK(settle(repair) == State::failed);
        CHECK(Memory::writes == 0 && Memory::bytes == before);
    }
}
void fixture_contract() {
    for (const auto version : fixture_versions) {
        const auto record = fixture(version);
        md::Data data{md::empty_model};
        md::Info info{};
        CHECK(md::decode(record, 0, tx, data, info) == 1);
        CHECK(record[4] == version && info.generation == 10);
        fixture_lines(data, version);
        CHECK(!data.model.tank.third_motor && data.model.tank.third_curve == 0);
        CHECK(data.model.tank.reverse_steering == (version < 9));
        Memory::reset();
        put(record, 0);
        directory(0, 0, 1);
        Converter converter;
        converter.initialize(tx);
        CHECK(settle(converter) == State::preview);
        CHECK(converter.confirm() && settle(converter) == State::complete);
        md::Record converted_record{};
        std::copy_n(Memory::bytes.begin() + md::base + md::record_size, converted_record.size(),
                    converted_record.begin());
        CHECK(converted_record[4] == 11);
        CHECK(md::decode(converted_record, 0, tx, data, info) == 1 && info.generation == 11);
        fixture_lines(data, version);
        CHECK(!data.model.tank.third_motor && data.model.tank.third_curve == 0);
        CHECK(data.model.tank.reverse_steering == (version < 9));
    }
}
} // namespace
int main() {
    fixture_contract();
    basic();
    refusals();
    interruptions();
    mixed_target_and_tank();
    damaged_inactive();
    newest_wrap();
    marker_prefix_repairs();
    marker_repair_refusals();
    std::puts("Model converter tests passed");
}
