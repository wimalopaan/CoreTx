// SPDX-License-Identifier: GPL-2.0-or-later
#include "boards/i6s/workload.hpp"
#include "coretx/storage/storage.hpp"

#include <cstdio>
#include <cstdlib>
#include <utility>

namespace {

namespace store = coretx::storage;

void check(const bool passed, const char *const expression, const int line) {
    if (!passed) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
        std::exit(EXIT_FAILURE);
    }
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

constexpr store::State fixture() {
    store::State state{};
    state.model = coretx::firmware::maximum_model();
    state.model.crsf_dashboard = true;
    state.setup.calibration.fill({10, 2000, 65000});
    state.setup.calibration[0] = {0, 1, 65535};
    state.setup.mapping[0] = {7, true};
    state.model.inputs[0].weight_percent = coretx::InputWeight::constant<-100>();
    state.model.inputs[0].offset_percent = coretx::InputOffset::constant<100>();
    state.model.inputs[0].expo_percent = coretx::ExpoPercent::constant<-100>();
    state.model.inputs[1].condition = {false, 3, coretx::SwitchPosition::down, true};
    state.model.mixes[0].source.inverted = true;
    state.model.mixes[0].carry_trim = false;
    state.model.mixes[0].weight_percent = coretx::MixWeight::constant<-500>();
    state.model.mixes[0].offset_percent = coretx::MixOffset::constant<500>();
    state.model.trims = {coretx::TrimSteps::constant<-127>(), coretx::TrimSteps::constant<127>(),
                         coretx::TrimSteps::constant<-37>(), coretx::TrimSteps::constant<42>()};
    state.model.trim_switches = 0b1010;
    state.model.inputs[1].source = {coretx::SourceKind::switch_position, 19, false};
    state.model.outputs[0] = {coretx::TravelPermille::constant<-1500>(),
                              coretx::TravelPermille::constant<1500>(),
                              coretx::SubtrimPermille::constant<-1000>(), true};
    for (std::size_t i = 0; i < state.model.multiswitch.size(); ++i) {
        auto &page = state.model.multiswitch[i];
        page.address = static_cast<std::int16_t>(i == 3 ? 255 : i);
        page.momentary = static_cast<std::uint8_t>(1U << i);
        for (auto &label : page.labels)
            label = {'L', 'I', 'G', 'H', 'T', 'S'};
    }
    auto &control = state.model.multiswitch[3].controls[7];
    control.set_address(127);
    control.set_output(0);
    control.set_condition({true, 15, coretx::SwitchPosition::pos6, true});
    return state;
}

constexpr void reseal(store::Record &record) {
    store::detail::Writer checksum{std::span{record}.subspan(12, 4)};
    checksum.word(store::detail::record_crc(record));
}

constexpr bool roundtrip() {
    const auto original = fixture();
    store::Record record{};
    if (!store::encode(original, 0x12345678, record) || (record[0] != 'R') || (record[1] != 'C') ||
        (record[2] != 'C') || (record[3] != 'S') || (record[4] != 6) || (record[5] != 0) ||
        (record[6] != 4) || (record[7] != 3) || (record[8] != 0x78) || (record[9] != 0x56) ||
        (record[10] != 0x34) || (record[11] != 0x12) || (record[20] != 1) || (record[22] != 0xFF) ||
        (record[23] != 0xFF) || (record[66] != 7) || (record[67] != 1)) {
        return false;
    }
    store::State restored{};
    const auto decoded = store::decode(record, restored);
    store::Record encoded_again{};
    if (!decoded || (decoded.generation != 0x12345678) ||
        !store::encode(restored, decoded.generation, encoded_again) || (encoded_again != record) ||
        (restored.model.inputs[1].condition.index != 3) ||
        (restored.model.inputs[1].condition.position != coretx::SwitchPosition::down) ||
        !restored.model.inputs[1].condition.inverted ||
        (restored.model.multiswitch != original.model.multiswitch) ||
        !restored.model.crsf_dashboard) {
        return false;
    }
    store::State scratch{};
    const auto selected = store::select_into(record, record, scratch);
    if (!selected || (selected.generation != decoded.generation) ||
        !store::encode(scratch, selected.generation, encoded_again) || (encoded_again != record)) {
        return false;
    }
    const auto controls = coretx::firmware::synthetic_controls(1234);
    const auto before = coretx::evaluate(original.model, controls);
    const auto after = coretx::evaluate(restored.model, controls);
    return before && after && (before.frame.inputs == after.frame.inputs) &&
           (before.frame.mixes == after.frame.mixes) &&
           (before.frame.channels == after.frame.channels);
}

constexpr bool malformed_fields(const std::size_t first = 0, const std::size_t last = 9) {
    const auto original = fixture();
    store::Record good{};
    if (!store::encode(original, 7, good)) {
        return false;
    }
    struct Mutation {
        std::size_t offset;
        std::uint8_t value;
        store::Error expected;
    };
    constexpr std::array mutations{
        Mutation{16, 255, store::Error::field},   Mutation{20, 0, store::Error::setup},
        Mutation{66, 8, store::Error::setup},     Mutation{900, 0, store::Error::length},
        Mutation{418, 17, store::Error::model},   Mutation{426, 11, store::Error::field},
        Mutation{428, 0xE0, store::Error::field}, Mutation{483, 1, store::Error::model},
        Mutation{485, 1, store::Error::model}};
    for (auto i = first; i < last; ++i) {
        const auto &mutation = mutations[i];
        auto damaged = good;
        damaged[mutation.offset] = mutation.value;
        reseal(damaged);
        auto destination = original;
        const auto decoded = store::decode(damaged, destination);
        store::Record unchanged{};
        if (decoded || (decoded.status.code != mutation.expected) ||
            !store::encode(destination, 7, unchanged) || (unchanged != good)) {
            return false;
        }
    }
    auto invalid = original;
    invalid.model.inputs[0].condition.index = 255;
    auto unchanged = good;
    return !store::encode(invalid, 8, unchanged) && (unchanged == good);
}

constexpr bool malformed_dense_fields() {
    const auto original = fixture();
    store::Record good{};
    if (!store::encode(original, 7, good))
        return false;
    struct Mutation {
        std::size_t offset;
        std::size_t bytes;
        unsigned shift;
        unsigned width;
        std::uint64_t value;
        store::Error error;
    };
    constexpr std::array mutations{Mutation{82, 7, 51, 1, 1, store::Error::field},
                                   Mutation{82, 7, 27, 8, 201, store::Error::field},
                                   Mutation{82, 7, 12, 5, 31, store::Error::model},
                                   Mutation{82, 7, 17, 4, 9, store::Error::model},
                                   Mutation{194, 6, 46, 1, 1, store::Error::field},
                                   Mutation{194, 6, 23, 2, 3, store::Error::model},
                                   Mutation{338, 5, 36, 1, 1, store::Error::field},
                                   Mutation{338, 5, 0, 12, 3001, store::Error::field},
                                   Mutation{780, 3, 15, 1, 1, store::Error::model},
                                   Mutation{780, 3, 0, 5, 28, store::Error::model},
                                   Mutation{780, 3, 12, 1, 0, store::Error::model}};
    for (const auto &mutation : mutations) {
        auto damaged = good;
        std::uint64_t word{};
        for (std::size_t i = 0; i < mutation.bytes; ++i)
            word |= std::uint64_t{damaged[mutation.offset + i]} << (i * 8);
        const auto mask = ((std::uint64_t{1} << mutation.width) - 1) << mutation.shift;
        word = (word & ~mask) | (mutation.value << mutation.shift);
        for (std::size_t i = 0; i < mutation.bytes; ++i)
            damaged[mutation.offset + i] = static_cast<std::uint8_t>(word >> (i * 8));
        reseal(damaged);
        auto destination = original;
        const auto result = store::decode(damaged, destination);
        store::Record unchanged{};
        if (result || (result.status.code != mutation.error) ||
            !store::encode(destination, 7, unchanged) || (unchanged != good))
            return false;
    }
    return true;
}

constexpr bool generation_selection() {
    const auto state = fixture();
    store::Record first{};
    store::Record second{};
    if (!store::encode(state, 0xFFFFFFFFU, first) || !store::encode(state, 0, second)) {
        return false;
    }
    auto selected = store::select(first, second);
    if (!selected || (selected.slot != 1) || (selected.generation != 0)) {
        return false;
    }
    selected = store::select(second, first);
    if (!selected || (selected.slot != 0)) {
        return false;
    }
    selected = store::select(first, first);
    if (!selected || (selected.slot != 0)) {
        return false;
    }
    auto other = state;
    other.model.outputs[0].reversed = false;
    if (!store::encode(other, 0xFFFFFFFFU, second) ||
        (store::select(first, second).status.code != store::Error::generation)) {
        return false;
    }
    if (!store::encode(state, 0x7FFFFFFFU, second)) {
        return false;
    }
    return store::select(first, second).status.code == store::Error::generation;
}

constexpr bool recovery_and_versions() {
    const auto state = fixture();
    store::State scratch{};
    store::Record first{};
    store::Record second{};
    if (!store::encode(state, 10, first) || !store::encode(state, 11, second)) {
        return false;
    }
    second[100] ^= 1;
    const auto recovered = store::select(first, second);
    if (!recovered || (recovered.slot != 0) ||
        (recovered.slots[1].code != store::Error::checksum)) {
        return false;
    }
    first[4] = 7;
    reseal(first);
    if ((store::select(first, second).status.code != store::Error::version) ||
        (store::select_into(first, second, scratch).status.code != store::Error::version)) {
        return false;
    }
    first.fill(0xFF);
    second.fill(0xFF);
    if ((store::select(first, second).status.code != store::Error::empty) ||
        (store::select_into(first, second, scratch).status.code != store::Error::empty)) {
        return false;
    }
    first[0] = 0;
    return (store::select(first, second).status.code == store::Error::no_valid_slot) &&
           (store::select_into(first, second, scratch).status.code == store::Error::no_valid_slot);
}

static_assert(store::crc32(std::array<std::uint8_t, 9>{'1', '2', '3', '4', '5', '6', '7', '8',
                                                       '9'}) == 0xCBF43926U);
static_assert(roundtrip());
static_assert(malformed_fields(0, 3));
static_assert(malformed_fields(3, 6));
static_assert(malformed_fields(6, 9));
static_assert(malformed_dense_fields());
static_assert(generation_selection());
static_assert(recovery_and_versions());

constexpr bool snapshot_extensions() {
    auto state = fixture();
    state.model.dashboard_channels = {16, 0, 8, 0, 3, 2, 1, 15};
    for (auto &line : state.model.logical_switches) {
        line.function = coretx::LogicalFunction::less_equal;
        line.a = 31;
        line.threshold = -1024;
        line.inverted_a = line.inverted = true;
    }
    state.model.multiswitch[3].address = 255;
    state.model.multiswitch[3].momentary = 0x81;
    state.model.multiswitch[3].labels[7] = {'H', 'O', 'R', 'N'};
    store::Record record{}, again{};
    store::State restored{};
    return store::encode(state, 4, record) && store::decode(record, restored) &&
           store::encode(restored, 4, again) && (record == again) &&
           (restored.model.dashboard_channels == state.model.dashboard_channels) &&
           (restored.model.logical_switches[7].threshold == -1024) &&
           restored.model.logical_switches[7].inverted &&
           (restored.model.multiswitch == state.model.multiswitch);
}

static_assert(snapshot_extensions());

constexpr bool obsolete_formats_are_rejected() {
    const auto original = fixture();
    store::Record current{};
    if (!store::encode(original, 7, current))
        return false;
    for (std::uint8_t version = 1; version < store::format_version; ++version) {
        auto obsolete = current;
        obsolete[4] = version;
        reseal(obsolete);
        auto destination = original;
        const auto result = store::decode(obsolete, destination);
        store::Record unchanged{};
        const auto selected = store::select(obsolete, current);
        if (result || (result.status.code != store::Error::obsolete) ||
            !store::encode(destination, 7, unchanged) || (unchanged != current) || !selected ||
            (selected.slot != 1) ||
            (store::select(obsolete, obsolete).status.code != store::Error::no_valid_slot))
            return false;
    }
    return true;
}
static_assert(obsolete_formats_are_rejected());

void torn_writes() {
    auto old = fixture();
    auto newer = old;
    newer.model.inputs[0].weight_percent = coretx::InputWeight::constant<75>();
    store::Record active{};
    store::Record previous{};
    store::Record next{};
    CHECK(store::encode(old, 7, active));
    CHECK(store::encode(old, 6, previous));
    CHECK(store::encode(newer, 8, next));
    for (const auto length :
         std::array<std::size_t, 13>{0, 1, 8, 12, 15, 16, 30, 92, 93, 626, 738, 1023, 1024}) {
        auto interrupted = previous;
        std::copy_n(next.begin(), length, interrupted.begin());
        const auto selection = store::select(active, interrupted);
        CHECK(selection);
        CHECK((selection.generation == 7) || (selection.generation == 8));
        store::State restored{};
        CHECK(store::decode(selection.slot == 0 ? active : interrupted, restored));
        store::State scratch{};
        const auto direct = store::select_into(active, interrupted, scratch);
        CHECK(direct && (direct.slot == selection.slot) &&
              (direct.generation == selection.generation));
        CHECK(scratch.model.multiswitch == restored.model.multiswitch);
        CHECK(scratch.model.inputs[0].weight_percent == restored.model.inputs[0].weight_percent);
        CHECK(restored.model.inputs[0].weight_percent.value() ==
              (selection.generation == 7 ? -100 : 75));
    }
    for (const auto length : std::array<std::size_t, 5>{1, 15, 16, 738, 1023}) {
        const auto selection = store::select(active, std::span{next}.first(length));
        CHECK(selection && (selection.slot == 0));
        CHECK(selection.slots[1].code == store::Error::truncated);
    }
    for (std::size_t i = 0; i < next.size(); ++i) {
        auto corrupt = next;
        corrupt[i] ^= 1;
        CHECK(!store::inspect(corrupt));
    }
}

} // namespace

int main() {
    CHECK(snapshot_extensions());
    CHECK(obsolete_formats_are_rejected());
    CHECK(roundtrip());
    CHECK(malformed_fields());
    CHECK(malformed_dense_fields());
    CHECK(generation_selection());
    CHECK(recovery_and_versions());
    torn_writes();
    std::puts("Binary storage tests passed");
}
