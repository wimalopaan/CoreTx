// SPDX-License-Identifier: GPL-2.0-or-later
#include "boards/common/crsf_menu.hpp"
#include "coretx/radio/crsf_config.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
namespace c = coretx::crsf;
namespace cfg = c::config;
using Bytes = std::vector<std::uint8_t>;

void require(const bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "CRSF config: %s\n", message);
        std::exit(EXIT_FAILURE);
    }
}
void string(Bytes &bytes, const char *text) {
    do {
        bytes.push_back(static_cast<std::uint8_t>(*text));
    } while (*text++ != '\0');
}
Bytes folder(const std::uint8_t parent, const Bytes &children) {
    Bytes bytes{parent, c::CRSF_FOLDER};
    string(bytes, "Folder");
    bytes.insert(bytes.end(), children.begin(), children.end());
    bytes.push_back(255);
    return bytes;
}
Bytes number(const std::uint8_t parent, const std::uint8_t value = 12) {
    Bytes bytes{parent, c::CRSF_UINT8};
    string(bytes, "Value");
    bytes.insert(bytes.end(), {value, 0, 100, 0, 0});
    return bytes;
}
Bytes command(const std::uint8_t status) {
    Bytes bytes{0, c::CRSF_COMMAND};
    string(bytes, "Bind");
    bytes.insert(bytes.end(), {status, 1});
    string(bytes, "Status");
    return bytes;
}
struct Link {
    cfg::Client client{};
    c::Telemetry telemetry{};
    cfg::Frame outbound{};
    std::uint32_t clock{};

    void reply(const std::uint8_t type, const Bytes &data,
               const std::uint8_t origin = c::CRSF_ADDRESS_CRSF_TRANSMITTER) {
        require(data.size() <= 58, "fixture frame fits wire limit");
        Bytes frame{c::CRSF_ADDRESS_BROADCAST, static_cast<std::uint8_t>(data.size() + 4), type,
                    c::CRSF_ADDRESS_RADIO_TRANSMITTER, origin};
        frame.insert(frame.end(), data.begin(), data.end());
        frame.push_back(c::crc8(std::span{frame}.subspan(2)));
        for (const auto byte : frame)
            telemetry.push(byte, coretx::TimestampUs{clock}, client);
        ++clock;
    }
    void entry(const std::uint8_t id, const Bytes &bytes) {
        std::size_t offset{};
        std::uint8_t chunk{};
        do {
            const auto count = std::min<std::size_t>(56, bytes.size() - offset);
            const auto remaining = (bytes.size() - offset - count + 55) / 56;
            Bytes data{id, static_cast<std::uint8_t>(remaining)};
            data.insert(data.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                        bytes.begin() + static_cast<std::ptrdiff_t>(offset + count));
            reply(c::CRSF_FRAMETYPE_PARAMETER_SETTINGS_ENTRY, data);
            offset += count;
            if (remaining != 0)
                request(id, ++chunk);
        } while (offset < bytes.size());
    }
    void request(const std::uint8_t id, const std::uint8_t chunk = 0) {
        require(client.next_frame(coretx::TimestampUs{clock}, outbound) == 8, "read frame size");
        require((outbound[2] == c::CRSF_FRAMETYPE_PARAMETER_READ) && (outbound[5] == id) &&
                    (outbound[6] == chunk),
                "requested id/chunk");
        require(c::crc8(std::span{outbound}.subspan(2, 5)) == outbound[7], "request CRC");
    }
    void start(const std::uint8_t count) {
        client.open(coretx::TimestampUs{clock});
        require(client.next_frame(coretx::TimestampUs{clock}, outbound) == 6, "broadcast ping");
        require((outbound[3] == 0) && (outbound[4] == 0xEA), "ping routing");
        Bytes info{};
        string(info, "Module");
        info.resize(info.size() + 14);
        info[info.size() - 2] = count;
        reply(c::CRSF_FRAMETYPE_DEVICE_INFO, info);
        require(client.device_count() == 1, "device discovered via shared framing");
        client.select(0);
        request(0);
    }
};

void dashboard_discovery() {
    Link link{};
    link.client.open(coretx::TimestampUs{});
    require(link.client.next_frame(coretx::TimestampUs{}, link.outbound) == 6,
            "visible device dashboard starts discovery");
    link.client.suspend();
    link.client.service(coretx::TimestampUs{2'000'000});
    require(link.client.next_frame(coretx::TimestampUs{2'000'000}, link.outbound) == 0,
            "hidden device dashboard stops discovery");
    link.client.resume(coretx::TimestampUs{2'000'000});
    require(link.client.next_frame(coretx::TimestampUs{2'000'000}, link.outbound) == 6,
            "returning to device dashboard resumes discovery immediately");
    link.client.service(coretx::TimestampUs{3'000'000});
    require(link.client.next_frame(coretx::TimestampUs{3'000'000}, link.outbound) == 6,
            "device dashboard keeps pinging while visible");
}

void breadth_first_and_focus() {
    Link link{};
    link.start(8);
    link.entry(0, folder(0, {5, 1, 3}));
    require(link.client.count() == 3, "root lists children before they finish loading");
    require(!link.client.parameter(1), "unloaded row remains pending");
    link.request(5);
    link.entry(5, folder(0, {6, 2}));
    require(link.client.parameter(5).has_value(), "progressively cached first row");
    link.request(1);
    link.entry(1, number(0));
    link.request(3);
    link.entry(3, folder(0, {7}));
    link.request(6);
    link.entry(6, folder(5, {4}));
    link.request(2);
    link.entry(2, number(5));
    link.request(7);
    link.entry(7, number(3));
    link.request(4);
    link.entry(4, number(6));
    require(!link.client.busy(), "breadth-first traversal finishes");

    link.client.refresh();
    link.request(0);
    link.entry(0, folder(0, {5, 1, 3}));
    link.request(5);
    link.entry(5, folder(0, {6, 2}));
    link.request(1);
    link.entry(1, number(0));
    link.request(3);
    link.entry(3, folder(0, {7}));
    link.request(6);
    link.client.enter(3);
    link.request(7);
    link.entry(6, folder(5, {4}));
    require(!link.client.parameter(6), "late reply from preempted folder is ignored");
    link.entry(7, number(3));
    link.request(6);
    link.entry(6, folder(5, {4}));
    link.request(2);
    link.entry(2, number(5));
    link.request(4);
    link.entry(4, number(6));
    require(!link.client.busy(), "visible priority then background traversal finishes");

    link.client.enter(5);
    link.client.suspend();
    require((link.client.mode() == cfg::Mode::closed) &&
                (link.client.view_mode() == cfg::Mode::parameters) && (link.client.folder() == 5),
            "suspended dashboard retains its folder without scheduling requests");
    require(link.client.next_frame(coretx::TimestampUs{link.clock}, link.outbound) == 0,
            "hidden dashboard does not transmit");
    link.client.resume(coretx::TimestampUs{link.clock});
    require((link.client.folder() == 5) && !link.client.parameter(2) &&
                link.client.parameter(7).has_value(),
            "resume invalidates only displayed folder parameters");
    link.request(5);
    link.entry(5, folder(0, {6, 2}));
    link.request(6);
    link.entry(6, folder(5, {4}));
    link.request(2);
    link.entry(2, number(5, 25));
    require(!link.client.busy() && (link.client.parameter(2)->value == 25) &&
                link.client.parameter(4).has_value(),
            "folder refresh finishes without reloading nested or unrelated parameters");
}

void chunk_integrity_and_errors() {
    Link link{};
    link.start(40);
    const auto root = folder(0, {1, 2});
    Bytes first{0, 1};
    first.insert(first.end(), root.begin(), root.begin() + 5);
    link.reply(c::CRSF_FRAMETYPE_PARAMETER_SETTINGS_ENTRY, first);
    require(link.client.count() == 0, "partial folder is not published");
    link.request(0, 1);
    link.reply(c::CRSF_FRAMETYPE_PARAMETER_SETTINGS_ENTRY, first);
    require(link.client.next_frame_size() == 0, "duplicate chunk is not appended");
    Bytes last{0, 0};
    last.insert(last.end(), root.begin() + 5, root.end());
    link.reply(c::CRSF_FRAMETYPE_PARAMETER_SETTINGS_ENTRY, last);
    require(link.client.count() == 2, "folder published after final chunk");
    link.request(1);
    link.entry(1, {0, c::CRSF_STRING, 'N', 0, 'X'});
    require(link.client.error() == cfg::Error::malformed, "missing terminator rejected");
    link.request(2);
    auto oversized = number(0);
    oversized.resize(280, 42);
    link.entry(2, oversized);
    require(link.client.error() == cfg::Error::capacity, "oversized parameter visibly rejected");
    require(!link.client.busy(), "errors do not leave endless loading");

    Link cyclic{};
    cyclic.start(2);
    cyclic.entry(0, folder(0, {1}));
    cyclic.request(1);
    cyclic.entry(1, folder(0, {2}));
    cyclic.request(2);
    cyclic.entry(2, folder(1, {1}));
    require(cyclic.client.error() == cfg::Error::malformed, "cyclic folder rejected");
}

void parameter_error_identification() {
    namespace md = coretx::firmware::crsf_menu_detail;
    Link link{};
    link.start(2);
    link.entry(0, folder(0, {254, 2}));
    link.request(254);
    link.entry(254, {0, c::CRSF_STRING, 'N', 0, 'X'});
    link.request(2);
    link.entry(2, number(0));
    const auto footer_matches = [](const cfg::Client &client, const std::string &message) {
        coretx::firmware::CrsfMenu menu{};
        coretx::ui::Framebuffer display{};
        menu.render(client, display, coretx::TimestampUs{});
        coretx::ui::Framebuffer expected{};
        expected.small_text(0, md::help_y, message);
        for (int y = md::help_y; y < coretx::ui::Framebuffer::height; ++y)
            for (int x = 0; x < coretx::ui::Framebuffer::width; ++x)
                if (display.pixel_at(x, y) != expected.pixel_at(x, y))
                    return false;
        return true;
    };
    require(footer_matches(link.client, std::string{md::tr::crsf_invalid} + " #254"),
            "error footer retains the failed ID after another parameter finishes");
    Link root{};
    root.start(0);
    root.entry(0, number(0));
    require(footer_matches(root.client, std::string{md::tr::crsf_invalid} + " #0"),
            "root parameter zero has a visible error ID");
    link.client.open(coretx::TimestampUs{});
    link.reply(c::CRSF_FRAMETYPE_DEVICE_INFO, {'N'});
    require(footer_matches(link.client, md::tr::crsf_invalid),
            "device-information errors do not reuse a stale parameter ID");
}

void large_folder_cache_and_scroll() {
    Link link{};
    link.start(80);
    Bytes children{};
    for (std::uint8_t i = 1; i <= 80; ++i)
        children.push_back(i);
    link.entry(0, folder(0, children));
    for (std::uint8_t i = 1; i <= 80; ++i) {
        link.request(i);
        link.entry(i, number(0));
    }
    require(link.client.count() == 80, "large folder tree is not cache-size limited");
    for (std::uint8_t i = 1; i <= 4; ++i)
        require(link.client.parameter(i).has_value(), "visible rows remain pinned");
    require(!link.client.parameter(30), "background entries may be evicted");
    link.client.window(29);
    for (std::uint8_t i = 30; i <= 33; ++i) {
        link.request(i);
        link.entry(i, number(0));
    }
    require(!link.client.busy(), "scrolling refills only missing visible rows");
    for (std::uint8_t i = 30; i <= 33; ++i)
        require(link.client.parameter(i).has_value(), "new viewport pinned");
    Link sparse{};
    sparse.start(1);
    sparse.entry(0, folder(0, {254}));
    sparse.request(254);
    sparse.entry(254, number(0));
    require(!sparse.client.busy(), "root child IDs, not advertised count, drive modern traversal");
}

void write_verification_and_folder_refresh() {
    Link link{};
    link.start(2);
    link.entry(0, folder(0, {1, 2}));
    link.request(1);
    link.entry(1, number(0));
    link.request(2);
    link.entry(2, number(0));
    require(link.client.write(1, 23), "write accepted");
    require(link.client.next_frame(coretx::TimestampUs{link.clock}, link.outbound) == 8,
            "write frame");
    require((link.outbound[2] == 0x2D) && (link.outbound[5] == 1) && (link.outbound[6] == 23),
            "value wire encoding");
    link.reply(0x2D, {1, 23}, 0xEC);
    require(link.client.next_frame_size() == 0, "foreign-origin ACK ignored");
    link.reply(0x2D, {1, 23});
    require(!link.client.parameter(2), "entire folder invalidated, not just written entry");
    link.request(0);
    link.entry(0, folder(0, {2, 1}));
    link.request(2);
    link.entry(2, number(0));
    link.request(1);
    link.entry(1, number(0, 23));
    require(link.client.item(0) == 2, "new folder child order is used");
    require(link.client.write(1, 24), "second write accepted");
    (void)link.client.next_frame(coretx::TimestampUs{link.clock}, link.outbound);
    link.reply(0x2D, {1, 25});
    require(link.client.error() == cfg::Error::verification, "mismatched write ACK reported");
    link.request(0);
    link.entry(0, folder(0, {2, 1}));
    link.request(2);
    link.entry(2, number(0));
    link.request(1);
    link.entry(1, number(0, 25));

    require(link.client.write(1, 26), "retry after verification error");
    (void)link.client.next_frame(coretx::TimestampUs{link.clock}, link.outbound);
    link.entry(1, number(0, 26));
    link.request(0);
}

void timeout_and_legacy() {
    for (const std::uint32_t initial : {0U, 0xFFF00000U}) {
        Link link{};
        link.clock = initial;
        link.start(2);
        for (unsigned retry = 0; retry < 2; ++retry) {
            link.clock += 500'000;
            link.request(0);
        }
        link.clock += 500'000;
        link.request(1);
        link.entry(1, number(0));
        link.request(2);
        link.entry(2, number(0));
        require(!link.client.busy() && (link.client.count() == 2),
                "legacy fallback and timer wrap");
        require(link.client.write(1, 50), "legacy write");
        (void)link.client.next_frame(coretx::TimestampUs{link.clock}, link.outbound);
        link.clock += 500'000;
        link.client.service(coretx::TimestampUs{link.clock});
        require(link.client.error() == cfg::Error::timeout, "unacknowledged write fails");
    }
}

void fixed_point_and_command() {
    Bytes floating{0, c::CRSF_FLOAT};
    string(floating, "Frequency");
    const auto integer = [&](const std::int32_t value) {
        const auto raw = static_cast<std::uint32_t>(value);
        for (unsigned i = 0; i < 4; ++i)
            floating.push_back(static_cast<std::uint8_t>(raw >> ((3U - i) * 8U)));
    };
    integer(-125);
    integer(-200);
    integer(200);
    integer(0);
    floating.push_back(2);
    integer(5);
    string(floating, "MHz");
    const auto parsed = cfg::parse(floating);
    require(parsed && (parsed->value == -125) && (parsed->decimals == 2) && (parsed->step == 5),
            "FLOAT uses scaled integers, not floating point");
    floating.resize(floating.size() - 4);
    require(cfg::parse(floating).has_value(), "absent optional unit is allowed");
    floating.push_back('X');
    require(!cfg::parse(floating), "present but unterminated unit is rejected");
    auto status_only = command(2);
    status_only.resize(status_only.size() - 7);
    require(cfg::parse(status_only).has_value(), "command status without optional info");

    Link link{};
    link.start(1);
    link.entry(0, folder(0, {1}));
    link.request(1);
    link.entry(1, command(0));
    require(link.client.start_command(1), "command start");
    (void)link.client.next_frame(coretx::TimestampUs{link.clock}, link.outbound);
    require(link.outbound[6] == 1, "START status");
    link.entry(1, command(3));
    require(link.client.command() == cfg::Command::confirmation, "confirmation required");
    require(link.client.confirm_command(), "confirm command");
    (void)link.client.next_frame(coretx::TimestampUs{link.clock}, link.outbound);
    require(link.outbound[6] == 4, "CONFIRM status");
    link.entry(1, command(2));
    link.entry(1, command(2));
    require(link.client.command() == cfg::Command::running, "unsolicited progress is accepted");
    link.clock += 100'000;
    (void)link.client.next_frame(coretx::TimestampUs{link.clock}, link.outbound);
    require(link.outbound[6] == 6, "periodic POLL");
    link.entry(1, command(0));
    require(link.client.command() == cfg::Command::idle, "command complete");
    link.request(0);

    Link running{};
    running.start(1);
    running.entry(0, folder(0, {1}));
    running.request(1);
    running.entry(1, command(2));
    require(running.client.start_command(1, coretx::TimestampUs{running.clock}),
            "already running command adopted without a duplicate START");
    require(running.client.cancel_command(), "cancel running command");
    (void)running.client.next_frame(coretx::TimestampUs{running.clock}, running.outbound);
    require(running.outbound[6] == 5, "CANCEL status");
    running.entry(1, command(2));
    require(running.client.command() == cfg::Command::cancelling,
            "late progress does not undo requested cancellation");
    running.entry(1, command(0));
    running.request(0);
}

void string_write_and_disconnected_edit() {
    using coretx::firmware::Key;
    Link link{};
    coretx::firmware::CrsfMenu menu{};
    menu.open(link.client, coretx::TimestampUs{});
    link.start(1);
    link.entry(0, folder(0, {1}));
    link.request(1);
    Bytes text{0, c::CRSF_STRING};
    string(text, "Name");
    string(text, "A");
    text.push_back(20);
    link.entry(1, text);
    menu.handle(Key::ok, link.client, coretx::TimestampUs{});
    menu.handle(Key::up, link.client, coretx::TimestampUs{});
    menu.handle(Key::ok_long, link.client, coretx::TimestampUs{});
    require(link.client.next_frame(coretx::TimestampUs{link.clock}, link.outbound) == 9,
            "string write frame");
    require((link.outbound[6] == 'B') && (link.outbound[7] == 0), "string NUL terminator");
    link.reply(0x2D, {1, 'B', 0});
    link.request(0);
    link.entry(0, folder(0, {1}));
    link.request(1);
    link.entry(1, text);
    menu.handle(Key::ok, link.client, coretx::TimestampUs{});
    link.client.transport_lost();
    require(!link.client.write(1, "More than twenty characters") &&
                (link.client.error() == cfg::Error::link),
            "invalid writes cannot clear a transport failure");
    menu.handle(Key::ok_long, link.client, coretx::TimestampUs{});
    require((link.client.error() == cfg::Error::link) && !link.client.pending_write(),
            "lost link cannot be revived by confirming a stale edit");
    menu.handle(Key::cancel, link.client, coretx::TimestampUs{});
    require(!menu.visible(), "lost link remains escapable");
}

void root_uses_selected_device_name() {
    Link link{};
    coretx::firmware::CrsfMenu menu{};
    menu.open(link.client, coretx::TimestampUs{});
    coretx::ui::Framebuffer expected{};
    expected.text(0, 0, coretx::ui::tr::crsf_devices);
    const auto title_matches = [&] {
        coretx::ui::Framebuffer display{};
        menu.render(link.client, display, coretx::TimestampUs{});
        for (int y = 0; y < display.character_height(); ++y)
            for (int x = 0; x < 122; ++x)
                if (display.pixel_at(x, y) != expected.pixel_at(x, y))
                    return false;
        return true;
    };
    require(title_matches(), "discovery header names CRSF devices");
    (void)link.client.next_frame(coretx::TimestampUs{}, link.outbound);
    for (const auto address : {c::CRSF_ADDRESS_CRSF_TRANSMITTER, c::CRSF_ADDRESS_CRSF_RECEIVER}) {
        Bytes info{};
        string(info, address == c::CRSF_ADDRESS_CRSF_TRANSMITTER ? "Module" : "Receiver");
        info.resize(info.size() + 14);
        link.reply(c::CRSF_FRAMETYPE_DEVICE_INFO, info, address);
    }
    link.client.select(1);
    expected.clear();
    expected.text(0, 0, "Receiver");
    require(title_matches(), "selected device name appears before the root definition arrives");
    link.request(0);
    Bytes data{0, 0};
    const auto root = folder(0, {});
    data.insert(data.end(), root.begin(), root.end());
    link.reply(c::CRSF_FRAMETYPE_PARAMETER_SETTINGS_ENTRY, data, c::CRSF_ADDRESS_CRSF_RECEIVER);
    require(title_matches(), "root wire name cannot replace the selected device-info name");
}

void menu_titles_and_value_details() {
    using coretx::firmware::Key;
    namespace md = coretx::firmware::crsf_menu_detail;
    Link link{};
    coretx::firmware::CrsfMenu menu{};
    menu.open(link.client, coretx::TimestampUs{});
    link.start(6);
    link.entry(0, folder(0, {1, 2, 3, 6}));
    link.request(1);
    Bytes selection{0, c::CRSF_TEXT_SELECTION};
    string(selection, "Mode");
    string(selection, "Low;High");
    selection.insert(selection.end(), {1, 0, 1, 0, 0});
    link.entry(1, selection);
    link.request(2);
    Bytes text{0, c::CRSF_STRING};
    string(text, "Name");
    string(text, "ABCDEFGHIJKLMN");
    text.push_back(20);
    link.entry(2, text);
    link.request(3);
    link.entry(3, folder(0, {4, 5}));
    link.request(6);
    Bytes info{0, c::CRSF_INFO};
    string(info, "Version");
    string(info, "Firmware");
    link.entry(6, info);
    link.request(4);
    auto status = command(0);
    status[0] = 3;
    link.entry(4, status);
    link.request(5);
    auto numeric = number(3);
    numeric.pop_back();
    string(numeric, "Hz");
    link.entry(5, numeric);

    coretx::ui::Framebuffer display{};
    const auto matches = [&](const coretx::ui::Framebuffer &expected, const int top,
                             const int bottom, const int left = 0, const int right = 128) {
        for (int y = top; y < bottom; ++y)
            for (int x = left; x < right; ++x)
                if (display.pixel_at(x, y) != expected.pixel_at(x, y))
                    return false;
        return true;
    };
    menu.render(link.client, display, coretx::TimestampUs{});
    coretx::ui::Framebuffer expected{};
    expected.text(0, md::header_y, "Module");
    require(matches(expected, 0, 8, 0, 122), "root wire name is replaced by the device-info name");
    constexpr int unused_y = md::list_y + cfg::visible_rows * md::row_height;
    const auto value_matches = [&](const std::string_view value, const std::size_t row,
                                   const bool selected,
                                   const std::size_t cursor = md::value_columns) {
        expected.clear();
        const int y = md::list_y + static_cast<int>(row) * md::row_height;
        if (selected)
            expected.rectangle(0, y - 1, 128, md::row_height);
        const int x = md::value_right - static_cast<int>(value.size()) * md::value_character_width;
        expected.small_text(x, y + 2, value, !selected);
        if (cursor < md::value_columns)
            expected.rectangle(x + static_cast<int>(cursor) * md::value_character_width,
                               y + md::row_height - 2, 3, 1, !selected);
        return matches(expected, y + 2, y + md::row_height - 1,
                       md::value_right - md::value_columns * md::value_character_width);
    };
    expected.clear();
    require(matches(expected, unused_y, md::help_y), "selection has no second footer");
    require(value_matches("High", 0, true), "selection value is right aligned");
    require(value_matches(">", 2, false), "folder indicator is right aligned");
    require(value_matches("Firmware", 3, false), "info text is right aligned");

    menu.handle(Key::down, link.client, coretx::TimestampUs{});
    menu.render(link.client, display, coretx::TimestampUs{});
    expected.clear();
    require(matches(expected, unused_y, md::help_y), "string has no second footer");
    require(value_matches("ABCDEFGHIJKLM", 1, true), "long string fits the right-aligned field");
    menu.handle(Key::ok, link.client, coretx::TimestampUs{});
    menu.render(link.client, display, coretx::TimestampUs{});
    require(value_matches("ABCDEFGHIJKLM", 1, true, 0), "string cursor is in its value row");
    for (std::size_t i = 0; i < md::value_columns; ++i)
        menu.handle(Key::ok, link.client, coretx::TimestampUs{});
    menu.render(link.client, display, coretx::TimestampUs{});
    require(value_matches("N", 1, true, 0), "string window follows the cursor beyond its width");
    menu.handle(Key::ok, link.client, coretx::TimestampUs{});
    menu.render(link.client, display, coretx::TimestampUs{});
    require(value_matches("N ", 1, true, 1), "string end cursor stays inside the value field");
    expected.clear();
    require(matches(expected, unused_y, md::help_y), "string editor has no second footer");
    menu.handle(Key::cancel, link.client, coretx::TimestampUs{});
    menu.handle(Key::down, link.client, coretx::TimestampUs{});
    menu.handle(Key::down, link.client, coretx::TimestampUs{});
    menu.render(link.client, display, coretx::TimestampUs{});
    expected.clear();
    require(matches(expected, unused_y, md::help_y), "selected info has no second footer");
    menu.handle(Key::up, link.client, coretx::TimestampUs{});
    menu.handle(Key::ok, link.client, coretx::TimestampUs{});
    menu.render(link.client, display, coretx::TimestampUs{});
    expected.clear();
    expected.text(0, md::header_y, "Folder");
    require(matches(expected, 0, 8, 0, 122), "subfolder name remains visible");
    expected.clear();
    require(matches(expected, unused_y, md::help_y), "command has no second footer");
    require(value_matches("Status", 0, true), "command information is right aligned");
    menu.handle(Key::down, link.client, coretx::TimestampUs{});
    menu.render(link.client, display, coretx::TimestampUs{});
    expected.clear();
    require(matches(expected, unused_y, md::help_y), "numeric units have no second footer");
    require(value_matches("12 Hz", 1, true), "numeric value and unit are right aligned");
    link.client.transport_lost();
    menu.render(link.client, display, coretx::TimestampUs{});
    expected.clear();
    expected.small_text(0, md::help_y, md::tr::crsf_link);
    require(matches(expected, md::help_y, coretx::ui::Framebuffer::height),
            "errors remain in the normal footer");
}

void progressive_menu_and_editing() {
    using coretx::firmware::Key;
    Link link{};
    coretx::firmware::CrsfMenu menu{};
    menu.open(link.client, coretx::TimestampUs{});
    link.start(2);
    link.entry(0, folder(0, {1, 2}));
    coretx::ui::Framebuffer pending{};
    menu.render(link.client, pending, coretx::TimestampUs{});
    link.request(1);
    link.entry(1, number(0));
    coretx::ui::Framebuffer progressive{};
    menu.render(link.client, progressive, coretx::TimestampUs{});
    require(progressive.bytes() != pending.bytes(), "menu updates before folder completes");
    coretx::ui::Framebuffer staged{};
    for (std::uint8_t band = 0; band < 8; ++band) {
        staged.clip_page(band);
        menu.render(link.client, staged, coretx::TimestampUs{});
    }
    require(staged.bytes() == progressive.bytes(), "band rendering matches full config menu");
    menu.handle(Key::ok, link.client, coretx::TimestampUs{});
    menu.handle(Key::up, link.client, coretx::TimestampUs{});
    require(link.client.parameter(1)->value == 12, "edit is a draft until confirmed");
    menu.handle(Key::cancel, link.client, coretx::TimestampUs{});
    coretx::ui::Framebuffer cancelled{};
    menu.render(link.client, cancelled, coretx::TimestampUs{});
    require(cancelled.bytes() == progressive.bytes(), "cancel restores displayed value");
    menu.handle(Key::ok, link.client, coretx::TimestampUs{});
    menu.handle(Key::up, link.client, coretx::TimestampUs{});
    menu.handle(Key::ok, link.client, coretx::TimestampUs{});
    require(link.client.next_frame(coretx::TimestampUs{link.clock}, link.outbound) == 8,
            "menu writes asynchronously");
    require(link.outbound[6] == 13, "menu increment encoded");
    link.reply(0x2D, {1, 13});
    require(link.client.busy(), "activity resumes for full folder refresh");
    menu.handle(Key::cancel_long, link.client, coretx::TimestampUs{});
    require(!menu.visible() && (link.client.mode() == cfg::Mode::closed), "leaving cancels loader");

    std::array<char, 24> buffer{};
    require(coretx::firmware::crsf_menu_detail::number(buffer, -125, 2) == "-1.25",
            "fixed-point decimal presentation");
    require(coretx::firmware::crsf_menu_detail::number(buffer, 4'294'967'295LL, 0) == "4294967295",
            "unsigned 32-bit presentation");
}
void dashboard_header_and_help() {
    Link link{};
    link.start(1);
    coretx::firmware::CrsfMenu menu{};
    coretx::ui::Framebuffer inactive{}, active{}, footer{};
    namespace md = coretx::firmware::crsf_menu_detail;
    menu.render(link.client, inactive, coretx::TimestampUs{link.clock});
    menu.render(link.client, active, coretx::TimestampUs{link.clock}, true,
                md::tr::crsf_dashboard_active_help);
    for (int y = 0; y < md::row_height; ++y)
        for (int x = 0; x < coretx::ui::Framebuffer::width; ++x)
            require(inactive.pixel_at(x, y) != active.pixel_at(x, y),
                    "active dashboard inverts the entire header including spinner");
    footer.small_text(0, md::help_y, md::tr::crsf_dashboard_active_help);
    for (int y = md::help_y; y < coretx::ui::Framebuffer::height; ++y)
        for (int x = 0; x < coretx::ui::Framebuffer::width; ++x)
            require(footer.pixel_at(x, y) == active.pixel_at(x, y),
                    "active dashboard advertises exit rather than long-OK reload");
    link.client.transport_lost();
    menu.render(link.client, active, coretx::TimestampUs{link.clock}, true,
                md::tr::crsf_dashboard_active_help);
    footer.clear();
    footer.small_text(0, md::help_y, md::tr::crsf_link);
    for (int y = md::help_y; y < coretx::ui::Framebuffer::height; ++y)
        for (int x = 0; x < coretx::ui::Framebuffer::width; ++x)
            require(footer.pixel_at(x, y) == active.pixel_at(x, y),
                    "dashboard navigation help never masks transport errors");
}
} // namespace

int main() {
    dashboard_header_and_help();
    dashboard_discovery();
    breadth_first_and_focus();
    chunk_integrity_and_errors();
    parameter_error_identification();
    large_folder_cache_and_scroll();
    write_verification_and_folder_refresh();
    timeout_and_legacy();
    fixed_point_and_command();
    string_write_and_disconnected_edit();
    root_uses_selected_device_name();
    menu_titles_and_value_details();
    progressive_menu_and_editing();
    std::printf("CRSF configuration tests passed (client %zu bytes)\n", sizeof(cfg::Client));
}
