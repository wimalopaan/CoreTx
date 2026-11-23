// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/common/calibration_storage.hpp"
#include "boards/common/crsf_menu.hpp"
#include "boards/common/dashboard.hpp"
#include "boards/common/input_menu.hpp"
#include "boards/common/input_storage.hpp"
#include "boards/common/menu.hpp"
#include "boards/common/radio_errors.hpp"
#include "boards/common/sender_input.hpp"
#include "boards/common/telemetry_adapters.hpp"
#include "boards/i6s/power.hpp"
#include "platform/stm32f072/buzzer.hpp"
#include "platform/stm32f072/crsf.hpp"
#include "platform/stm32f072/lcd.hpp"

#include <type_traits>

namespace coretx::firmware {

enum class ApplicationState : std::uint8_t { starting, running, input_error, fatal, powered_off };
enum class ApplicationFailure : std::uint8_t { none, not_initialized, hardware, lcd, stack };
struct NoMenu {};

namespace application_detail {
struct MultiSwitchState {
    coretx::multiswitch::Pages pages{};
    std::optional<std::size_t> tapped{};
    std::uint8_t selected{};
    bool select{};
};

template <bool Menu, bool MultiSwitch> struct CrsfUiState {
    [[no_unique_address]] std::conditional_t<Menu, CrsfMenu, DisabledFeature> menu{};
    [[no_unique_address]]
    std::conditional_t<Menu && MultiSwitch, MultiSwitchState, DisabledFeature> multiswitch{};
    bool dashboard_active{};
    bool dashboard_saved{};
    bool dashboard_interacting{};
};
} // namespace application_detail

template <typename Board, typename Input = PhysicalInput<typename Board::Keys>> class Application {
    inline static constexpr bool has_menu = (Board::name == "I6X") || (Board::name == "I6S");
    using Adc = typename Board::Adc;
    using Display = typename Board::Display;
    using Probe = typename Board::Probe;
    using Memory = typename Board::Memory;
    using Inputs = typename Board::Inputs;
    using ExternalRFModule = typename Board::ExternalRFModule;
    using Radio = typename Board::Radio;
    using InternalRFModule = typename Board::InternalRFModule;
    inline static constexpr bool has_internal_rf = !std::is_void_v<InternalRFModule>;
    inline static constexpr bool has_external_rf = !std::is_void_v<ExternalRFModule>;
    inline static constexpr bool has_crsf_ui = has_menu && has_external_rf;
    inline static constexpr bool has_multiswitch = has_crsf_ui && features::multiswitch;
    using Watchdog = typename Board::Watchdog;
    inline static constexpr bool has_power = requires {
        Board::power_button_pressed();
        Board::release_power();
    };

  public:
    [[nodiscard]] constexpr ApplicationState initialize() noexcept {
        state_ = ApplicationState::fatal;
        failure_ = ApplicationFailure::none;
        if constexpr (has_power)
            power_ = {};
        sampled_at_ = {};
        diagnostics_.invalidate();
        telemetry_ = {};
        overview_phase_ = 0;
        Board::power_off();
        if (Board::initialize() != stm32f072::HardwareError::none) {
            return fail(ApplicationFailure::hardware);
        }
        if (Display::initialize() != LcdError::none) {
            return fail(ApplicationFailure::lcd);
        }
        state_ = Adc::initialize() == AdcError::none ? ApplicationState::starting
                                                     : ApplicationState::input_error;
        input_.initialize();
        if constexpr (!Input::touch)
            trim_keys_.reset();
        Inputs::initialize();
        Probe::initialize();
        radio_storage_.initialize(Board::transmitter_id());
        if constexpr (has_menu) {
            models_.initialize(Board::transmitter_id());
            model_applied_ = false;
            menu_ = {};
            if constexpr (has_external_rf) {
                crsf_.menu = {};
                if constexpr (has_multiswitch) {
                    crsf_.multiswitch.pages = {};
                    crsf_.multiswitch.select = false;
                }
                crsf_.dashboard_active = false;
                crsf_.dashboard_saved = false;
                crsf_.dashboard_interacting = false;
            }
            framebuffer_.compact(false);
        }
        render();
        drawn_at_ = Board::now_us();
        return state_;
    }
    [[nodiscard]] constexpr ApplicationState service(TimestampUs now) noexcept {
        if ((state_ == ApplicationState::fatal) || (state_ == ApplicationState::powered_off)) {
            return state_;
        }
        if (!Board::stack_intact()) {
            return fail(ApplicationFailure::stack);
        }
        if constexpr (has_power) {
            if (power_.update(now, Board::power_button_pressed())) {
                select_output(RadioOutput::off, now);
                Board::power_off();
                Inputs::stop();
                diagnostics_.defer_refresh();
                if constexpr (has_menu)
                    (void)menu_.handle(Key::cancel_long, models_, diagnostics_, output_);
                diagnostics_.handle(Key::cancel);
                diagnostics_.flush_refresh();
                overview_phase_ = 0;
                drawn_at_ = now - redraw_interval;
            }
        }
        if (Display::service(framebuffer_) != LcdError::none) {
            return fail(ApplicationFailure::lcd);
        }
        if (!Board::stack_intact()) {
            return fail(ApplicationFailure::stack);
        }
        Probe::service(now);
        const bool model_memory = [&] constexpr {
            if constexpr (has_menu)
                return models_.owns_memory();
            else
                return false;
        }();
        if (!radio_storage_.owns_memory() && !model_memory && !input_storage_.owns_memory())
            calibration_storage_.service(now, Probe::result(), diagnostics_);
        if (!calibration_storage_.busy() && !model_memory && !radio_storage_.owns_memory())
            input_storage_.service(now, Probe::result());
        if (!input_applied_ && input_storage_.loaded() &&
            (diagnostics_.phase() == CalibrationPhase::idle)) {
            overview_phase_ = 0;
            if (!diagnostics_.set_input_setup(
                    input_storage_.setup(),
                    (input_storage_.status() == RadioStorageStatus::ready) ||
                        (input_storage_.status() == RadioStorageStatus::saved))) {
                radio_message_ = coretx::ui::tr::inputs_invalid;
            } else
                apply_inputs();
            input_applied_ = true;
        }
        if (!calibration_storage_.busy() && !model_memory && !input_storage_.owns_memory() &&
            input_applied_)
            radio_storage_.service(now, Probe::result());
        if (!settings_applied_ && radio_storage_.loaded()) {
            settings_ = radio_storage_.settings();
            settings_applied_ = true;
            if constexpr (has_internal_rf) {
                internal_.initialize(settings_.transmitter, settings_.receiver, now);
                internal_.set_channel_mode(settings_.channel_mode);
            }
            select_output(has_menu ? RadioOutput::off : settings_.output, now);
        }
        if constexpr (has_menu) {
            // Live field previews must not be journaled by an asynchronous receiver update.
            if (settings_applied_ && !calibration_storage_.busy() &&
                !radio_storage_.owns_memory() && !input_storage_.owns_memory() &&
                !menu_.pending_model_edit())
                models_.service(
                    now, Probe::result(),
                    (calibration_storage_.state() == CalibrationStorageState::loaded) ||
                            (calibration_storage_.state() == CalibrationStorageState::saved)
                        ? calibration_storage_.legacy_model()
                        : diagnostic_model,
                    settings_);
            if (models_.take_changed()) {
                if constexpr (!Input::touch)
                    trim_keys_.reset();
                overview_phase_ = 0;
                menu_.model_changed(models_.active());
                diagnostics_.use_model(models_.data().model);
                if constexpr (has_multiswitch) {
                    crsf_.multiswitch.pages = {};
                    crsf_.multiswitch.select = false;
                }
                settings_ = models_.data().radio;
                if constexpr (has_internal_rf) {
                    internal_.deactivate();
                    internal_.initialize(settings_.transmitter, settings_.receiver,
                                         Board::now_us());
                    internal_.set_channel_mode(settings_.channel_mode);
                }
                const auto selected_output =
                    model_applied_ || shutting_down() ? RadioOutput::off : settings_.output;
                model_applied_ = true;
                select_output(selected_output, Board::now_us());
            }
        }
        if constexpr (has_menu)
            diagnostics_.allow_logical_commit(!menu_.pending_model_edit());
        // Loading input settings can restart the ADC with a newer initialization timestamp.
        now = Board::now_us();
        const auto event = Adc::service(now);
        if ((event == ScanEvent::failed) || (Adc::error() != AdcError::none)) {
            diagnostics_.invalidate();
            telemetry_.invalidate(telemetry::transmitter_battery);
            state_ = ApplicationState::input_error;
        } else if (event == ScanEvent::complete) {
            sampled_at_ = now;
            const auto &samples = Adc::samples();
            diagnostics_.observe(std::span{samples}, now);
            telemetry_.publish(telemetry::transmitter_battery,
                               battery_millivolts(samples[battery_index]), now);
            state_ = ApplicationState::running;
        }
        diagnostics_.observe_digital(Inputs::read(), now);
        Inputs::service(now, diagnostics_.take_click() && !shutting_down());
        display_ready_ = display_ready_ || !Display::busy();
        if constexpr (has_external_rf) {
            if constexpr (has_menu) {
                if constexpr (has_multiswitch) {
                    if (!multiswitch_slot())
                        crsf_.multiswitch.select = false;
                }
                diagnostics_.constrain_page();
                sync_crsf_dashboard(now);
            }
        }
        const auto key =
            input_.service(now, !Input::needs_display || display_ready_, input_context());
#ifdef CORETX_I6S_TOUCH_TRACE
        if (key != Key::none)
            trace_key_ = key;
#endif
        // Keep RC evaluation out of nested UI handlers on the 2 KiB stack.
        diagnostics_.defer_refresh();
        if (!shutting_down())
            handle(key, now);
        if constexpr (requires { input_.rear_buttons(); }) {
            const bool setup = !dashboard_touch() && !shutting_down();
            diagnostics_.observe_setup_controls(setup, input_.rear_buttons());
            input_.set_setup_mode(now, setup);
        }
        diagnostics_.flush_refresh();
        if constexpr (has_menu && !Input::touch) {
            const auto buttons = input_.trim_buttons();
            if (shutting_down() || (diagnostics_.phase() != CalibrationPhase::idle))
                trim_keys_.reset();
            else if (models_.editable() &&
                     trim_keys_.update(models_.data().model, diagnostics_.setup(), buttons, now)) {
                diagnostics_.refresh_model();
                Inputs::service(now, diagnostics_.input_setup().trim_beep);
                if (!models_.save_trims(now))
                    radio_message_ = models_.message();
            }
            diagnostics_.observe_trims(buttons);
        }
        if constexpr (has_external_rf) {
            if constexpr (has_menu) {
                sync_crsf_dashboard(now);
                if constexpr (has_multiswitch) {
                    crsf_.multiswitch.pages.configure(diagnostics_.model());
                    const auto slot = multiswitch_operable() ? multiswitch_slot() : std::nullopt;
                    const auto held = Input::touch ? input_.held_switch()
                                      : crsf_.multiswitch.select && input_.ok_held()
                                          ? std::optional<std::size_t>{crsf_.multiswitch.selected}
                                          : std::nullopt;
                    const auto tapped =
                        Input::touch ? input_.tapped_switch() : crsf_.multiswitch.tapped;
                    const auto *const controls =
                        multiswitch_signal_operable() ? &diagnostics_.prepared().controls : nullptr;
                    if (crsf_.multiswitch.pages.update(slot, held, tapped, controls,
                                                       diagnostics_.evaluation().frame.logical) &&
                        diagnostics_.model().multiswitch_beep && !shutting_down())
                        Inputs::service(now, true, ClickTone::multiswitch);
                    crsf_.multiswitch.tapped.reset();
                }
            }
        }
        if constexpr (has_internal_rf) {
            if (settings_applied_ && !shutting_down()) {
                internal_.service(Board::now_us(), diagnostics_.evaluation().frame.channels,
                                  (state_ == ApplicationState::running) &&
                                      diagnostics_.output_valid(),
                                  sampled_at_);
                if (internal_.take_receiver_changed()) {
                    telemetry_.clear(telemetry::Source::afhds2a);
                    settings_.receiver = internal_.session().receiver();
                    save_radio();
                }
                if (output_ == RadioOutput::internal)
                    publish_telemetry(internal_.telemetry(), telemetry_);
            }
        }
        if constexpr (has_external_rf) {
            if (output_ == RadioOutput::external) {
                (void)ExternalRFModule::service(
                    Board::now_us(), diagnostics_.evaluation().frame.channels,
                    (state_ == ApplicationState::running) && diagnostics_.output_valid(),
                    sampled_at_, [&]() constexpr -> coretx::multiswitch::Pages * {
                        if constexpr (has_multiswitch)
                            return &crsf_.multiswitch.pages;
                        else
                            return nullptr;
                    }());
                publish_telemetry(ExternalRFModule::telemetry(), telemetry_, Board::now_us());
            }
        }
        telemetry_.service(Board::now_us());
        if (!Display::busy() &&
            ((overview_phase_ != 0) || ((now - drawn_at_) >= redraw_interval))) {
            render();
#ifdef CORETX_I6S_TOUCH_TRACE
            if (overview_phase_ == 0) {
                input_.render_trace(framebuffer_, trace_key_);
                framebuffer_.compact();
                framebuffer_.text(72, 56, "P");
                framebuffer_.unsigned_number(95, 56, static_cast<unsigned>(diagnostics_.phase()));
                framebuffer_.text(104, 56, "M");
                framebuffer_.unsigned_number(127, 56, menu_.visible());
                framebuffer_.compact(false);
                if (Memory::error() != I2cError::none) {
                    const auto storage = Memory::trace();
                    framebuffer_.rectangle(0, 48, 128, 16, false);
                    framebuffer_.compact();
                    framebuffer_.text(0, 48, "EE S");
                    framebuffer_.unsigned_number(27, 48, storage.stage);
                    framebuffer_.text(36, 48, "A");
                    framebuffer_.unsigned_number(79, 48, storage.address);
                    framebuffer_.text(88, 48, "P");
                    framebuffer_.unsigned_number(127, 48, storage.position);
                    framebuffer_.text(0, 56, "E");
                    framebuffer_.unsigned_number(31, 56, static_cast<unsigned>(Memory::error()));
                    framebuffer_.text(40, 56, "ISR");
                    framebuffer_.unsigned_number(127, 56, storage.status);
                    framebuffer_.compact(false);
                }
            }
#endif
            if (overview_phase_ == 0)
                drawn_at_ = Board::now_us();
        }
        if constexpr (has_power) {
            if (shutting_down() && (shutdown_error() == nullptr) && shutdown_ready()) {
                state_ = ApplicationState::powered_off;
                Inputs::stop();
                Board::release_power();
            }
        }
        return state_;
    }

    [[nodiscard]] constexpr ApplicationState state() const noexcept { return state_; }
    [[nodiscard]] constexpr ApplicationFailure failure() const noexcept { return failure_; }
    [[nodiscard]] constexpr const Diagnostics &diagnostics() const noexcept { return diagnostics_; }
    [[nodiscard]] constexpr RadioOutput output() const noexcept { return output_; }
    [[nodiscard]] constexpr const telemetry::Pool &telemetry() const noexcept { return telemetry_; }
    [[nodiscard]] constexpr const Menu &menu() const noexcept
        requires(has_menu)
    {
        return menu_;
    }
    [[nodiscard]] const CrsfMenu &crsf_menu() const noexcept
        requires(has_crsf_ui)
    {
        return crsf_.menu;
    }
    [[nodiscard]] constexpr const ModelStorage<Memory> &models() const noexcept
        requires(has_menu)
    {
        return models_;
    }

  private:
    [[nodiscard]] constexpr bool shutting_down() const noexcept {
        if constexpr (has_power)
            return power_.requested();
        return false;
    }
    [[nodiscard]] constexpr bool shutdown_ready() const noexcept {
        if (calibration_storage_.busy() || radio_storage_.busy() || input_storage_.busy() ||
            Memory::busy())
            return false;
        if constexpr (has_menu)
            return !models_.busy();
        return true;
    }
    [[nodiscard]] constexpr const char *shutdown_error() const noexcept {
        if (Memory::error() != I2cError::none)
            return error_message(Memory::error());
        if ((Probe::state() == StorageProbeState::not_found) ||
            (Probe::state() == StorageProbeState::failed))
            return coretx::ui::tr::models_unique_eeprom;
        const auto calibration = calibration_storage_.state();
        if ((calibration == CalibrationStorageState::multiple) ||
            (calibration == CalibrationStorageState::io_error) ||
            (calibration == CalibrationStorageState::invalid) ||
            (calibration == CalibrationStorageState::verify_error))
            return coretx::ui::tr::power_off_error;
        const auto failed = [](const RadioStorageStatus status) constexpr {
            return (status == RadioStorageStatus::missing) ||
                   (status == RadioStorageStatus::multiple) ||
                   (status == RadioStorageStatus::io_error) ||
                   (status == RadioStorageStatus::invalid) ||
                   (status == RadioStorageStatus::version) ||
                   (status == RadioStorageStatus::verify);
        };
        if (failed(input_storage_.status()))
            return input_storage_.message();
        if (failed(radio_storage_.status()))
            return coretx::ui::tr::power_off_error;
        if constexpr (has_menu)
            if (models_.loaded() && !models_.editable())
                return models_.message() != nullptr ? models_.message()
                                                    : coretx::ui::tr::power_off_error;
        return nullptr;
    }
#ifdef CORETX_I6S_TOUCH_TRACE
    Key trace_key_{};
#endif
    [[nodiscard]] constexpr SenderInputContext input_context() const noexcept {
        SenderInputContext context{};
        if constexpr (has_multiswitch) {
            if constexpr (has_menu)
                if (const auto slot = multiswitch_slot()) {
                    context.active = true;
                    context.momentary = diagnostics_.model().multiswitch[*slot].momentary;
                }
        }
        context.taps = !context.active && crsf_dashboard_page();
        if constexpr (requires { input_.rear_buttons(); }) {
            context.jog_enabled = diagnostics_.valid() && !input_menu_.visible() &&
                                  (diagnostics_.phase() == CalibrationPhase::idle);
            const auto &setup = diagnostics_.input_setup();
            if (is_pot(setup.modes[4]))
                context.vra = decode_pot(diagnostics_.samples()[6], setup.calibration[4], true);
            if (is_pot(setup.modes[5]))
                context.vrb = decode_pot(diagnostics_.samples()[7], setup.calibration[5], true);
            context.jog_enabled =
                context.jog_enabled && (is_pot(setup.modes[4]) || is_pot(setup.modes[5]));
        }
        return context;
    }
    [[nodiscard]] constexpr bool dashboard_touch() const noexcept {
        if constexpr (has_menu)
            if (menu_.visible())
                return false;
        if constexpr (has_crsf_ui)
            if (crsf_.menu.visible() && !crsf_dashboard_page())
                return false;
        return !input_menu_.visible() && (diagnostics_.phase() == CalibrationPhase::idle);
    }
    [[nodiscard]] constexpr bool crsf_dashboard_page() const noexcept {
        if constexpr (has_crsf_ui)
            return diagnostics_.model().crsf_dashboard && !menu_.visible() &&
                   !input_menu_.visible() && (diagnostics_.phase() == CalibrationPhase::idle) &&
                   (diagnostics_.page() ==
                    (Diagnostics::page_count +
                     (features::multiswitch ? coretx::multiswitch::count(diagnostics_.model())
                                            : 0)));
        return false;
    }
    constexpr void sync_crsf_dashboard(const TimestampUs now) noexcept {
        if constexpr (has_crsf_ui) {
            const bool active = crsf_dashboard_page() && (output_ == RadioOutput::external);
            if (active == crsf_.dashboard_active) {
                if (active && !crsf_.dashboard_interacting)
                    crsf_.menu.handle(Key::none, ExternalRFModule::config(), now);
                else if (!active && crsf_.dashboard_saved && !crsf_.menu.visible())
                    crsf_.menu.suspend(ExternalRFModule::config());
                return;
            }
            crsf_.dashboard_active = active;
            crsf_.dashboard_interacting = false;
            if (active) {
                if (crsf_.dashboard_saved)
                    crsf_.menu.resume(ExternalRFModule::config(), now);
                else
                    crsf_.menu.open(ExternalRFModule::config(), now);
            } else {
                crsf_.menu.suspend(ExternalRFModule::config());
                crsf_.dashboard_saved = true;
            }
        }
    }
    [[nodiscard]] constexpr std::optional<std::size_t> multiswitch_slot() const noexcept {
        if constexpr (has_multiswitch) {
            if (!menu_.visible() && !crsf_.menu.visible() && !input_menu_.visible() &&
                (diagnostics_.phase() == CalibrationPhase::idle) &&
                (diagnostics_.page() >= Diagnostics::page_count))
                return coretx::multiswitch::slot(diagnostics_.model(),
                                                 diagnostics_.page() - Diagnostics::page_count);
        }
        return std::nullopt;
    }
    [[nodiscard]] constexpr bool multiswitch_operable() const noexcept {
        if constexpr (has_multiswitch)
            return multiswitch_signal_operable() && models_.editable();
        return false;
    }
    [[nodiscard]] constexpr bool multiswitch_signal_operable() const noexcept {
        if constexpr (has_multiswitch)
            return (output_ == RadioOutput::external) && (state_ == ApplicationState::running) &&
                   diagnostics_.output_valid() && models_.loaded() &&
                   (input_.message() == nullptr) && (ExternalRFModule::error() == CrsfError::none);
        return false;
    }
    constexpr void apply_inputs() noexcept {
        const bool extended = features::extended_inputs && diagnostics_.input_setup().extra_analog;
        if (extended != adc_extended_) {
            if (Adc::set_extended(extended) != AdcError::none) {
                radio_message_ = error_message(Adc::error());
                state_ = ApplicationState::input_error;
            } else
                adc_extended_ = extended;
        }
        Inputs::configure(diagnostics_.digital_pin_mask());
        diagnostics_.invalidate();
    }
    constexpr void save_radio() noexcept {
        if constexpr (has_menu) {
            models_.data().radio = settings_;
            if (!models_.save())
                radio_message_ = models_.message();
        } else
            radio_storage_.save(settings_);
    }
    constexpr void select_output(const RadioOutput output, const TimestampUs now) noexcept {
        telemetry_.clear(telemetry::Source::afhds2a);
        telemetry_.clear(telemetry::Source::crsf);
        if constexpr (has_external_rf) {
            if constexpr (has_menu)
                crsf_.menu.close(ExternalRFModule::config());
            crsf_.dashboard_active = false;
            crsf_.dashboard_saved = false;
            crsf_.dashboard_interacting = false;
            ExternalRFModule::power_off();
        }
        if constexpr (has_internal_rf) {
            internal_.deactivate();
        }
        output_ = RadioOutput::off;
        radio_message_ = nullptr;
        if ((output != RadioOutput::off) &&
            (!input_applied_ || ((input_storage_.status() != RadioStorageStatus::ready) &&
                                 (input_storage_.status() != RadioStorageStatus::empty) &&
                                 (input_storage_.status() != RadioStorageStatus::saved) &&
                                 (input_storage_.status() != RadioStorageStatus::missing)))) {
            radio_message_ = input_storage_.message();
            return;
        }
        if ((output == RadioOutput::external) && !diagnostics_.input_setup().external_module) {
            radio_message_ = coretx::ui::tr::inputs_module_disabled;
            return;
        }
        if (((output == RadioOutput::internal) && !has_internal_rf) ||
            ((output == RadioOutput::external) && !has_external_rf)) {
            radio_message_ = coretx::ui::tr::radio_not_built;
            return;
        }
        output_ = output;
        if constexpr (has_external_rf) {
            if (output == RadioOutput::external)
                ExternalRFModule::initialize(now);
            if constexpr (has_menu) {
                if ((output == RadioOutput::external) && models_.loaded() &&
                    !ExternalRFModule::set_baud(models_.data().baud_index, now))
                    radio_message_ = coretx::ui::tr::bad_request;
            }
        }
        if constexpr (has_internal_rf) {
            if (output == RadioOutput::internal) {
                if (internal_.error() != Afhds2aError::none) {
                    internal_.initialize(settings_.transmitter, settings_.receiver, now);
                    internal_.set_channel_mode(settings_.channel_mode);
                }
                internal_.activate();
            }
        }
        (void)now;
    }
    constexpr void handle(const Key key, const TimestampUs now) noexcept {
        if (key != Key::none)
            overview_phase_ = 0;
        if constexpr (Input::touch) {
            if (dashboard_touch()) {
                const auto tap = input_.tapped();
                if (!tap && ((key == Key::ok) || (key == Key::cancel))) {
                    diagnostics_.handle(key == Key::cancel ? Key::down : Key::up);
                    return;
                }
                if constexpr (has_crsf_ui) {
                    if (crsf_dashboard_page() && (key != Key::ok_long)) {
                        if (output_ != RadioOutput::external) {
                            if (key != Key::none)
                                radio_message_ = coretx::ui::tr::crsf_select_external;
                        } else if (ExternalRFModule::error() != CrsfError::none) {
                            if (key != Key::none)
                                radio_message_ = coretx::ui::tr::crsf_link;
                        } else if (tap)
                            crsf_.menu.tap(tap->x, tap->y, ExternalRFModule::config(), now);
                        else
                            crsf_.menu.handle(key, ExternalRFModule::config(), now);
                        return;
                    }
                }
                if ((key == Key::up) || (key == Key::down))
                    return;
            }
        }
        if constexpr (has_external_rf) {
            if constexpr (has_menu) {
                if (crsf_dashboard_page() && !Input::touch) {
                    auto &client = ExternalRFModule::config();
                    if (crsf_.dashboard_interacting) {
                        const bool leave =
                            (key == Key::cancel_long) ||
                            (Input::touch && (key == Key::ok_long) && !crsf_.menu.editing());
                        if (leave && (client.command() == coretx::crsf::config::Command::idle)) {
                            if (!client.pending_write()) {
                                crsf_.menu.finish_edit(client);
                                crsf_.dashboard_interacting = false;
                            }
                        } else if ((key == Key::cancel) &&
                                   (client.mode() == coretx::crsf::config::Mode::devices) &&
                                   !crsf_.menu.editing()) {
                            crsf_.dashboard_interacting = false;
                        } else
                            crsf_.menu.handle(key, client, now);
                        return;
                    }
                    if (key == Key::ok) {
                        if (output_ != RadioOutput::external)
                            radio_message_ = coretx::ui::tr::crsf_select_external;
                        else if (ExternalRFModule::error() != CrsfError::none)
                            radio_message_ = coretx::ui::tr::crsf_link;
                        else
                            crsf_.dashboard_interacting = true;
                        return;
                    }
                } else if (crsf_.menu.visible() && !crsf_dashboard_page()) {
                    crsf_.menu.handle(key, ExternalRFModule::config(), now);
                    return;
                }
            }
        }
        if (input_menu_.visible()) {
            const auto command =
                input_menu_.handle(key, diagnostics_, output_, input_storage_.writable(),
                                   calibration_storage_.busy() || radio_storage_.owns_memory() ||
                                       input_storage_.owns_memory() || model_owns_memory(),
                                   input_storage_.message(), input_storage_.can_recover());
            if (command == InputMenuCommand::save) {
                if (!input_storage_.save(diagnostics_.input_setup()))
                    radio_message_ = input_storage_.message();
                apply_inputs();
            } else if (command == InputMenuCommand::recover) {
                if (input_storage_.recover()) {
                    input_applied_ = false;
                    radio_message_ = nullptr;
                } else
                    radio_message_ = input_storage_.message() != nullptr
                                         ? input_storage_.message()
                                         : coretx::ui::tr::bad_request;
            } else if (command == InputMenuCommand::calibrate) {
                diagnostics_.begin_calibration();
                if constexpr (has_menu)
                    menu_.handle(Key::cancel_long, models_, diagnostics_, output_);
            }
            return;
        }
        if constexpr (has_menu) {
            if constexpr (has_internal_rf) {
                if (((key == Key::cancel) || (key == Key::cancel_long)) &&
                    internal_.session().binding()) {
                    (void)internal_.bind(now);
                    telemetry_.clear(telemetry::Source::afhds2a);
                }
            }
            if constexpr (has_multiswitch) {
                if (const auto slot = multiswitch_slot(); slot && !Input::touch) {
                    if (crsf_.multiswitch.select) {
                        if ((key == Key::cancel) || (key == Key::cancel_long))
                            crsf_.multiswitch.select = false;
                        else if ((key == Key::up) || (key == Key::down))
                            crsf_.multiswitch.selected = static_cast<std::uint8_t>(
                                (crsf_.multiswitch.selected + (key == Key::down ? 1U : 7U)) % 8U);
                        else if ((key == Key::ok) && multiswitch_operable())
                            crsf_.multiswitch.tapped = crsf_.multiswitch.selected;
                        return;
                    }
                    if (key == Key::ok) {
                        if (multiswitch_operable()) {
                            crsf_.multiswitch.select = true;
                            crsf_.multiswitch.selected = 0;
                        } else
                            radio_message_ = coretx::ui::tr::crsf_select_external;
                        return;
                    }
                }
            }
            if ((key == Key::ok_long) && !menu_.visible() &&
                (diagnostics_.phase() == CalibrationPhase::idle)) {
                menu_.open();
                if constexpr (has_multiswitch) {
                    crsf_.multiswitch.select = false;
                }
                radio_message_ = nullptr;
                return;
            }
            if (menu_.visible()) {
                if ((key == Key::ok) &&
                    (calibration_storage_.busy() || radio_storage_.owns_memory() ||
                     input_storage_.owns_memory())) {
                    radio_message_ = coretx::ui::tr::saving;
                    return;
                }
                const auto command = menu_.handle(key, models_, diagnostics_, output_);
                if (command != MenuCommand::none)
                    radio_message_ = nullptr;
                switch (command) {
                case MenuCommand::output:
                    select_output(menu_.requested_output(), now);
                    settings_.output = output_;
                    save_radio();
                    break;
                case MenuCommand::format:
                    settings_.channel_mode = menu_.requested_format();
                    if constexpr (has_internal_rf) {
                        internal_.set_channel_mode(settings_.channel_mode);
                    }
                    save_radio();
                    break;
                case MenuCommand::bind:
                    if constexpr (has_internal_rf) {
                        radio_message_ = output_ != RadioOutput::internal
                                             ? coretx::ui::tr::radio_select_internal
                                         : !internal_.bind(now) ? coretx::ui::tr::not_initialized
                                                                : nullptr;
                        if (radio_message_ == nullptr) {
                            telemetry_.clear(telemetry::Source::afhds2a);
                        }
                    } else {
                        radio_message_ = coretx::ui::tr::radio_not_built;
                    }
                    break;
                case MenuCommand::baud:
                    models_.data().baud_index = menu_.requested_baud();
                    if constexpr (has_external_rf) {
                        if ((output_ == RadioOutput::external) &&
                            !ExternalRFModule::set_baud(models_.data().baud_index, now))
                            radio_message_ = coretx::ui::tr::bad_request;
                    }
                    (void)models_.save();
                    break;
                case MenuCommand::calibrate:
                    if (!calibration_storage_.busy() && !models_.busy())
                        diagnostics_.begin_calibration();
                    menu_.handle(Key::cancel_long, models_, diagnostics_, output_);
                    break;
                case MenuCommand::stick_mode:
                    calibration_storage_.save_setup(diagnostics_.setup());
                    break;
                case MenuCommand::input_setup:
                    input_menu_.open();
                    break;
                case MenuCommand::trim_beep: {
                    auto setup = diagnostics_.input_setup();
                    setup.trim_beep = !setup.trim_beep;
                    if (input_storage_.save(setup))
                        diagnostics_.set_trim_beep(setup.trim_beep);
                    else
                        radio_message_ = input_storage_.message() != nullptr
                                             ? input_storage_.message()
                                             : coretx::ui::tr::bad_request;
                    break;
                }
                case MenuCommand::external_module: {
                    auto setup = diagnostics_.input_setup();
                    setup.external_module = !setup.external_module;
                    if (diagnostics_.phase() != CalibrationPhase::idle)
                        radio_message_ = coretx::ui::tr::bad_request;
                    else if (!input_storage_.save(setup))
                        radio_message_ = input_storage_.message() != nullptr
                                             ? input_storage_.message()
                                             : coretx::ui::tr::bad_request;
                    else if (!diagnostics_.set_input_setup(setup))
                        radio_message_ = coretx::ui::tr::bad_request;
                    else
                        apply_inputs();
                    break;
                }
                case MenuCommand::crsf_config:
                    if constexpr (has_external_rf) {
                        if (output_ != RadioOutput::external)
                            radio_message_ = coretx::ui::tr::crsf_select_external;
                        else if (ExternalRFModule::error() != CrsfError::none)
                            radio_message_ = coretx::ui::tr::crsf_link;
                        else
                            crsf_.menu.open(ExternalRFModule::config(), now);
                    } else {
                        radio_message_ = coretx::ui::tr::radio_not_built;
                    }
                    break;
                case MenuCommand::initialize_models:
                    if (features::secure_ui && (output_ != RadioOutput::off))
                        radio_message_ = coretx::ui::tr::menu_rf_off;
                    else if (!models_.initialize_models(diagnostics_.model(), settings_))
                        radio_message_ = models_.message();
                    break;
                case MenuCommand::none:
                    break;
                }
                return;
            }
            if (key == Key::cancel_long) {
                calibration_storage_.handle(Key::cancel, diagnostics_);
                return;
            }
            if ((key == Key::ok) && models_.busy()) {
                radio_message_ = models_.message();
                return;
            }
        }
        if constexpr (has_internal_rf) {
            if ((key == Key::cancel) && internal_.session().binding()) {
                (void)internal_.bind(now);
                telemetry_.clear(telemetry::Source::afhds2a);
                return;
            }
        }
        if ((key == Key::ok) && (radio_storage_.owns_memory() || input_storage_.owns_memory()))
            return;
        const auto phase = diagnostics_.phase();
        if ((phase == CalibrationPhase::range) && (key == Key::ok))
            Watchdog::mark(1);
        calibration_storage_.handle(key, diagnostics_);
        if ((phase == CalibrationPhase::range) && (key == Key::ok)) {
            Watchdog::mark(2);
            if ((diagnostics_.phase() == CalibrationPhase::idle) && diagnostics_.calibrated() &&
                !input_storage_.save(diagnostics_.input_setup()))
                radio_message_ = input_storage_.message();
            Watchdog::mark(3);
        }
        (void)now;
    }
    constexpr void begin_menu_band() noexcept { framebuffer_.clip_page(overview_phase_); }
    constexpr void finish_menu_band() noexcept {
        framebuffer_.reset_clip();
        if (++overview_phase_ == 8) {
            overview_phase_ = 0;
            Display::begin_frame();
        }
    }
    constexpr void render() noexcept {
        if (shutting_down()) {
            framebuffer_.clear();
            framebuffer_.compact();
            framebuffer_.text(0, 0, coretx::ui::tr::power_off);
            const auto error = shutdown_error();
            framebuffer_.text(0, 20, error != nullptr ? error : coretx::ui::tr::saving);
            if ((error != nullptr) && (error != coretx::ui::tr::power_off_error))
                framebuffer_.text(0, 48, coretx::ui::tr::power_off_error);
            framebuffer_.compact(false);
            overview_phase_ = 0;
            Display::begin_frame();
            return;
        }
        const char *message = radio_message_;
        bool binding{};
        if constexpr (has_internal_rf) {
            binding = internal_.session().binding();
            if (const auto error = internal_.error(); error != Afhds2aError::none) {
                const auto bus_error = Radio::info().error;
                message = (error == Afhds2aError::bus) && (bus_error != A7105Error::none)
                              ? error_message(bus_error)
                              : error_message(error);
            }
        }
        if constexpr (has_external_rf) {
            if constexpr (has_menu) {
                if (crsf_.menu.visible() || crsf_dashboard_page()) {
                    begin_menu_band();
                    const bool dashboard = crsf_dashboard_page();
                    const char *const navigation_help =
                        !dashboard                     ? nullptr
                        : Input::touch                 ? coretx::ui::tr::crsf_dashboard_touch_help
                        : !crsf_.dashboard_interacting ? coretx::ui::tr::crsf_dashboard_help
                                                       : coretx::ui::tr::crsf_dashboard_active_help;
                    crsf_.menu.render(ExternalRFModule::config(), framebuffer_, Board::now_us(),
                                      dashboard && crsf_.dashboard_interacting, navigation_help,
                                      dashboard && Input::touch);
                    const auto input_error = input_.message();
                    const auto adc_error = error_message(Adc::error());
                    const auto notice = crsf_dashboard_page() && (output_ != RadioOutput::external)
                                            ? coretx::ui::tr::crsf_select_external
                                        : input_error != nullptr ? input_error
                                        : adc_error != nullptr   ? adc_error
                                                                 : radio_message_;
                    if (notice != nullptr) {
                        framebuffer_.rectangle(0, 58, 128, 6, false);
                        framebuffer_.small_text(0, 58, notice);
                    }
                    finish_menu_band();
                    return;
                }
            }
        }
        if (input_menu_.visible()) {
            begin_menu_band();
            input_menu_.render(diagnostics_, framebuffer_, input_storage_.message(),
                               input_storage_.can_recover());
            if (radio_message_ != nullptr) {
                framebuffer_.rectangle(0, 58, 128, 6, false);
                framebuffer_.small_text(0, 58, radio_message_);
            }
            finish_menu_band();
            return;
        }
        if constexpr (has_menu) {
            if (menu_.visible()) {
                begin_menu_band();
                menu_.render(models_, diagnostics_, output_, framebuffer_, binding);
                const auto input_error = input_.message();
                const auto adc_error = error_message(Adc::error());
                const auto notice = input_error != nullptr ? input_error
                                    : adc_error != nullptr ? adc_error
                                                           : message;
                if (notice != nullptr) {
                    framebuffer_.rectangle(0, 58, 128, 6, false);
                    framebuffer_.small_text(0, 58, notice);
                }
                finish_menu_band();
                return;
            }
        }
        const bool model_saving = [&] constexpr {
            if constexpr (has_menu)
                return models_.loaded() && models_.busy() && !models_.background_loading();
            else
                return false;
        }();
        const auto model_name = [&] constexpr -> std::string_view {
            if constexpr (has_menu)
                return models_.loaded() ? models_.data().name.data() : "--";
            else
                return "DIAG";
        }();
        const auto model_error = [&] constexpr -> const char * {
            if constexpr (has_menu)
                return !models_.loaded() || !models_.editable() ? models_.message() : nullptr;
            else
                return nullptr;
        }();
        if constexpr (has_multiswitch) {
            if constexpr (has_menu) {
                if (const auto slot = multiswitch_slot()) {
                    begin_menu_band();
                    framebuffer_.clear();
                    render_multiswitch(diagnostics_.model().multiswitch[*slot], *slot,
                                       crsf_.multiswitch.pages.switches(*slot),
                                       crsf_.multiswitch.select
                                           ? std::optional<std::size_t>{crsf_.multiswitch.selected}
                                           : std::nullopt,
                                       framebuffer_, Input::touch && multiswitch_operable());
                    framebuffer_.small_text(0, 52, model_name);
                    const auto notice =
                        input_.message() != nullptr              ? input_.message()
                        : error_message(Adc::error()) != nullptr ? error_message(Adc::error())
                        : model_error != nullptr                 ? model_error
                        : (output_ == RadioOutput::external) &&
                                (ExternalRFModule::error() != CrsfError::none)
                            ? coretx::ui::tr::crsf_link
                        : message != nullptr           ? message
                        : !diagnostics_.calibrated()   ? coretx::ui::tr::need_cal
                        : !diagnostics_.valid()        ? coretx::ui::tr::wait_for_adc
                        : !diagnostics_.output_valid() ? coretx::ui::tr::control_error
                        : !multiswitch_operable()      ? coretx::ui::tr::crsf_select_external
                        : Input::touch                 ? coretx::ui::tr::multiswitch_touch_help
                        : crsf_.multiswitch.select && diagnostics_.model()
                                                          .multiswitch[*slot]
                                                          .controls[crsf_.multiswitch.selected]
                                                          .condition()
                                                          .enabled
                            ? coretx::ui::tr::multiswitch_bound_help
                        : crsf_.multiswitch.select ? coretx::ui::tr::multiswitch_select_help
                                                   : coretx::ui::tr::multiswitch_help;
                    framebuffer_.small_text(0, 58, notice);
                    finish_menu_band();
                    return;
                }
            }
        }
        if ((diagnostics_.page() == Diagnostics::overview_page) &&
            (diagnostics_.phase() == CalibrationPhase::idle) &&
            (error_message(Adc::error()) == nullptr) && (input_.message() == nullptr) &&
            (model_error == nullptr) && diagnostics_.calibrated() && diagnostics_.output_valid()) {
            if constexpr (has_menu) {
                if ((overview_phase_ != 0) && (overview_model_ != models_.active()))
                    overview_phase_ = 0;
                overview_model_ = static_cast<std::uint8_t>(models_.active());
            }
            if (overview_phase_ == 0) {
                framebuffer_.clear();
                render_overview(diagnostics_, framebuffer_, telemetry_, output_, Board::now_us(),
                                model_name, nullptr, OverviewSection::header);
                overview_phase_ = 1;
                return;
            }
            if (overview_phase_ <= overview_channel_count) {
                render_overview(diagnostics_, framebuffer_, telemetry_, output_, Board::now_us(),
                                model_name, nullptr, OverviewSection::channel,
                                overview_phase_ - 1U);
                ++overview_phase_;
                return;
            }
            render_overview(diagnostics_, framebuffer_, telemetry_, output_, Board::now_us(),
                            model_name, nullptr, OverviewSection::footer);
            overview_phase_ = 0;
            if (input_storage_.message() != nullptr) {
                framebuffer_.rectangle(0, 58, 128, 6, false);
                framebuffer_.small_text(0, 58, input_storage_.message());
            }
            Display::begin_frame();
            return;
        }
        overview_phase_ = 0;
        render_dashboard(
            diagnostics_, error_message(Adc::error()), framebuffer_,
            {Probe::state(), Probe::error(), Probe::acknowledged(),
             radio_storage_.owns_memory() || model_saving
                 ? coretx::ui::tr::saving
                 : calibration_storage_.label(diagnostics_.calibrated()),
             Watchdog::resets(), input_storage_.message(), Board::name, Watchdog::last_fault()},
            telemetry_, output_, input_.message(), Board::now_us(), model_name, model_error);
        if ((diagnostics_.page() != Diagnostics::system_page) &&
            (input_storage_.message() != nullptr)) {
            framebuffer_.rectangle(0, 58, 128, 6, false);
            framebuffer_.small_text(0, 58, input_storage_.message());
        }
        Display::begin_frame();
    }
    constexpr ApplicationState fail(const ApplicationFailure failure) noexcept {
        Board::power_off();
        Inputs::stop();
        failure_ = failure;
        state_ = ApplicationState::fatal;
        return state_;
    }

    inline static constexpr auto redraw_interval = 20'000_us;
    // Return to sampling and RF publication between overview sections and menu bands.
    std::uint8_t overview_phase_{};
    std::uint8_t overview_model_{};
    Diagnostics diagnostics_{Board::sb_three_position};
    [[no_unique_address]] Input input_{};
    [[no_unique_address]] std::conditional_t<Input::touch, NoMenu, TrimKeys> trim_keys_{};
    CalibrationStorage<Memory> calibration_storage_{};
    RadioStorage<Memory> radio_storage_{};
    InputStorage<Memory> input_storage_{Board::name == "I6S"};
    InputMenu input_menu_{};
    [[no_unique_address]] std::conditional_t<has_menu, ModelStorage<Memory>, NoMenu> models_{};
    [[no_unique_address]] std::conditional_t<has_menu, Menu, NoMenu> menu_{};
    [[no_unique_address]]
    std::conditional_t<has_external_rf, application_detail::CrsfUiState<has_menu, has_multiswitch>,
                       DisabledFeature> crsf_{};
    RadioSettings settings_{};
    telemetry::Pool telemetry_{};
    [[no_unique_address]]
    std::conditional_t<has_internal_rf, InternalRFModule, DisabledFeature> internal_{};
    coretx::ui::Framebuffer framebuffer_{};
    TimestampUs drawn_at_{};
    TimestampUs sampled_at_{};
    ApplicationState state_{ApplicationState::fatal};
    ApplicationFailure failure_{ApplicationFailure::not_initialized};
    RadioOutput output_{};
    const char *radio_message_{};
    bool settings_applied_{};
    bool display_ready_{};
    bool model_applied_{};
    bool input_applied_{};
    bool adc_extended_{};
    [[no_unique_address]]
    std::conditional_t<has_power, i6s::PowerButton, DisabledFeature> power_{};
    [[nodiscard]] constexpr bool model_owns_memory() const noexcept {
        if constexpr (has_menu)
            return models_.owns_memory();
        else
            return false;
    }
};

} // namespace coretx::firmware
