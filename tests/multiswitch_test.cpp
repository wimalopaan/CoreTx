// SPDX-License-Identifier: GPL-2.0-or-later
#include "boards/common/multiswitch_ui.hpp"
#include "boards/i6s/multiswitch_touch.hpp"

#include <cstdio>

namespace {
namespace ms = coretx::multiswitch;
namespace fw = coretx::firmware;
using coretx::TimestampUs;
using fw::Key;

constexpr bool wire_vectors() {
    return ms::encode(0, 0) == ms::Frame{0xC8, 10, 0x32, 0, 0xEA, 0xA1, 7, 0, 0, 0, 0x7E, 0xDE} &&
           ms::encode(0, 1) == ms::Frame{0xC8, 10, 0x32, 0, 0xEA, 0xA1, 7, 0, 0, 1, 0xC4, 0x9A} &&
           ms::encode(255, 0x80) ==
               ms::Frame{0xC8, 10, 0x32, 0, 0xEA, 0xA1, 7, 255, 0x40, 0, 0x1C, 0x1E} &&
           ms::encode(42, 0xFF) ==
               ms::Frame{0xC8, 10, 0x32, 0, 0xEA, 0xA1, 7, 42, 0x55, 0x55, 0xFE, 0xC0};
}

constexpr bool pages_and_repeats() {
    coretx::Model model{};
    model.multiswitch[1].address = 0;
    model.multiswitch[3].address = 255;
    model.multiswitch[1].momentary = 2;
    if ((ms::count(model) != 2) || (ms::slot(model, 0) != 1) || (ms::slot(model, 1) != 3) ||
        ms::slot(model, 2))
        return false;
    for (const std::uint32_t base : {0U, 0xFFFFF000U}) {
        ms::Pages pages{};
        pages.configure(model);
        if ((pages.due(TimestampUs{base}) != 1) || !pages.update(1, 1, 0) ||
            (pages.switches(1) != 3) || (pages.states() != (3U << 8)) ||
            (pages.available() != 0xFF00FF00U))
            return false;
        pages.sent(1, TimestampUs{base});
        if ((pages.due(TimestampUs{base + 1}) != 3) || (pages.frame(1) != ms::encode(0, 3)))
            return false;
        pages.sent(3, TimestampUs{base + 1});
        if (pages.due(TimestampUs{base + 99'999}) || (pages.due(TimestampUs{base + 100'000}) != 1))
            return false;
        if (!pages.update(std::nullopt, std::nullopt, std::nullopt) || (pages.switches(1) != 1) ||
            !pages.update(1, std::nullopt, 0) || (pages.switches(1) != 0))
            return false;
        pages.update(3, std::nullopt, 7);
        if (pages.states() != (0x80U << 24))
            return false;
        auto changed = model;
        changed.multiswitch[1].address = 42;
        pages.configure(changed);
        if ((pages.switches(1) != 0) || (pages.switches(3) != 0x80))
            return false;
        changed.multiswitch[3].momentary = 0x80;
        pages.configure(changed);
        if (pages.switches(3) != 0)
            return false;
        changed.multiswitch.fill(coretx::MultiSwitchPage{});
        pages.configure(changed);
        if ((pages.due(TimestampUs{base + 200'000}) != 1) || (pages.frame(1) != ms::encode(0, 0)))
            return false;
        pages.sent(1, TimestampUs{base + 200'000});
        if (pages.due(TimestampUs{base + 200'000}))
            return false;
    }
    model.multiswitch[0].address = -2;
    if (coretx::validate(model).code != coretx::ErrorCode::multiswitch_settings)
        return false;
    model.multiswitch[0].address = 256;
    return coretx::validate(model).code == coretx::ErrorCode::multiswitch_settings;
}

constexpr bool assigned_targets() {
    coretx::Model model{};
    auto &page = model.multiswitch[0];
    page.address = 42;
    page.momentary = 1;
    auto &physical = page.controls[0];
    auto &logical = page.controls[1];
    model.logical_switches[0].function = coretx::LogicalFunction::equal;
    if (!physical.set_address(255) || !physical.set_output(7) ||
        !physical.set_condition({true, 0, coretx::SwitchPosition::down}) ||
        !logical.set_condition({true, coretx::switch_count, coretx::SwitchPosition::down}) ||
        !coretx::validate(model))
        return false;
    coretx::Controls controls{};
    controls.active_switches = 1;
    controls.switches[0] = coretx::SwitchPosition::down;
    coretx::LogicalFrame frame{};
    frame.available = frame.active = 1;
    ms::Pages pages{};
    pages.configure(model);
    pages.update(std::nullopt, std::nullopt, std::nullopt, &controls, frame);
    if (pages.switches(0) != 3)
        return false;
    pages.update(0, 0, 0, &controls, frame);
    if (pages.switches(0) != 3)
        return false; // Assigned functions ignore both touch and physical activation.
    bool saw_physical{}, saw_logical{};
    while (const auto due = pages.due(TimestampUs{1})) {
        const auto packet = pages.frame(*due);
        saw_physical = saw_physical || (packet == ms::encode(255, 0x80));
        saw_logical = saw_logical || (packet == ms::encode(42, 2));
        pages.sent(*due, TimestampUs{1});
    }
    if (!saw_physical || !saw_logical)
        return false;
    physical.set_address(254);
    pages.configure(model);
    pages.update(std::nullopt, std::nullopt, std::nullopt, &controls, frame);
    auto due = pages.due(TimestampUs{2});
    if (!due || (pages.frame(*due) != ms::encode(255, 0)))
        return false;
    pages.sent(*due, TimestampUs{2});
    due = pages.due(TimestampUs{2});
    if (!due || (pages.frame(*due) != ms::encode(254, 0x80)))
        return false;
    pages.sent(*due, TimestampUs{2});
    controls.active_switches = 0;
    frame.available = 0;
    pages.update(std::nullopt, std::nullopt, std::nullopt, &controls, frame);
    if (pages.switches(0) != 0)
        return false;
    page.address = -1;
    pages.configure(model);
    controls.active_switches = frame.available = 1;
    pages.update(std::nullopt, std::nullopt, std::nullopt, &controls, frame);
    if (pages.switches(0) != 0)
        return false;
    page.address = 42;
    physical.set_address(-1);
    physical.set_output(1);
    if (coretx::validate(model).code != coretx::ErrorCode::multiswitch_duplicate)
        return false;
    page.address = -1;
    return static_cast<bool>(coretx::validate(model));
}

constexpr bool all_addresses_and_retirement() {
    coretx::Model model{};
    for (std::size_t page = 0; page < model.multiswitch.size(); ++page) {
        model.multiswitch[page].address = static_cast<std::int16_t>(page * ms::switch_count);
        for (std::size_t button = 0; button < ms::switch_count; ++button) {
            auto &control = model.multiswitch[page].controls[button];
            control.set_address(static_cast<int>(page * ms::switch_count + button));
            control.set_condition({true, 0, coretx::SwitchPosition::down});
        }
    }
    if (!coretx::validate(model))
        return false;
    coretx::Controls controls{};
    controls.active_switches = 1;
    controls.switches[0] = coretx::SwitchPosition::down;
    ms::Pages pages{};
    pages.configure(model);
    pages.update(std::nullopt, std::nullopt, std::nullopt, &controls);
    std::uint32_t seen{};
    while (const auto due = pages.due(TimestampUs{1})) {
        const auto packet = pages.frame(*due);
        const auto address = packet[ms::set4::address_index];
        if ((address >= ms::maximum_addresses) ||
            (packet !=
             ms::encode(address, static_cast<std::uint8_t>(1U << (address % ms::switch_count)))))
            return false;
        seen |= std::uint32_t{1} << address;
        pages.sent(*due, TimestampUs{1});
    }
    if (seen != std::numeric_limits<std::uint32_t>::max())
        return false;
    pages.configure(model);
    pages.update(std::nullopt, std::nullopt, std::nullopt, &controls);
    if (pages.due(TimestampUs{2}))
        return false; // Unchanged source states must not flood pending packets.
    model.multiswitch[0].controls[0].set_address(100);
    pages.configure(model);
    auto due = pages.due(TimestampUs{2});
    if (!due || (pages.frame(*due) != ms::encode(0, 0)))
        return false;
    pages.sent(*due, TimestampUs{2});
    pages.configure(model);
    due = pages.due(TimestampUs{2});
    if (!due || (pages.frame(*due) != ms::encode(100, 1)))
        return false;
    pages.sent(*due, TimestampUs{2});
    return !pages.due(TimestampUs{100'000}) && pages.due(TimestampUs{100'001}).has_value();
}

constexpr bool changes_precede_repeats() {
    coretx::Model model{};
    model.multiswitch[0].address = 0;
    model.multiswitch[1].address = 1;
    ms::Pages pages{};
    pages.configure(model);
    pages.sent(0, TimestampUs{0});
    pages.sent(1, TimestampUs{0});
    pages.update(1, std::nullopt, 0);
    if ((pages.due(TimestampUs{100'000}) != 1) || (pages.frame(1) != ms::encode(1, 1)))
        return false;
    pages.sent(1, TimestampUs{100'000});
    return pages.due(TimestampUs{100'000}) == 0;
}

constexpr bool touch_buttons() {
    fw::MultiSwitchTouch navigation{};
    fw::i6s::TouchSample sample{};
    std::uint32_t now{};
    const auto service = [&](const bool healthy = true,
                             const std::uint8_t momentary = 2) constexpr {
        return navigation.service(TimestampUs{now += 1000}, &sample, healthy, momentary);
    };
    service(); // Require a release before accepting touches on a newly selected page.
    for (std::size_t i = 0; i < 8; ++i) {
        sample.contacts = 1;
        sample.x = static_cast<std::uint8_t>(3 + (i % 4) * 32);
        sample.y = static_cast<std::uint8_t>(16 + (i / 4) * 18);
        if ((service(true, 0) != Key::none) || navigation.tapped() || (navigation.pressed() != i))
            return false;
        sample.contacts = 0;
        if ((service(true, 0) != Key::none) || (navigation.tapped() != i) || navigation.pressed())
            return false;
        service();
        if (navigation.tapped())
            return false;
    }
    // A resting finger toggles before the lift, exactly once.
    sample = {0, 0, 3, 16, 1, fw::i6s::Gesture::none};
    service(true, 0);
    now += fw::MultiSwitchTouch::toggle_hold.count();
    if ((service(true, 0) != Key::none) || (navigation.tapped() != 0) || navigation.pressed())
        return false;
    service(true, 0);
    sample.contacts = 0;
    if ((service(true, 0) != Key::none) || navigation.tapped())
        return false;
    // A swipe leaves the tap slop before the hold time and does not toggle.
    sample = {0, 0, 3, 16, 1, fw::i6s::Gesture::none};
    service(true, 0);
    sample.x = 40;
    service(true, 0);
    now += fw::MultiSwitchTouch::toggle_hold.count();
    service(true, 0);
    sample.contacts = 0;
    service(true, 0);
    if (navigation.tapped())
        return false;
    service();
    sample = {0, 0, 34, 16, 1, fw::i6s::Gesture::none};
    // Momentary tiles report holding instead of a pending toggle.
    if ((service() != Key::none) || (navigation.held() != 1) || navigation.pressed())
        return false;
    sample.x = 70;
    if ((service() != Key::ok) || navigation.held())
        return false;
    sample.x = 34;
    service();
    if (navigation.held())
        return false; // Leaving a held button cancels it until a real release.
    sample.contacts = 0;
    if ((service() != Key::none) || navigation.tapped())
        return false;
    sample.contacts = 1;
    service();
    if (navigation.held() != 1)
        return false;
    now += 242'000;
    (void)navigation.service(TimestampUs{now}, nullptr, true, 2);
    if (navigation.held() != 1)
        return false;
    service();
    now += fw::i6s::TouchNavigation::release_timeout_us.count();
    (void)navigation.service(TimestampUs{now}, nullptr, true, 2);
    if (navigation.held() || navigation.tapped())
        return false;
    service();
    if (navigation.held())
        return false;
    sample.contacts = 0;
    service();
    sample.contacts = 1;
    service();
    service(false);
    if (navigation.held())
        return false;
    sample.contacts = 0;
    service();
    sample = {0, 0, 2, 16, 1, fw::i6s::Gesture::none};
    service();
    sample.x = 8;
    service();
    sample.x = 16;
    if (service() != Key::ok)
        return false;
    sample.contacts = 0;
    service();
    if (navigation.tapped())
        return false;
    sample = {0, 0, 2, 2, 1, fw::i6s::Gesture::none};
    service();
    sample.contacts = 0;
    return service() == Key::none;
}

constexpr bool tile_rendering() {
    coretx::MultiSwitchPage page{};
    page.address = 42;
    page.labels[0] = {'L', 'I', 'G', 'H', 'T', 'S'};
    page.momentary = 0x33;
    coretx::ui::Framebuffer display{}, expected{};
    fw::render_multiswitch(page, 3, 0x55, 7, display);
    expected.compact();
    for (std::size_t i = 0; i < 8; ++i) {
        const int left = 1 + static_cast<int>(i % 4) * 32;
        const int top = 14 + static_cast<int>(i / 4) * 18;
        const bool on = (i % 2) == 0;
        expected.rectangle(left, top, 30, 17);
        if (!on)
            expected.rectangle(left + 1, top + 1, 28, 15, false);
        if ((i % 4) < 2) {
            expected.rectangle(left + 2, top + 2, 26, 13, !on);
            expected.rectangle(left + 3, top + 3, 24, 11, on);
        }
        const std::array<char, 1> number{static_cast<char>('1' + i)};
        const std::string_view label = i == 0 ? "LIGHTS" : std::string_view{number.data(), 1};
        expected.text(left + (30 - static_cast<int>(label.size()) * 4) / 2, top + 4, label, !on);
        if (i == 7)
            expected.rectangle(left + 4, top + 12, 22, 1, !on);
        for (int y = top; y < (top + 17); ++y)
            for (int x = left; x < (left + 30); ++x)
                if (display.pixel_at(x, y) != expected.pixel_at(x, y))
                    return false;
    }
    if (!display.pixel_at(101, 44) || display.pixel_at(34, 15))
        return false;
    std::array<char, 6> buffer{};
    const std::array<char, 6> empty{};
    const std::array<char, 6> padded{' ', 'H', 0, 'I', ' ', ' '};
    return (fw::multiswitch_label(empty, buffer, 7) == "8") &&
           (fw::multiswitch_label(padded, buffer, 0) == "H I");
}

constexpr bool fast_touch_swipes() {
    constexpr std::array gestures{fw::i6s::Gesture::up, fw::i6s::Gesture::down,
                                  fw::i6s::Gesture::right, fw::i6s::Gesture::left};
    constexpr std::array keys{Key::up, Key::down, Key::ok, Key::cancel};
    for (unsigned direction = 0; direction < gestures.size(); ++direction) {
        for (const bool hardware : {false, true}) {
            fw::MultiSwitchTouch navigation{};
            fw::i6s::TouchSample sample{};
            (void)navigation.service(TimestampUs{0}, &sample, true, 0);
            std::array<std::uint8_t, 7> packet{0, 0, 1, 0, 64, 0, 128};
            if (!fw::i6s::decode_touch(packet, sample))
                return false;
            sample.gesture = hardware ? gestures[direction] : fw::i6s::Gesture::none;
            if (navigation.service(TimestampUs{1}, &sample, true, 0) !=
                (hardware ? keys[direction] : Key::none))
                return false;
            packet[3] = 0x40;
            packet[4] = direction == 0 ? 32 : direction == 1 ? 96 : 64;
            packet[6] = direction == 2 ? 96 : direction == 3 ? 160 : 128;
            if (!fw::i6s::decode_touch(packet, sample) ||
                (navigation.service(TimestampUs{10'000}, &sample, true, 0) !=
                 (hardware ? Key::none : keys[direction])) ||
                navigation.tapped())
                return false;
        }
    }
    return true;
}

// Switch index physical_switch_count is T1 on the i6X and SE on the i6S.
constexpr auto first_extra_label = [] {
    const auto name = fw::switch_names[coretx::physical_switch_count];
    return std::array<char, 3>{name[0], name[1], 'v'};
}();

constexpr bool bound_tile_rendering() {
    coretx::MultiSwitchPage page{};
    page.address = 42;
    page.labels.fill({'L', 'I', 'G', 'H', 'T', 'S'});
    page.momentary = 0x33;
    using coretx::SwitchPosition;
    constexpr std::array<coretx::Condition, 8> conditions{{
        {true, 0, SwitchPosition::up, false},
        {true, 1, SwitchPosition::down, false},
        {true, 2, SwitchPosition::middle, false},
        {true, 4, SwitchPosition::pos4, false},
        {true, coretx::physical_switch_count, SwitchPosition::down, false},
        {true, 4, SwitchPosition::pos4, true},
        {true, coretx::switch_count, SwitchPosition::down, false},
        {true, coretx::switch_count + 1, SwitchPosition::up, true},
    }};
    constexpr std::array<std::string_view, 8> labels{
        "SA^", "SBv", "SC-", "VRA4", {first_extra_label.data(), 3}, "!VRA4", "L1", "L2"};
    for (std::size_t i = 0; i < conditions.size(); ++i)
        if (!page.controls[i].set_condition(conditions[i]))
            return false;
    for (const bool selected : {false, true}) {
        coretx::ui::Framebuffer display{}, expected{};
        fw::render_multiswitch(page, 0, 0x55,
                               selected ? std::optional<std::size_t>{0} : std::nullopt, display);
        expected.compact();
        for (std::size_t i = 0; i < conditions.size(); ++i) {
            const int left = 1 + static_cast<int>(i % 4) * 32;
            const int top = 14 + static_cast<int>(i / 4) * 18;
            const bool on = (i % 2) == 0;
            expected.rectangle(left, top, 30, 17);
            if (!on)
                expected.rectangle(left + 1, top + 1, 28, 15, false);
            if ((i % 4) < 2) {
                expected.rectangle(left + 2, top + 2, 26, 13, !on);
                expected.rectangle(left + 3, top + 3, 24, 11, on);
            }
            expected.text(left + 3, top + 3, "LIGHTS", !on);
            expected.text(left + (30 - static_cast<int>(labels[i].size()) * 4) / 2, top + 9,
                          labels[i], !on);
            for (int y = top; y < (top + 17); ++y)
                for (int x = left; x < (left + 30); ++x)
                    if (display.pixel_at(x, y) != expected.pixel_at(x, y))
                        return false;
        }
    }
    return true;
}

static_assert(wire_vectors());
constexpr bool active_headers() {
    coretx::MultiSwitchPage page{};
    page.address = 42;
    coretx::ui::Framebuffer inactive{}, selected{}, touch{};
    fw::render_multiswitch(page, 0, 0, std::nullopt, inactive);
    fw::render_multiswitch(page, 0, 0, 0, selected);
    fw::render_multiswitch(page, 0, 0, std::nullopt, touch, true);
    for (int y = 0; y < coretx::ui::Framebuffer::height; ++y)
        for (int x = 0; x < coretx::ui::Framebuffer::width; ++x) {
            if (y < fw::multiswitch_header_height) {
                if ((inactive.pixel_at(x, y) == selected.pixel_at(x, y)) ||
                    (touch.pixel_at(x, y) != selected.pixel_at(x, y)))
                    return false;
            } else if (touch.pixel_at(x, y) != inactive.pixel_at(x, y))
                return false;
        }
    return true;
}
static_assert(active_headers());
static_assert(pages_and_repeats());
static_assert(assigned_targets());
static_assert(all_addresses_and_retirement());
static_assert(changes_precede_repeats());
static_assert(touch_buttons());
static_assert(fast_touch_swipes());
static_assert(tile_rendering());
static_assert(bound_tile_rendering());
} // namespace

int main() {
    if (!wire_vectors() || !pages_and_repeats() || !touch_buttons() || !tile_rendering() ||
        !active_headers() || !assigned_targets() || !all_addresses_and_retirement() ||
        !changes_precede_repeats() || !bound_tile_rendering() || !fast_touch_swipes()) {
        std::fputs("MultiSwitch tests failed\n", stderr);
        return 1;
    }
}
