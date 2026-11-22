// SPDX-License-Identifier: GPL-2.0-or-later
#include "coretx/ui/ui.hpp"
#include "platform/desktop/file_storage.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

namespace {

namespace store = coretx::storage;
using coretx::host::FileError;
using coretx::host::FileStorage;

void check(const bool passed, const char *const expression, const int line) {
    if (!passed) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
        std::exit(EXIT_FAILURE);
    }
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

struct TemporaryFile {
    char name[32] = "/tmp/coretx-storage-test-XXXXXX";

    TemporaryFile() {
        const int descriptor = ::mkstemp(name);
        CHECK(descriptor >= 0);
        CHECK(::close(descriptor) == 0);
    }
    ~TemporaryFile() { CHECK(::unlink(name) == 0); }
};

store::State fixture() { return {coretx::ui::simulator_model(), coretx::ui::simulator_setup()}; }

void reopen_and_edits() {
    TemporaryFile file{};
    auto state = fixture();
    FileStorage store{};
    CHECK(store.open(file.name));
    CHECK(store.load(state).selection.status.code == store::Error::empty);
    FileStorage competing{};
    CHECK(competing.open(file.name).code == FileError::lock);
    auto saved = store.save(state);
    CHECK(saved && saved.written && (saved.generation == 0));
    saved = store.save(state);
    CHECK(saved && !saved.written && (saved.generation == 0));

    coretx::ui::Interface ui{};
    auto raw = coretx::ui::simulator_raw_controls();
    ui.handle(coretx::ui::Key::next_page, state.model, raw, state.setup);
    ui.handle(coretx::ui::Key::right, state.model, raw, state.setup);
    ui.handle(coretx::ui::Key::enter, state.model, raw, state.setup);
    ui.handle(coretx::ui::Key::left, state.model, raw, state.setup);
    CHECK(state.model.inputs[0].weight_percent.value() == 99);
    ui.handle(coretx::ui::Key::back, state.model, raw, state.setup);
    CHECK(state.model.inputs[0].weight_percent.value() == 100);
    CHECK(!store.save(state).written);
    state.model.inputs[0].weight_percent = coretx::InputWeight::constant<73>();
    state.setup.calibration[1] = {100, 1800, 3800};
    state.setup.mapping[0] = {1, true};
    saved = store.save(state);
    CHECK(saved && saved.written && (saved.generation == 1));
    CHECK(store.close());
    CHECK(store.open(file.name));
    store::State restored{};
    const auto loaded = store.load(restored);
    CHECK(loaded && (loaded.selection.slot == 1) && (loaded.selection.generation == 1));
    CHECK(restored.model.inputs[0].weight_percent.value() == 73);
    CHECK(restored.setup.calibration[1].center == 1800);
    CHECK((restored.setup.mapping[0].physical_index == 1) && restored.setup.mapping[0].reversed);
}

void failed_write_preserves_previous() {
    TemporaryFile file{};
    FileStorage store{};
    auto state = fixture();
    CHECK(store.open(file.name));
    CHECK(store.save(state));
    state.model.inputs[0].weight_percent = coretx::InputWeight::constant<42>();

    struct rlimit original{};
    CHECK(::getrlimit(RLIMIT_FSIZE, &original) == 0);
    auto limited = original;
    limited.rlim_cur = store::slot_size + 64;
    const auto handler = std::signal(SIGXFSZ, SIG_IGN);
    CHECK(handler != SIG_ERR);
    CHECK(::setrlimit(RLIMIT_FSIZE, &limited) == 0);
    const auto failed = store.save(state);
    CHECK(::setrlimit(RLIMIT_FSIZE, &original) == 0);
    CHECK(std::signal(SIGXFSZ, handler) != SIG_ERR);
    CHECK(!failed && (failed.file.code == FileError::write));
    CHECK(store.close());
    CHECK(store.open(file.name));
    store::State restored{};
    auto loaded = store.load(restored);
    CHECK(loaded && (loaded.selection.slot == 0) && (loaded.selection.generation == 0));
    CHECK(loaded.selection.slots[1].code == store::Error::truncated);
    CHECK(restored.model.inputs[0].weight_percent.value() == 100);
    const auto repaired = store.save(state);
    CHECK(repaired && (repaired.generation == 1));
    loaded = store.load(restored);
    CHECK(loaded && (restored.model.inputs[0].weight_percent.value() == 42));
}

void invalid_and_corrupted() {
    TemporaryFile file{};
    FileStorage store{};
    auto closed_state = fixture();
    CHECK(store.load(closed_state).file.code == FileError::not_open);
    CHECK(store.open(file.name));
    const auto valid = fixture();
    CHECK(store.save(valid));
    auto invalid = valid;
    invalid.setup.calibration[0] = {};
    CHECK(store.save(invalid).codec.code == store::Error::setup);
    auto restored = fixture();
    CHECK(store.load(restored));
    CHECK(store.close());
    const int descriptor = ::open(file.name, O_WRONLY);
    CHECK(descriptor >= 0);
    std::array<std::uint8_t, 8> zero{};
    CHECK(::pwrite(descriptor, zero.data(), zero.size(), 0) == static_cast<ssize_t>(zero.size()));
    CHECK(::close(descriptor) == 0);
    CHECK(store.open(file.name));
    const auto loaded = store.load(restored);
    CHECK(!loaded && (loaded.selection.status.code == store::Error::no_valid_slot));
    CHECK(store.save(valid).codec.code == store::Error::no_valid_slot);
}

} // namespace

int main() {
    reopen_and_edits();
    failed_write_preserves_previous();
    invalid_and_corrupted();
    std::puts("File storage tests passed");
}
