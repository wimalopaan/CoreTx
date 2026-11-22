// SPDX-License-Identifier: GPL-2.0-or-later
#include "boards/common/radio_storage.hpp"
#include "platform/stm32f072/afhds2a.hpp"

#include "static_fixture.hpp"

#include <cstdio>
#include <cstdlib>

namespace {
namespace protocol = coretx::afhds2a;
namespace fw = coretx::firmware;
using namespace coretx::literals;

constexpr bool packets_and_binding() {
    protocol::Session session{};
    if (!session.initialize({0x12345678}, {}))
        return false;
    protocol::Hops hops{};
    if (!protocol::make_hops({0x12345678}, hops))
        return false;
    for (std::size_t i = 0; i < hops.size(); ++i) {
        if ((hops[i] < 1) || (hops[i] > 164))
            return false;
        for (std::size_t j = 0; j < i; ++j) {
            const auto difference = static_cast<int>(hops[i]) - hops[j];
            if ((difference > -5) && (difference < 5))
                return false;
        }
    }
    session.start_bind(coretx::TimestampUs{0xFFFFF000U});
    protocol::Channels channels{};
    const auto first = session.prepare(channels);
    if ((first.packet[0] != 0xBB) || (first.channel != 0x0D) || (first.packet[1] != 0x78) ||
        (first.packet[5] != 0xFF) ||
        !std::equal(hops.begin(), hops.end(), first.packet.begin() + 11))
        return false;
    session.transmitted();
    protocol::Received reply{};
    reply[0] = 0xBC;
    reply[1] = 0x78;
    reply[2] = 0x56;
    reply[3] = 0x34;
    reply[4] = 0x12;
    reply[5] = 0xBE;
    reply[6] = 0xBA;
    reply[7] = 0xFE;
    reply[8] = 0xCA;
    reply[9] = 1;
    reply[1] ^= 1;
    if (session.receive(reply, coretx::TimestampUs{}))
        return false;
    reply[1] ^= 1;
    if (!session.receive(reply, coretx::TimestampUs{}))
        return false;
    for (unsigned i = 0; i < 4; ++i) {
        const auto plan = session.prepare(channels);
        if ((plan.packet[0] != 0xBC) || (plan.packet[9] != 2) || !session.binding() ||
            session.take_receiver_changed())
            return false;
        session.transmitted();
    }
    if (session.binding() || (session.receiver().value != 0xCAFEBABE) ||
        !session.take_receiver_changed() || session.take_receiver_changed())
        return false;
    const auto initial_data = session.prepare(channels);
    if ((initial_data.packet[0] != 0x58) || (initial_data.channel != hops[1]))
        return false;
    if (session.prepare(channels).packet[0] != 0xAA)
        return false;
    if (session.prepare(channels).packet[0] != 0x56)
        return false;
    channels[0] = -1024;
    channels[1] = 1024;
    channels[14] = -1024;
    channels[15] = 1024;
    const auto data = session.prepare(channels);
    if ((data.packet[0] != 0x58) || (data.packet[9] != 0xDC) || (data.packet[10] != 3) ||
        (data.packet[11] != 0xDC) || (data.packet[12] != 7) || (data.packet[14] != 5) ||
        (data.packet[16] != 5) || (data.packet[18] != 5) || (data.packet[20] != 5) ||
        (data.packet[34] != 0x55) || (data.packet[36] != 0x85))
        return false;
    const auto failsafe = protocol::failsafe({0x12345678}, session.receiver());
    for (std::size_t i = 0; i < 14; ++i)
        if ((failsafe[9 + i * 2] != 0xDC) || (failsafe[10 + i * 2] != 5))
            return false;
    session.start_bind(coretx::TimestampUs{10});
    session.cancel_bind();
    return session.bound() && !session.binding();
}

constexpr bool standard_channel_packets() {
    protocol::Channels channels{};
    channels[0] = -1024;
    channels[1] = 1024;
    channels[14] = -1024;
    channels[15] = 1024;
    const protocol::Packet expected{0x58, 0x78, 0x56, 0x34, 0x12, 0xBE, 0xBA, 0xFE, 0xCA, 0xDC,
                                    0x03, 0xDC, 0x07, 0xDC, 0x05, 0xDC, 0x05, 0xDC, 0x05, 0xDC,
                                    0x05, 0xDC, 0x05, 0xDC, 0x05, 0xDC, 0x05, 0xDC, 0x05, 0xDC,
                                    0x05, 0xDC, 0x05, 0xDC, 0x45, 0xDC, 0x85, 0x00};
    if (protocol::sticks({0x12345678}, {0xCAFEBABE}, channels, 4) != expected)
        return false;
    protocol::Session restored{};
    protocol::Hops hops{};
    if (!restored.initialize({0x12345678}, {0xCAFEBABE}) ||
        !protocol::make_hops({0x12345678}, hops))
        return false;
    const auto first = restored.prepare(channels);
    return (first.packet[0] == 0x58) && (first.channel == hops[0]) &&
           (restored.prepare(channels).packet[0] == 0xAA) &&
           (restored.prepare(channels).packet[0] == 0x56);
}

constexpr bool extended_channel_packets() {
    protocol::Channels channels{};
    channels[0] = channels[14] = -1024;
    channels[1] = channels[15] = 1024;
    const protocol::Packet expected{0x58, 0x78, 0x56, 0x34, 0x12, 0xBE, 0xBA, 0xFE, 0xCA, 0xDC,
                                    0xC3, 0xDC, 0xD7, 0xDC, 0x35, 0xDC, 0xC5, 0xDC, 0xD5, 0xDC,
                                    0x75, 0xDC, 0x65, 0xDC, 0xD5, 0xDC, 0x65, 0xDC, 0x05, 0xDC,
                                    0x05, 0xDC, 0x05, 0xDC, 0x45, 0xDC, 0x85, 0x00};
    const protocol::Packet expected_failsafe{
        0x56, 0x78, 0x56, 0x34, 0x12, 0xBE, 0xBA, 0xFE, 0xCA, 0xDC, 0xC5, 0xDC, 0xD5,
        0xDC, 0x55, 0xDC, 0xC5, 0xDC, 0xD5, 0xDC, 0x55, 0xDC, 0x05, 0xDC, 0x05, 0xDC,
        0x05, 0xDC, 0x05, 0xDC, 0x05, 0xDC, 0x05, 0xDC, 0x05, 0xDC, 0x05, 0x00};
    return (protocol::sticks({0x12345678}, {0xCAFEBABE}, channels, 4,
                             protocol::ChannelMode::extended16, 25) == expected) &&
           (protocol::failsafe({0x12345678}, {0xCAFEBABE}, protocol::ChannelMode::extended16) ==
            expected_failsafe);
}

constexpr bool paired_receiver_lqi() {
    protocol::Session session{};
    protocol::Channels channels{};
    if (!session.initialize({0x12345678}, {0xCAFEBABE}))
        return false;
    session.set_channel_mode(protocol::ChannelMode::extended16);
    const auto lqi = [](const protocol::Packet &packet) {
        return (packet[22] >> 4) | (packet[24] & 0xF0) | ((packet[26] & 0xF0) << 4);
    };
    if (lqi(session.prepare(channels).packet) != 1000)
        return false;
    (void)session.prepare(channels);
    (void)session.prepare(channels);
    protocol::Received telemetry{};
    telemetry.fill(0xFF);
    const auto header = protocol::header(0xAA, {0x12345678}, {0xCAFEBABE});
    std::copy_n(header.begin(), 9, telemetry.begin());
    telemetry[9] = 0xFE;
    telemetry[10] = 0;
    telemetry[11] = 25;
    telemetry[12] = 0;
    for (const std::size_t identity : {1U, 5U}) {
        auto unrelated = telemetry;
        unrelated[identity] ^= 1;
        if (session.receive(unrelated, coretx::TimestampUs{100}) ||
            (lqi(session.prepare(channels).packet) != 1000))
            return false;
    }
    if (!session.receive(telemetry, coretx::TimestampUs{200}) ||
        (lqi(session.prepare(channels).packet) != 1750))
        return false;
    telemetry[11] = 101;
    if (!session.receive(telemetry, coretx::TimestampUs{300}) ||
        (lqi(session.prepare(channels).packet) != 1750))
        return false;
    telemetry[0] = 0xAC;
    telemetry[11] = 0;
    if (!session.receive(telemetry, coretx::TimestampUs{400}) ||
        (lqi(session.prepare(channels).packet) != 1750))
        return false;
    telemetry[0] = 0xAA;
    telemetry[9] = 0xFF;
    telemetry[13] = 0xFE;
    telemetry[15] = 0;
    return session.receive(telemetry, coretx::TimestampUs{500}) &&
           (lqi(session.prepare(channels).packet) == 1750);
}

struct Hardware {
    fw::A7105Info info{fw::A7105State::ready, fw::A7105Error::none};
    std::array<std::uint8_t, 64> registers{};
    protocol::Packet packet{};
    protocol::Received received{};
    fw::A7105FrontEnd front{};
    std::uint8_t value{};
    std::uint8_t rx_flags{};
    bool pending{};
    bool complete{};
    bool transmitting{};
    bool rx_available{};
    bool auto_bind{};
    bool calibration_stuck{};
    bool tx_stuck{};
    unsigned tx_count{};
};
template <typename Test> struct Bus {
    static void reset() noexcept {}
    static void initialize(coretx::TimestampUs) {
        auto &h = Test::context;
        h.info = {fw::A7105State::ready, fw::A7105Error::none};
    }
    static void service(coretx::TimestampUs) {
        auto &h = Test::context;
        if (h.pending) {
            h.pending = false;
            h.complete = true;
        }
    }
    static auto info() {
        auto &h = Test::context;
        return h.info;
    }
    static bool busy() {
        auto &h = Test::context;
        return h.pending;
    }
    static bool completed() {
        auto &h = Test::context;
        return h.complete;
    }
    static auto value() {
        auto &h = Test::context;
        return h.value;
    }
    static std::span<const std::uint8_t> payload() {
        auto &h = Test::context;
        return h.received;
    }
    static void power_off() {
        auto &h = Test::context;
        h.front = fw::A7105FrontEnd::off;
        h.info.state = fw::A7105State::off;
    }
    static void front_end(fw::A7105FrontEnd front) {
        auto &h = Test::context;
        h.front = front;
    }
    static bool command(const std::uint8_t cmd, const std::span<const std::uint8_t> bytes,
                        const std::uint8_t reads, coretx::TimestampUs) {
        auto &h = Test::context;
        if (h.pending)
            std::abort();
        h.pending = true;
        h.complete = false;
        h.value = 0;
        if (cmd == 0xD0) {
            if (h.front != fw::A7105FrontEnd::transmit)
                std::abort();
            h.transmitting = true;
            ++h.tx_count;
        } else if ((cmd == 0xC0) && h.auto_bind &&
                   ((h.packet[0] == 0xBB) || (h.packet[0] == 0xBC))) {
            h.received.fill(0);
            h.received[0] = 0xBC;
            std::copy(h.packet.begin() + 1, h.packet.begin() + 5, h.received.begin() + 1);
            h.received[5] = 0xBE;
            h.received[6] = 0xBA;
            h.received[7] = 0xFE;
            h.received[8] = 0xCA;
            h.received[9] = 1;
            h.rx_available = true;
        } else if (cmd == 0x05) {
            if (bytes.size() != h.packet.size())
                std::abort();
            std::copy(bytes.begin(), bytes.end(), h.packet.begin());
        } else if (cmd == 0x40) {
            if (h.transmitting) {
                h.value = h.tx_stuck ? 1 : 0;
                h.transmitting = h.tx_stuck;
            } else {
                h.value = static_cast<std::uint8_t>((h.rx_available ? 0 : 1) | h.rx_flags);
            }
        } else if (cmd == 0x45) {
            h.rx_available = false;
        } else if (reads != 0) {
            h.value = h.registers[cmd & 0x3FU];
            if ((cmd == 0x42) && !h.calibration_stuck)
                h.value = 0;
        } else if ((cmd < 0x40) && !bytes.empty()) {
            h.registers[cmd] = bytes[0];
        }
        return true;
    }
};

bool cooperative_radio(const std::uint8_t rx_flags = 0) {
    struct TestScenario {};
    using Test = coretx::test::FakeApplication<TestScenario, Hardware, Bus>;
    Test::reset();
    auto &hardware = Test::context;
    hardware.rx_flags = rx_flags;
    fw::Afhds2a<Test::Board> radio{};
    radio.initialize({0x12345678}, {}, coretx::TimestampUs{});
    protocol::Channels channels{};
    for (std::uint32_t t = 0; t < 2'000; t += 10)
        radio.service(coretx::TimestampUs{t}, channels, true, coretx::TimestampUs{t});
    if (!radio.ready() || (hardware.tx_count != 0) || (hardware.registers[0x26] != 0x3B) ||
        (hardware.front != fw::A7105FrontEnd::off))
        return false;
    radio.activate();
    if (!radio.bind(coretx::TimestampUs{2'000}))
        return false;
    hardware.auto_bind = true;
    for (std::uint32_t t = 2'000; t < 30'000; t += 10)
        radio.service(coretx::TimestampUs{t}, channels, false, coretx::TimestampUs{});
    if (radio.session().binding() || !radio.session().bound() || !radio.take_receiver_changed() ||
        (hardware.tx_count != 5) || (radio.error() != fw::Afhds2aError::none))
        return false;
    for (std::uint32_t t = 30'000; t < 40'000; t += 10)
        radio.service(coretx::TimestampUs{t}, channels, true, coretx::TimestampUs{t});
    if (hardware.tx_count <= 5)
        return false;
    radio.deactivate();
    const auto transmissions = hardware.tx_count;
    for (std::uint32_t t = 40'000; t < 50'000; t += 10)
        radio.service(coretx::TimestampUs{t}, channels, true, coretx::TimestampUs{t});
    if ((hardware.tx_count != transmissions) || (hardware.front != fw::A7105FrontEnd::off))
        return false;
    struct FailedTestScenario {};
    using FailedTest = coretx::test::FakeApplication<FailedTestScenario, Hardware, Bus>;
    FailedTest::reset();
    auto &failed = FailedTest::context;
    failed.calibration_stuck = true;
    fw::Afhds2a<FailedTest::Board> bad{};
    bad.initialize({1}, {}, coretx::TimestampUs{});
    for (std::uint32_t t = 0; t < 10'000; t += 10)
        bad.service(coretx::TimestampUs{t}, channels, true, coretx::TimestampUs{t});
    return (bad.error() == fw::Afhds2aError::calibration) && (failed.tx_count == 0);
}

bool crc_failed_bind_is_ignored() {
    struct TestScenario {};
    using Test = coretx::test::FakeApplication<TestScenario, Hardware, Bus>;
    Test::reset();
    auto &hardware = Test::context;
    hardware.auto_bind = true;
    hardware.rx_flags = fw::a7105::mode_crc_fail;
    fw::Afhds2a<Test::Board> radio{};
    protocol::Channels channels{};
    radio.initialize({0x12345678}, {}, coretx::TimestampUs{});
    radio.activate();
    for (std::uint32_t t = 0; t < 2'000; t += 10)
        radio.service(coretx::TimestampUs{t}, channels, false, coretx::TimestampUs{});
    if (!radio.bind(coretx::TimestampUs{2'000}))
        return false;
    for (std::uint32_t t = 2'000; t < 22'000; t += 10)
        radio.service(coretx::TimestampUs{t}, channels, false, coretx::TimestampUs{});
    return radio.session().binding() && !radio.session().bound() &&
           !radio.take_receiver_changed() && (hardware.tx_count > 4) &&
           (radio.error() == fw::Afhds2aError::none);
}

bool receiver_voltage_crc_gate() {
    for (const std::uint8_t flags :
         {std::uint8_t{0}, fw::a7105::mode_fec_fail, fw::a7105::mode_crc_fail}) {
        struct TestScenario {};
        using Test = coretx::test::FakeApplication<TestScenario, Hardware, Bus>;
        Test::reset();
        auto &hardware = Test::context;
        hardware.rx_flags = flags;
        hardware.received.fill(0xFF);
        const auto header = protocol::header(0xAA, {0x12345678}, {0xCAFEBABE});
        std::copy_n(header.begin(), 9, hardware.received.begin());
        hardware.received[9] = 0x00;
        hardware.received[10] = 0;
        hardware.received[11] = 0xF4;
        hardware.received[12] = 0x01;
        fw::Afhds2a<Test::Board> radio{};
        protocol::Channels channels{};
        radio.initialize({0x12345678}, {0xCAFEBABE}, coretx::TimestampUs{});
        radio.activate();
        for (std::uint32_t t = 0; t < 12'000; t += 10) {
            if (hardware.front == fw::A7105FrontEnd::receive)
                hardware.rx_available = true;
            radio.service(coretx::TimestampUs{t}, channels, true, coretx::TimestampUs{t});
        }
        const auto reading = radio.telemetry().voltage;
        if ((radio.error() != fw::Afhds2aError::none) || (radio.session().frames() == 0) ||
            (reading.received != (flags != fw::a7105::mode_crc_fail)) ||
            (reading.received && (reading.centivolts != 500)))
            return false;
        radio.deactivate();
        if (radio.telemetry().voltage.received)
            return false;
    }
    return true;
}

constexpr bool records() {
    const fw::RadioSettings settings{{0x12345678}, {0xCAFEBABE}, fw::RadioOutput::internal};
    auto bytes = fw::radio_storage::encode(settings, 42);
    const auto decoded = fw::radio_storage::decode(bytes);
    if (!decoded.valid() || (decoded.settings != settings) || (decoded.generation != 42))
        return false;
    auto extended = settings;
    extended.channel_mode = protocol::ChannelMode::extended16;
    if (fw::radio_storage::decode(fw::radio_storage::encode(extended, 43)).settings != extended)
        return false;
    auto legacy = bytes;
    legacy[4] = 1;
    coretx::storage::detail::Writer legacy_crc{std::span{legacy}.last(4)};
    legacy_crc.word(coretx::storage::crc32(std::span{legacy}.first(60)));
    const auto migrated = fw::radio_storage::decode(legacy);
    if (!migrated.valid() || (migrated.settings != extended) || (migrated.generation != 42))
        return false;
    auto invalid_mode = bytes;
    invalid_mode[6] = 2;
    coretx::storage::detail::Writer invalid_crc{std::span{invalid_mode}.last(4)};
    invalid_crc.word(coretx::storage::crc32(std::span{invalid_mode}.first(60)));
    if (fw::radio_storage::decode(invalid_mode).status != fw::RadioStorageStatus::invalid)
        return false;
    bytes[15] ^= 1;
    return !fw::radio_storage::decode(bytes).valid();
}

bool radio_shutdown_and_deadlines() {
    for (const std::uint32_t base : {0U, 0xFFFFF000U}) {
        struct TestScenario {};
        using Test = coretx::test::FakeApplication<TestScenario, Hardware, Bus>;
        Test::reset();
        auto &hardware = Test::context;
        fw::Afhds2a<Test::Board> radio{};
        protocol::Channels channels{};
        radio.initialize({0x12345678}, {0xCAFEBABE}, coretx::TimestampUs{base});
        radio.activate();
        for (std::uint32_t t = 0; t < 10'000; t += 10)
            radio.service(coretx::TimestampUs{base + t}, channels, true,
                          coretx::TimestampUs{base + t});
        const auto sent = hardware.tx_count;
        radio.service(coretx::TimestampUs{base + 20'000}, channels, true,
                      coretx::TimestampUs{base + 10'000});
        if (hardware.front != fw::A7105FrontEnd::off)
            return false;
        for (std::uint32_t t = 20'010; t < 30'000; t += 10)
            radio.service(coretx::TimestampUs{base + t}, channels, false,
                          coretx::TimestampUs{base});
        if ((hardware.tx_count != sent) || (radio.error() != fw::Afhds2aError::none))
            return false;
        hardware.tx_stuck = true;
        for (std::uint32_t t = 30'000; t < 35'000; t += 10)
            radio.service(coretx::TimestampUs{base + t}, channels, true,
                          coretx::TimestampUs{base + t});
        if ((radio.error() != fw::Afhds2aError::tx_timeout) ||
            (hardware.front != fw::A7105FrontEnd::off))
            return false;
        struct BindingTestScenario {};
        using BindingTest = coretx::test::FakeApplication<BindingTestScenario, Hardware, Bus>;
        BindingTest::reset();
        auto &binding = BindingTest::context;
        fw::Afhds2a<BindingTest::Board> unbound{};
        unbound.initialize({0x12345678}, {}, coretx::TimestampUs{base});
        unbound.activate();
        for (std::uint32_t t = 0; t < 2'000; t += 10)
            unbound.service(coretx::TimestampUs{base + t}, channels, false, coretx::TimestampUs{});
        if (!unbound.bind(coretx::TimestampUs{base + 2'000}))
            return false;
        unbound.service(coretx::TimestampUs{base + 60'002'000U}, channels, false,
                        coretx::TimestampUs{});
        if ((unbound.error() != fw::Afhds2aError::bind_timeout) ||
            (binding.front != fw::A7105FrontEnd::off))
            return false;
    }
    return true;
}

bool expired_packet_is_not_transmitted() {
    struct TestScenario {};
    using Test = coretx::test::FakeApplication<TestScenario, Hardware, Bus>;
    Test::reset();
    auto &hardware = Test::context;
    fw::Afhds2a<Test::Board> radio{};
    protocol::Channels channels{};
    radio.initialize({0x12345678}, {}, coretx::TimestampUs{});
    radio.activate();
    for (std::uint32_t t = 0; t < 2'000; t += 10)
        radio.service(coretx::TimestampUs{t}, channels, false, coretx::TimestampUs{});
    if (!radio.bind(coretx::TimestampUs{2'000}))
        return false;
    for (std::uint32_t t = 2'000; t < 12'000; t += 1'000)
        radio.service(coretx::TimestampUs{t}, channels, false, coretx::TimestampUs{});
    if ((radio.error() != fw::Afhds2aError::preparation_timeout) || (hardware.tx_count != 0) ||
        (hardware.front != fw::A7105FrontEnd::off))
        return false;
    radio.service(coretx::TimestampUs{100'000}, channels, false, coretx::TimestampUs{});
    return (radio.error() == fw::Afhds2aError::preparation_timeout) && (hardware.tx_count == 0) &&
           (hardware.front == fw::A7105FrontEnd::off);
}

bool transmission_completion_window() {
    struct TestScenario {};
    using Test = coretx::test::FakeApplication<TestScenario, Hardware, Bus>;
    Test::reset();
    auto &hardware = Test::context;
    fw::Afhds2a<Test::Board> radio{};
    protocol::Channels channels{};
    radio.initialize({0x12345678}, {0xCAFEBABE}, coretx::TimestampUs{});
    for (std::uint32_t t = 0; t < 2'000; t += 10)
        radio.service(coretx::TimestampUs{t}, channels, false, coretx::TimestampUs{});
    radio.activate();
    std::uint32_t t = 2'000;
    for (; (t < 3'000) && (hardware.tx_count == 0); t += 10)
        radio.service(coretx::TimestampUs{t}, channels, true, coretx::TimestampUs{t});
    if (hardware.tx_count != 1)
        return false;
    const auto started = t;
    for (; t < (started + 1'550); t += 10) {
        radio.service(coretx::TimestampUs{t}, channels, true, coretx::TimestampUs{t});
        if ((radio.session().frames() != 0) || (hardware.front != fw::A7105FrontEnd::transmit))
            return false;
    }
    for (; t < (started + 1'800); t += 10)
        radio.service(coretx::TimestampUs{t}, channels, true, coretx::TimestampUs{t});
    return (radio.session().frames() == 1) && (hardware.tx_count == 1) &&
           (hardware.front == fw::A7105FrontEnd::receive) &&
           (radio.error() == fw::Afhds2aError::none);
}

bool gio2_completes_transmission(const bool binding) {
    struct TestScenario {};
    using Test = coretx::test::FakeApplication<TestScenario, Hardware, Bus>;
    Test::reset();
    auto &hardware = Test::context;
    fw::Afhds2a<Test::Board> radio{};
    protocol::Channels channels{};
    radio.initialize({0x12345678}, {0xCAFEBABE}, coretx::TimestampUs{});
    for (std::uint32_t t = 0; t < 2'000; t += 10)
        radio.service(coretx::TimestampUs{t}, channels, true, coretx::TimestampUs{t});
    radio.gio2_event();
    radio.activate();
    if (binding && !radio.bind(coretx::TimestampUs{2'000}))
        return false;
    std::uint32_t t = 2'000;
    for (; (t < 3'000) && (hardware.tx_count == 0); t += 10)
        radio.service(coretx::TimestampUs{t}, channels, true, coretx::TimestampUs{t});
    const auto started = t;
    for (; t < (started + 1'400); t += 10)
        radio.service(coretx::TimestampUs{t}, channels, true, coretx::TimestampUs{t});
    if (radio.session().frames() != 0)
        return false;
    radio.gio2_event();
    for (; t < (started + 1'450); t += 10)
        radio.service(coretx::TimestampUs{t}, channels, true, coretx::TimestampUs{t});
    return (hardware.tx_count == 1) && (radio.session().frames() == 1) &&
           (hardware.front == (binding ? fw::A7105FrontEnd::off : fw::A7105FrontEnd::receive)) &&
           (radio.error() == fw::Afhds2aError::none);
}

bool stale_idle_frame_resumes() {
    struct TestScenario {};
    using Test = coretx::test::FakeApplication<TestScenario, Hardware, Bus>;
    Test::reset();
    auto &hardware = Test::context;
    hardware.rx_flags = fw::a7105::mode_crc_fail;
    hardware.rx_available = true;
    fw::Afhds2a<Test::Board> radio{};
    protocol::Channels channels{};
    radio.initialize({0x12345678}, {0xCAFEBABE}, coretx::TimestampUs{});
    radio.activate();
    for (std::uint32_t t = 0; t < 3'700; t += 10)
        radio.service(coretx::TimestampUs{t}, channels, true, coretx::TimestampUs{t});
    if (hardware.pending || (hardware.front != fw::A7105FrontEnd::off) || (hardware.tx_count != 1))
        return false;
    for (std::uint32_t t = 3'700; t < 20'000; t += 10)
        radio.service(coretx::TimestampUs{t}, channels, false, coretx::TimestampUs{});
    if (hardware.tx_count != 1)
        return false;
    for (std::uint32_t t = 20'000; t < 30'000; t += 10)
        radio.service(coretx::TimestampUs{t}, channels, true, coretx::TimestampUs{t});
    return (radio.error() == fw::Afhds2aError::none) && (hardware.tx_count >= 3);
}

struct Eeprom {
    std::array<std::uint8_t, 2176> bytes{};
    std::span<std::uint8_t> input{};
    std::span<const std::uint8_t> output{};
    std::uint16_t address{};
    unsigned writes{};
    bool pending{};
    bool fail{};
    fw::i6x::I2cError error{};
    constexpr Eeprom() { bytes.fill(0xFF); }
};
template <typename Test> struct Memory {
    static void reset() noexcept {}
    static void select(std::uint8_t) {
        auto &e = Test::context;
        if (e.pending)
            std::abort();
    }
    static bool busy() {
        auto &e = Test::context;
        return e.pending;
    }
    static auto error() {
        auto &e = Test::context;
        return e.error;
    }
    static bool read(std::uint16_t address, std::span<std::uint8_t> bytes) {
        auto &e = Test::context;
        if (e.pending || ((address + bytes.size()) > e.bytes.size()))
            std::abort();
        e.input = bytes;
        e.output = {};
        e.address = address;
        return e.pending = true;
    }
    static bool write(std::uint16_t address, std::span<const std::uint8_t> bytes) {
        auto &e = Test::context;
        if (e.pending || bytes.empty() || ((address / 64) != ((address + bytes.size() - 1) / 64)) ||
            ((address + bytes.size()) > e.bytes.size()))
            std::abort();
        e.output = bytes;
        e.input = {};
        e.address = address;
        ++e.writes;
        return e.pending = true;
    }
    static void service(coretx::TimestampUs) {
        auto &e = Test::context;
        if (e.fail) {
            e.error = fw::i6x::I2cError::bus;
        } else if (!e.input.empty()) {
            std::copy_n(e.bytes.begin() + e.address, e.input.size(), e.input.begin());
        } else {
            std::copy(e.output.begin(), e.output.end(), e.bytes.begin() + e.address);
        }
        e.pending = false;
    }
};
template <typename Journal> void journal_step(Journal &journal) {
    journal.service(coretx::TimestampUs{}, {fw::i6x::StorageProbeState::found, {}, 1});
}
template <typename Journal> void journal_run(Journal &journal) {
    for (unsigned i = 0; i < 12; ++i)
        journal_step(journal);
}
bool legacy_settings_migration() {
    constexpr fw::RadioSettings expected{
        {0x12345678}, {0xCAFEBABE}, fw::RadioOutput::internal, protocol::ChannelMode::extended16};
    auto legacy = fw::radio_storage::encode(expected, 7);
    legacy[4] = 1;
    legacy[6] = 0;
    coretx::storage::detail::Writer checksum{std::span{legacy}.last(4)};
    checksum.word(coretx::storage::crc32(std::span{legacy}.first(60)));
    struct Scenario {};
    using Test = coretx::test::FakeApplication<Scenario, Eeprom, Memory>;
    using Journal = fw::RadioStorage<Test::Board>;
    Test::reset();
    auto &eeprom = Test::context;
    std::copy(legacy.begin(), legacy.end(), eeprom.bytes.begin() + 2048);
    Journal journal{};
    journal.initialize(expected.transmitter);
    journal_run(journal);
    if ((journal.settings() != expected) || (journal.status() != fw::RadioStorageStatus::ready) ||
        (eeprom.writes != 0))
        return false;
    auto changed = expected;
    changed.channel_mode = protocol::ChannelMode::standard14;
    journal.save(changed);
    journal_run(journal);
    struct RebootScenario {};
    using RebootTest = coretx::test::FakeApplication<RebootScenario, Eeprom, Memory>;
    RebootTest::reset();
    RebootTest::context.bytes = eeprom.bytes;
    fw::RadioStorage<RebootTest::Board> reboot{};
    reboot.initialize(expected.transmitter);
    journal_run(reboot);
    return (reboot.status() == fw::RadioStorageStatus::ready) && (reboot.settings() == changed) &&
           (eeprom.bytes[2116] == 2) &&
           std::equal(legacy.begin(), legacy.end(), eeprom.bytes.begin() + 2048);
}
bool persistence() {
    constexpr fw::RadioSettings original{{0x12345678}, {0xCAFEBABE}, fw::RadioOutput::internal};
    struct Scenario {};
    using Test = coretx::test::FakeApplication<Scenario, Eeprom, Memory>;
    using Journal = fw::RadioStorage<Test::Board>;
    Test::reset();
    auto &saved = Test::context;
    Journal first{};
    first.initialize(original.transmitter);
    journal_run(first);
    if (!first.loaded() || (first.status() != fw::RadioStorageStatus::empty) ||
        (first.settings().output != fw::RadioOutput::off))
        return false;
    first.save(original);
    journal_run(first);
    if ((first.status() != fw::RadioStorageStatus::saved) || (saved.writes != 3))
        return false;
    for (unsigned interruption = 0; interruption < 6; ++interruption) {
        struct WritingScenario {};
        using WritingTest = coretx::test::FakeApplication<WritingScenario, Eeprom, Memory>;
        WritingTest::reset();
        auto &eeprom = WritingTest::context;
        eeprom.bytes = saved.bytes;
        fw::RadioStorage<WritingTest::Board> writer{};
        writer.initialize(original.transmitter);
        journal_run(writer);
        if (writer.settings() != original)
            return false;
        auto changed = original;
        changed.channel_mode = protocol::ChannelMode::extended16;
        changed.output = fw::RadioOutput::external;
        writer.save(changed);
        for (unsigned i = 0; i < interruption; ++i)
            journal_step(writer);
        eeprom.pending = false;
        struct RebootScenario {};
        using RebootTest = coretx::test::FakeApplication<RebootScenario, Eeprom, Memory>;
        RebootTest::reset();
        RebootTest::context.bytes = eeprom.bytes;
        fw::RadioStorage<RebootTest::Board> reboot{};
        reboot.initialize(original.transmitter);
        journal_run(reboot);
        if (!reboot.loaded() || (reboot.status() != fw::RadioStorageStatus::ready) ||
            ((reboot.settings() != original) && (reboot.settings() != changed)))
            return false;
    }
    auto future = fw::radio_storage::encode(original, 100);
    future[4] = fw::radio_storage::version + 1;
    coretx::storage::detail::Writer checksum{std::span{future}.last(4)};
    checksum.word(coretx::storage::crc32(std::span{future}.first(60)));
    std::copy(future.begin(), future.end(), saved.bytes.begin() + 2112);
    Journal newer{};
    newer.initialize(original.transmitter);
    journal_run(newer);
    const auto writes = saved.writes;
    newer.save(original);
    journal_run(newer);
    if ((newer.status() != fw::RadioStorageStatus::version) || (saved.writes != writes))
        return false;
    struct FailedScenario {};
    using FailedTest = coretx::test::FakeApplication<FailedScenario, Eeprom, Memory>;
    FailedTest::reset();
    auto &failed = FailedTest::context;
    failed.fail = true;
    fw::RadioStorage<FailedTest::Board> unreadable{};
    unreadable.initialize(original.transmitter);
    journal_run(unreadable);
    return (unreadable.status() == fw::RadioStorageStatus::io_error) &&
           (unreadable.settings().output == fw::RadioOutput::off);
}

static_assert(packets_and_binding());
static_assert(standard_channel_packets());
static_assert(extended_channel_packets());
static_assert(paired_receiver_lqi());
static_assert(records());
} // namespace

int main() {
    if (!packets_and_binding() || !standard_channel_packets() || !extended_channel_packets() ||
        !paired_receiver_lqi() || !cooperative_radio() ||
        !cooperative_radio(fw::a7105::mode_fec_fail) || !crc_failed_bind_is_ignored() ||
        !receiver_voltage_crc_gate() || !records() || !persistence() ||
        !legacy_settings_migration() || !radio_shutdown_and_deadlines() ||
        !expired_packet_is_not_transmitted() || !transmission_completion_window() ||
        !stale_idle_frame_resumes() || !gio2_completes_transmission(false) ||
        !gio2_completes_transmission(true)) {
        std::fputs("AFHDS 2A tests failed\n", stderr);
        return 1;
    }
}
