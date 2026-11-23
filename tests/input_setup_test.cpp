// SPDX-License-Identifier: GPL-2.0-or-later
#include "boards/common/extended_inputs.hpp"
#include "boards/common/input_menu.hpp"
#include "boards/common/input_storage.hpp"
#include "boards/common/menu.hpp"
#include "boards/i6s/touch_navigation.hpp"
#include "platform/stm32f072/eeprom.hpp"
#include "platform/stm32f072/lcd.hpp"
#include "static_fixture.hpp"

#include <cstdio>
#include <cstdlib>

namespace {
using namespace coretx;
namespace fw = coretx::firmware;
namespace features = fw::features;
#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression);                     \
            std::abort();                                                                          \
        }                                                                                          \
    } while (false)

struct MemoryState {
    std::array<std::uint8_t, 16384> bytes{};
    int writes_left{100};
    constexpr MemoryState() {
        bytes.fill(0xA5);
        std::fill(bytes.begin() + fw::input_storage::base_address, bytes.end(), 0xFF);
    }
};
template <typename Test> struct Memory {
    static constexpr std::size_t page_size = 64;
    static void reset() noexcept {}
    static void select(std::uint8_t) {}
    static void service(TimestampUs) {}
    static bool busy() { return false; }
    static fw::I2cError error() { return fw::I2cError::none; }
    static bool read(std::uint16_t address, std::span<std::uint8_t> result) {
        auto &memory = Test::context;
        CHECK((result.size() <= page_size) && ((address + result.size()) <= memory.bytes.size()));
        std::copy_n(memory.bytes.begin() + address, result.size(), result.begin());
        return true;
    }
    static bool write(std::uint16_t address, std::span<const std::uint8_t> data) {
        auto &memory = Test::context;
        CHECK((data.size() <= page_size) && (((address % page_size) + data.size()) <= page_size));
        CHECK((address >= fw::input_storage::base_address) &&
              ((address + data.size()) <= (fw::input_storage::base_address + 256)));
        if (memory.writes_left-- <= 0)
            return false;
        std::copy(data.begin(), data.end(), memory.bytes.begin() + address);
        return true;
    }
};
template <typename Store> void service(Store &store) {
    for (unsigned i = 0; i < 32; ++i)
        store.service(TimestampUs{i * 1000U},
                      {fw::StorageProbeState::found, fw::I2cError::none, 1});
}
void journal() {
    struct Scenario {};
    using Test = test::FakeApplication<Scenario, MemoryState, Memory>;
    using Store = fw::InputStorage<Test::Board>;
    Test::reset();
    auto &memory = Test::context;
    Store failed;
    failed.service(TimestampUs{0}, {fw::StorageProbeState::failed, fw::I2cError::bus, 0});
    CHECK(failed.loaded() && (failed.status() == fw::RadioStorageStatus::io_error) &&
          !failed.writable() && (failed.message() != nullptr));
    Store store{features::i6s};
    service(store);
    CHECK((store.status() == fw::RadioStorageStatus::empty) && store.writable());
    InputSetup first{features::i6s};
    first.modes[4] = InputMode::pot_detent;
    first.calibration[4] = {100, 1500, 4000};
    CHECK(store.save(first));
    service(store);
    CHECK(store.status() == fw::RadioStorageStatus::saved);
    const auto snapshot = memory;
    InputSetup second = first;
    second.extra_analog = true;
    second.set_digital_mode(0, DigitalInputMode::two_position);
    second.set_digital_mode(1, DigitalInputMode::two_position);
    second.trim_beep = false;
    second.modes[6] = InputMode::six_position;
    for (int prefix = 0; prefix <= 4; ++prefix) {
        memory = snapshot;
        Store writing;
        service(writing);
        CHECK(writing.setup() == first);
        memory.writes_left = prefix;
        CHECK(writing.save(second));
        service(writing);
        if (prefix < 4)
            CHECK(!writing.writable() && (writing.status() == fw::RadioStorageStatus::io_error));
        struct RebootScenario {};
        using RebootTest = test::FakeApplication<RebootScenario, MemoryState, Memory>;
        RebootTest::reset();
        RebootTest::context.bytes = memory.bytes;
        fw::InputStorage<RebootTest::Board> reboot;
        service(reboot);
        CHECK(reboot.writable());
        CHECK(reboot.setup() == (prefix == 4 ? second : first));
        CHECK(std::all_of(memory.bytes.begin(),
                          memory.bytes.begin() + fw::input_storage::base_address,
                          [](auto byte) { return byte == 0xA5; }));
    }
    auto record = fw::input_storage::encode(first, 7);
    CHECK(fw::input_storage::decode(record).valid());
    const auto reseal = [](fw::input_storage::Record &bytes) {
        storage::detail::Writer checksum{std::span{bytes}.last(4)};
        checksum.word(storage::crc32(std::span{bytes}.first(124)));
    };
    record[4] = 1;
    reseal(record);
    CHECK(fw::input_storage::decode(record).setup == first);
    CHECK(fw::input_storage::decode(record).valid());
    record[7] = 1;
    reseal(record);
    CHECK(!fw::input_storage::decode(record).valid());
    record = fw::input_storage::encode(second, 7);
    CHECK(fw::input_storage::decode(record).setup == second);
    record[7] = 16;
    reseal(record);
    CHECK(!fw::input_storage::decode(record).valid());
    record = fw::input_storage::encode(first, 7);
    record[25] ^= 1;
    CHECK(!fw::input_storage::decode(record).valid());
    record = fw::input_storage::encode(first, 7);
    record[4] = fw::input_storage::format_version + 1;
    storage::detail::Writer checksum{std::span{record}.last(4)};
    checksum.word(storage::crc32(std::span{record}.first(124)));
    memory = snapshot;
    std::copy(record.begin(), record.end(),
              memory.bytes.begin() + fw::input_storage::base_address + 128);
    Store future;
    service(future);
    CHECK((future.status() == fw::RadioStorageStatus::version) && !future.writable());
    CHECK(!future.save(first));
    CHECK(!future.can_recover() && !future.recover());
    memory.bytes.fill(0);
    Store foreign;
    service(foreign);
    CHECK((foreign.status() == fw::RadioStorageStatus::invalid) && !foreign.writable());
    CHECK(!foreign.save(first));
    for (const auto generation : {0U, 0x80000000U, 0xFFFFFFFFU}) {
        memory = snapshot;
        const auto conflicting = fw::input_storage::encode(second, generation);
        const auto original = fw::input_storage::encode(first, 0);
        std::copy(original.begin(), original.end(),
                  memory.bytes.begin() + fw::input_storage::base_address);
        std::copy(conflicting.begin(), conflicting.end(),
                  memory.bytes.begin() + fw::input_storage::base_address + 128);
        struct RebootScenario {};
        using RebootTest = test::FakeApplication<RebootScenario, MemoryState, Memory>;
        RebootTest::reset();
        RebootTest::context.bytes = memory.bytes;
        fw::InputStorage<RebootTest::Board> reboot;
        service(reboot);
        if (generation == 0xFFFFFFFFU)
            CHECK(reboot.writable() && (reboot.setup() == first));
        else
            CHECK(!reboot.writable() && (reboot.status() == fw::RadioStorageStatus::invalid));
    }
}
void recovery() {
    struct Scenario {};
    using Test = test::FakeApplication<Scenario, MemoryState, Memory>;
    using Store = fw::InputStorage<Test::Board>;
    Test::reset();
    auto &memory = Test::context;
    InputSetup old{features::i6s};
    old.set_digital_mode(0, DigitalInputMode::two_position);
    old.set_digital_mode(1, DigitalInputMode::two_position);
    old.modes[4] = InputMode::pot_detent;
    auto conflicting = old;
    conflicting.extra_analog = true;
    const auto first = fw::input_storage::encode(old, 0);
    const auto second = fw::input_storage::encode(conflicting, 0);
    const auto base = fw::input_storage::base_address;
    std::copy(first.begin(), first.end(), memory.bytes.begin() + base);
    std::copy(second.begin(), second.end(), memory.bytes.begin() + base + 128);
    const auto snapshot = memory;
    for (int prefix = 0; prefix <= 5; ++prefix) {
        memory = snapshot;
        Store store{features::i6s};
        service(store);
        CHECK((store.status() == fw::RadioStorageStatus::invalid) && store.can_recover());
        memory.writes_left = prefix;
        CHECK(store.recover() && !store.loaded() && !store.writable());
        CHECK(store.owns_memory() && !store.recover());
        service(store);
        CHECK(store.loaded() && !store.owns_memory());
        if (prefix == 5) {
            CHECK((store.status() == fw::RadioStorageStatus::saved) && store.writable());
            CHECK(store.setup() == InputSetup{features::i6s});
            auto edited = store.setup();
            edited.modes[4] = InputMode::pot_detent;
            CHECK(store.save(edited));
        } else
            CHECK((store.status() == fw::RadioStorageStatus::io_error) && !store.writable());
        CHECK(
            std::equal(memory.bytes.begin(), memory.bytes.begin() + base, snapshot.bytes.begin()));
        CHECK(std::equal(memory.bytes.begin() + base + 256, memory.bytes.end(),
                         snapshot.bytes.begin() + base + 256));
        struct RebootScenario {};
        using Reboot = test::FakeApplication<RebootScenario, MemoryState, Memory>;
        Reboot::reset();
        Reboot::context.bytes = memory.bytes;
        fw::InputStorage<Reboot::Board> reboot{features::i6s};
        service(reboot);
        if ((prefix == 1) || (prefix == 5)) {
            CHECK(reboot.writable());
            CHECK(reboot.setup() == (prefix == 1 ? old : InputSetup{features::i6s}));
        } else
            CHECK((reboot.status() == fw::RadioStorageStatus::invalid) && !reboot.writable());
    }
    memory = snapshot;
    Store verification{features::i6s};
    service(verification);
    CHECK(verification.recover());
    for (unsigned i = 0; (i < 32) && (memory.writes_left > 95); ++i)
        verification.service(TimestampUs{i * 1000U},
                             {fw::StorageProbeState::found, fw::I2cError::none, 1});
    CHECK(memory.writes_left == 95);
    memory.bytes[base + 14] ^= 1;
    service(verification);
    CHECK((verification.status() == fw::RadioStorageStatus::verify) && !verification.writable());

    fw::Diagnostics diagnostics{features::i6s};
    fw::InputMenu menu;
    menu.open();
    const auto key = [&](const fw::Key value, const fw::RadioOutput output = fw::RadioOutput::off,
                         const bool busy = false, const bool recoverable = true) {
        return menu.handle(value, diagnostics, output, false, busy, ui::tr::inputs_invalid,
                           recoverable);
    };
    if constexpr (features::secure_ui)
        CHECK(key(fw::Key::ok, fw::RadioOutput::external) == fw::InputMenuCommand::none);
    CHECK(key(fw::Key::ok, fw::RadioOutput::off, true) == fw::InputMenuCommand::none);
    CHECK(key(fw::Key::ok) == fw::InputMenuCommand::none);
    CHECK(key(fw::Key::ok) == fw::InputMenuCommand::none); // No is selected initially.
    CHECK(key(fw::Key::ok) == fw::InputMenuCommand::none);
    CHECK(key(fw::Key::down) == fw::InputMenuCommand::none);
    CHECK(key(fw::Key::cancel) == fw::InputMenuCommand::none);
    CHECK(menu.visible());
    CHECK(key(fw::Key::ok) == fw::InputMenuCommand::none);
    CHECK(key(fw::Key::down) == fw::InputMenuCommand::none);
    if constexpr (features::secure_ui)
        CHECK(key(fw::Key::ok, fw::RadioOutput::external) == fw::InputMenuCommand::none);
    CHECK(key(fw::Key::ok, fw::RadioOutput::off, true) == fw::InputMenuCommand::none);
    CHECK(key(fw::Key::ok) == fw::InputMenuCommand::recover);
    CHECK(menu.visible() && (diagnostics.input_setup() == InputSetup{features::i6s}));
    menu.open();
    CHECK(key(fw::Key::ok, fw::RadioOutput::off, false, false) == fw::InputMenuCommand::none);
    CHECK(key(fw::Key::down, fw::RadioOutput::off, false, false) == fw::InputMenuCommand::none);
    CHECK(key(fw::Key::ok, fw::RadioOutput::off, false, false) == fw::InputMenuCommand::none);
}
void input_rf_ui_interlocks() {
    for (const auto output :
         {fw::RadioOutput::off, fw::RadioOutput::internal, fw::RadioOutput::external}) {
        const bool allowed = !features::secure_ui || (output == fw::RadioOutput::off);
        fw::Diagnostics diagnostics{features::i6s};
        fw::InputMenu menu;
        menu.open();
        const auto before = diagnostics.input_setup();
        const auto key = [&](const fw::Key value, const bool recovery) {
            return menu.handle(value, diagnostics, output, !recovery, false,
                               recovery ? ui::tr::inputs_invalid : nullptr, recovery);
        };
        CHECK(key(fw::Key::ok, false) == fw::InputMenuCommand::none);
        CHECK(key(fw::Key::up, false) == fw::InputMenuCommand::none);
        CHECK(key(fw::Key::ok, false) ==
              (allowed ? fw::InputMenuCommand::save : fw::InputMenuCommand::none));
        CHECK((diagnostics.input_setup() != before) == allowed);
        menu.open();
        CHECK(key(fw::Key::ok, true) == fw::InputMenuCommand::none);
        CHECK(key(fw::Key::down, true) == fw::InputMenuCommand::none);
        CHECK(key(fw::Key::ok, true) ==
              (allowed ? fw::InputMenuCommand::recover : fw::InputMenuCommand::none));
    }
}
void decoding() {
    for (const auto kind : {SourceKind::analog, SourceKind::switch_position}) {
        ui::Framebuffer display, expected;
        ui::detail::draw_source(display, 0, 0, {kind, 255, false}, true);
        expected.text(0, 0, "?");
        CHECK(display.bytes() == expected.bytes());
    }
    {
        ui::Framebuffer display, expected;
        fw::menu_detail::condition(display, 0, 0, {true, 255, SwitchPosition::up, false}, true);
        expected.text(0, 0, "?");
        CHECK(display.bytes() == expected.bytes());
    }
    CHECK(decode_pot(1500, {100, 1500, 4000}, true) == 0);
    CHECK(decode_pot(1500, {100, 1500, 4000}, false) < 0);
    CHECK(decode_pot(0, {100, 1500, 4000}, true) == (-resolution));
    CHECK(decode_pot(4095, {100, 1500, 4000}, true) == resolution);
    for (int position = 0; position < 6; ++position) {
        const auto value = decode_input_switch(static_cast<std::uint16_t>(position * 683 + 100),
                                               InputMode::six_position, {0, 2048, 4095});
        CHECK(static_cast<int>(value) == (position + 2));
        CHECK((switch_value(value) >= (-resolution)) && (switch_value(value) <= resolution));
    }
    fw::Diagnostics diagnostics{features::i6s};
    auto calibration = diagnostics.setup();
    calibration.calibration.fill({0, 2048, 4095});
    CHECK(diagnostics.restore_calibration(calibration));
    auto setup = diagnostics.input_setup();
    setup.modes[0] = InputMode::off;
    setup.modes[1] = InputMode::pot_detent;
    setup.modes[2] = InputMode::six_position;
    setup.modes[4] = InputMode::off;
    CHECK(diagnostics.set_input_setup(setup));
    CHECK(!diagnostics.output_valid());
    std::array<std::uint16_t, 16> samples{};
    samples.fill(2048);
    samples[5] = 1000;
    diagnostics.observe(samples, TimestampUs{100000});
    samples[5] = 3200;
    diagnostics.observe(samples, TimestampUs{200000});
    CHECK(diagnostics.take_click() && !diagnostics.take_click());
    diagnostics.observe(samples, TimestampUs{300000});
    CHECK(!diagnostics.take_click());
    if constexpr (features::extended_inputs) {
        auto digital_setup = diagnostics.input_setup();
        digital_setup.set_digital_mode(0, DigitalInputMode::two_position);
        CHECK(diagnostics.set_input_setup(digital_setup));
        diagnostics.observe(samples, TimestampUs{400000});
        diagnostics.observe_digital(0, TimestampUs{400000});
        CHECK(diagnostics.prepared().controls.switches[11] == SwitchPosition::up);
        diagnostics.observe_digital(1, TimestampUs{401000});
        diagnostics.observe_digital(1, TimestampUs{420999});
        CHECK(diagnostics.prepared().controls.switches[11] == SwitchPosition::up);
        diagnostics.observe_digital(1, TimestampUs{421000});
        CHECK(diagnostics.prepared().controls.switches[11] == SwitchPosition::down);
        setup = digital_setup;
    }
    CHECK((diagnostics.analog_mask() & (1U << 7)) != 0);
    CHECK((diagnostics.analog_mask() & (1U << 4)) == 0);
    CHECK(((diagnostics.switch_mask() & 3U) == 0) && (diagnostics.six_mask() == 4));
    Model model{};
    model.inputs.resize<1>();
    model.mixes.resize<1>();
    model.inputs[0].source = {SourceKind::analog, 4, false};
    model.mixes[0].source = {SourceKind::maximum, 0, false};
    model.mixes[0].condition = {true, 0, SwitchPosition::up, true};
    const auto result = evaluate(model, diagnostics.prepared().controls);
    CHECK(result && (result.frame.inputs[0] == 0) && (result.frame.channels[0] == 0));
    model.mixes[0].condition = {};
    model.mixes[0].source = {SourceKind::switch_position, 2, false};
    CHECK(evaluate(model, diagnostics.prepared().controls).frame.channels[0] ==
          switch_value(diagnostics.prepared().controls.switches[2]));
    ui::Interface editor;
    editor.open(ui::Page::inputs);
    editor.set_sources(diagnostics.analog_mask(), diagnostics.switch_mask(),
                       diagnostics.six_mask());
    Controls controls{};
    editor.handle(ui::Key::enter, model, controls);
    editor.handle(ui::Key::right, model, controls);
    CHECK(model.inputs[0].source.index == 5);
    editor.handle(ui::Key::back, model, controls);
    CHECK(model.inputs[0].source.index == 4);
    for (unsigned i = 0; i < 8; ++i)
        editor.handle(ui::Key::right, model, controls);
    editor.handle(ui::Key::enter, model, controls);
    editor.handle(ui::Key::right, model, controls);
    editor.handle(ui::Key::enter, model, controls);
    CHECK((model.inputs[0].condition.index == 2) &&
          (model.inputs[0].condition.position == SwitchPosition::pos1));
    editor.handle(ui::Key::right, model, controls);
    editor.handle(ui::Key::enter, model, controls);
    for (unsigned i = 0; i < 10; ++i)
        editor.handle(ui::Key::right, model, controls);
    editor.handle(ui::Key::enter, model, controls);
    CHECK(model.inputs[0].condition.position == SwitchPosition::pos6);
    fw::InputMenu menu;
    menu.open();
    CHECK(menu.handle(fw::Key::ok, diagnostics, fw::RadioOutput::external, true, false, nullptr) ==
          fw::InputMenuCommand::none);
    CHECK(diagnostics.input_setup() == setup);
}
struct Registers {
    struct Cell {
        std::uintptr_t address{};
        std::uint32_t value{};
    };
    std::array<Cell, 64> cells{};
    std::size_t count{};
    unsigned writes{};

    std::uint32_t &cell(const std::uintptr_t address) {
        for (std::size_t i = 0; i < count; ++i)
            if (cells[i].address == address)
                return cells[i].value;
        CHECK(count < cells.size());
        cells[count].address = address;
        return cells[count++].value;
    }
    std::uint32_t read(const std::uintptr_t address) { return cell(address); }
    void write(const std::uintptr_t address, const std::uint32_t value) {
        ++writes;
        cell(address) = value;
    }
    [[nodiscard]] static std::uint32_t now_us() noexcept { return 0; }
};
struct HardwareScenario {};
using Hardware = test::RegisterFixture<HardwareScenario, Registers>;
void hardware() {
    Hardware::reset_registers();
    auto &registers = Hardware::registers;
    using Inputs = fw::ExtendedInputs<Hardware::Io, fw::Hardware>;
    Inputs::initialize();
    Inputs::configure(features::i6s ? 31 : 3);
    if constexpr (features::extended_inputs) {
        const auto check_pin = [&](const fw::Pin pin) {
            CHECK((registers.read(pin.port + offsetof(GPIO_TypeDef, MODER)) &
                   (3U << (2U * pin.index))) == 0);
            CHECK(((registers.read(pin.port + offsetof(GPIO_TypeDef, PUPDR)) >> (2U * pin.index)) &
                   3U) == 1);
        };
        for (const auto input : fw::Hardware::digital_inputs)
            check_pin(input.pin());
    }
    CHECK(Inputs::read() == (features::extended_inputs ? (features::i6s ? 31 : 3) : 0));
    Inputs::service(TimestampUs{100000}, true);
    const auto control = TIM1_BASE + offsetof(TIM_TypeDef, CR1);
    CHECK((registers.read(control) & TIM_CR1_CEN) != 0);
    Inputs::service(TimestampUs{109999}, false);
    CHECK((registers.read(control) & TIM_CR1_CEN) != 0);
    Inputs::service(TimestampUs{110000}, false);
    CHECK(registers.read(control) == 0);
    Inputs::service(TimestampUs{200000}, true, fw::ClickTone::multiswitch);
    CHECK(registers.read(TIM1_BASE + offsetof(TIM_TypeDef, ARR)) == ((1'000'000U / 1100U) - 1U));
    CHECK(registers.read(TIM1_BASE + offsetof(TIM_TypeDef, CCR1)) == ((1'000'000U / 1100U) / 2U));
    Inputs::service(TimestampUs{209999}, false);
    CHECK((registers.read(control) & TIM_CR1_CEN) != 0);
    Inputs::service(TimestampUs{210000}, false);
    CHECK(registers.read(control) == 0);
    Inputs::configure(0);
    CHECK(Inputs::read() == 0);
    if constexpr (features::extended_inputs)
        for (const auto input : fw::Hardware::digital_inputs) {
            const auto pin = input.pin();
            const auto shift = 2U * pin.index;
            const auto mode =
                (registers.read(pin.port + offsetof(GPIO_TypeDef, MODER)) >> shift) & 3U;
            const auto pull =
                (registers.read(pin.port + offsetof(GPIO_TypeDef, PUPDR)) >> shift) & 3U;
            CHECK(mode == (input.released == fw::DigitalInput::Released::analog ? 3U : 2U));
            CHECK(pull == (input.released == fw::DigitalInput::Released::analog  ? 0U
                           : input.released == fw::DigitalInput::Released::swdio ? 1U
                                                                                 : 2U));
        }
    if constexpr (features::extended_inputs && features::i6s) {
        const auto uart_mode = GPIOB_BASE + offsetof(GPIO_TypeDef, MODER);
        const auto uart_pulls = GPIOB_BASE + offsetof(GPIO_TypeDef, PUPDR);
        registers.cell(uart_mode) = 2U << 12 | 2U << 14;
        registers.cell(uart_pulls) = 1U << 12;
        Inputs::configure(0x13);
        CHECK(registers.read(uart_mode) == (2U << 12 | 2U << 14));
        CHECK(registers.read(uart_pulls) == (1U << 12));
        CHECK(Inputs::read() == 0x13);
        Inputs::configure(0x10);
        CHECK(registers.read(uart_mode) == (2U << 12 | 2U << 14));
        CHECK(Inputs::read() == 0x10);
        Inputs::configure(0x1C);
        CHECK((registers.read(uart_mode) & (15U << 12)) == 0);
        CHECK(Inputs::read() == 0x1C);
        Inputs::configure(0);
    }
}
struct FirstBuzzerScenario {};
struct SecondBuzzerScenario {};
using FirstBuzzer = test::RegisterFixture<FirstBuzzerScenario, Registers>;
using SecondBuzzer = test::RegisterFixture<SecondBuzzerScenario, Registers>;
void buzzer_lifecycle() {
    using Buzzer = fw::Buzzer<FirstBuzzer::Io, 1, fw::Pin{GPIOA_BASE, 8}>;
    using Other = fw::Buzzer<SecondBuzzer::Io, 1, fw::Pin{GPIOA_BASE, 8}>;
    FirstBuzzer::reset_registers();
    SecondBuzzer::reset_registers();
    Buzzer::initialize();
    Other::initialize();
    auto &registers = FirstBuzzer::registers;
    auto &other = SecondBuzzer::registers;
    const auto control = TIM1_BASE + offsetof(TIM_TypeDef, CR1);
    const auto enable = TIM1_BASE + offsetof(TIM_TypeDef, CCER);
    CHECK((registers.read(fw::stm32f072::registers::rcc_ahbenr) & RCC_AHBENR_GPIOAEN) != 0);
    CHECK((registers.read(fw::stm32f072::registers::rcc_apb2enr) & RCC_APB2ENR_TIM1EN) != 0);
    CHECK(((registers.read(GPIOA_BASE + offsetof(GPIO_TypeDef, MODER)) >> 16) & 3U) == 2);
    CHECK(((registers.read(GPIOA_BASE + offsetof(GPIO_TypeDef, AFR) + sizeof(std::uint32_t))) &
           15U) == 2);
    CHECK(((registers.read(GPIOA_BASE + offsetof(GPIO_TypeDef, PUPDR)) >> 16) & 3U) == 0);
    CHECK(registers.read(TIM1_BASE + offsetof(TIM_TypeDef, PSC)) == 47);
    CHECK(registers.read(TIM1_BASE + offsetof(TIM_TypeDef, DIER)) == 0);
    CHECK(registers.read(TIM1_BASE + offsetof(TIM_TypeDef, CCMR1)) ==
          (TIM_CCMR1_OC1M_1 | TIM_CCMR1_OC1M_2 | TIM_CCMR1_OC1PE));
    CHECK(registers.read(TIM1_BASE + offsetof(TIM_TypeDef, BDTR)) == TIM_BDTR_MOE);
    CHECK((registers.read(control) == 0) && (registers.read(enable) == TIM_CCER_CC1P));

    const TimestampUs start{0xFFFF'F000U};
    Buzzer::click(start);
    CHECK(registers.read(TIM1_BASE + offsetof(TIM_TypeDef, ARR)) == ((1'000'000U / 2200U) - 1U));
    CHECK(registers.read(TIM1_BASE + offsetof(TIM_TypeDef, CCR1)) == ((1'000'000U / 2200U) / 2U));
    CHECK(registers.read(TIM1_BASE + offsetof(TIM_TypeDef, CNT)) == 0);
    CHECK(registers.read(TIM1_BASE + offsetof(TIM_TypeDef, EGR)) == TIM_EGR_UG);
    CHECK(registers.read(control) == (TIM_CR1_ARPE | TIM_CR1_CEN));
    CHECK(registers.read(enable) == (TIM_CCER_CC1E | TIM_CCER_CC1P));
    Other::service(start + DurationUs{10000});
    CHECK((other.read(control) == 0) && (other.read(enable) == TIM_CCER_CC1P));
    Other::click(TimestampUs{100});
    Buzzer::service(start + DurationUs{9999});
    CHECK(registers.read(control) != 0);
    Buzzer::service(start + DurationUs{10000});
    CHECK((registers.read(control) == 0) && (registers.read(enable) == TIM_CCER_CC1P));
    Buzzer::click(start + DurationUs{20000}, fw::ClickTone::multiswitch);
    CHECK(registers.read(TIM1_BASE + offsetof(TIM_TypeDef, ARR)) == ((1'000'000U / 1100U) - 1U));
    CHECK(registers.read(TIM1_BASE + offsetof(TIM_TypeDef, CCR1)) == ((1'000'000U / 1100U) / 2U));
    Buzzer::click(start + DurationUs{25000});
    CHECK(registers.read(TIM1_BASE + offsetof(TIM_TypeDef, ARR)) == ((1'000'000U / 2200U) - 1U));
    Buzzer::service(start + DurationUs{34999});
    CHECK(registers.read(control) != 0);
    Buzzer::service(start + DurationUs{35000});
    CHECK((registers.read(control) == 0) && (registers.read(enable) == TIM_CCER_CC1P));
    CHECK(other.read(control) != 0);
    Other::service(TimestampUs{10099});
    CHECK(other.read(control) != 0);
    Other::service(TimestampUs{10100});
    CHECK(other.read(control) == 0);

    Buzzer::click(TimestampUs{100000});
    Buzzer::stop();
    CHECK((registers.read(control) == 0) && (registers.read(enable) == TIM_CCER_CC1P));
    const auto stopped_writes = registers.writes;
    Buzzer::service(TimestampUs{110000});
    CHECK(registers.writes == stopped_writes);
    Buzzer::click(TimestampUs{200000});
    Buzzer::initialize();
    CHECK((registers.read(control) == 0) && (registers.read(enable) == TIM_CCER_CC1P));
    const auto initialized_writes = registers.writes;
    Buzzer::service(TimestampUs{210000});
    CHECK(registers.writes == initialized_writes);
    FirstBuzzer::reset_registers();
    Buzzer::initialize();
    Buzzer::click(TimestampUs{0});
    Buzzer::service(TimestampUs{10000});
    CHECK((registers.read(control) == 0) && (other.read(control) == 0));
}

template <unsigned Timer, fw::Pin Output>
concept ValidBuzzer = requires { typename fw::Buzzer<FirstBuzzer::Io, Timer, Output>; };
static_assert(!ValidBuzzer<6, fw::Pin{GPIOA_BASE, 8}>);
static_assert(!ValidBuzzer<7, fw::Pin{GPIOA_BASE, 8}>);
static_assert(!ValidBuzzer<4, fw::Pin{GPIOA_BASE, 8}>);
static_assert(!ValidBuzzer<1, fw::Pin{GPIOA_BASE, 9}>); // CH2 is not CH1.
static_assert(!ValidBuzzer<3, fw::Pin{GPIOB_BASE, 16}>);
static_assert(!ValidBuzzer<99, fw::Pin{GPIOA_BASE, 8}>);

struct GeneralTimerScenario {};
struct SmallTimerScenario {};
struct MainOutputTimerScenario {};
struct CounterTimerScenario {};
struct PairedTimerScenario {};
struct LastTimerScenario {};
template <typename Scenario, unsigned Timer, fw::Pin Output, std::uintptr_t Base,
          std::uintptr_t ClockRegister, std::uint32_t ClockMask, unsigned Alternate,
          bool MainOutput>
void remapped_buzzer() {
    using Fixture = test::RegisterFixture<Scenario, Registers>;
    using Sound = fw::Buzzer<typename Fixture::Io, Timer, Output>;
    Fixture::reset_registers();
    auto &registers = Fixture::registers;
    // Ordinary timers must never access their reserved BDTR offset.
    constexpr auto bdtr = Base + offsetof(TIM_TypeDef, BDTR);
    registers.cell(bdtr) = 0x1234;
    registers.cell(ClockRegister) = 0x8000'0000U;
    Sound::initialize();
    CHECK(registers.read(ClockRegister) == (0x8000'0000U | ClockMask));
    CHECK(registers.read(bdtr) == (MainOutput ? TIM_BDTR_MOE : 0x1234));
    const auto afr =
        Output.port + offsetof(GPIO_TypeDef, AFR) + (Output.index / 8U) * sizeof(std::uint32_t);
    CHECK(((registers.read(afr) >> ((Output.index % 8U) * 4U)) & 15U) == Alternate);
    CHECK(((registers.read(Output.port + offsetof(GPIO_TypeDef, MODER)) >> (Output.index * 2U)) &
           3U) == 2);
    CHECK(registers.read(Base + offsetof(TIM_TypeDef, PSC)) == 47);
    Sound::click(TimestampUs{0xFFFF'F000U});
    CHECK(registers.read(Base + offsetof(TIM_TypeDef, CCR1)) == ((1'000'000U / 2200U) / 2U));
    CHECK(registers.read(Base + offsetof(TIM_TypeDef, CR1)) == (TIM_CR1_CEN | TIM_CR1_ARPE));
    Sound::service(TimestampUs{0xFFFF'F000U} + DurationUs{10'000});
    CHECK(registers.read(Base + offsetof(TIM_TypeDef, CR1)) == 0);
    CHECK(registers.read(TIM1_BASE + offsetof(TIM_TypeDef, CR1)) == 0);
}

struct RemappedHardware : fw::i6x::Hardware {
    static constexpr fw::I2cPins storage{1, {GPIOB_BASE, 6}, {GPIOB_BASE, 7}};
    static constexpr fw::LcdPins lcd{GPIOA_BASE,      {GPIOB_BASE, 3}, {GPIOB_BASE, 4},
                                     {GPIOB_BASE, 5}, {GPIOD_BASE, 2}, {GPIOD_BASE, 7},
                                     {GPIOF_BASE, 3}, {GPIOC_BASE, 9}};
};
struct RemappedBusScenario {};
void remapped_storage_and_display() {
    using Fixture = test::RegisterFixture<RemappedBusScenario, Registers>;
    using Probe = fw::StorageProbe<Fixture::Io, RemappedHardware>;
    using Memory = fw::Eeprom<Fixture::Io, RemappedHardware>;
    using Display = fw::Lcd<Fixture::Io, RemappedHardware>;
    Fixture::reset_registers();
    auto &registers = Fixture::registers;
    Probe::initialize();
    CHECK((registers.read(fw::stm32f072::registers::rcc_apb1enr) & RCC_APB1ENR_I2C1EN) != 0);
    CHECK((registers.read(fw::stm32f072::registers::rcc_apb1enr) & RCC_APB1ENR_I2C2EN) == 0);
    CHECK(((registers.read(GPIOB_BASE + offsetof(GPIO_TypeDef, AFR)) >> 24) & 0xFFU) == 0x11);
    Probe::service(TimestampUs{0});
    const auto request = (0x50U << 1U) | I2C_CR2_AUTOEND | I2C_CR2_START;
    CHECK(registers.read(I2C1_BASE + offsetof(I2C_TypeDef, CR2)) == request);
    Probe::service(TimestampUs{10'000});
    CHECK(Probe::error() == fw::I2cError::timeout);
    CHECK(registers.read(I2C1_BASE + offsetof(I2C_TypeDef, CR1)) == 0);
    Memory::select(0x50);
    std::array<std::uint8_t, 1> byte{0x42};
    CHECK(Memory::write(0x1234, byte));
    Memory::service(TimestampUs{20'000});
    CHECK(registers.read(I2C1_BASE + offsetof(I2C_TypeDef, CR2)) ==
          (request | (3U << I2C_CR2_NBYTES_Pos)));
    CHECK(registers.read(I2C2_BASE + offsetof(I2C_TypeDef, CR2)) == 0);
    CHECK(Display::initialize() == fw::LcdError::none);
    CHECK((registers.read(fw::stm32f072::registers::rcc_ahbenr) & RCC_AHBENR_GPIOAEN) != 0);
    CHECK((registers.read(fw::stm32f072::registers::rcc_ahbenr) & RCC_AHBENR_GPIOEEN) == 0);
    CHECK((registers.read(GPIOA_BASE + offsetof(GPIO_TypeDef, MODER)) & 0xFFFFU) == 0x5555);
    CHECK(registers.read(GPIOE_BASE + offsetof(GPIO_TypeDef, MODER)) == 0);
}
void setup_rc_hold() {
    fw::Diagnostics diagnostics{true};
    auto calibration = diagnostics.setup();
    calibration.calibration.fill({0, 2048, 4095});
    CHECK(diagnostics.restore_calibration(calibration));
    std::array<std::uint16_t, 16> samples{};
    samples.fill(2048);
    samples[6] = 1000;
    samples[7] = 3000;
    diagnostics.observe(samples, TimestampUs{100'000});
    diagnostics.observe_setup_controls(false, 1);
    const auto before = diagnostics.prepared().controls;
    CHECK((diagnostics.switch_mask() & (3U << physical_switch_count)) ==
          (3U << physical_switch_count));
    CHECK(!diagnostics.extra_digital_enabled());
    CHECK(before.switches[physical_switch_count] == SwitchPosition::down);
    CHECK(before.switches[physical_switch_count + 1] == SwitchPosition::up);
    diagnostics.observe_setup_controls(true, 1);
    samples[0] = 0;
    samples[6] = 4095;
    samples[7] = 0;
    diagnostics.observe(samples, TimestampUs{200'000});
    diagnostics.observe_setup_controls(true, 2);
    const auto held = diagnostics.prepared().controls;
    CHECK((held.analogs[4] == before.analogs[4]) && (held.analogs[5] == before.analogs[5]));
    CHECK(held.analogs[0] != before.analogs[0]);
    CHECK(held.switches[physical_switch_count] == SwitchPosition::down);
    CHECK(held.switches[physical_switch_count + 1] == SwitchPosition::up);
    CHECK((diagnostics.samples()[6] == 4095) && (diagnostics.samples()[7] == 0));
    diagnostics.observe_setup_controls(false, 2);
    const auto released = diagnostics.prepared().controls;
    CHECK((released.analogs[4] == resolution) && (released.analogs[5] == -resolution));
    CHECK(released.switches[physical_switch_count] == SwitchPosition::up);
    CHECK(released.switches[physical_switch_count + 1] == SwitchPosition::down);
}

void digital_configuration() {
    fw::Diagnostics diagnostics{features::i6s};
    auto calibration = diagnostics.setup();
    calibration.calibration.fill({0, 2048, 4095});
    CHECK(diagnostics.restore_calibration(calibration));
    std::array<std::uint16_t, 16> samples{};
    samples.fill(2048);
    InputSetup setup{features::i6s};
    setup.external_module = false;
    setup.modes.fill(InputMode::off);
    for (std::size_t i = 0; i < (features::i6s ? digital_input_count : 2U); ++i) {
        setup.digital_modes = 0;
        setup.set_digital_mode(i, DigitalInputMode::two_position);
        CHECK(diagnostics.set_input_setup(setup));
        diagnostics.observe(samples, TimestampUs{100'000});
        diagnostics.observe_digital(static_cast<std::uint8_t>(1U << i), TimestampUs{100'000});
        const auto mask = features::extended_inputs ? 1U << (auxiliary_count + i) : 0U;
        CHECK((diagnostics.switch_mask() & 0xF800U) == mask);
        CHECK((diagnostics.prepared().controls.active_switches & 0xF800U) == mask);
        CHECK(diagnostics.digital_pin_mask() == (features::extended_inputs ? 1U << i : 0U));
        if constexpr (features::extended_inputs)
            CHECK(diagnostics.prepared().controls.switches[auxiliary_count + i] ==
                  SwitchPosition::down);
        Model model{};
        model.inputs.resize<1>();
        model.inputs[0].source = {SourceKind::analog, 3, false};
        ui::Interface editor;
        editor.open(ui::Page::inputs);
        editor.set_sources(diagnostics.analog_mask(), diagnostics.switch_mask(), 0);
        Controls controls{};
        editor.handle(ui::Key::enter, model, controls);
        editor.handle(ui::Key::right, model, controls);
        CHECK(model.inputs[0].source.kind ==
              (features::extended_inputs ? SourceKind::switch_position : SourceKind::maximum));
        if constexpr (features::extended_inputs)
            CHECK(model.inputs[0].source.index == auxiliary_count + i);
        setup.set_digital_mode(i, DigitalInputMode::off);
        CHECK(diagnostics.set_input_setup(setup));
        CHECK((diagnostics.switch_mask() & 0xF800U) == 0);
        model.inputs[0].source = {SourceKind::analog, 3, false};
        editor.open(ui::Page::inputs);
        editor.set_sources(diagnostics.analog_mask(), diagnostics.switch_mask(), 0);
        editor.handle(ui::Key::enter, model, controls);
        editor.handle(ui::Key::right, model, controls);
        CHECK(model.inputs[0].source.kind == SourceKind::maximum);
    }
    if constexpr (!features::extended_inputs)
        return;
    for (const std::size_t pair : {0U, 2U}) {
        if (!features::i6s && (pair == 2))
            continue;
        setup.digital_modes = 0;
        setup.set_digital_mode(pair, DigitalInputMode::three_position);
        CHECK(diagnostics.set_input_setup(setup));
        diagnostics.observe(samples, TimestampUs{100'000});
        diagnostics.observe_digital(0, TimestampUs{100'000});
        CHECK(diagnostics.output_valid());
        CHECK(diagnostics.digital_pin_mask() == (3U << pair));
        CHECK((diagnostics.switch_mask() & 0xF800U) == (1U << (auxiliary_count + pair)));
        CHECK(diagnostics.prepared().controls.switches[auxiliary_count + pair] ==
              SwitchPosition::middle);
        const auto choice = fw::menu_detail::condition_choice(
            {true, static_cast<std::uint8_t>(auxiliary_count + pair), SwitchPosition::up, false});
        CHECK(fw::menu_detail::select_condition(
                  fw::menu_detail::change_condition(choice, 1, diagnostics), false)
                  .position == SwitchPosition::middle);
        std::uint32_t now = 100'000;
        for (const auto contacts : {1U, 2U, 0U, 3U, 1U}) {
            const auto pressed = static_cast<std::uint8_t>(contacts << pair);
            diagnostics.observe_digital(pressed, TimestampUs{now += 1000});
            diagnostics.observe_digital(pressed, TimestampUs{now += 20'000});
            if (contacts == 3) {
                CHECK(!diagnostics.output_valid());
                CHECK(diagnostics.prepared().status.code == PreparationError::switch_position);
            } else {
                CHECK(diagnostics.output_valid());
                const auto position = contacts == 1   ? SwitchPosition::up
                                      : contacts == 2 ? SwitchPosition::down
                                                      : SwitchPosition::middle;
                CHECK(diagnostics.prepared().controls.switches[auxiliary_count + pair] == position);
            }
        }
    }
    if constexpr (features::i6s) {
        setup.digital_modes = 0;
        setup.set_digital_mode(2, DigitalInputMode::three_position);
        setup.set_digital_mode(4, DigitalInputMode::two_position);
        setup.external_module = true;
        CHECK(diagnostics.set_input_setup(setup));
        CHECK(diagnostics.digital_pin_mask() == 0x10);
        CHECK((diagnostics.switch_mask() & 0xF800U) == (1U << 15));
        setup.external_module = false;
        CHECK(diagnostics.set_input_setup(setup));
        CHECK(diagnostics.digital_pin_mask() == 0x1C);
    }
}

void extended_analog_sources() {
    if constexpr (!features::extended_inputs)
        return;
    fw::Diagnostics diagnostics{features::i6s};
    auto calibration = diagnostics.setup();
    calibration.calibration.fill({0, 2048, 4095});
    CHECK(diagnostics.restore_calibration(calibration));
    std::array<std::uint16_t, 16> samples{};
    samples.fill(2048);
    InputSetup setup{features::i6s};
    setup.extra_analog = true;
    setup.modes.fill(InputMode::off);
    for (std::size_t extra = 0; extra < fw::extra_adc_count; ++extra) {
        const auto index = 6 + extra;
        for (const auto mode : {InputMode::pot, InputMode::three_position, InputMode::off}) {
            setup.modes[index] = mode;
            CHECK(diagnostics.set_input_setup(setup));
            samples[11 + extra] = 4095;
            diagnostics.observe(samples, TimestampUs{100'000});
            Model model{};
            model.inputs.resize<1>();
            model.inputs[0].source = {SourceKind::analog, 3, false};
            ui::Interface editor;
            editor.open(ui::Page::inputs);
            editor.set_sources(diagnostics.analog_mask(), diagnostics.switch_mask(), 0);
            Controls controls{};
            editor.handle(ui::Key::enter, model, controls);
            editor.handle(ui::Key::right, model, controls);
            if (mode == InputMode::off)
                CHECK(model.inputs[0].source.kind == SourceKind::maximum);
            else if (mode == InputMode::pot) {
                CHECK(model.inputs[0].source.kind == SourceKind::analog);
                CHECK(model.inputs[0].source.index == auxiliary_analog_indices[index]);
                CHECK(diagnostics.prepared().controls.analogs[auxiliary_analog_indices[index]] ==
                      resolution);
            } else {
                CHECK(model.inputs[0].source.kind == SourceKind::switch_position);
                CHECK(model.inputs[0].source.index == index);
                CHECK(diagnostics.prepared().controls.switches[index] == SwitchPosition::down);
                if constexpr (features::i6s) {
                    constexpr std::array<std::string_view, 5> names{"SWI", "SWJ", "SWK", "SWL",
                                                                    "SWM"};
                    ui::Framebuffer actual, expected;
                    ui::detail::draw_source(actual, 0, 0, model.inputs[0].source, true);
                    expected.text(0, 0, names[extra]);
                    CHECK(actual.bytes() == expected.bytes());
                }
            }
        }
    }
}

void digital_menu() {
    if constexpr (!features::extended_inputs)
        return;
    fw::Diagnostics diagnostics{features::i6s};
    fw::InputMenu menu;
    menu.open();
    const auto key = [&](const fw::Key pressed) {
        return menu.handle(pressed, diagnostics, fw::RadioOutput::off, true, false, nullptr);
    };
    for (unsigned i = 0; i < 7; ++i)
        (void)key(fw::Key::down);
    (void)key(fw::Key::ok);
    (void)key(fw::Key::up);
    CHECK(key(fw::Key::ok) == fw::InputMenuCommand::save);
    CHECK(diagnostics.input_setup().digital_mode(0) == DigitalInputMode::two_position);
    (void)key(fw::Key::ok);
    (void)key(fw::Key::up);
    CHECK(key(fw::Key::ok) == fw::InputMenuCommand::save);
    CHECK(diagnostics.input_setup().digital_mode(0) == DigitalInputMode::three_position);
    CHECK(diagnostics.input_setup().digital_mode(1) == DigitalInputMode::off);
    if constexpr (features::i6s) {
        (void)key(fw::Key::down);
        (void)key(fw::Key::ok);
        (void)key(fw::Key::up);
        CHECK(key(fw::Key::ok) == fw::InputMenuCommand::save);
        CHECK(diagnostics.input_setup().digital_mode(4) == DigitalInputMode::two_position);
        CHECK(diagnostics.input_setup().digital_mode(2) == DigitalInputMode::off);
        CHECK(diagnostics.input_setup().digital_mode(3) == DigitalInputMode::off);
        (void)key(fw::Key::up);
    }
    (void)key(fw::Key::ok);
    (void)key(fw::Key::up);
    CHECK(key(fw::Key::ok) == fw::InputMenuCommand::save);
    CHECK(diagnostics.input_setup().digital_mode(0) == DigitalInputMode::off);
}

void digital_journal() {
    InputSetup setup{features::i6s};
    setup.external_module = false;
    setup.trim_beep = false;
    setup.set_digital_mode(0, DigitalInputMode::three_position);
    setup.set_digital_mode(2, DigitalInputMode::three_position);
    setup.set_digital_mode(4, DigitalInputMode::two_position);
    const auto encoded = fw::input_storage::encode(setup, 19);
    const auto decoded = fw::input_storage::decode(encoded);
    CHECK(decoded.valid() && (decoded.setup == setup) && (decoded.generation == 19));
    for (const auto version : {1U, 2U}) {
        auto legacy = encoded;
        legacy[4] = static_cast<std::uint8_t>(version);
        legacy[6] = 1;
        legacy[7] = static_cast<std::uint8_t>(version == 2 ? 1 : 0);
        storage::detail::Writer checksum{std::span{legacy}.last(4)};
        checksum.word(storage::crc32(std::span{legacy}.first(124)));
        const auto old = fw::input_storage::decode(legacy);
        CHECK(old.valid());
        CHECK(old.setup.external_module == !features::i6s);
        CHECK(old.setup.trim_beep == (version == 1));
        for (std::size_t i = 0; i < digital_input_count; ++i)
            CHECK(old.setup.digital_mode(i) == ((features::i6s || (i < 2))
                                                    ? DigitalInputMode::two_position
                                                    : DigitalInputMode::off));
    }
    setup.set_digital_mode(1, DigitalInputMode::two_position);
    CHECK(!valid_input_setup(setup));
    setup.digital_modes = 3;
    CHECK(!valid_input_setup(setup));
    setup.digital_modes = 1U << 10;
    CHECK(!valid_input_setup(setup));
    setup.digital_modes = 0;
    setup.set_digital_mode(4, DigitalInputMode::three_position);
    CHECK(!valid_input_setup(setup));
}

void touch_hold() {
    fw::i6s::TouchNavigation navigation;
    fw::i6s::TouchSample sample{};
    sample.contacts = 1;
    CHECK(navigation.service(TimestampUs{0}, &sample, true) == fw::Key::none);
    for (unsigned t = 20000; t < 800000; t += 20000)
        CHECK(navigation.service(TimestampUs{t}, &sample, true) == fw::Key::none);
    CHECK(navigation.service(TimestampUs{800000}, &sample, true) == fw::Key::ok_long);
    CHECK(navigation.service(TimestampUs{820000}, &sample, true) == fw::Key::none);
    sample.contacts = 0;
    CHECK(navigation.service(TimestampUs{840000}, &sample, true) == fw::Key::none);
}
} // namespace
int main() {
    digital_configuration();
    extended_analog_sources();
    digital_menu();
    digital_journal();
    setup_rc_hold();
    journal();
    recovery();
    input_rf_ui_interlocks();
    decoding();
    hardware();
    buzzer_lifecycle();
    remapped_buzzer<GeneralTimerScenario, 3, fw::Pin{GPIOB_BASE, 4}, TIM3_BASE,
                    fw::stm32f072::registers::rcc_apb1enr, RCC_APB1ENR_TIM3EN, 1, false>();
    remapped_buzzer<SmallTimerScenario, 14, fw::Pin{GPIOA_BASE, 4}, TIM14_BASE,
                    fw::stm32f072::registers::rcc_apb1enr, RCC_APB1ENR_TIM14EN, 4, false>();
    remapped_buzzer<MainOutputTimerScenario, 16, fw::Pin{GPIOB_BASE, 8}, TIM16_BASE,
                    fw::stm32f072::registers::rcc_apb2enr, RCC_APB2ENR_TIM16EN, 2, true>();
    remapped_buzzer<CounterTimerScenario, 2, fw::Pin{GPIOA_BASE, 15}, TIM2_BASE,
                    fw::stm32f072::registers::rcc_apb1enr, RCC_APB1ENR_TIM2EN, 2, false>();
    remapped_buzzer<PairedTimerScenario, 15, fw::Pin{GPIOA_BASE, 2}, TIM15_BASE,
                    fw::stm32f072::registers::rcc_apb2enr, RCC_APB2ENR_TIM15EN, 0, true>();
    remapped_buzzer<LastTimerScenario, 17, fw::Pin{GPIOB_BASE, 9}, TIM17_BASE,
                    fw::stm32f072::registers::rcc_apb2enr, RCC_APB2ENR_TIM17EN, 2, true>();
    remapped_storage_and_display();
    touch_hold();
}
