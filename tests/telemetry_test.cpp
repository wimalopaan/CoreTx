// SPDX-License-Identifier: GPL-2.0-or-later
#include "boards/common/telemetry_adapters.hpp"
#include "tests/renderers/telemetry.hpp"

#include <cstdio>
#include <limits>

namespace {
namespace tm = coretx::telemetry;
namespace fw = coretx::firmware;
using coretx::TimestampUs;

constexpr bool sources_and_capacity() {
    tm::Pool pool{};
    const tm::Key local{tm::Source::local, tm::Quantity::temperature, 1};
    const tm::Key remote{tm::Source::afhds2a, tm::Quantity::temperature, 1};
    pool.publish(local, 250, TimestampUs{1});
    pool.publish(remote, 300, TimestampUs{2});
    for (std::uint8_t i = 2; i < 16; ++i)
        pool.publish({tm::Source::crsf, tm::Quantity::temperature, i}, i, TimestampUs{3});
    pool.publish(local, 260, TimestampUs{4});
    if ((pool.entries().size() != 16) || pool.full() || (pool.find(local)->value != 260) ||
        (pool.find(remote)->value != 300))
        return false;
    const tm::Key overflow{tm::Source::local, tm::Quantity::rpm};
    pool.publish(overflow, 1000, TimestampUs{5});
    if (!pool.full() || (pool.find(overflow) != nullptr) || (pool.find(remote)->value != 300))
        return false;
    coretx::ui::Framebuffer display{};
    coretx::ui::Framebuffer expected{};
    fw::render_telemetry(pool, display, TimestampUs{5}, 0, tm::Source::crsf);
    expected.small_text(0, 10, coretx::ui::tr::telemetry_full);
    for (int y = 10; y < 16; ++y)
        for (int x = 0; x < 128; ++x)
            if (display.pixel_at(x, y) != expected.pixel_at(x, y))
                return false;
    pool.clear(tm::Source::crsf);
    pool.publish(overflow, 1000, TimestampUs{6});
    return !pool.full() && (pool.entries().size() == 3) && (pool.find(local)->value == 260) &&
           (pool.find(remote)->value == 300) && (pool.find(overflow)->value == 1000);
}

constexpr bool independent_freshness() {
    for (const std::uint32_t base : {0U, 0xFFFFF000U}) {
        tm::Pool pool{};
        const tm::Key remote{tm::Source::crsf, tm::Quantity::link_quality};
        pool.publish(tm::transmitter_battery, 6200, TimestampUs{base});
        pool.publish(remote, 0, TimestampUs{base + 100});
        pool.service(TimestampUs{base + 500'000});
        if (pool.find(tm::transmitter_battery)->fresh(TimestampUs{base + 500'000}) ||
            !pool.find(remote)->fresh(TimestampUs{base + 500'000}))
            return false;
        pool.publish(tm::transmitter_battery, 6200, TimestampUs{base});
        pool.service(TimestampUs{base + 2'000'100});
        // Expired readings stay expired even after an entire 32-bit timer wrap.
        if (pool.find(tm::transmitter_battery)->fresh(TimestampUs{base}) ||
            pool.find(remote)->fresh(TimestampUs{base + 100}))
            return false;
        pool.publish(tm::transmitter_battery, 6100, TimestampUs{base + 2'000'101});
        pool.invalidate(tm::transmitter_battery);
        if (pool.find(tm::transmitter_battery)->fresh(TimestampUs{base + 2'000'101}))
            return false;
        pool.publish(tm::transmitter_battery, 6000, TimestampUs{base + 2'000'102});
        pool.clear(tm::Source::crsf);
        if (!pool.find(tm::transmitter_battery)->fresh(TimestampUs{base + 2'000'102}) ||
            (pool.find(remote) != nullptr))
            return false;
    }
    return true;
}

constexpr bool decoder_adapters() {
    tm::Pool pool{};
    coretx::crsf::Telemetry crsf{};
    fw::publish_telemetry(crsf, pool, TimestampUs{0});
    if (!pool.entries().empty())
        return false;
    std::array<std::uint8_t, 14> frame{0xEA, 0x0C, 0x14, 0xB5, 0xAE, 97, 0xF8};
    frame.back() = coretx::crsf::crc8(std::span{frame}.subspan(2, 11));
    for (const auto byte : frame)
        crsf.push(byte, TimestampUs{100});
    fw::publish_telemetry(crsf, pool, TimestampUs{200});
    const tm::Key rssi{tm::Source::crsf, tm::Quantity::rssi, 1};
    const tm::Key lq{tm::Source::crsf, tm::Quantity::link_quality};
    if ((pool.entries().size() != 4) || (pool.find(rssi)->value != -75) ||
        (pool.find(lq)->value != 97) || (pool.find(lq)->updated_at != TimestampUs{100}))
        return false;
    crsf.invalidate();
    fw::publish_telemetry(crsf, pool, TimestampUs{201});
    if (pool.find(lq)->valid)
        return false;
    for (const auto byte : frame)
        crsf.push(byte, TimestampUs{300});
    fw::publish_telemetry(crsf, pool, TimestampUs{301});
    if (!pool.find(lq)->valid)
        return false;
    fw::publish_telemetry(crsf, pool, TimestampUs{1'000'000});
    pool.service(TimestampUs{2'000'300});
    if (pool.find(lq)->valid)
        return false;

    coretx::afhds2a::Session afhds{};
    if (!afhds.initialize({0x12345678}, {0xCAFEBABE}))
        return false;
    fw::publish_telemetry(afhds.telemetry(), pool);
    const tm::Key errors{tm::Source::afhds2a, tm::Quantity::receiver_error};
    if (pool.find(errors) != nullptr)
        return false;
    coretx::afhds2a::Received packet{};
    packet.fill(0xFF);
    const auto header = coretx::afhds2a::header(0xAA, {0x12345678}, {0xCAFEBABE});
    std::copy_n(header.begin(), 9, packet.begin());
    packet[9] = 0xFE;
    packet[11] = 25;
    if (!afhds.receive(packet, TimestampUs{500}))
        return false;
    fw::publish_telemetry(afhds.telemetry(), pool);
    if ((pool.find(errors)->value != 25) || (pool.find(errors)->updated_at != TimestampUs{500}))
        return false;
    packet[0] = 0xAC;
    packet[11] = 0;
    if (!afhds.receive(packet, TimestampUs{1'000'000}))
        return false;
    fw::publish_telemetry(afhds.telemetry(), pool);
    pool.service(TimestampUs{2'000'500});
    if (pool.find(errors)->valid || (pool.find(errors)->value != 25))
        return false;
    fw::publish_telemetry(afhds.telemetry(), pool);
    if (pool.find(errors)->valid)
        return false;
    afhds.start_bind(TimestampUs{2'000'501});
    return !afhds.telemetry().received;
}

constexpr bool crsf_battery_voltage() {
    for (const std::uint32_t base : {0U, 0xFFFFF000U}) {
        coretx::crsf::Telemetry decoder{};
        tm::Pool pool{};
        std::array<std::uint8_t, 12> frame{0xEA, 10, 0x08, 0, 52, 0, 0, 0, 0, 0, 100};
        const auto checksum = [&]() constexpr {
            frame.back() = coretx::crsf::crc8(std::span{frame}.subspan(2, 9));
        };
        const auto push = [&](const auto &bytes, const std::uint32_t offset) constexpr {
            for (const auto byte : bytes)
                decoder.push(byte, TimestampUs{base + offset});
            fw::publish_telemetry(decoder, pool, TimestampUs{base + offset});
        };
        checksum();
        auto corrupt = frame;
        corrupt.back() ^= 1;
        push(corrupt, 50);
        const tm::Key voltage{tm::Source::crsf, tm::Quantity::flight_battery_voltage};
        if ((pool.find(voltage) != nullptr) || (decoder.crc_errors() == 0))
            return false;
        decoder.reset_stream();
        push(frame, 100);
        const auto *entry = pool.find(voltage);
        if ((entry == nullptr) || (entry->value != 5200) ||
            (entry->updated_at != TimestampUs{base + 100}) ||
            !entry->fresh(TimestampUs{base + 100}) || decoder.fresh(TimestampUs{base + 100}))
            return false;
        std::array<std::uint8_t, 11> short_frame{0xEA, 9, 0x08, 0, 60};
        short_frame.back() = coretx::crsf::crc8(std::span{short_frame}.subspan(2, 8));
        push(short_frame, 200);
        if ((decoder.framing_errors() == 0) ||
            (decoder.battery().updated_at != TimestampUs{base + 100}))
            return false;
        std::array<std::uint8_t, 14> link{0xEA, 12, 0x14, 0xB5, 0xAE, 97, 0xF8};
        link.back() = coretx::crsf::crc8(std::span{link}.subspan(2, 11));
        push(link, 1'000'000);
        decoder.service(TimestampUs{base + 2'000'100});
        fw::publish_telemetry(decoder, pool, TimestampUs{base + 2'000'100});
        if (pool.find(voltage)->valid || !decoder.fresh(TimestampUs{base + 2'000'100}))
            return false;
        fw::publish_telemetry(decoder, pool, TimestampUs{base + 100});
        if (pool.find(voltage)->valid)
            return false;
        frame[4] = 0;
        checksum();
        push(frame, 3'000'000);
        if (!pool.find(voltage)->valid || (pool.find(voltage)->value != 0) ||
            decoder.fresh(TimestampUs{base + 3'000'000}))
            return false;
        decoder.invalidate();
        fw::publish_telemetry(decoder, pool, TimestampUs{base + 3'000'001});
        if (pool.find(voltage)->valid)
            return false;
        frame[3] = frame[4] = 0xFF;
        checksum();
        push(frame, 3'000'002);
        if (pool.find(voltage)->valid)
            return false;
        std::array<std::uint8_t, 13> sensor{0xEA, 11, 0x08, 0x01, 0x02, 0, 0, 0, 0, 0, 100, 4};
        sensor.back() = coretx::crsf::crc8(std::span{sensor}.subspan(2, 10));
        push(sensor, 3'000'003);
        const tm::Key numbered{tm::Source::crsf, tm::Quantity::flight_battery_voltage, 4};
        if ((pool.find(numbered) == nullptr) || (pool.find(numbered)->value != 25'800) ||
            !pool.find(numbered)->valid)
            return false;
    }
    return true;
}

constexpr bool formatting_and_pages() {
    struct Case {
        tm::Quantity quantity{};
        std::int32_t value{};
        std::string_view text{};
    };
    for (const auto &test :
         {Case{tm::Quantity::transmitter_voltage, 6200, "6.20 V"},
          Case{tm::Quantity::current, -2345, "-2.34 A"},
          Case{tm::Quantity::temperature, -1, "-0.1 C"}, Case{tm::Quantity::rssi, -75, "-75 dBm"},
          Case{tm::Quantity::link_quality, 0, "0 %"},
          Case{tm::Quantity::consumption, std::numeric_limits<std::int32_t>::min(),
               "-2147483648 mAh"}}) {
        coretx::ui::Framebuffer display{};
        coretx::ui::Framebuffer expected{};
        const tm::Entry entry{test.value, TimestampUs{0}, {tm::Source::local, test.quantity}, true};
        fw::render_telemetry_value(entry, display, 18, TimestampUs{0});
        expected.text(128 - static_cast<int>(test.text.size()) * 6, 18, test.text);
        if (display.bytes() != expected.bytes())
            return false;
        display.clear();
        expected.clear();
        fw::render_telemetry_value(entry, display, 18, TimestampUs{500'000});
        const std::string_view stale = coretx::ui::tr::stale;
        expected.text(128 - static_cast<int>(stale.size()) * 6, 18, stale);
        if (display.bytes() != expected.bytes())
            return false;
    }
    tm::Pool pool{};
    for (std::uint8_t i = 0; i < 5; ++i)
        pool.publish({tm::Source::local, tm::Quantity::temperature, i}, 250, TimestampUs{0});
    coretx::ui::Framebuffer display{};
    fw::render_telemetry(pool, display, TimestampUs{0}, 1, tm::Source::local);
    coretx::ui::Framebuffer expected{};
    expected.text(10, 18, coretx::ui::tr::telemetry_temperature);
    expected.unsigned_number(
        10 +
            static_cast<int>(std::string_view{coretx::ui::tr::telemetry_temperature}.size() + 1) *
                6 -
            1,
        18, 4);
    for (int y = 18; y < 25; ++y)
        for (int x = 10; x < 60; ++x)
            if (display.pixel_at(x, y) != expected.pixel_at(x, y))
                return false;
    return fw::telemetry_pages(pool) == 2;
}

constexpr bool unknown_quantity_is_explicit() {
    const tm::Entry entry{
        42, TimestampUs{0}, {tm::Source::local, static_cast<tm::Quantity>(255)}, true};
    coretx::ui::Framebuffer display{};
    coretx::ui::Framebuffer expected{};
    fw::render_telemetry_value(entry, display, 18, TimestampUs{0});
    const std::string_view unknown = coretx::ui::tr::unknown;
    expected.text(128 - static_cast<int>(unknown.size()) * 6, 18, unknown);
    return (display.bytes() == expected.bytes()) &&
           (tm::unit(entry.key.quantity) == tm::Unit::unknown);
}

constexpr bool receiver_voltage() {
    for (const auto mode :
         {coretx::afhds2a::ChannelMode::standard14, coretx::afhds2a::ChannelMode::extended16}) {
        coretx::afhds2a::Session session{};
        if (!session.initialize({0x12345678}, {0xCAFEBABE}))
            return false;
        session.set_channel_mode(mode);
        tm::Pool pool{};
        pool.publish(tm::transmitter_battery, 6200, TimestampUs{0});
        fw::publish_telemetry(session.telemetry(), pool);
        if (pool.entries().size() != 1)
            return false;
        const auto packet = [](const std::uint8_t type, const std::size_t offset,
                               const std::uint8_t id, const std::uint16_t value) {
            coretx::afhds2a::Received result{};
            result.fill(0xFF);
            const auto header = coretx::afhds2a::header(type, {0x12345678}, {0xCAFEBABE});
            std::copy_n(header.begin(), 9, result.begin());
            for (std::size_t i = 9; i < offset; i += 4)
                result[i] = 0x01;
            result[offset] = id;
            result[offset + 1] = 2;
            result[offset + 2] = static_cast<std::uint8_t>(value);
            result[offset + 3] = static_cast<std::uint8_t>(value >> 8);
            return result;
        };
        const tm::Key voltage{tm::Source::afhds2a, tm::Quantity::receiver_voltage, 2};
        const auto valid = packet(0xAA, 33, 0x00, 512);
        for (const std::size_t identity : {1U, 5U}) {
            auto unrelated = valid;
            unrelated[identity] ^= 1;
            if (session.receive(unrelated, TimestampUs{50}) || session.telemetry().voltage.received)
                return false;
        }
        if (!session.receive(valid, TimestampUs{100}))
            return false;
        fw::publish_telemetry(session.telemetry(), pool);
        const auto *entry = pool.find(voltage);
        if ((entry == nullptr) || (entry->value != 5120) ||
            (entry->updated_at != TimestampUs{100}) ||
            (pool.find(tm::transmitter_battery)->value != 6200))
            return false;
        for (const std::uint8_t id : {std::uint8_t{0x03}, std::uint8_t{0x04}, std::uint8_t{0x7F},
                                      std::uint8_t{0xFC}, std::uint8_t{0xFD}}) {
            auto other = packet(0xAA, 9, id, 600);
            if ((id == 0xFC) || (id == 0xFD)) {
                other[13] = 0x00;
                other[15] = 1;
            }
            if (!session.receive(other, TimestampUs{200}) ||
                (session.telemetry().voltage.updated_at != TimestampUs{100}) ||
                (session.telemetry().voltage.centivolts != 512))
                return false;
        }
        if (!session.receive(valid, TimestampUs{400}))
            return false;
        auto extended = packet(0xAC, 9, 0x00, 600);
        extended[11] = 4;
        if (!session.receive(extended, TimestampUs{500}))
            return false;
        auto terminated = valid;
        terminated[9] = 0xFF;
        if (!session.receive(terminated, TimestampUs{600}))
            return false;
        if (!session.receive(packet(0xAA, 9, 0xFE, 25), TimestampUs{1'000'000}))
            return false;
        fw::publish_telemetry(session.telemetry(), pool);
        pool.service(TimestampUs{2'000'400});
        const tm::Key errors{tm::Source::afhds2a, tm::Quantity::receiver_error};
        if (pool.find(voltage)->valid || !pool.find(errors)->valid ||
            (pool.find(voltage)->updated_at != TimestampUs{400}))
            return false;
        if (!session.receive(packet(0xAA, 9, 0x00, 0), TimestampUs{2'000'401}))
            return false;
        fw::publish_telemetry(session.telemetry(), pool);
        if (!pool.find(voltage)->valid || (pool.find(voltage)->value != 0))
            return false;
        session.start_bind(TimestampUs{2'000'402});
        fw::publish_telemetry(session.telemetry(), pool);
        if (pool.find(voltage)->valid || session.telemetry().voltage.received ||
            (pool.find(tm::transmitter_battery)->value != 6200))
            return false;
    }
    return true;
}

static_assert(receiver_voltage());
static_assert(crsf_battery_voltage());
static_assert(sources_and_capacity());
static_assert(independent_freshness());
static_assert(decoder_adapters());
static_assert(formatting_and_pages());
static_assert(unknown_quantity_is_explicit());
} // namespace

int main() {
    if (!sources_and_capacity() || !independent_freshness() || !decoder_adapters() ||
        !formatting_and_pages() || !unknown_quantity_is_explicit() || !receiver_voltage() ||
        !crsf_battery_voltage()) {
        std::fputs("Unified telemetry tests failed\n", stderr);
        return 1;
    }
}
