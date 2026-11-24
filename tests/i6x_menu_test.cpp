// SPDX-License-Identifier: GPL-2.0-or-later
#include "boards/common/menu.hpp"
#include "static_fixture.hpp"

#include <cstdio>
#include <cstdlib>
#include <utility>

namespace {
namespace fw = coretx::firmware;
namespace md = fw::models;
using coretx::afhds2a::ChannelMode;
using coretx::firmware::Key;
using coretx::firmware::RadioOutput;
constexpr coretx::afhds2a::TxId tx{0x12345678};

struct Context {
    std::array<std::uint8_t, 16384> bytes{};
    fw::I2cError error{};
    bool fail_read{};
    bool corrupt_verify{};
    unsigned writes{};
    unsigned reads{};
    constexpr Context() { bytes.fill(0xFF); }
};
template <typename Test> struct Memory {
    static void reset() noexcept {}
    inline static constexpr std::size_t page_size = 64;
    static void select(std::uint8_t address) {
        if (address != 0x50)
            std::abort();
    }
    static void service(coretx::TimestampUs) {}
    static bool busy() { return false; }
    static fw::I2cError error() {
        auto &c = Test::context;
        return c.error;
    }
    static bool read(std::uint16_t address, std::span<std::uint8_t> bytes) {
        auto &c = Test::context;
        ++c.reads;
        if (c.fail_read) {
            c.error = fw::I2cError::timeout;
            return false;
        }
        if (((address + bytes.size()) > c.bytes.size()) || (bytes.size() > page_size))
            std::abort();
        std::copy_n(c.bytes.begin() + address, bytes.size(), bytes.begin());
        if (c.corrupt_verify && (c.writes > 0))
            bytes[0] ^= 1;
        return true;
    }
    static bool write(std::uint16_t address, std::span<const std::uint8_t> bytes) {
        auto &c = Test::context;
        if (((address + bytes.size()) > c.bytes.size()) || (bytes.size() > page_size) ||
            (((address % page_size) + bytes.size()) > page_size))
            std::abort();
        std::copy(bytes.begin(), bytes.end(), c.bytes.begin() + address);
        ++c.writes;
        return true;
    }
};
constexpr fw::StorageProbeResult probe{fw::StorageProbeState::found, {}, 1};
constexpr coretx::firmware::RadioSettings legacy_radio{
    tx, {0x87654321}, RadioOutput::internal, ChannelMode::standard14};
template <typename Store>
void tick(Store &store, const coretx::Model &legacy = fw::diagnostic_model) {
    store.service(coretx::TimestampUs{1000}, probe, legacy, legacy_radio);
}
template <typename Store>
bool settle(Store &store, const coretx::Model &legacy = fw::diagnostic_model) {
    for (int i = 0; i < 1500; ++i) {
        tick(store, legacy);
        if (store.loaded() && !store.busy())
            return store.editable();
    }
    return false;
}
template <typename Store>
bool boot(Store &store, const coretx::Model &legacy = fw::diagnostic_model) {
    store.initialize(tx);
    return settle(store, legacy);
}

bool legacy_curve_record(const md::Data &data, const std::uint8_t version, md::Record &record) {
    namespace codec = coretx::storage::detail;
    if (!md::encode(data, 7, true, 0xFFFFFFFF, record))
        return false;
    record[4] = version;
    std::fill(record.begin() + 34, record.end(), 0xFF);
    codec::Writer out{std::span{record}.first(md::record_size - 4), 34};
    if (!codec::transfer_model_counts(out, data.model) ||
        !codec::transfer_model_lines(out, data.model, false) ||
        !codec::transfer_model_extension(out, data.model) ||
        !codec::transfer_multiswitch(out, data.model, version >= 8) ||
        !codec::transfer_dashboard_options(out, data.model, true) ||
        !codec::transfer_multiswitch_controls(out, data.model))
        return false;
    if ((version >= 8) && !md::transfer_curves(out, data.model, true))
        return false;
    if ((version >= 9) && !codec::transfer_tank(out, data.model))
        return false;
    codec::Writer crc{std::span{record}.last(4)};
    crc.word(coretx::storage::crc32(std::span{record}.first(md::record_size - 4)));
    return out.good;
}

bool codec() {
    md::Data data{};
    data.radio = legacy_radio;
    data.model.inputs.resize<coretx::max_input_lines>();
    data.model.mixes.resize<coretx::max_mix_lines>();
    for (std::size_t i = 0; i < data.model.inputs.size(); ++i) {
        data.model.inputs[i] = {};
        data.model.inputs[i].destination = static_cast<std::uint8_t>(i / 2);
        data.model.inputs[i].source.index = static_cast<std::uint8_t>(i);
        data.model.inputs[i].source.inverted = (i % 2) != 0;
        data.model.inputs[i].condition = {
            true, static_cast<std::uint8_t>(i),
            static_cast<coretx::SwitchPosition>(static_cast<int>(i % 9) - 1), (i % 2) != 0};
        data.model.inputs[i].side = static_cast<coretx::InputSide>(i % 3);
        data.model.inputs[i].trim = static_cast<coretx::TrimSource>(i % 6);
        data.model.inputs[i].curve = static_cast<std::uint8_t>(i % 5);
    }
    for (std::size_t i = 0; i < data.model.mixes.size(); ++i) {
        data.model.mixes[i] = {};
        data.model.mixes[i].destination = static_cast<std::uint8_t>(i / 2);
        data.model.mixes[i].source = {coretx::SourceKind::input, static_cast<std::uint8_t>(i % 8),
                                      (i % 2) != 0};
        data.model.mixes[i].condition = {
            true, static_cast<std::uint8_t>(i % 16),
            static_cast<coretx::SwitchPosition>(static_cast<int>(i % 9) - 1), (i % 2) != 0};
        data.model.mixes[i].carry_trim = (i % 2) == 0;
        data.model.mixes[i].curve = static_cast<std::uint8_t>(i % 5);
    }
    data.model.outputs[15].reversed = true;
    data.model.dashboard_channels = {16, 0, 9, 2, 0, 15, 1, 8};
    for (auto &line : data.model.logical_switches) {
        line.function = coretx::LogicalFunction::srff;
        line.a = 0;
        line.b = 1;
        line.position_a = line.position_b = coretx::SwitchPosition::down;
        line.inverted_b = line.inverted = true;
        line.threshold = -1024;
    }
    data.baud_index = 7;
    data.model.crsf_dashboard = true;
    data.model.multiswitch_beep = false;
    data.model.curves[3] = {{-99, -17, 99}, {100, -100, 73, 0, -19}};
    for (std::size_t i = 0; i < data.model.multiswitch.size(); ++i) {
        auto &page = data.model.multiswitch[i];
        page.address = static_cast<std::int16_t>(i == 3 ? 255 : i);
        page.momentary = static_cast<std::uint8_t>(1U << i);
        for (std::size_t label = 0; label < page.labels.size(); ++label)
            for (std::size_t character = 0; character < page.labels[label].size(); ++character) {
                const auto code = (i * 48 + label * 6 + character) % 96;
                page.labels[label][character] = code == 0 ? '\0' : static_cast<char>(31 + code);
            }
    }
    auto &control = data.model.multiswitch[3].controls[7];
    control.set_address(127);
    control.set_output(0);
    control.set_condition({true, coretx::switch_count + 7, coretx::SwitchPosition::up, true});
    md::Record record{}, again{};
    if (!md::encode(data, 7, true, 0xFFFFFFFF, record))
        return false;
    md::Data restored{};
    md::Info info{};
    if ((md::decode(record, 7, tx, restored, info) != 1) || !info.present ||
        (info.generation != 0xFFFFFFFF) || !md::encode(restored, 7, true, info.generation, again) ||
        (again != record) || (restored.model.dashboard_channels != data.model.dashboard_channels) ||
        (restored.model.logical_switches[7].function != coretx::LogicalFunction::srff) ||
        !restored.model.logical_switches[7].inverted_b ||
        (restored.model.logical_switches[7].threshold != -1024))
        return false;
    if ((restored.model.multiswitch != data.model.multiswitch) || !restored.model.crsf_dashboard ||
        restored.model.multiswitch_beep || (restored.model.curves[3].x != data.model.curves[3].x) ||
        (restored.model.curves[3].y != data.model.curves[3].y))
        return false;
    for (std::size_t i = 0; i < data.model.inputs.size(); ++i)
        if (restored.model.inputs[i].curve != data.model.inputs[i].curve)
            return false;
    for (std::size_t i = 0; i < data.model.mixes.size(); ++i)
        if (restored.model.mixes[i].curve != data.model.mixes[i].curve)
            return false;
    if (!legacy_curve_record(data, 6, record))
        return false;
    if (md::decode(record, 7, tx, restored, info) != 2)
        return false;
    data.model.multiswitch_beep = true;
    if (!legacy_curve_record(data, 6, record))
        return false;
    restored.model.multiswitch_beep = false;
    if ((md::decode(record, 7, tx, restored, info) != 1) || !restored.model.multiswitch_beep ||
        !md::encode(restored, 7, true, info.generation, record) ||
        (restored.model.multiswitch != data.model.multiswitch))
        return false;
    if ((restored.model.curves[3].x != coretx::Curve{}.x) ||
        (restored.model.curves[3].y != coretx::Curve{}.y))
        return false;
    data.model.multiswitch_beep = false;
    if (!legacy_curve_record(data, 7, record))
        return false;
    restored.model.curves[3] = data.model.curves[3];
    restored.model.inputs[0].curve = restored.model.mixes[0].curve = 4;
    if ((md::decode(record, 7, tx, restored, info) != 1) || restored.model.multiswitch_beep ||
        (restored.model.curves[3].x != coretx::Curve{}.x) ||
        (restored.model.curves[3].y != coretx::Curve{}.y))
        return false;
    for (const auto &line : restored.model.inputs)
        if (line.curve != 0)
            return false;
    for (const auto &line : restored.model.mixes)
        if (line.curve != 0)
            return false;
    if (!legacy_curve_record(data, 8, record) || (md::decode(record, 7, tx, restored, info) != 1) ||
        (restored.model.curves[3].x != data.model.curves[3].x) ||
        (restored.model.curves[3].y != data.model.curves[3].y) || restored.model.tank.enabled)
        return false;
    for (const auto &line : restored.model.mixes)
        if (line.role != coretx::MixRole::manual)
            return false;
    record = again;
    if ((md::decode(record, 6, tx, restored, info) != 2) ||
        (md::decode(record, 7, {0x87654321}, restored, info) != 2))
        return false;
    record[60] ^= 1;
    if (md::decode(record, 7, tx, restored, info) != 2)
        return false;
    record = again;
    record[700] ^= 1;
    if (md::decode(record, 7, tx, restored, info) != 2)
        return false;
    record = again;
    record[4] = md::format_version + 1;
    coretx::storage::detail::Writer checksum{std::span{record}.last(4)};
    checksum.word(coretx::storage::crc32(std::span{record}.first(md::record_size - 4)));
    if (md::decode(record, 7, tx, restored, info) != 3)
        return false;
    for (auto &line : data.model.inputs)
        line.curve = 4;
    for (auto &line : data.model.mixes)
        line.curve = 4;
    if (!md::encode(data, 7, true, 1, record) || (md::decode(record, 7, tx, restored, info) != 1))
        return false;
    for (const auto &line : restored.model.inputs)
        if (line.curve != 4)
            return false;
    for (const auto &line : restored.model.mixes)
        if (line.curve != 4)
            return false;
    data.model.multiswitch[0].labels[0][0] = '\x7f';
    return !md::encode(data, 7, true, 1, record);
}

#if CORETX_MODUL_TANKMIX
bool tank_codec() {
    md::Data data{};
    data.radio = legacy_radio;
    data.model.inputs.resize<coretx::max_input_lines>();
    data.model.mixes.resize<18>();
    for (std::size_t i = 0; i < data.model.inputs.size(); ++i) {
        data.model.inputs[i].destination = static_cast<std::uint8_t>(i / 2);
        data.model.inputs[i].source.index = static_cast<std::uint8_t>(i);
        data.model.inputs[i].curve = 4;
    }
    for (std::size_t i = 0; i < data.model.mixes.size(); ++i) {
        data.model.mixes[i].destination = static_cast<std::uint8_t>(i / 2);
        data.model.mixes[i].source.index = static_cast<std::uint8_t>(i % coretx::input_count);
        data.model.mixes[i].curve = 4;
    }
    auto tank = data.model.tank;
    tank.enabled = true;
    tank.drive_input = 7;
    tank.steering_input = 6;
    tank.left_channel = 14;
    tank.right_channel = 15;
    tank.steering_percent = 73;
    tank.hysteresis_percent = 20;
    tank.reverse_steering = false;
    if (!coretx::configure_tank(data.model, tank))
        return false;
    md::Record record{}, again{};
    md::Data restored{};
    md::Info info{};
    if (!md::encode(data, 7, true, 11, record) ||
        (md::decode(record, 7, tx, restored, info) != 1) || (restored.model.tank != tank) ||
        !md::encode(restored, 7, true, info.generation, again) || (record != again) ||
        (restored.model.mixes.size() != coretx::max_mix_lines))
        return false;
    for (std::size_t i = 0; i < restored.model.mixes.size(); ++i)
        if ((restored.model.mixes[i].role != data.model.mixes[i].role) ||
            (restored.model.mixes[i].curve != data.model.mixes[i].curve))
            return false;
    data.model.tank.enabled = false;
    return !md::encode(data, 7, true, 11, record);
}
#endif

bool obsolete_models_can_be_recreated() {
    md::Data data{};
    data.radio = legacy_radio;
    md::Record current{};
    if (!md::encode(data, 0, true, 7, current))
        return false;
    for (std::uint8_t version = 1; version < coretx::storage::oldest_format_version; ++version) {
        auto obsolete = current;
        obsolete[4] = version;
        coretx::storage::detail::Writer checksum{std::span{obsolete}.last(4)};
        checksum.word(coretx::storage::crc32(std::span{obsolete}.first(md::record_size - 4)));
        md::Info info{};
        if (md::decode(obsolete, 0, tx, data, info) != 3)
            return false;
    }
    struct Scenario {};
    using Test = coretx::test::FakeApplication<Scenario, Context, Memory>;
    Test::reset();
    current[4] = 4;
    coretx::storage::detail::Writer checksum{std::span{current}.last(4)};
    checksum.word(coretx::storage::crc32(std::span{current}.first(md::record_size - 4)));
    std::copy(current.begin(), current.end(), Test::context.bytes.begin() + md::base);
    fw::ModelStorage<Test::Board> store{};
    store.initialize(tx);
    (void)settle(store);
    return !store.editable() && store.can_initialize() && store.initialize_models() &&
           settle(store) && store.editable() && (store.data().radio.output == RadioOutput::off);
}

bool active_first_loading() {
    for (const std::size_t active : {0U, 3U, 7U}) {
        struct Scenario {};
        using Test = coretx::test::FakeApplication<Scenario, Context, Memory>;
        using Store = fw::ModelStorage<Test::Board>;
        Test::reset();
        auto &c = Test::context;
        md::Data data{};
        data.radio = legacy_radio;
        md::Record record{};
        for (std::size_t index = 0; index < md::count; ++index) {
            md::default_name(data.name, index);
            data.model.inputs[0].weight_percent = coretx::InputWeight::constant<61>();
            if (!md::encode(data, index, true, 0xFFFFFFFFU, record))
                return false;
            std::copy(record.begin(), record.end(),
                      c.bytes.begin() + md::base + index * 2 * md::record_size);
            if (index == active) {
                data.model.inputs[0].weight_percent = coretx::InputWeight::constant<73>();
                if (!md::encode(data, index, true, 0, record))
                    return false;
                std::copy(record.begin(), record.end(),
                          c.bytes.begin() + md::base + (index * 2 + 1) * md::record_size);
            }
        }
        std::array<std::uint8_t, 64> directory{};
        directory.fill(0xFF);
        coretx::storage::detail::Writer out{directory};
        out.word(md::directory_magic);
        out.field(std::uint8_t{1});
        out.field(static_cast<std::uint8_t>(active));
        out.word(std::uint32_t{0});
        coretx::storage::detail::Writer crc{std::span{directory}.last(4)};
        crc.word(coretx::storage::crc32(std::span{directory}.first(60)));
        std::copy(directory.begin(), directory.end(), c.bytes.begin() + md::directory_base);
        Store store{};
        store.initialize(tx);
        for (unsigned i = 0; !store.loaded() && (i < 1000); ++i) {
            tick(store);
            if (!store.loaded() && (store.editable() || store.take_changed() || store.save()))
                return false;
        }
        if (!store.loaded() || store.background_loading() || store.busy() || !store.editable() ||
            !store.take_changed() || (c.reads != 195) || (store.active() != active) ||
            (store.entries()[active].bank != 1) ||
            (store.data().model.inputs[0].weight_percent.value() != 73))
            return false;
        fw::Diagnostics diagnostics{};
        diagnostics.use_model(store.data().model);
        fw::Menu menu{};
        menu.open();
        const auto press = [&](const Key key) {
            return menu.handle(key, store, diagnostics, RadioOutput::off);
        };
        (void)press(Key::down);
        (void)press(Key::ok);
        for (unsigned i = 0; i < 3; ++i)
            (void)press(Key::down);
        (void)press(Key::ok);
        (void)press(Key::ok);
        (void)press(Key::up);
        if ((press(Key::ok) != fw::MenuCommand::output) ||
            (menu.requested_output() !=
             (fw::features::crsf ? RadioOutput::external : RadioOutput::off)))
            return false;
        for (unsigned i = 0; store.background_loading() && (i < 300); ++i) {
            tick(store);
            if (store.take_changed() || (store.data().model.inputs[0].weight_percent.value() != 73))
                return false;
        }
        if (!store.editable() || store.busy() || (c.reads != 195) || (c.writes != 0))
            return false;
        for (const auto &entry : store.entries())
            if (!entry.valid || !entry.present)
                return false;
    }
    return true;
}

bool background_loading_errors() {
    for (const bool future : {false, true}) {
        struct Scenario {};
        using Test = coretx::test::FakeApplication<Scenario, Context, Memory>;
        using Store = fw::ModelStorage<Test::Board>;
        Test::reset();
        auto &c = Test::context;
        md::Data data{};
        data.radio = legacy_radio;
        data.model.inputs[0].weight_percent = coretx::InputWeight::constant<73>();
        md::Record record{};
        if (!md::encode(data, 0, true, 1, record))
            return false;
        std::copy(record.begin(), record.end(), c.bytes.begin() + md::base);
        if (!md::encode(data, 7, true, 1, record))
            return false;
        if (future) {
            record[4] = md::format_version + 1;
            coretx::storage::detail::Writer crc{std::span{record}.last(4)};
            crc.word(coretx::storage::crc32(std::span{record}.first(md::record_size - 4)));
        } else
            record[700] ^= 1;
        std::copy(record.begin(), record.end(), c.bytes.begin() + md::base + 14 * md::record_size);
        const auto before = c.bytes;
        Store store{};
        store.initialize(tx);
        for (unsigned i = 0; !store.loaded() && (i < 1000); ++i)
            tick(store);
        for (unsigned i = 0; store.background_loading() && (i < 300); ++i)
            tick(store);
        if (!store.loaded() || store.editable() || store.busy() || store.take_changed() ||
            store.save() || store.create(false) || (c.bytes != before) || (c.writes != 0) ||
            (store.data().model.inputs[0].weight_percent.value() != 73) ||
            !store.recovery_required() || (store.data().radio.receiver != legacy_radio.receiver) ||
            (store.message() !=
             (future ? coretx::ui::tr::models_version : coretx::ui::tr::models_error)))
            return false;
    }
    return true;
}

bool multiswitch_menu_persistence() {
    if constexpr (!coretx::firmware::features::multiswitch)
        return true;
    struct Scenario {};
    using Test = coretx::test::FakeApplication<Scenario, Context, Memory>;
    using Store = fw::ModelStorage<Test::Board>;
    Test::reset();
    Store store{};
    if (!boot(store))
        return false;
    fw::Diagnostics diagnostics{};
    diagnostics.use_model(store.data().model);
    fw::Menu menu{};
    menu.open();
    const auto press = [&](const Key key) {
        (void)menu.handle(key, store, diagnostics, RadioOutput::off);
    };
    press(Key::down);
    press(Key::ok);
    for (unsigned i = 0; i < 6; ++i)
        press(Key::down);
    press(Key::ok);
    if (menu.screen() != fw::MenuScreen::multiswitch)
        return false;
    for (unsigned i = 0; i < coretx::multiswitch_page_count; ++i)
        press(Key::down);
    press(Key::ok);
    if (store.data().model.multiswitch_beep || (menu.screen() != fw::MenuScreen::multiswitch))
        return false;
    press(Key::ok); // Saving must not toggle again.
    if (!settle(store) || store.data().model.multiswitch_beep)
        return false;
    press(Key::up);
    press(Key::ok);
    press(Key::ok);
    press(Key::up);
    press(Key::cancel);
    if (store.data().model.multiswitch[3].address != -1)
        return false;
    press(Key::ok);
    press(Key::up);
    press(Key::ok);
    if (!settle(store) || (store.data().model.multiswitch[3].address != 0) ||
        (diagnostics.dashboard_pages() != 3))
        return false;
    press(Key::down);
    press(Key::ok); // Button 1.
    press(Key::ok); // Text.
    press(Key::up);
    press(Key::cancel);
    if (store.data().model.multiswitch[3].labels[0][0] != 0)
        return false;
    press(Key::ok);
    press(Key::up);
    press(Key::ok);
    press(Key::up);
    press(Key::ok_long);
    if (!settle(store) ||
        (store.data().model.multiswitch[3].labels[0] != std::array<char, 6>{'A', 'A'}))
        return false;
    press(Key::down);
    press(Key::ok);
    press(Key::up);
    press(Key::ok);
    if (!settle(store) || (store.data().model.multiswitch[3].momentary != 1))
        return false;
    auto &control = store.data().model.multiswitch[3].controls[0];
    press(Key::down); // Output: duplicates are rejected without changing the live model.
    press(Key::ok);
    press(Key::up);
    press(Key::ok);
    if (control.output() != 0)
        return false;
    {
        coretx::ui::Framebuffer display{}, expected{};
        menu.render(store, diagnostics, RadioOutput::off, display);
        expected.small_text(0, 58, coretx::ui::tr::multiswitch_duplicate);
        for (int y = 58; y < 64; ++y)
            for (int x = 0; x < 128; ++x)
                if (display.pixel_at(x, y) != expected.pixel_at(x, y)) {
                    std::fprintf(stderr, "MultiSwitch duplicate notice mismatch at %d,%d\n", x, y);
                    return false;
                }
    }
    press(Key::cancel);
    press(Key::down); // Own address 1, retaining page address 0.
    press(Key::ok);
    press(Key::up);
    press(Key::up);
    press(Key::ok);
    if (!settle(store) || (control.address() != 1))
        return false;
    press(Key::up);
    press(Key::ok);
    press(Key::up);
    press(Key::ok);
    if (!settle(store) || (control.output() != 1))
        return false;
    press(Key::down);
    press(Key::down); // Combined switch choice SA^.
    press(Key::ok);
    press(Key::up);
    {
        coretx::ui::Framebuffer display{}, expected{};
        menu.render(store, diagnostics, RadioOutput::off, display);
        const int y = fw::menu_detail::list_y + 3 * fw::menu_detail::row_height;
        expected.rectangle(0, y - 1, 128, fw::menu_detail::row_height);
        expected.text(101, y, "SA^", false);
        for (int row = y; row < (y + 8); ++row)
            for (int column = 101; column < 119; ++column)
                if (display.pixel_at(column, row) != expected.pixel_at(column, row))
                    return false;
        if (control.condition().enabled)
            return false;
    }
    press(Key::ok);
    if (!settle(store) || !control.condition().enabled || (control.condition().index != 0) ||
        (control.condition().position != coretx::SwitchPosition::up))
        return false;
    press(Key::down);
    press(Key::ok);
    press(Key::up);
    press(Key::ok);
    if (!settle(store) || (control.condition().position != coretx::SwitchPosition::down))
        return false;
    press(Key::ok);
    for (unsigned i = 0; i < (coretx::switch_count * fw::menu_detail::condition_positions); ++i)
        press(Key::up);
    press(Key::cancel);
    if ((control.condition().index != 0) ||
        (control.condition().position != coretx::SwitchPosition::down))
        return false;
    store.data().model.logical_switches[0].function = coretx::LogicalFunction::equal;
    if (!store.save() || !settle(store))
        return false;
    press(Key::ok);
    for (unsigned i = 0; i < (coretx::switch_count * fw::menu_detail::condition_positions); ++i)
        press(Key::up);
    press(Key::ok);
    if (!settle(store) || (control.condition().index != coretx::switch_count))
        return false;
    press(Key::ok);
    press(Key::down); // Logical false; reopening Switch must retain that active level.
    press(Key::ok);
    if (!settle(store) || (control.condition().position != coretx::SwitchPosition::up))
        return false;
    {
        auto overview = menu;
        (void)overview.handle(Key::cancel, store, diagnostics, RadioOutput::off);
        coretx::ui::Framebuffer display{}, expected{};
        overview.render(store, diagnostics, RadioOutput::off, display);
        const auto y = fw::menu_detail::list_y + fw::menu_detail::row_height;
        expected.rectangle(0, y - 1, 128, fw::menu_detail::row_height);
        expected.text(1, y, "2@1", false);
        for (int row = y; row < (y + 8); ++row)
            for (int column = 1; column < 19; ++column)
                if (display.pixel_at(column, row) != expected.pixel_at(column, row)) {
                    std::fprintf(stderr, "MultiSwitch target column mismatch at %d,%d\n", column,
                                 row);
                    return false;
                }
    }
    press(Key::ok);
    press(Key::ok);
    if (!settle(store) || (control.condition().position != coretx::SwitchPosition::up))
        return false;
    press(Key::cancel);
    press(Key::cancel);
    press(Key::cancel);
    for (unsigned i = 0; i < 7; ++i)
        press(Key::down);
    press(Key::ok);
    if (!settle(store) || !store.data().model.crsf_dashboard ||
        (diagnostics.dashboard_pages() != 4))
        return false;
    Store reboot{};
    if (!boot(reboot) || (reboot.data().model.multiswitch != store.data().model.multiswitch) ||
        !reboot.data().model.crsf_dashboard || reboot.data().model.multiswitch_beep)
        return false;
    if (!reboot.create(true) || !settle(reboot) ||
        (reboot.data().model.multiswitch[3].momentary != 1) || reboot.data().model.multiswitch_beep)
        return false;
    return reboot.create(false) && settle(reboot) && !reboot.data().model.crsf_dashboard &&
           reboot.data().model.multiswitch_beep &&
           (reboot.data().model.multiswitch == std::array<coretx::MultiSwitchPage, 4>{});
}

constexpr bool multiswitch_condition_choices() {
    fw::Diagnostics diagnostics{};
    auto model = fw::diagnostic_model;
    model.logical_switches[0].function = coretx::LogicalFunction::greater;
    diagnostics.use_model(model);
    coretx::InputSetup setup{};
    setup.modes.fill(coretx::InputMode::off);
    setup.modes[0] = coretx::InputMode::six_position;
    setup.modes[2] = coretx::InputMode::three_position;
    if (!diagnostics.set_input_setup(setup))
        return false;
    int choice = -1;
    const auto step = [&](const int direction) constexpr {
        choice = fw::menu_detail::change_condition(choice, direction, diagnostics);
        return fw::menu_detail::select_condition(choice, false);
    };
    for (int position = 2; position <= 7; ++position) {
        const auto condition = step(1);
        if (!condition.enabled || (condition.index != 0) ||
            (static_cast<int>(condition.position) != position))
            return false;
    }
    for (int position = -1; position <= 1; ++position) {
        const auto condition = step(1);
        if ((condition.index != 2) || (static_cast<int>(condition.position) != position))
            return false;
    }
    const auto logical_false = step(1);
    const auto logical_true = step(1);
    if ((logical_false.index != coretx::switch_count) ||
        (logical_false.position != coretx::SwitchPosition::up) ||
        (logical_true.index != coretx::switch_count) ||
        (logical_true.position != coretx::SwitchPosition::down) ||
        !fw::menu_detail::select_condition(choice, true).inverted)
        return false;
    const auto last = choice;
    (void)step(1);
    if (choice != last)
        return false;
    for (unsigned i = 0; i < 11; ++i)
        (void)step(-1);
    if (choice != -1)
        return false;
    setup.modes.fill(coretx::InputMode::off);
    model.logical_switches[0].function = coretx::LogicalFunction::disabled;
    return diagnostics.set_input_setup(setup) &&
           (fw::menu_detail::change_condition(-1, 1, diagnostics) == -1);
}

bool dashboard_feature_gates() {
    struct Scenario {};
    using Test = coretx::test::FakeApplication<Scenario, Context, Memory>;
    using Store = fw::ModelStorage<Test::Board>;
    Test::reset();
    Store store{};
    if (!boot(store))
        return false;
    store.data().model.crsf_dashboard = true;
    store.data().model.multiswitch[0].address = 42;
    fw::Diagnostics diagnostics{};
    diagnostics.use_model(store.data().model);
    const auto expected = fw::Diagnostics::page_count +
                          (coretx::firmware::features::crsf ? 1U : 0U) +
                          (coretx::firmware::features::multiswitch ? 1U : 0U);
    if (diagnostics.dashboard_pages() != expected)
        return false;
    fw::Menu menu{};
    menu.open();
    const auto press = [&](const Key key) {
        (void)menu.handle(key, store, diagnostics, RadioOutput::off);
    };
    press(Key::down);
    press(Key::ok);
    for (std::size_t i = 0; i < fw::menu_detail::trim_row; ++i)
        press(Key::down);
    press(Key::ok);
    if (menu.screen() != fw::MenuScreen::trims)
        return false;
    press(Key::cancel);
    const auto destination =
        coretx::firmware::features::crsf ? fw::menu_detail::crsf_dashboard_row : 5U;
    for (std::size_t i = 0; i < destination; ++i)
        press(Key::down);
    press(Key::ok);
    if constexpr (coretx::firmware::features::crsf)
        return (menu.screen() == fw::MenuScreen::setup) && !store.data().model.crsf_dashboard &&
               settle(store);
    else
        return (menu.screen() == fw::MenuScreen::logical_switches) &&
               store.data().model.crsf_dashboard;
}

bool dashboard_and_logical_menu_persistence() {
    struct Scenario {};
    using Test = coretx::test::FakeApplication<Scenario, Context, Memory>;
    using Store = fw::ModelStorage<Test::Board>;
    Test::reset();
    Store store{};
    if (!boot(store))
        return false;
    fw::Diagnostics diagnostics{};
    diagnostics.use_model(store.data().model);
    fw::Menu menu{};
    menu.open();
    const auto press = [&](const Key key) {
        (void)menu.handle(key, store, diagnostics, RadioOutput::off);
    };
    press(Key::down); // Model setup.
    press(Key::ok);
    for (int i = 0; i < 4; ++i)
        press(Key::down);
    press(Key::ok);
    if (menu.screen() != fw::MenuScreen::dashboard_channels)
        return false;
    press(Key::ok);
    for (int i = 0; i < 15; ++i)
        press(Key::up);
    press(Key::cancel);
    if (store.data().model.dashboard_channels[0] != 1)
        return false;
    press(Key::ok);
    for (int i = 0; i < 15; ++i)
        press(Key::up);
    press(Key::ok);
    if (!settle(store) || (store.data().model.dashboard_channels[0] != 16))
        return false;
    press(Key::down);
    press(Key::ok);
    press(Key::down);
    press(Key::down);
    press(Key::ok);
    if (!settle(store) || (store.data().model.dashboard_channels[1] != 0))
        return false;
    press(Key::cancel);
    for (int i = 0; i < 5; ++i)
        press(Key::down);
    press(Key::ok);
    press(Key::ok);
    if (menu.screen() != fw::MenuScreen::logical_editor)
        return false;
    press(Key::ok);
    for (int i = 0; i < 10; ++i)
        press(Key::up); // SRFF.
    press(Key::ok);
    press(Key::down);
    press(Key::down);
    press(Key::ok);
    press(Key::up);
    press(Key::up); // A down.
    press(Key::ok);
    press(Key::down);
    press(Key::down);
    press(Key::ok);
    press(Key::up); // B = SB.
    press(Key::ok);
    press(Key::down);
    press(Key::ok);
    press(Key::up);
    press(Key::up); // B down.
    press(Key::ok_long);
    if (!settle(store) || (menu.screen() != fw::MenuScreen::logical_switches) ||
        (store.data().model.logical_switches[0].function != coretx::LogicalFunction::srff))
        return false;
    // Draft cycles must not enter the model or EEPROM.
    press(Key::ok);
    press(Key::down);
    press(Key::ok);
    for (std::size_t i = 0; i < coretx::switch_count; ++i)
        press(Key::up);
    press(Key::ok_long);
    if ((menu.screen() != fw::MenuScreen::logical_editor) ||
        (store.data().model.logical_switches[0].a != 0) || store.busy())
        return false;
    press(Key::cancel_long);
    Store reboot{};
    if (!boot(reboot) || (reboot.data().model.dashboard_channels[0] != 16) ||
        (reboot.data().model.dashboard_channels[1] != 0) ||
        (reboot.data().model.logical_switches[0].b != 1))
        return false;
    coretx::Controls controls{};
    coretx::LogicalState live{};
    controls.switches[0] = coretx::SwitchPosition::down;
    (void)coretx::evaluate(reboot.data().model, controls, live);
    controls.switches[0] = coretx::SwitchPosition::up;
    if ((live.latches != 1) ||
        ((coretx::evaluate(reboot.data().model, controls).frame.logical.active & 1U) != 0))
        return false;
    if (!reboot.create(true) || !settle(reboot) ||
        (reboot.data().model.logical_switches[0].function != coretx::LogicalFunction::srff) ||
        (reboot.data().model.dashboard_channels[0] != 16))
        return false;
    if (!reboot.create(false) || !settle(reboot) ||
        (reboot.data().model.logical_switches[0].function != coretx::LogicalFunction::disabled) ||
        (reboot.data().model.dashboard_channels[0] != 1))
        return false;
    return reboot.select(0) && settle(reboot) && (reboot.data().model.dashboard_channels[0] == 16);
}

bool model_management() {
    struct Scenario {};
    using Test = coretx::test::FakeApplication<Scenario, Context, Memory>;
    using Store = fw::ModelStorage<Test::Board>;
    Test::reset();
    auto &c = Test::context;
    std::fill_n(c.bytes.begin(), 2176, 0x5A);
    auto legacy = fw::diagnostic_model;
    legacy.inputs[0].weight_percent = coretx::InputWeight::constant<73>();
    Store store{};
    if (!boot(store, legacy) || (store.active() != 0) || !store.entries()[0].present ||
        (store.data().model.inputs[0].weight_percent.value() != 73) ||
        (store.data().radio != legacy_radio))
        return false;
    if (!store.create(false) || !settle(store) || (store.active() != 1) ||
        (store.data().model.inputs.size() != 4) || (store.data().radio.receiver.value != 0) ||
        (store.data().radio.channel_mode != ChannelMode::extended16))
        return false;
    store.data().radio.receiver = {0x10203040};
    store.data().baud_index = 4;
    if (!store.save() || !settle(store) || !store.select(0) || !settle(store) ||
        (store.data().radio != legacy_radio))
        return false;
    if (!store.create(true, 1) || !settle(store) || (store.active() != 2) ||
        (store.data().radio.receiver.value != 0x10203040) || (store.data().baud_index != 4))
        return false;
    const md::Name name{'G', 'l', 'i', 'd', 'e', 'r'};
    if (!store.rename(1, name) || !settle(store) || (store.active() != 2) ||
        (store.entries()[1].name != name))
        return false;
    if (store.erase(2) || !store.erase(0) || !settle(store) || store.entries()[0].present)
        return false;
    if (!store.create(false) || !settle(store) || (store.active() != 0))
        return false;
    for (int i = 0; i < 5; ++i)
        if (!store.create(false) || !settle(store))
            return false;
    if (store.create(false))
        return false;
    struct rebootScenario {};
    using rebootTest = coretx::test::FakeApplication<rebootScenario, Context, Memory>;
    rebootTest::reset();
    rebootTest::context.bytes = c.bytes;
    fw::ModelStorage<rebootTest::Board> reboot{};
    if (!boot(reboot) || (reboot.active() != store.active()) ||
        (reboot.entries()[1].name != name) || !reboot.select(1) || !settle(reboot) ||
        (reboot.data().radio.receiver.value != 0x10203040) || (reboot.data().baud_index != 4))
        return false;
    return std::all_of(c.bytes.begin(), c.bytes.begin() + 2176, [](auto b) { return b == 0x5A; });
}

bool interrupted_journals() {
    for (int operation = 0; operation < 3; ++operation) {
        struct Scenario {};
        using Test = coretx::test::FakeApplication<Scenario, Context, Memory>;
        using Store = fw::ModelStorage<Test::Board>;
        Test::reset();
        auto &c = Test::context;
        Store store{};
        if (!boot(store))
            return false;
        if (operation == 0) {
            store.data().model.inputs[0].weight_percent = coretx::InputWeight::constant<61>();
            store.data().model.dashboard_channels[0] = 16;
            store.data().model.logical_switches[0].function = coretx::LogicalFunction::srff;
            if (!store.save())
                return false;
        } else if (operation == 1) {
            if (!store.create(false))
                return false;
        } else {
            if (!store.create(false) || !settle(store) || !store.select(0))
                return false;
        }
        if (operation < 2) {
            for (int i = 0; i < 10; ++i)
                tick(store);
        } else {
            // Target record has been loaded; interrupt directory commit after invalidation.
            for (int i = 0; i < 13; ++i)
                tick(store);
        }
        struct rebootScenario {};
        using rebootTest = coretx::test::FakeApplication<rebootScenario, Context, Memory>;
        rebootTest::reset();
        rebootTest::context.bytes = c.bytes;
        fw::ModelStorage<rebootTest::Board> reboot{};
        if (!boot(reboot) || (reboot.active() != (operation == 2 ? 1U : 0U)) ||
            ((operation == 0) && (reboot.data().model.inputs[0].weight_percent.value() != 100)) ||
            ((operation == 0) && ((reboot.data().model.dashboard_channels[0] != 1) ||
                                  (reboot.data().model.logical_switches[0].function !=
                                   coretx::LogicalFunction::disabled))) ||
            ((operation == 1) && reboot.entries()[1].present))
            return false;
    }
    return true;
}

bool failures_and_wrap() {
    struct Scenario {};
    using Test = coretx::test::FakeApplication<Scenario, Context, Memory>;
    using Store = fw::ModelStorage<Test::Board>;
    Test::reset();
    auto &c = Test::context;
    Store store{};
    if (!boot(store))
        return false;
    store.data().model.inputs[0].weight_percent = coretx::InputWeight::constant<42>();
    c.corrupt_verify = true;
    if (!store.save())
        return false;
    for (int i = 0; i < 100; ++i)
        tick(store);
    if (store.editable() || store.save() || (store.message()[0] == 0))
        return false;
    struct WrappedScenario {};
    using WrappedTest = coretx::test::FakeApplication<WrappedScenario, Context, Memory>;
    using WrappedStore = fw::ModelStorage<WrappedTest::Board>;
    WrappedTest::reset();
    auto &wrapped = WrappedTest::context;
    md::Data data{};
    data.radio = legacy_radio;
    md::Record old{}, current{};
    if (!md::encode(data, 0, true, 0xFFFFFFFF, old))
        return false;
    data.baud_index = 5;
    if (!md::encode(data, 0, true, 0, current))
        return false;
    std::copy(old.begin(), old.end(), wrapped.bytes.begin() + md::base);
    std::copy(current.begin(), current.end(), wrapped.bytes.begin() + md::base + md::record_size);
    WrappedStore resumed{};
    if (!boot(resumed) || (resumed.data().baud_index != 5))
        return false;
    current[4] = md::format_version + 1;
    coretx::storage::detail::Writer checksum{std::span{current}.last(4)};
    checksum.word(coretx::storage::crc32(std::span{current}.first(md::record_size - 4)));
    std::copy(current.begin(), current.end(), wrapped.bytes.begin() + md::base + md::record_size);
    const auto before = wrapped.bytes;
    WrappedStore newer{};
    newer.initialize(tx);
    (void)settle(newer);
    return !newer.editable() && !newer.save() && (before == wrapped.bytes);
}

bool queued_save_and_directory_version() {
    struct Scenario {};
    using Test = coretx::test::FakeApplication<Scenario, Context, Memory>;
    using Store = fw::ModelStorage<Test::Board>;
    Test::reset();
    auto &c = Test::context;
    Store store{};
    if (!boot(store))
        return false;
    store.data().radio.receiver = {0x10203040};
    if (!store.save())
        return false;
    for (int i = 0; i < 6; ++i)
        tick(store);
    store.data().radio.receiver = {0x11223344};
    if (!store.save() || !settle(store))
        return false;
    struct rebootScenario {};
    using rebootTest = coretx::test::FakeApplication<rebootScenario, Context, Memory>;
    rebootTest::reset();
    rebootTest::context.bytes = c.bytes;
    fw::ModelStorage<rebootTest::Board> reboot{};
    if (!boot(reboot) || (reboot.data().radio.receiver.value != 0x11223344))
        return false;
    // Future directory versions may also increase the number of model slots.
    std::array<std::uint8_t, 64> directory{};
    directory.fill(0xFF);
    coretx::storage::detail::Writer out{directory};
    out.word(md::directory_magic);
    out.field(std::uint8_t{2});
    out.field(std::uint8_t{20});
    out.word(std::uint32_t{9});
    coretx::storage::detail::Writer crc{std::span{directory}.last(4)};
    crc.word(coretx::storage::crc32(std::span{directory}.first(60)));
    std::copy(directory.begin(), directory.end(), c.bytes.begin() + md::directory_base);
    const auto before = c.bytes;
    Store future{};
    future.initialize(tx);
    (void)settle(future);
    return !future.editable() && !future.save() && future.can_initialize() && (before == c.bytes);
}

bool trim_menu_persistence() {
    struct Scenario {};
    using Test = coretx::test::FakeApplication<Scenario, Context, Memory>;
    using Store = fw::ModelStorage<Test::Board>;
    Test::reset();
    Store store{};
    if (!boot(store))
        return false;
    fw::Diagnostics diagnostics{};
    diagnostics.use_model(store.data().model);
    fw::Menu menu{};
    menu.open();
    const auto press = [&](Key key, RadioOutput output = coretx::firmware::features::secure_ui
                                                             ? RadioOutput::off
                                                             : RadioOutput::external) {
        (void)menu.handle(key, store, diagnostics, output);
    };
    press(Key::down);
    press(Key::ok);
    for (std::size_t i = 0; i < fw::menu_detail::trim_row; ++i)
        press(Key::down);
    press(Key::ok);
    if (menu.screen() != fw::MenuScreen::trims)
        return false;
    if constexpr (!coretx::firmware::features::i6s) {
        if constexpr (coretx::firmware::features::secure_ui) {
            press(Key::ok, RadioOutput::external);
            if (store.data().model.trim_switches != 0)
                return false;
        }
        press(Key::ok);
        press(Key::up);
        press(Key::cancel);
        if (store.data().model.trim_switches != 0)
            return false;
        press(Key::ok);
        press(Key::up);
        press(Key::ok);
        if (!settle(store) || (store.data().model.trim_switches != 1))
            return false;
        press(Key::down);
    }
    press(Key::ok, RadioOutput::external);
    for (unsigned i = 0; i < 10; ++i)
        press(Key::up, RadioOutput::external);
    press(Key::ok, RadioOutput::external);
    if (!settle(store))
        return false;
    Store reboot{};
    return boot(reboot) && (reboot.data().model.trims[0].value() == 10) &&
           (reboot.data().model.trim_switches == (coretx::firmware::features::i6s ? 0 : 1));
}

bool rf_ui_interlocks() {
    struct Scenario {};
    using Test = coretx::test::FakeApplication<Scenario, Context, Memory>;
    Test::reset();
    fw::ModelStorage<Test::Board> store{};
    if (!boot(store))
        return false;
    fw::Diagnostics diagnostics{};
    diagnostics.use_model(store.data().model);
    for (const auto output : {RadioOutput::off, RadioOutput::internal, RadioOutput::external}) {
        const bool allowed = !coretx::firmware::features::secure_ui || (output == RadioOutput::off);
        for (const auto &[row, command] : {std::pair{1U, fw::MenuCommand::calibrate},
                                           std::pair{2U, fw::MenuCommand::input_setup}}) {
            fw::Menu menu{};
            menu.open();
            const auto press = [&](const Key key) {
                return menu.handle(key, store, diagnostics, output);
            };
            (void)press(Key::down);
            (void)press(Key::down);
            (void)press(Key::ok);
            for (unsigned i = 0; i < row; ++i)
                (void)press(Key::down);
            if (press(Key::ok) != (allowed ? command : fw::MenuCommand::none))
                return false;
        }
        {
            fw::Menu menu{};
            menu.open();
            const auto press = [&](const Key key) {
                return menu.handle(key, store, diagnostics, output);
            };
            (void)press(Key::down);
            (void)press(Key::down);
            (void)press(Key::ok);
            (void)press(Key::ok);
            if (press(Key::ok) != (allowed ? fw::MenuCommand::stick_mode : fw::MenuCommand::none))
                return false;
        }
        if constexpr (coretx::firmware::features::a7105 || coretx::firmware::features::crsf) {
            fw::Menu menu{};
            menu.open();
            const auto press = [&](const Key key) {
                return menu.handle(key, store, diagnostics, output);
            };
            (void)press(Key::down);
            (void)press(Key::ok);
            for (unsigned i = 0; i < 3; ++i)
                (void)press(Key::down);
            (void)press(Key::ok);
            (void)press(Key::down);
            (void)press(Key::ok);
            const auto command =
                coretx::firmware::features::a7105 ? fw::MenuCommand::format : fw::MenuCommand::baud;
            if (press(Key::ok) != (allowed ? command : fw::MenuCommand::none))
                return false;
            if constexpr (coretx::firmware::features::a7105 && coretx::firmware::features::crsf) {
                (void)press(Key::down);
                (void)press(Key::down);
                (void)press(Key::ok);
                if (press(Key::ok) != (allowed ? fw::MenuCommand::baud : fw::MenuCommand::none))
                    return false;
            }
        }
    }
    fw::Menu menu{};
    menu.open();
    for (const auto key : {Key::ok, Key::ok, Key::down})
        (void)menu.handle(key, store, diagnostics, RadioOutput::internal);
    (void)menu.handle(Key::ok, store, diagnostics, RadioOutput::internal);
    if constexpr (coretx::firmware::features::secure_ui)
        return !store.busy() && !store.entries()[1].present;
    else
        return settle(store) && store.entries()[1].present;
}

bool initialize_foreign_memory() {
    struct Scenario {};
    using Test = coretx::test::FakeApplication<Scenario, Context, Memory>;
    using Store = fw::ModelStorage<Test::Board>;
    Test::reset();
    auto &c = Test::context;
    std::fill(c.bytes.begin() + md::base, c.bytes.begin() + md::directory_base + 128, 0);
    const auto prefix = c.bytes;
    Store store{};
    store.initialize(tx);
    (void)settle(store);
    if (store.editable() || !store.can_initialize() || !store.initialize_models() ||
        !settle(store) || (store.data().radio.output != RadioOutput::off) ||
        !store.entries()[0].present)
        return false;
    struct restoredScenario {};
    using restoredTest = coretx::test::FakeApplication<restoredScenario, Context, Memory>;
    restoredTest::reset();
    restoredTest::context.bytes = c.bytes;
    fw::ModelStorage<restoredTest::Board> restored{};
    return boot(restored) && (restored.active() == 0) &&
           std::equal(c.bytes.begin(), c.bytes.begin() + 2176, prefix.begin()) &&
           (restored.data().radio.receiver.value == 0) && (restored.data().model.mixes.size() == 4);
}

bool interrupted_model_reset() {
    for (const unsigned cut : {1U, 2U, 5U, 50U, 400U, 440U, 450U, 470U}) {
        struct Scenario {};
        using Test = coretx::test::FakeApplication<Scenario, Context, Memory>;
        using Store = fw::ModelStorage<Test::Board>;
        Test::reset();
        auto &c = Test::context;
        std::fill(c.bytes.begin(), c.bytes.begin() + md::base, 0x37);
        std::fill(c.bytes.begin() + md::base, c.bytes.begin() + md::directory_base, 0x55);
        const auto before = c.bytes;
        Store store{};
        store.initialize(tx);
        (void)settle(store);
        if (!store.can_initialize() || !store.initialize_models())
            return false;
        for (unsigned i = 0; i < cut; ++i)
            tick(store);
        const bool marked = std::any_of(c.bytes.begin() + fw::model_migration::address,
                                        c.bytes.begin() + fw::model_migration::address +
                                            fw::model_migration::record_size,
                                        [](const auto b) { return b != 0xFF; });
        const auto interrupted = c.bytes;
        Store reboot{};
        reboot.initialize(tx);
        (void)settle(reboot);
        if (marked) {
            if (reboot.editable() || !reboot.recovery_required() || reboot.save() ||
                (c.bytes != interrupted) || !reboot.initialize_models() || !settle(reboot))
                return false;
        } else if (!reboot.editable())
            return false;
        if ((reboot.data().model.mixes.size() != 4) ||
            (reboot.data().radio.output != RadioOutput::off) ||
            (reboot.data().radio.receiver.value != 0) ||
            !std::equal(c.bytes.begin(), c.bytes.begin() + md::base, before.begin()))
            return false;
    }
    return true;
}

bool pending_migration_blocks_models() {
    for (unsigned variant = 0; variant < 3; ++variant) {
        struct Scenario {};
        using Test = coretx::test::FakeApplication<Scenario, Context, Memory>;
        Test::reset();
        auto &c = Test::context;
        md::Data data{};
        data.radio = legacy_radio;
        md::Record record{};
        if (!md::encode(data, 0, true, 0, record))
            return false;
        std::copy(record.begin(), record.end(), c.bytes.begin() + md::base);
        fw::model_migration::Record marker{};
        fw::model_migration::encode(variant == 0 ? fw::model_migration::Kind::conversion
                                                 : fw::model_migration::Kind::reset,
                                    md::format_version, marker);
        if (variant == 2)
            marker[0] = 0; // A torn marker must also block startup, without guessing its meaning.
        std::copy(marker.begin(), marker.end(), c.bytes.begin() + fw::model_migration::address);
        const auto before = c.bytes;
        fw::ModelStorage<Test::Board> store{};
        store.initialize(tx);
        (void)settle(store);
        if (store.editable() || !store.recovery_required() || store.take_changed() ||
            store.save() || (c.bytes != before) ||
            (store.message() != coretx::ui::tr::models_migration))
            return false;
    }
    return true;
}

bool stick_modes() {
    fw::Diagnostics diagnostics{};
    coretx::ControlSetup setup{};
    for (auto &axis : setup.calibration)
        axis = {0, 2048, 4095};
    setup.mapping[0].reversed = setup.mapping[1].reversed = true;
    if (!diagnostics.restore_calibration(setup))
        return false;
    fw::StockSamples samples{};
    samples.fill(2048);
    samples[0] = 0;
    samples[2] = 4095;
    samples[3] = 1024;
    diagnostics.observe(samples, coretx::TimestampUs{100});
    constexpr std::array<std::array<std::int16_t, 4>, 4> expected{
        std::array<std::int16_t, 4>{1024, 1024, 0, -512},
        {1024, 0, 1024, -512},
        {-512, 0, 1024, 1024},
        {-512, 1024, 0, 1024}};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (!diagnostics.set_stick_mode(i + 1) ||
            !fw::Diagnostics::valid_setup(diagnostics.setup()) ||
            (diagnostics.stick_mode() != (i + 1)) ||
            !std::equal(expected[i].begin(), expected[i].end(),
                        diagnostics.prepared().controls.analogs.begin()))
            return false;
    }
    setup.mapping[0] = {1, true};
    setup.mapping[1] = {0, true};
    return !fw::Diagnostics::valid_setup(setup) && !diagnostics.set_stick_mode(0);
}

bool menu_scrolling() {
    struct Scenario {};
    using Test = coretx::test::FakeApplication<Scenario, Context, Memory>;
    using Store = fw::ModelStorage<Test::Board>;
    Test::reset();
    Store store{};
    if (!boot(store))
        return false;
    fw::Diagnostics diagnostics{};
    fw::Menu menu{};
    menu.open();
    const auto press = [&](const Key key) {
        (void)menu.handle(key, store, diagnostics, RadioOutput::off);
    };
    press(Key::ok);
    press(Key::ok);
    for (int i = 0; i < 4; ++i)
        press(Key::down);
    coretx::ui::Framebuffer display{}, expected{};
    menu.render(store, diagnostics, RadioOutput::off, display);
    expected.rectangle(0, 41, 128, 10);
    expected.text(1, 42, coretx::ui::tr::menu_delete, false);
    for (int y = 41; y < 51; ++y)
        for (int x = 0; x < 128; ++x)
            if (display.pixel_at(x, y) != expected.pixel_at(x, y))
                return false;
    return true;
}

bool editor_navigation() {
    struct Scenario {};
    using Test = coretx::test::FakeApplication<Scenario, Context, Memory>;
    using Store = fw::ModelStorage<Test::Board>;
    Test::reset();
    Store store{};
    if (!boot(store))
        return false;
    fw::Diagnostics diagnostics{};
    coretx::ControlSetup setup{};
    for (auto &axis : setup.calibration)
        axis = {0, 2048, 4095};
    setup.mapping[0].reversed = setup.mapping[1].reversed = true;
    if (!diagnostics.restore_calibration(setup))
        return false;
    diagnostics.use_model(store.data().model);
    fw::StockSamples samples{};
    samples.fill(2048);
    samples[0] = 0;
    diagnostics.observe(samples, coretx::TimestampUs{1000});
    const auto initial = diagnostics.evaluation().frame.channels[0];
    fw::Menu menu{};
    menu.open();
    coretx::ui::Framebuffer root{};
    menu.render(store, diagnostics, RadioOutput::off, root);
    const auto press = [&](const Key key) {
        (void)menu.handle(key, store, diagnostics, RadioOutput::off);
    };
    press(Key::down);
    press(Key::ok);
    press(Key::ok);
    press(Key::ok);
    press(Key::down);
    press(Key::ok);
    press(Key::down);
    if ((store.data().model.inputs[0].weight_percent.value() != 99) ||
        (diagnostics.evaluation().frame.channels[0] == initial))
        return false;
    press(Key::cancel_long);
    if (menu.visible() || (store.data().model.inputs[0].weight_percent.value() != 100) ||
        (diagnostics.evaluation().frame.channels[0] != initial))
        return false;
    menu.open();
    press(Key::down);
    press(Key::ok);
    press(Key::ok);
    press(Key::ok_long);
    press(Key::down);
    press(Key::ok);
    if ((store.data().model.inputs.size() != 7) || !settle(store))
        return false;
    coretx::ui::Framebuffer display{};
    menu.render(store, diagnostics, RadioOutput::off, display);
    if ((display.character_width() != 6) || (display.character_height() != 10))
        return false;
    press(Key::cancel_long);
    menu.open();
    press(Key::ok);
    press(Key::ok);
    const auto active = store.active();
    (void)menu.handle(Key::ok, store, diagnostics, RadioOutput::internal);
    if ((store.active() != active) || store.busy())
        return false;
    coretx::ui::Framebuffer upper{}, lower{};
    upper.compact();
    lower.compact();
    upper.text(0, 0, "ABC");
    lower.text(0, 0, "abc");
    if (upper.bytes() == lower.bytes())
        return false;
    upper.clear();
    upper.text(-2, -1, "abc");
    upper.text(127, 63, "XYZ");
    return true;
}

bool curves_menu_persistence() {
    struct Scenario {};
    using Test = coretx::test::FakeApplication<Scenario, Context, Memory>;
    using Store = fw::ModelStorage<Test::Board>;
    Test::reset();
    Store store{};
    if (!boot(store))
        return false;
    store.data().model.inputs[0].curve = 4;
    store.data().model.mixes[0].curve = 2;
    if (!store.save() || !settle(store))
        return false;
    fw::Diagnostics diagnostics{};
    diagnostics.use_model(store.data().model);
    fw::Menu menu{};
    menu.open();
    const auto press = [&](const Key key) {
        (void)menu.handle(key, store, diagnostics, RadioOutput::off);
    };
    press(Key::down);
    press(Key::ok);
    for (std::size_t i = 0; i < fw::menu_detail::curves_row; ++i)
        press(Key::down);
    press(Key::ok);
    if (menu.screen() != fw::MenuScreen::curves)
        return false;
    for (int i = 0; i < 3; ++i)
        press(Key::down);
    press(Key::ok);
    if (menu.screen() != fw::MenuScreen::editor)
        return false;
    press(Key::down);
    press(Key::down);
    press(Key::ok);
    for (int i = 0; i < 5; ++i)
        press(Key::up);
    press(Key::ok);
    if (!settle(store) || (store.data().model.curves[3].y[2] != 5))
        return false;
    press(Key::ok_long);
    press(Key::ok);
    for (int i = 0; i < 100; ++i)
        press(Key::up);
    if (store.data().model.curves[3].x[1] != 49)
        return false;
    press(Key::cancel);
    if (menu.pending_model_edit() || (store.data().model.curves[3].x[1] != 0))
        return false;
    press(Key::ok);
    for (int i = 0; i < 10; ++i)
        press(Key::up);
    press(Key::ok);
    if (!settle(store))
        return false;
    press(Key::up);
    press(Key::up);
    press(Key::ok);
    if (menu.pending_model_edit())
        return false;
    coretx::ui::Framebuffer display{};
    menu.render(store, diagnostics, RadioOutput::off, display);
    if ((display.character_width() != 6) || !display.pixel_at(125, 12))
        return false;
    press(Key::cancel);
    if (menu.screen() != fw::MenuScreen::curves)
        return false;
    Store reboot{};
    return boot(reboot) && (reboot.data().model.curves[3].x[1] == 10) &&
           (reboot.data().model.curves[3].y[2] == 5) &&
           (reboot.data().model.inputs[0].curve == 4) && (reboot.data().model.mixes[0].curve == 2);
}

#if CORETX_MODUL_TANKMIX
bool tank_menu_persistence() {
    struct Scenario {};
    using Test = coretx::test::FakeApplication<Scenario, Context, Memory>;
    using Store = fw::ModelStorage<Test::Board>;
    Test::reset();
    Store store{};
    if (!boot(store))
        return false;
    fw::Diagnostics diagnostics{};
    diagnostics.use_model(store.data().model);
    fw::Menu menu{};
    menu.open();
    const auto press = [&](const Key key) {
        (void)menu.handle(key, store, diagnostics, RadioOutput::off);
    };
    press(Key::down);
    press(Key::ok);
    for (std::size_t i = 0; i < fw::menu_detail::tank_row; ++i)
        press(Key::down);
    press(Key::ok);
    if (menu.screen() != fw::MenuScreen::editor)
        return false;
    press(Key::ok);
    press(Key::up);
    press(Key::ok);
    if (store.data().model.tank.enabled || !menu.pending_model_edit())
        return false;
    press(Key::cancel);
    press(Key::down);
    press(Key::down);
    press(Key::down);
    press(Key::ok);
    for (int i = 0; i < 10; ++i)
        press(Key::up);
    press(Key::ok);
    if (!settle(store))
        return false;
    press(Key::down);
    press(Key::ok);
    for (int i = 0; i < 10; ++i)
        press(Key::up);
    press(Key::ok);
    if (!settle(store))
        return false;
    for (int i = 0; i < 4; ++i)
        press(Key::up);
    press(Key::ok);
    press(Key::up);
    press(Key::ok);
    if (!settle(store) || !store.data().model.tank.enabled)
        return false;
    for (int i = 0; i < 5; ++i)
        press(Key::down);
    press(Key::ok);
    for (int i = 0; i < 25; ++i)
        press(Key::down);
    press(Key::ok);
    if (!settle(store) || (store.data().model.tank.steering_percent != 75))
        return false;
    press(Key::down);
    press(Key::ok);
    press(Key::up);
    press(Key::up);
    press(Key::ok);
    if (!settle(store))
        return false;
    press(Key::down);
    press(Key::ok);
    press(Key::down);
    press(Key::cancel);
    if (!store.data().model.tank.reverse_steering)
        return false;
    press(Key::ok);
    press(Key::down);
    press(Key::ok);
    if (!settle(store) || store.data().model.tank.reverse_steering)
        return false;
    Store reboot{};
    if (!boot(reboot) || !reboot.data().model.tank.enabled ||
        reboot.data().model.tank.reverse_steering ||
        (reboot.data().model.tank.hysteresis_percent != 5) ||
        (reboot.data().model.tank.steering_percent != 75))
        return false;
    diagnostics.use_model(reboot.data().model);
    menu.open();
    const auto next = [&](const Key key) {
        (void)menu.handle(key, reboot, diagnostics, RadioOutput::off);
    };
    next(Key::down);
    next(Key::ok);
    next(Key::down);
    next(Key::ok);
    std::size_t managed{};
    while ((managed < reboot.data().model.mixes.size()) &&
           (reboot.data().model.mixes[managed].role == coretx::MixRole::manual)) {
        next(Key::down);
        ++managed;
    }
    if (managed == reboot.data().model.mixes.size())
        return false;
    next(Key::ok);
    next(Key::ok);
    if (menu.pending_model_edit())
        return false;
    next(Key::cancel);
    next(Key::ok_long);
    next(Key::down);
    next(Key::ok);
    if (reboot.data().model.mixes.size() != store.data().model.mixes.size())
        return false;
    coretx::ui::Framebuffer display{};
    menu.render(reboot, diagnostics, RadioOutput::off, display);
    next(Key::cancel);
    next(Key::cancel);
    for (std::size_t i = 1; i < fw::menu_detail::tank_row; ++i)
        next(Key::down);
    next(Key::ok);
    next(Key::ok);
    next(Key::down);
    next(Key::ok);
    if (!settle(reboot) || reboot.data().model.tank.enabled)
        return false;
    for (const auto &line : reboot.data().model.mixes)
        if (line.role != coretx::MixRole::manual)
            return false;
    Store disabled{};
    return boot(disabled) && !disabled.data().model.tank.enabled &&
           (disabled.data().model.tank.steering_percent == 75) &&
           (disabled.data().model.tank.left_channel == 10);
}
#else
bool tank_module_disabled() {
    struct Scenario {};
    using Test = coretx::test::FakeApplication<Scenario, Context, Memory>;
    using Store = fw::ModelStorage<Test::Board>;
    namespace codec = coretx::storage::detail;
    Test::reset();
    Store store{};
    if (!boot(store))
        return false;
    fw::Diagnostics diagnostics{};
    diagnostics.use_model(store.data().model);
    fw::Menu menu{};
    menu.open();
    const auto press = [&](const Key key) {
        (void)menu.handle(key, store, diagnostics, RadioOutput::off);
    };
    press(Key::down);
    press(Key::ok);
    for (unsigned i = 0; i < 30; ++i)
        press(Key::down);
    press(Key::ok);
    if (menu.screen() != fw::MenuScreen::curves)
        return false;
    md::Data data{};
    data.radio = legacy_radio;
    data.model.tank.steering_percent = 73;
    data.model.tank.reverse_steering = false;
    md::Record record{};
    md::Data restored{};
    md::Info info{};
    if (!legacy_curve_record(data, 9, record) || (md::decode(record, 7, tx, restored, info) != 1) ||
        (restored.model.tank != data.model.tank))
        return false;
    codec::Reader cursor{std::span{record}.first(md::record_size - 4), 34};
    if (!codec::transfer_model_counts(cursor, restored.model) ||
        !codec::transfer_model_lines(cursor, restored.model, true) ||
        !codec::transfer_model_extension(cursor, restored.model) ||
        !codec::transfer_multiswitch(cursor, restored.model, true) ||
        !codec::transfer_dashboard_options(cursor, restored.model, true) ||
        !codec::transfer_multiswitch_controls(cursor, restored.model) ||
        !md::transfer_curves(cursor, restored.model, true))
        return false;
    record[cursor.position] |= 1;
    codec::Writer crc{std::span{record}.last(4)};
    crc.word(coretx::storage::crc32(std::span{record}.first(md::record_size - 4)));
    if (md::decode(record, 7, tx, restored, info) != 4)
        return false;
    std::copy(record.begin(), record.end(),
              Test::context.bytes.begin() + md::base + 7 * 2 * md::record_size);
    const auto before = Test::context.bytes;
    Store reboot{};
    return !boot(reboot) && !reboot.editable() && !reboot.save() && (Test::context.bytes == before);
}
#endif
} // namespace

int main() {
    int failed{};
    const auto check = [&](const bool ok, const char *name) {
        if (!ok) {
            std::fprintf(stderr, "FAILED: %s\n", name);
            ++failed;
        }
    };
    check(codec(), "bounded model codec, CRC, identity and version");
#if CORETX_MODUL_TANKMIX
    check(tank_codec(), "maximum-size firmware tank settings and module ownership roundtrip");
#endif
    check(active_first_loading(), "all model banks are checked before exposing the active model");
    check(background_loading_errors(), "errors in inactive models block startup and all writes");
    check(obsolete_models_can_be_recreated(), "obsolete models rejected, explicit reset allowed");
    check(dashboard_feature_gates(), "dashboard pages and model menus follow feature options");
    check(trim_menu_persistence(), "trim modes, cancellation and values persist per model");
    check(rf_ui_interlocks(), "optional RF UI interlocks");
    check(multiswitch_menu_persistence(), "MultiSwitch edits, cancellation, reboot, copy/new");
    check(multiswitch_condition_choices(), "combined switch choices use available switch levels");
    check(dashboard_and_logical_menu_persistence(),
          "dashboard/LS edits, cancel, cycle, reboot, copy/new");
    check(model_management(), "eight models, copy, names, per-model RF and reboot");
    check(interrupted_journals(), "power loss during model and directory writes");
    check(failures_and_wrap(), "verify failure, generation wrap and newer version protection");
    check(queued_save_and_directory_version(), "queued receiver save and future directory layout");
    check(initialize_foreign_memory(),
          "explicit initialization preserves global settings, clearing models and receiver IDs");
    check(interrupted_model_reset(),
          "interrupted model resets remain blocked and can be restarted");
    check(pending_migration_blocks_models(), "pending or torn migration markers block startup");
    check(stick_modes(), "global stick modes preserve physical calibration and polarity");
    check(menu_scrolling(), "6x10 model actions scroll the fifth row into view");
    check(editor_navigation(), "compact menu, live editor, cancellation and RF-off gate");
    check(curves_menu_persistence(), "curve point editor, X/Y selection, cancellation and reboot");
#if CORETX_MODUL_TANKMIX
    check(tank_menu_persistence(), "tank setup, owned read-only mixers, disable and reboot");
#else
    check(tank_module_disabled(), "tank-free menu and unsupported-model write protection");
#endif
    return failed != 0;
}
