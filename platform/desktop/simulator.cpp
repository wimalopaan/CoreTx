// SPDX-License-Identifier: GPL-2.0-or-later
#include "coretx/ui/ui.hpp"
#include "platform/desktop/file_storage.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>

namespace {

using coretx::ui::Framebuffer;
using coretx::ui::Key;
using coretx::ui::PointerEvent;
using coretx::ui::PointerKind;

struct Viewport {
    SDL_Surface *surface{};
    SDL_Rect area{};
    int scale{};
    int window_width{};
    int window_height{};
};

bool viewport(SDL_Window *const window, Viewport &view) {
    view.surface = SDL_GetWindowSurface(window);
    if (!view.surface || !SDL_GetWindowSize(window, &view.window_width, &view.window_height)) {
        return false;
    }
    view.scale =
        std::min(view.surface->w / Framebuffer::width, view.surface->h / Framebuffer::height);
    view.area = {(view.surface->w - Framebuffer::width * view.scale) / 2,
                 (view.surface->h - Framebuffer::height * view.scale) / 2,
                 Framebuffer::width * view.scale, Framebuffer::height * view.scale};
    return true;
}

// The simulator is a board of its own; it uses the FS-i6X input names.
constexpr coretx::ui::SwitchNames switch_names{"SA",  "SB",  "SC",  "SD",  "VRA", "VRB", "VRC",
                                               "VRD", "VRE", "VRF", "VRG", "SE",  "SF",  "SG",
                                               "SH",  "SI",  "T1",  "T2",  "T3",  "T4"};

bool pointer(SDL_Window *const window, coretx::ui::Interface &ui, coretx::Model &model,
             coretx::RawControls &controls, coretx::ControlSetup &setup, const PointerKind &kind,
             const float x, const float y) {
    Viewport view{};
    if (!viewport(window, view)) {
        return false;
    }
    if ((view.scale == 0) || !std::isfinite(x) || !std::isfinite(y) || (x < 0) || (y < 0) ||
        (x >= static_cast<float>(view.window_width)) ||
        (y >= static_cast<float>(view.window_height))) {
        ui.handle(PointerEvent{PointerKind::cancel}, model, controls, setup);
        return true;
    }
    // SDL3 supplies floating-point desktop coordinates; only this host boundary converts them.
    const auto px =
        static_cast<std::int64_t>(static_cast<int>(x)) * view.surface->w / view.window_width -
        view.area.x;
    const auto py =
        static_cast<std::int64_t>(static_cast<int>(y)) * view.surface->h / view.window_height -
        view.area.y;
    if ((px < 0) || (py < 0) || (px >= view.area.w) || (py >= view.area.h)) {
        ui.handle(PointerEvent{PointerKind::cancel}, model, controls, setup);
    } else {
        ui.handle({kind, static_cast<std::int16_t>(px / view.scale),
                   static_cast<std::int16_t>(py / view.scale)},
                  model, controls, setup, (SDL_GetModState() & SDL_KMOD_SHIFT) != 0);
    }
    return true;
}

struct Resources {
    SDL_Window *window{};
    SDL_Surface *source{};
    ~Resources() {
        SDL_DestroySurface(source);
        SDL_DestroyWindow(window);
        SDL_Quit();
    }
};

int failure(const char *const operation) {
    std::fprintf(stderr, "%s: %s\n", operation, SDL_GetError());
    return 1;
}

int storage_failure(const char *const operation, const coretx::host::FileStatus &status) {
    std::fprintf(stderr, "%s: %s%s%s\n", operation, coretx::host::error_message(status.code),
                 status.system_error ? ": " : "",
                 status.system_error ? std::strerror(status.system_error) : "");
    return 1;
}

void report_slots(const coretx::storage::Selection &selection) {
    for (std::size_t i = 0; i < selection.slots.size(); ++i) {
        const auto &status = selection.slots[i];
        if ((status.code != coretx::storage::Error::none) &&
            (status.code != coretx::storage::Error::empty)) {
            std::fprintf(stderr, "Storage slot %zu: %s (offset %zu)\n", i,
                         coretx::storage::error_message(status.code), status.offset);
        }
    }
}

constexpr Key translate(const SDL_KeyboardEvent &event) {
    switch (event.scancode) {
    case SDL_SCANCODE_UP:
        return Key::up;
    case SDL_SCANCODE_DOWN:
        return Key::down;
    case SDL_SCANCODE_LEFT:
        return Key::left;
    case SDL_SCANCODE_RIGHT:
        return Key::right;
    case SDL_SCANCODE_RETURN:
        return event.repeat ? Key::none : Key::enter;
    case SDL_SCANCODE_M:
        return event.repeat ? Key::none : Key::menu;
    case SDL_SCANCODE_ESCAPE:
        return event.repeat ? Key::none : Key::back;
    case SDL_SCANCODE_TAB:
        if (event.repeat) {
            return Key::none;
        }
        return (event.mod & SDL_KMOD_SHIFT) ? Key::previous_page : Key::next_page;
    default:
        return Key::none;
    }
}

bool present(const Resources &resources, const Framebuffer &display,
             std::array<std::uint32_t, Framebuffer::width * Framebuffer::height> &pixels) {
    for (int y = 0; y < Framebuffer::height; ++y) {
        for (int x = 0; x < Framebuffer::width; ++x) {
            pixels[static_cast<std::size_t>(y * Framebuffer::width + x)] =
                display.pixel_at(x, y) ? 0xFF152013U : 0xFFBCD6A1U;
        }
    }
    Viewport view{};
    if (!viewport(resources.window, view)) {
        return false;
    }
    auto *destination = view.surface;
    if (!SDL_FillSurfaceRect(destination, nullptr, SDL_MapSurfaceRGB(destination, 24, 24, 24))) {
        return false;
    }
    if (view.scale > 0) {
        if (!SDL_BlitSurfaceScaled(resources.source, nullptr, destination, &view.area,
                                   SDL_SCALEMODE_NEAREST)) {
            return false;
        }
    }
    return SDL_UpdateWindowSurface(resources.window);
}

bool queue_key(SDL_Window *const window, const SDL_Scancode &scancode) {
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.windowID = SDL_GetWindowID(window);
    event.key.scancode = scancode;
    event.key.down = true;
    return SDL_PushEvent(&event);
}

bool queue_smoke_events(SDL_Window *const window) {
    constexpr std::array keys{
        SDL_SCANCODE_RIGHT,  SDL_SCANCODE_TAB,    SDL_SCANCODE_RIGHT,  SDL_SCANCODE_RETURN,
        SDL_SCANCODE_LEFT,   SDL_SCANCODE_ESCAPE, SDL_SCANCODE_RETURN, SDL_SCANCODE_LEFT,
        SDL_SCANCODE_RETURN, SDL_SCANCODE_TAB,    SDL_SCANCODE_RIGHT,  SDL_SCANCODE_RETURN,
        SDL_SCANCODE_LEFT,   SDL_SCANCODE_RETURN, SDL_SCANCODE_TAB,    SDL_SCANCODE_RETURN,
        SDL_SCANCODE_RIGHT,  SDL_SCANCODE_RETURN, SDL_SCANCODE_TAB};
    for (const auto scancode : keys) {
        if (!queue_key(window, scancode)) {
            return false;
        }
    }
    return true;
}

bool queue_mouse(SDL_Window *const window, const PointerKind &kind, const int x, const int y) {
    Viewport view{};
    if (!viewport(window, view)) {
        return false;
    }
    const auto wx = static_cast<float>(
        static_cast<std::int64_t>(view.area.x + x * view.scale + view.scale / 2) *
        view.window_width / view.surface->w);
    const auto wy = static_cast<float>(
        static_cast<std::int64_t>(view.area.y + y * view.scale + view.scale / 2) *
        view.window_height / view.surface->h);
    SDL_Event event{};
    if (kind == PointerKind::move) {
        event.type = SDL_EVENT_MOUSE_MOTION;
        event.motion.windowID = SDL_GetWindowID(window);
        event.motion.state = SDL_BUTTON_LMASK;
        event.motion.x = wx;
        event.motion.y = wy;
    } else {
        event.type =
            kind == PointerKind::press ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        event.button.windowID = SDL_GetWindowID(window);
        event.button.button = SDL_BUTTON_LEFT;
        event.button.down = kind == PointerKind::press;
        event.button.x = wx;
        event.button.y = wy;
    }
    return SDL_PushEvent(&event);
}

bool click(SDL_Window *const window, const int x, const int y) {
    return queue_mouse(window, PointerKind::press, x, y) &&
           queue_mouse(window, PointerKind::release, x, y);
}

bool clicks(SDL_Window *const window, const std::initializer_list<SDL_Point> &points) {
    for (const auto &point : points) {
        if (!click(window, point.x, point.y)) {
            return false;
        }
    }
    return true;
}

bool queue_editor_smoke_events(SDL_Window *const window) {
    constexpr SDL_Point page{20, 8};
    constexpr SDL_Point field{80, 8};
    constexpr SDL_Point ok{110, 56};
    constexpr SDL_Point plus{80, 56};
    constexpr SDL_Point minus{48, 56};
    constexpr SDL_Point row2{110, 28};
    constexpr SDL_Point row3{110, 36};
    for (std::size_t i = 0; i < coretx::ui::page_count - 3; ++i)
        if (!click(window, page.x, page.y))
            return false;
    if (!clicks(window, {ok, plus, ok}) || !queue_key(window, SDL_SCANCODE_M) ||
        !queue_key(window, SDL_SCANCODE_ESCAPE)) {
        return false;
    }
    for (int i = 0; i < 6; ++i) {
        if (!click(window, field.x, field.y)) {
            return false;
        }
    }
    // Retarget the duplicate, then select the negated switch condition "!SA-".
    if (!clicks(window, {row2, plus, ok, field, row3, minus, minus, ok})) {
        return false;
    }
    // Duplicate again; reject the first delete and explicitly confirm the second.
    if (!clicks(window, {ok, plus, ok, ok, plus, plus, ok, ok, ok, plus, ok})) {
        return false;
    }
    return clicks(window, {page, ok, ok, page, page});
}

bool queue_touch_smoke_events(SDL_Window *const window) {
    if (!SDL_SetWindowSize(window, 1001, 701) || !SDL_SyncWindow(window)) {
        return false;
    }
    for (const auto type :
         {SDL_EVENT_WINDOW_FOCUS_LOST, SDL_EVENT_WINDOW_MOUSE_LEAVE, SDL_EVENT_WINDOW_RESIZED}) {
        if (!queue_mouse(window, PointerKind::press, 80, 56)) {
            return false;
        }
        SDL_Event cancel{};
        cancel.type = type;
        cancel.window.windowID = SDL_GetWindowID(window);
        if (!SDL_PushEvent(&cancel) || !queue_mouse(window, PointerKind::release, 80, 56)) {
            return false;
        }
    }
    if (!click(window, -1, -1) || !click(window, 80, 56) || !click(window, 20, 8) ||
        !click(window, 80, 8) || !click(window, 110, 20) || !click(window, 48, 56) ||
        !click(window, 16, 56) || !click(window, 110, 20) || !click(window, 48, 56) ||
        !click(window, 110, 56) || !click(window, 20, 8) || !click(window, 80, 8) ||
        !click(window, 110, 20) || !click(window, 48, 56) || !click(window, 110, 56) ||
        !click(window, 20, 8)) {
        return false;
    }
    for (int i = 0; i < 7; ++i) {
        if (!queue_mouse(window, PointerKind::press, 20, 44) ||
            !queue_mouse(window, PointerKind::move, 20, 20) ||
            !queue_mouse(window, PointerKind::release, 20, 20)) {
            return false;
        }
    }
    return click(window, 110, 44) && click(window, 80, 56) && click(window, 110, 56) &&
           click(window, 20, 8) && queue_editor_smoke_events(window);
}

bool queue_calibration_smoke_events(SDL_Window *const window) {
    // Narrow physical P1 to 1024..3072, retaining center 2048.
    if (!click(window, 80, 8) || !click(window, 20, 28)) {
        return false;
    }
    for (int i = 0; i < 32; ++i) {
        if (!queue_key(window, SDL_SCANCODE_LEFT)) {
            return false;
        }
    }
    for (int i = 0; i < 64; ++i) {
        if (!queue_key(window, SDL_SCANCODE_RIGHT)) {
            return false;
        }
    }
    return click(window, 110, 56) && click(window, 20, 44);
}

} // namespace

int main(const int argc, char **const argv) {
    std::string_view mode{};
    const char *state_argument{};
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument{argv[i]};
        if ((argument == "--state") && (state_argument == nullptr) && ((i + 1) < argc)) {
            state_argument = argv[++i];
        } else if (mode.empty() &&
                   ((argument == "--smoke-test") || (argument == "--touch-smoke-test") ||
                    (argument == "--calibration-smoke-test") ||
                    (argument == "--storage-smoke-test"))) {
            mode = argument;
        } else {
            std::fprintf(stderr, "Invalid simulator argument: %s\n", argv[i]);
            return 1;
        }
    }
    const bool keyboard_smoke = mode == "--smoke-test";
    const bool touch_smoke = mode == "--touch-smoke-test";
    const bool calibration_smoke = mode == "--calibration-smoke-test";
    const bool storage_smoke = mode == "--storage-smoke-test";
    const bool smoke = !mode.empty();
    const bool persist = !smoke || (state_argument != nullptr);
    if (storage_smoke && (state_argument == nullptr)) {
        std::fputs("--storage-smoke-test requires --state FILE\n", stderr);
        return 1;
    }

    std::array<std::uint32_t, Framebuffer::width * Framebuffer::height> pixels{};
    Resources resources{};
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        return failure("SDL_Init");
    }
    coretx::storage::State state{coretx::ui::simulator_model(), coretx::ui::simulator_setup()};
    auto &model = state.model;
    auto &setup = state.setup;
    coretx::host::FileStorage store{};
    if (persist) {
        std::string path{};
        if (state_argument) {
            path = state_argument;
        } else {
            char *const directory = SDL_GetPrefPath("CoreTX", "simulator");
            if (!directory) {
                return failure("SDL_GetPrefPath");
            }
            path = directory;
            SDL_free(directory);
            path += "state.bin";
        }
        std::fprintf(stderr, "Storage: %s\n", path.c_str());
        if (const auto status = store.open(path.c_str()); !status) {
            return storage_failure("Storage open", status);
        }
        const auto loaded = store.load(state);
        if (!loaded.file) {
            return storage_failure("Storage load", loaded.file);
        }
        report_slots(loaded.selection);
        if (loaded.selection.status.code == coretx::storage::Error::empty) {
            std::fputs("No stored state; starting with explicit simulator defaults\n", stderr);
            if (storage_smoke) {
                return 1;
            }
        } else if (!loaded) {
            std::fprintf(stderr, "Storage load: %s\n",
                         coretx::storage::error_message(loaded.selection.status.code));
            return 1;
        } else {
            std::fprintf(stderr, "Loaded storage slot %u, generation %lu\n",
                         static_cast<unsigned>(loaded.selection.slot),
                         static_cast<unsigned long>(loaded.selection.generation));
        }
    }
    resources.window =
        SDL_CreateWindow("CoreTX - mouse/touch or keyboard; Q quits", Framebuffer::width * 6,
                         Framebuffer::height * 6, SDL_WINDOW_RESIZABLE);
    if (!resources.window) {
        return failure("SDL_CreateWindow");
    }
    if (!SDL_SetWindowMinimumSize(resources.window, Framebuffer::width * 2,
                                  Framebuffer::height * 2)) {
        return failure("SDL_SetWindowMinimumSize");
    }
    resources.source =
        SDL_CreateSurfaceFrom(Framebuffer::width, Framebuffer::height, SDL_PIXELFORMAT_ARGB8888,
                              pixels.data(), Framebuffer::width * 4);
    if (!resources.source) {
        return failure("SDL_CreateSurfaceFrom");
    }
    const int loaded_input_weight = model.inputs[0].weight_percent.value();
    if ((keyboard_smoke && !queue_smoke_events(resources.window)) ||
        (touch_smoke && !queue_touch_smoke_events(resources.window)) ||
        (calibration_smoke && !queue_calibration_smoke_events(resources.window)) ||
        (storage_smoke && (!queue_key(resources.window, SDL_SCANCODE_TAB) ||
                           !queue_key(resources.window, SDL_SCANCODE_RIGHT) ||
                           !queue_key(resources.window, SDL_SCANCODE_RETURN) ||
                           !queue_key(resources.window, SDL_SCANCODE_LEFT)))) {
        return failure("SDL smoke setup");
    }

    auto controls = coretx::ui::simulator_raw_controls();
    controls.trims = model.trims;
    coretx::ui::Interface ui{};
    ui.set_switch_names(switch_names);
    Framebuffer display{};
    coretx::Status previous_error{};
    coretx::PreparationStatus previous_preparation_error{};
    coretx::LogicalState logical_state{};
    bool running = true;
    bool smoke_verified = false;
    Uint64 next_frame{};
    while (running) {
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            if ((event.type == SDL_EVENT_QUIT) ||
                (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN) {
                if (event.key.scancode == SDL_SCANCODE_Q) {
                    running = false;
                } else {
                    ui.handle(translate(event.key), model, controls, setup,
                              (event.key.mod & SDL_KMOD_SHIFT) != 0);
                }
            } else if (((event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) ||
                        (event.type == SDL_EVENT_MOUSE_BUTTON_UP)) &&
                       (event.button.button == SDL_BUTTON_LEFT)) {
                if (!pointer(resources.window, ui, model, controls, setup,
                             event.button.down ? PointerKind::press : PointerKind::release,
                             event.button.x, event.button.y)) {
                    return failure("SDL pointer mapping");
                }
            } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
                if ((event.motion.state & SDL_BUTTON_LMASK) == 0) {
                    ui.handle(PointerEvent{PointerKind::cancel}, model, controls, setup);
                } else if (!pointer(resources.window, ui, model, controls, setup, PointerKind::move,
                                    event.motion.x, event.motion.y)) {
                    return failure("SDL pointer mapping");
                }
            } else if ((event.type == SDL_EVENT_WINDOW_FOCUS_LOST) ||
                       (event.type == SDL_EVENT_WINDOW_MOUSE_LEAVE) ||
                       (event.type == SDL_EVENT_WINDOW_RESIZED) ||
                       (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) ||
                       (event.type == SDL_EVENT_WINDOW_HIDDEN)) {
                ui.handle(PointerEvent{PointerKind::cancel}, model, controls, setup);
            }
        }
        if (!running) {
            break;
        }
        const auto now = SDL_GetTicks();
        if (now >= next_frame) {
            ui.observe(controls);
            const auto prepared = coretx::prepare_controls(setup, controls);
            coretx::Evaluation evaluation{};
            if (prepared) {
                auto next = logical_state;
                evaluation = coretx::evaluate(model, prepared.controls, next);
                if (evaluation && !ui.editing() && !ui.calibrating())
                    logical_state = next;
            } else if ((prepared.status.code != previous_preparation_error.code) ||
                       (prepared.status.index != previous_preparation_error.index)) {
                std::fprintf(stderr, "Control preparation[%zu]: %s\n", prepared.status.index,
                             coretx::preparation_error_message(prepared.status.code));
            }
            previous_preparation_error = prepared.status;
            if (prepared && !evaluation &&
                ((evaluation.status.code != previous_error.code) ||
                 (evaluation.status.stage != previous_error.stage) ||
                 (evaluation.status.index != previous_error.index))) {
                std::fprintf(stderr, "%s[%zu]: %s\n", coretx::stage_name(evaluation.status.stage),
                             evaluation.status.index,
                             coretx::error_message(evaluation.status.code));
            }
            previous_error = evaluation.status;
            ui.render(model, controls, prepared, evaluation, display);
            if (!present(resources, display, pixels)) {
                return failure("SDL display update");
            }
            next_frame = now + 33;
            if (smoke && !smoke_verified) {
                const bool calibration_ok = calibration_smoke && !ui.calibrating() && prepared &&
                                            evaluation && (setup.calibration[0].minimum == 1024) &&
                                            (setup.calibration[0].center == 2048) &&
                                            (setup.calibration[0].maximum == 3072) &&
                                            (prepared.controls.analogs[0] == 1024) &&
                                            (evaluation.frame.channels[0] == 1024);
                const bool storage_ok =
                    storage_smoke && prepared && evaluation && ui.editing() &&
                    (controls.trims == model.trims) &&
                    (model.inputs[0].weight_percent.value() ==
                     std::max<int>(coretx::InputWeight::minimum, loaded_input_weight - 1)) &&
                    (setup.calibration[0].minimum == 1024) &&
                    (setup.calibration[0].center == 2048) && (setup.calibration[0].maximum == 3072);
                if (storage_smoke ? !storage_ok
                    : calibration_smoke
                        ? !calibration_ok
                        : ((ui.page() != coretx::ui::Page::monitor) || ui.editing() ||
                           (ui.dialog() != coretx::ui::Dialog::none) || !prepared ||
                           (prepared.controls.analogs[0] != 16) ||
                           (model.inputs[0].weight_percent.value() != 99) ||
                           (model.mixes[0].weight_percent.value() != 99) ||
                           (keyboard_smoke ? model.outputs[0].minimum_permille.value() != -990
                                           : model.outputs[15].minimum_permille.value() != -990) ||
                           (touch_smoke &&
                            ((model.inputs.size() != 9) || (model.mixes.size() != 9) ||
                             (model.inputs[2].destination != 1) ||
                             !model.inputs[2].condition.enabled ||
                             (model.inputs[2].condition.index != 0) ||
                             (model.inputs[2].condition.position !=
                              coretx::SwitchPosition::middle) ||
                             !model.inputs[2].condition.inverted ||
                             (model.mixes[1].weight_percent.value() != 0))) ||
                           !evaluation || (evaluation.frame.channels[0] == 0))) {
                    std::fputs("SDL simulator smoke scenario failed\n", stderr);
                    return 1;
                }
                smoke_verified = true;
                SDL_Event quit{};
                quit.type = SDL_EVENT_QUIT;
                if (!SDL_PushEvent(&quit)) {
                    return failure("SDL_PushEvent quit");
                }
            }
        }
        SDL_Delay(1);
    }
    if (smoke) {
        if (!smoke_verified) {
            std::fputs("SDL simulator closed before the smoke scenario completed\n", stderr);
            return 1;
        }
    }
    if (persist) {
        // BACK cancels scalar edits and calibration drafts before taking the saved snapshot.
        ui.handle(Key::back, model, controls, setup);
        model.trims = controls.trims;
        const auto saved = store.save(state);
        if (!saved.file) {
            return storage_failure("Storage save", saved.file);
        }
        if (!saved.codec) {
            std::fprintf(stderr, "Storage save: %s\n",
                         coretx::storage::error_message(saved.codec.code));
            return 1;
        }
        std::fprintf(stderr, "%s storage generation %lu\n", saved.written ? "Saved" : "Unchanged",
                     static_cast<unsigned long>(saved.generation));
        if (const auto status = store.close(); !status) {
            return storage_failure("Storage close", status);
        }
    }
    if (smoke) {
        std::puts("SDL3 simulator smoke scenario passed");
    }
}
