// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/common/crsf_menu.hpp"
#include "coretx/features.hpp"
#include "coretx/radio/multiswitch.hpp"
#include "platform/stm32f072/crsf.hpp"

#include <optional>
#include <type_traits>

namespace coretx::firmware {

namespace crsf_ui_detail {
struct MultiSwitchState {
    coretx::multiswitch::Pages pages{};
    std::optional<std::size_t> tapped{};
    std::uint8_t selected{};
    bool select{};

    // Key selection of a tile without touch: ok enters selection, up/down move, ok toggles.
    constexpr bool handle(const Key key, const bool operable, const char *&message) noexcept {
        if (select) {
            if ((key == Key::cancel) || (key == Key::cancel_long))
                select = false;
            else if ((key == Key::up) || (key == Key::down))
                selected = static_cast<std::uint8_t>(
                    (selected + (key == Key::down ? 1U : coretx::multiswitch::switch_count - 1U)) %
                    coretx::multiswitch::switch_count);
            else if ((key == Key::ok) && operable)
                tapped = selected;
            return true;
        }
        if (key != Key::ok)
            return false;
        if (operable) {
            select = true;
            selected = 0;
        } else
            message = coretx::ui::tr::crsf_select_external;
        return true;
    }
    [[nodiscard]] constexpr std::optional<std::size_t> selection() const noexcept {
        return select ? std::optional<std::size_t>{selected} : std::nullopt;
    }
};
} // namespace crsf_ui_detail

// UI state of the external CRSF module: the parameter menu, the dashboard page that embeds it
// and the multiswitch pages that publish their state through the module.
template <typename Module, bool MultiSwitch> struct CrsfUi {
    constexpr void reset() noexcept {
        menu = {};
        reset_multiswitch();
        forget_dashboard();
    }
    constexpr void reset_multiswitch() noexcept {
        if constexpr (MultiSwitch) {
            multiswitch.pages = {};
            multiswitch.select = false;
        }
    }
    constexpr void close() noexcept {
        menu.close(Module::config());
        forget_dashboard();
    }
    // Message explaining why the module cannot be configured, or nullptr.
    [[nodiscard]] static constexpr const char *unavailable(const bool external) noexcept {
        if (!external)
            return coretx::ui::tr::crsf_select_external;
        if (Module::error() != CrsfError::none)
            return coretx::ui::tr::crsf_link;
        return nullptr;
    }

    // Opens, resumes or suspends the embedded menu when the dashboard page becomes (in)active.
    constexpr void sync_dashboard(const bool active, const TimestampUs now) noexcept {
        if (active == dashboard_active) {
            if (active && !dashboard_interacting)
                menu.handle(Key::none, Module::config(), now);
            else if (!active && dashboard_saved && !menu.visible())
                menu.suspend(Module::config());
            return;
        }
        dashboard_active = active;
        dashboard_interacting = false;
        if (active) {
            if (dashboard_saved)
                menu.resume(Module::config(), now);
            else
                menu.open(Module::config(), now);
        } else {
            menu.suspend(Module::config());
            dashboard_saved = true;
        }
    }
    // Key navigation on the dashboard page; returns false if the key is not consumed.
    constexpr bool handle_dashboard(const Key key, const TimestampUs now, const bool external,
                                    const char *&message) noexcept {
        auto &client = Module::config();
        if (dashboard_interacting) {
            if ((key == Key::cancel_long) &&
                (client.command() == coretx::crsf::config::Command::idle)) {
                if (!client.pending_write()) {
                    menu.finish_edit(client);
                    dashboard_interacting = false;
                }
            } else if ((key == Key::cancel) &&
                       (client.mode() == coretx::crsf::config::Mode::devices) && !menu.editing()) {
                dashboard_interacting = false;
            } else
                menu.handle(key, client, now);
            return true;
        }
        if (key != Key::ok)
            return false;
        if (const auto error = unavailable(external))
            message = error;
        else
            dashboard_interacting = true;
        return true;
    }

    CrsfMenu menu{};
    [[no_unique_address]]
    std::conditional_t<MultiSwitch, crsf_ui_detail::MultiSwitchState, DisabledFeature>
        multiswitch{};
    bool dashboard_active{};
    bool dashboard_saved{};
    bool dashboard_interacting{};

  private:
    constexpr void forget_dashboard() noexcept {
        dashboard_active = false;
        dashboard_saved = false;
        dashboard_interacting = false;
    }
};

} // namespace coretx::firmware
