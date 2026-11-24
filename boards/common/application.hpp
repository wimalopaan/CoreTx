// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/common/crsf_ui.hpp"
#include "boards/common/dashboard.hpp"
#include "boards/common/input_menu.hpp"
#include "boards/common/menu.hpp"
#include "boards/common/radio_errors.hpp"
#include "boards/common/sender_input.hpp"
#include "boards/common/storage_set.hpp"
#include "boards/common/telemetry_adapters.hpp"
#include "platform/stm32f072/buzzer.hpp"
#include "platform/stm32f072/crsf.hpp"
#include "platform/stm32f072/lcd.hpp"

#include <type_traits>

namespace coretx::firmware {

enum class ApplicationState : std::uint8_t { starting, running, input_error, fatal, powered_off };
enum class ApplicationFailure : std::uint8_t { none, not_initialized, hardware, lcd, stack };

namespace application_detail {
template <typename Board> struct BoardPower {
    using Type = void;
};
template <typename Board>
    requires requires { typename Board::Power; }
struct BoardPower<Board> {
    using Type = typename Board::Power;
};
} // namespace application_detail

template <typename Board, typename Input = PhysicalInput<typename Board::Keys>> class Application {
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
    inline static constexpr bool has_crsf_ui = has_external_rf;
    inline static constexpr bool has_multiswitch = has_crsf_ui && features::multiswitch;
    using Watchdog = typename Board::Watchdog;
    using Power = typename application_detail::BoardPower<Board>::Type;
    inline static constexpr bool has_power = !std::is_void_v<Power> && requires {
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
        storage_.initialize(Board::transmitter_id());
        model_applied_ = false;
        recovery_ = Recovery::none;
        recovery_selected_ = recovery_drawn_ = false;
        menu_ = {};
        if constexpr (has_external_rf)
            crsf_.reset();
        framebuffer_.compact(false);

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
        if constexpr (has_power)
            service_power_button(now);
        if (Display::service(framebuffer_) != LcdError::none) {
            return fail(ApplicationFailure::lcd);
        }
        if (!Board::stack_intact()) {
            return fail(ApplicationFailure::stack);
        }
        service_storage(now);
        if (storage_.models.recovery_required() &&
            ((recovery_ == Recovery::none) || (recovery_ == Recovery::resetting))) {
            select_output(RadioOutput::off, now);
            Board::power_off();
            recovery_ = Recovery::choice;
            recovery_selected_ = false;
            drawn_at_ = now - redraw_interval;
        } else if ((recovery_ == Recovery::resetting) && storage_.models.editable())
            recovery_ = Recovery::none;
        diagnostics_.allow_logical_commit(!menu_.pending_model_edit());

        // Loading input settings can restart the ADC with a newer initialization timestamp.
        now = Board::now_us();
        sample_inputs(now);
        service_keys(now);
        if constexpr (has_external_rf) {
            sync_crsf_dashboard(now);
            if constexpr (has_multiswitch)
                service_multiswitch(now);
        }
        service_rf();
        telemetry_.service(Board::now_us());
        service_display(now);
        if ((recovery_ == Recovery::power_off) && recovery_drawn_ && !Display::busy()) {
            Inputs::stop();
            if constexpr (has_power)
                Board::release_power();
            state_ = ApplicationState::powered_off;
        }
        if constexpr (has_power)
            finish_shutdown(now);
        return state_;
    }

    [[nodiscard]] constexpr ApplicationState state() const noexcept { return state_; }
    [[nodiscard]] constexpr ApplicationFailure failure() const noexcept { return failure_; }
    [[nodiscard]] constexpr const Diagnostics &diagnostics() const noexcept { return diagnostics_; }
    [[nodiscard]] constexpr RadioOutput output() const noexcept { return output_; }
    [[nodiscard]] constexpr bool storage_pending() const noexcept {
        return !storage_.shutdown_ready() || menu_.editing() || input_menu_.editing() ||
               (diagnostics_.phase() != CalibrationPhase::idle);
    }
    [[nodiscard]] constexpr const telemetry::Pool &telemetry() const noexcept { return telemetry_; }
    [[nodiscard]] constexpr const Menu &menu() const noexcept { return menu_; }
    [[nodiscard]] const CrsfMenu &crsf_menu() const noexcept
        requires(has_crsf_ui)
    {
        return crsf_.menu;
    }
    [[nodiscard]] constexpr const ModelStorage<Memory> &models() const noexcept {
        return storage_.models;
    }

  private:
    // A held power button closes all menus, switches RF off and starts the shutdown screen.
    constexpr void service_power_button(const TimestampUs now) noexcept {
        if (power_.update(now, Board::power_button_pressed())) {
            if (storage_.models.recovery_required()) {
                recovery_ = Recovery::power_off;
                drawn_at_ = now - redraw_interval;
                return;
            }
            select_output(RadioOutput::off, now);
            Board::power_off();
            Inputs::stop();
            diagnostics_.defer_refresh();
            (void)menu_.handle(Key::cancel_long, storage_.models, diagnostics_, output_);
            diagnostics_.handle(Key::cancel);
            diagnostics_.flush_refresh();
            overview_phase_ = 0;
            drawn_at_ = now - redraw_interval;
        }
    }
    // Loads stored records in order (calibration, inputs, radio, models) and applies them.
    constexpr void service_storage(const TimestampUs now) noexcept {
        storage_.service_inputs(now, diagnostics_);
        if (!input_applied_ && storage_.input.loaded() &&
            (diagnostics_.phase() == CalibrationPhase::idle)) {
            overview_phase_ = 0;
            if (!diagnostics_.set_input_setup(
                    storage_.input.setup(),
                    (storage_.input.status() == RadioStorageStatus::ready) ||
                        (storage_.input.status() == RadioStorageStatus::saved))) {
                radio_message_ = coretx::ui::tr::inputs_invalid;
            } else
                apply_inputs();
            input_applied_ = true;
        }
        if (input_applied_)
            storage_.service_radio(now);
        if (!settings_applied_ && storage_.radio.loaded()) {
            settings_ = storage_.radio.settings();
            settings_applied_ = true;
            if constexpr (has_internal_rf) {
                internal_.initialize(settings_.transmitter, settings_.receiver, now);
                internal_.set_channel_mode(settings_.channel_mode);
            }
            select_output(RadioOutput::off, now);
        }
        // Live field previews must not be journaled by an asynchronous receiver update.
        if (settings_applied_ && !menu_.pending_model_edit())
            storage_.service_models(now, storage_.models.data().model, settings_);
        if (storage_.models.take_changed()) {
            if constexpr (!Input::touch)
                trim_keys_.reset();
            overview_phase_ = 0;
            menu_.model_changed(storage_.models.active());
            diagnostics_.use_model(storage_.models.data().model);
            if constexpr (has_external_rf)
                crsf_.reset_multiswitch();
            settings_ = storage_.models.data().radio;
            if constexpr (has_internal_rf) {
                internal_.deactivate();
                internal_.initialize(settings_.transmitter, settings_.receiver, Board::now_us());
                internal_.set_channel_mode(settings_.channel_mode);
            }
            const auto selected_output =
                model_applied_ || shutting_down() ? RadioOutput::off : settings_.output;
            model_applied_ = true;
            select_output(selected_output, Board::now_us());
        }
    }
    constexpr void sample_inputs(const TimestampUs now) noexcept {
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
    }
    constexpr void service_keys(const TimestampUs now) noexcept {
        display_ready_ = display_ready_ || !Display::busy();
        if constexpr (has_external_rf) {
            if constexpr (has_multiswitch) {
                if (!multiswitch_slot())
                    crsf_.multiswitch.select = false;
            }
            diagnostics_.constrain_page();
            sync_crsf_dashboard(now);
        }
        const auto key =
            input_.service(now, !Input::needs_display || display_ready_, input_context());
        if constexpr (Input::touch) {
            // Audible confirmation that a touch stroke was recognized.
            if ((key != Key::none) && diagnostics_.input_setup().trim_beep && !shutting_down())
                Inputs::service(now, true);
        }
#ifdef CORETX_I6S_TOUCH_TRACE
        if (key != Key::none)
            trace_key_ = key;
#endif
        // Keep RC evaluation out of nested UI handlers on the 2 KiB stack.
        diagnostics_.defer_refresh();
        if (!shutting_down()) {
            if (recovery_ != Recovery::none)
                handle_recovery(key, now);
            else if (storage_.models.loaded())
                handle(key, now);
        }
        if constexpr (requires { input_.rear_buttons(); }) {
            const bool setup =
                ((recovery_ != Recovery::none) || !dashboard_touch()) && !shutting_down();
            diagnostics_.observe_setup_controls(setup, input_.rear_buttons());
            input_.set_setup_mode(now, setup);
        }
        diagnostics_.flush_refresh();
        if constexpr (!Input::touch) {
            const auto buttons = input_.trim_buttons();
            if (shutting_down() || (diagnostics_.phase() != CalibrationPhase::idle))
                trim_keys_.reset();
            else if (storage_.models.editable() &&
                     trim_keys_.update(storage_.models.data().model, diagnostics_.setup(), buttons,
                                       now)) {
                diagnostics_.refresh_model();
                Inputs::service(now, diagnostics_.input_setup().trim_beep);
                if (!storage_.models.save_trims(now))
                    radio_message_ = storage_.models.message();
            }
            diagnostics_.observe_trims(buttons);
        }
    }
    // Publishes the multiswitch pages through the CRSF module and plays the toggle click.
    constexpr void service_multiswitch(const TimestampUs now) noexcept {
        crsf_.multiswitch.pages.configure(diagnostics_.model());
        const auto slot = multiswitch_operable() ? multiswitch_slot() : std::nullopt;
        const auto held = Input::touch       ? input_.held_switch()
                          : input_.ok_held() ? crsf_.multiswitch.selection()
                                             : std::nullopt;
        const auto tapped = Input::touch ? input_.tapped_switch() : crsf_.multiswitch.tapped;
        const auto *const controls =
            multiswitch_signal_operable() ? &diagnostics_.prepared().controls : nullptr;
        if (crsf_.multiswitch.pages.update(slot, held, tapped, controls,
                                           diagnostics_.evaluation().frame.logical) &&
            diagnostics_.model().multiswitch_beep && !shutting_down())
            Inputs::service(now, true, ClickTone::multiswitch);
        crsf_.multiswitch.tapped.reset();
    }
    constexpr void service_rf() noexcept {
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
    }
    constexpr void service_display(const TimestampUs now) noexcept {
        if (!Display::busy() &&
            ((overview_phase_ != 0) || ((now - drawn_at_) >= redraw_interval))) {
            render();
#ifdef CORETX_I6S_TOUCH_TRACE
            if ((overview_phase_ == 0) && (recovery_ == Recovery::none))
                input_.template render_trace<Memory>(framebuffer_, trace_key_,
                                                     static_cast<unsigned>(diagnostics_.phase()),
                                                     menu_.visible());
#endif
            if (overview_phase_ == 0)
                drawn_at_ = Board::now_us();
        }
    }
    // Releases the supply latch once the farewell has played and all records are saved.
    constexpr void finish_shutdown(const TimestampUs now) noexcept {
        if (const auto tone = power_.next_tone(now); tone.hz != 0)
            Inputs::play(now, tone.hz, tone.duration);
        if (shutting_down() && (shutdown_error() == nullptr) && shutdown_ready() &&
            power_.animation_done(now)) {
            state_ = ApplicationState::powered_off;
            Inputs::stop();
            Board::release_power();
        }
    }
    [[nodiscard]] constexpr bool shutting_down() const noexcept {
        if constexpr (has_power)
            return power_.requested() && (recovery_ == Recovery::none);
        return false;
    }
    [[nodiscard]] constexpr bool shutdown_ready() const noexcept {
        return storage_.shutdown_ready();
    }
    [[nodiscard]] constexpr const char *shutdown_error() const noexcept {
        return storage_.shutdown_error();
    }
#ifdef CORETX_I6S_TOUCH_TRACE
    Key trace_key_{};
#endif
    [[nodiscard]] constexpr SenderInputContext input_context() const noexcept {
        SenderInputContext context{};
        if (recovery_ != Recovery::none) {
            context.taps = true;
            return context;
        }
        if constexpr (has_multiswitch) {
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
        if constexpr (has_crsf_ui)
            crsf_.sync_dashboard(crsf_dashboard_page() && (output_ == RadioOutput::external), now);
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
            return multiswitch_signal_operable() && storage_.models.editable();
        return false;
    }
    [[nodiscard]] constexpr bool multiswitch_signal_operable() const noexcept {
        if constexpr (has_multiswitch)
            return (output_ == RadioOutput::external) && (state_ == ApplicationState::running) &&
                   diagnostics_.output_valid() && storage_.models.loaded() &&
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
        storage_.models.data().radio = settings_;
        if (!storage_.models.save())
            radio_message_ = storage_.models.message();
    }
    constexpr void select_output(const RadioOutput output, const TimestampUs now) noexcept {
        telemetry_.clear(telemetry::Source::afhds2a);
        telemetry_.clear(telemetry::Source::crsf);
        if constexpr (has_external_rf) {
            crsf_.close();
            ExternalRFModule::power_off();
        }
        if constexpr (has_internal_rf) {
            internal_.deactivate();
        }
        output_ = RadioOutput::off;
        radio_message_ = nullptr;
        if ((output != RadioOutput::off) && !storage_.models.editable()) {
            radio_message_ = storage_.models.message();
            return;
        }
        if ((output != RadioOutput::off) &&
            (!input_applied_ || ((storage_.input.status() != RadioStorageStatus::ready) &&
                                 (storage_.input.status() != RadioStorageStatus::empty) &&
                                 (storage_.input.status() != RadioStorageStatus::saved) &&
                                 (storage_.input.status() != RadioStorageStatus::missing)))) {
            radio_message_ = storage_.input.message();
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
            if ((output == RadioOutput::external) && storage_.models.loaded() &&
                !ExternalRFModule::set_baud(storage_.models.data().baud_index, now))
                radio_message_ = coretx::ui::tr::bad_request;
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
            if (handle_touch_dashboard(key, now))
                return;
        }
        if constexpr (has_external_rf) {
            if (crsf_dashboard_page() && !Input::touch) {
                if (crsf_.handle_dashboard(key, now, output_ == RadioOutput::external,
                                           radio_message_))
                    return;
            } else if (crsf_.menu.visible() && !crsf_dashboard_page()) {
                crsf_.menu.handle(key, ExternalRFModule::config(), now);
                return;
            }
        }
        if (input_menu_.visible()) {
            handle_input_menu(key);
            return;
        }
        if constexpr (has_internal_rf) {
            if (((key == Key::cancel) || (key == Key::cancel_long)) &&
                internal_.session().binding()) {
                (void)internal_.bind(now);
                telemetry_.clear(telemetry::Source::afhds2a);
            }
        }
        if constexpr (has_multiswitch) {
            if (multiswitch_slot() && !Input::touch &&
                crsf_.multiswitch.handle(key, multiswitch_operable(), radio_message_))
                return;
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
            handle_menu(key, now);
            return;
        }
        handle_calibration(key, now);
    }
    // Touch on the dashboard pages: plain taps on the ok/cancel areas flip pages, the CRSF
    // dashboard receives everything else.
    [[nodiscard]] constexpr bool handle_touch_dashboard(const Key key,
                                                        const TimestampUs now) noexcept {
        if (!dashboard_touch())
            return false;
        const auto tap = input_.tapped();
        if (!tap && ((key == Key::ok) || (key == Key::cancel))) {
            diagnostics_.handle(key == Key::cancel ? Key::down : Key::up);
            return true;
        }
        if constexpr (has_crsf_ui) {
            if (crsf_dashboard_page() && (key != Key::ok_long)) {
                if (const auto error = crsf_.unavailable(output_ == RadioOutput::external)) {
                    if (key != Key::none)
                        radio_message_ = error;
                } else if (tap)
                    crsf_.menu.tap(tap->x, tap->y, ExternalRFModule::config(), now);
                else
                    crsf_.menu.handle(key, ExternalRFModule::config(), now);
                return true;
            }
        }
        return (key == Key::up) || (key == Key::down);
    }
    constexpr void handle_input_menu(const Key key) noexcept {
        const auto command = input_menu_.handle(
            key, diagnostics_, output_, storage_.input.writable(), storage_.memory_busy(),
            storage_.input.message(), storage_.input.can_recover());
        if (command == InputMenuCommand::save) {
            if (!storage_.input.save(diagnostics_.input_setup()))
                radio_message_ = storage_.input.message();
            apply_inputs();
        } else if (command == InputMenuCommand::recover) {
            if (storage_.input.recover()) {
                input_applied_ = false;
                radio_message_ = nullptr;
            } else
                radio_message_ = storage_.input.message() != nullptr ? storage_.input.message()
                                                                     : coretx::ui::tr::bad_request;
        } else if (command == InputMenuCommand::calibrate) {
            diagnostics_.begin_calibration();
            menu_.handle(Key::cancel_long, storage_.models, diagnostics_, output_);
        }
    }
    constexpr void handle_menu(const Key key, const TimestampUs now) noexcept {
        if ((key == Key::ok) && storage_.settings_busy()) {
            radio_message_ = coretx::ui::tr::saving;
            return;
        }
        const auto command = menu_.handle(key, storage_.models, diagnostics_, output_);
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
            storage_.models.data().baud_index = menu_.requested_baud();
            if constexpr (has_external_rf) {
                if ((output_ == RadioOutput::external) &&
                    !ExternalRFModule::set_baud(storage_.models.data().baud_index, now))
                    radio_message_ = coretx::ui::tr::bad_request;
            }
            (void)storage_.models.save();
            break;
        case MenuCommand::calibrate:
            if (!storage_.calibration.busy() && !storage_.models.busy())
                diagnostics_.begin_calibration();
            menu_.handle(Key::cancel_long, storage_.models, diagnostics_, output_);
            break;
        case MenuCommand::stick_mode:
            storage_.calibration.save_setup(diagnostics_);
            break;
        case MenuCommand::input_setup:
            input_menu_.open();
            break;
        case MenuCommand::trim_beep: {
            auto setup = diagnostics_.input_setup();
            setup.trim_beep = !setup.trim_beep;
            if (storage_.input.save(setup))
                diagnostics_.set_trim_beep(setup.trim_beep);
            else
                radio_message_ = storage_.input.message() != nullptr ? storage_.input.message()
                                                                     : coretx::ui::tr::bad_request;
            break;
        }
        case MenuCommand::external_module: {
            auto setup = diagnostics_.input_setup();
            setup.external_module = !setup.external_module;
            if (diagnostics_.phase() != CalibrationPhase::idle)
                radio_message_ = coretx::ui::tr::bad_request;
            else if (!storage_.input.save(setup))
                radio_message_ = storage_.input.message() != nullptr ? storage_.input.message()
                                                                     : coretx::ui::tr::bad_request;
            else if (!diagnostics_.set_input_setup(setup))
                radio_message_ = coretx::ui::tr::bad_request;
            else
                apply_inputs();
            break;
        }
        case MenuCommand::crsf_config:
            if constexpr (has_external_rf) {
                radio_message_ = crsf_.unavailable(output_ == RadioOutput::external);
                if (radio_message_ == nullptr)
                    crsf_.menu.open(ExternalRFModule::config(), now);
            } else {
                radio_message_ = coretx::ui::tr::radio_not_built;
            }
            break;
        case MenuCommand::initialize_models:
            if (features::secure_ui && (output_ != RadioOutput::off))
                radio_message_ = coretx::ui::tr::menu_rf_off;
            else if (!storage_.models.initialize_models())
                radio_message_ = storage_.models.message();
            break;
        case MenuCommand::none:
            break;
        }
    }
    // Without an open menu, keys drive the calibration on the dashboard pages.
    constexpr void handle_calibration(const Key key, const TimestampUs now) noexcept {
        if (key == Key::cancel_long) {
            storage_.calibration.handle(Key::cancel, diagnostics_);
            return;
        }
        if ((key == Key::ok) && storage_.models.busy()) {
            radio_message_ = storage_.models.message();
            return;
        }

        if constexpr (has_internal_rf) {
            if ((key == Key::cancel) && internal_.session().binding()) {
                (void)internal_.bind(now);
                telemetry_.clear(telemetry::Source::afhds2a);
                return;
            }
        }
        if ((key == Key::ok) && (storage_.radio.owns_memory() || storage_.input.owns_memory()))
            return;
        const auto phase = diagnostics_.phase();
        if ((phase == CalibrationPhase::range) && (key == Key::ok))
            Watchdog::mark(1);
        storage_.calibration.handle(key, diagnostics_);
        if ((phase == CalibrationPhase::range) && (key == Key::ok)) {
            Watchdog::mark(2);
            if ((diagnostics_.phase() == CalibrationPhase::idle) && diagnostics_.calibrated() &&
                !storage_.input.save(diagnostics_.input_setup()))
                radio_message_ = storage_.input.message();
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
    struct ModelView {
        std::string_view name;
        const char *error;
        bool saving;
    };
    [[nodiscard]] constexpr ModelView model_view() const noexcept {
        const auto &models = storage_.models;
        return {models.loaded() ? std::string_view{models.data().name.data()} : "--",
                !models.loaded() || !models.editable() ? models.message() : nullptr,
                models.loaded() && models.busy() && !models.background_loading()};
    }
    // Touch or ADC failures take precedence over all other notices.
    [[nodiscard]] constexpr const char *input_notice() const noexcept {
        if (const auto error = input_.message())
            return error;
        return error_message(Adc::error());
    }
    // The pending radio message, replaced by an internal RF module failure.
    [[nodiscard]] constexpr const char *radio_notice() const noexcept {
        if constexpr (has_internal_rf) {
            if (const auto error = internal_.error(); error != Afhds2aError::none) {
                const auto bus_error = Radio::info().error;
                return (error == Afhds2aError::bus) && (bus_error != A7105Error::none)
                           ? error_message(bus_error)
                           : error_message(error);
            }
        }
        return radio_message_;
    }

    constexpr void render() noexcept {
        if (shutting_down()) {
            render_shutdown();
            overview_phase_ = 0;
            Display::begin_frame();
            return;
        }
        if (recovery_ != Recovery::none) {
            render_recovery();
            overview_phase_ = 0;
            Display::begin_frame();
            return;
        }
        if constexpr (has_external_rf) {
            if (crsf_.menu.visible() || crsf_dashboard_page()) {
                begin_menu_band();
                render_crsf();
                finish_menu_band();
                return;
            }
        }
        if (input_menu_.visible()) {
            begin_menu_band();
            input_menu_.render(diagnostics_, framebuffer_, storage_.input.message(),
                               storage_.input.can_recover());
            if (radio_message_ != nullptr)
                draw_notice(radio_message_);
            finish_menu_band();
            return;
        }
        if (menu_.visible()) {
            begin_menu_band();
            bool binding{};
            if constexpr (has_internal_rf)
                binding = internal_.session().binding();
            menu_.render(storage_.models, diagnostics_, output_, framebuffer_, binding);
            const auto input_error = input_notice();
            if (const auto notice = input_error != nullptr ? input_error : radio_notice())
                draw_notice(notice);
            finish_menu_band();
            return;
        }
        const auto model = model_view();
        if constexpr (has_multiswitch) {
            if (const auto slot = multiswitch_slot()) {
                begin_menu_band();
                render_multiswitch_page(*slot, model);
                finish_menu_band();
                return;
            }
        }
        if ((diagnostics_.page() == Diagnostics::overview_page) &&
            (diagnostics_.phase() == CalibrationPhase::idle) && (input_notice() == nullptr) &&
            (model.error == nullptr) && diagnostics_.calibrated() && diagnostics_.output_valid()) {
            render_overview_page(model.name);
            return;
        }
        overview_phase_ = 0;
        render_dashboard_page(model);
        Display::begin_frame();
    }
    enum class Recovery : std::uint8_t { none, choice, confirm, resetting, power_off };
    constexpr void handle_recovery(Key key, const TimestampUs now) noexcept {
        if ((recovery_ == Recovery::resetting) || (recovery_ == Recovery::power_off))
            return;
        if constexpr (Input::touch) {
            if (const auto point = input_.tapped(); point && (point->y >= 30) && (point->y < 54)) {
                recovery_selected_ = point->y >= 42;
                key = Key::ok;
            }
        }
        if ((key == Key::up) || (key == Key::down))
            recovery_selected_ = key == Key::down;
        else if ((key == Key::cancel) || (key == Key::cancel_long)) {
            recovery_ = Recovery::choice;
            recovery_selected_ = false;
        } else if (key == Key::ok) {
            if (recovery_ == Recovery::choice) {
                recovery_ = recovery_selected_ ? Recovery::confirm : Recovery::power_off;
                recovery_selected_ = false;
            } else if (!recovery_selected_)
                recovery_ = Recovery::choice;
            else if (storage_.models.initialize_models()) {
                recovery_ = Recovery::resetting;
                recovery_selected_ = false;
            }
        }
        if (key != Key::none)
            drawn_at_ = now - redraw_interval;
    }
    constexpr void render_recovery() noexcept {
        namespace tr = coretx::ui::tr;
        framebuffer_.clear();
        framebuffer_.compact();
        framebuffer_.text(0, 0, tr::models_recovery);
        if (recovery_ == Recovery::power_off) {
            framebuffer_.text(0, 20, tr::models_recovery_switch_off);
            framebuffer_.text(0, 40, tr::models_recovery_converter);
            recovery_drawn_ = true;
        } else if (recovery_ == Recovery::resetting)
            framebuffer_.text(0, 24, storage_.models.message());
        else {
            const bool confirm = recovery_ == Recovery::confirm;
            framebuffer_.text(0, 10,
                              confirm ? tr::models_recovery_confirm : storage_.models.message());
            if (const auto version = storage_.models.detected_version(); !confirm && version) {
                const std::uint32_t expected = storage_.models.expected_version();
                const auto minimum = expected == models::format_version
                                         ? coretx::storage::oldest_format_version
                                         : expected;
                framebuffer_.text(0, 20, "Format:");
                framebuffer_.unsigned_number(47, 20, *version);
                framebuffer_.text(56, 20, "/");
                framebuffer_.unsigned_number(75, 20, minimum);
                framebuffer_.text(80, 20, "-");
                framebuffer_.unsigned_number(99, 20, expected);
            }
            framebuffer_.text(0, recovery_selected_ ? 44 : 32, ">");
            framebuffer_.text(8, 32,
                              confirm ? tr::models_recovery_cancel : tr::models_recovery_off);
            framebuffer_.text(8, 44,
                              confirm ? tr::models_recovery_delete : tr::models_recovery_reset);
        }
        framebuffer_.compact(false);
    }
    constexpr void render_shutdown() noexcept {
        framebuffer_.clear();
        const auto error = shutdown_error();
        if constexpr (has_power) {
            if ((error == nullptr) && !power_.animation_done(Board::now_us())) {
                power_.render(framebuffer_, Board::now_us());
                return;
            }
        }
        framebuffer_.compact();
        framebuffer_.text(0, 0, coretx::ui::tr::power_off);
        framebuffer_.text(0, 20, error != nullptr ? error : coretx::ui::tr::saving);
        if ((error != nullptr) && (error != coretx::ui::tr::power_off_error))
            framebuffer_.text(0, 48, coretx::ui::tr::power_off_error);
        framebuffer_.compact(false);
    }
    constexpr void render_crsf() noexcept {
        const bool dashboard = crsf_dashboard_page();
        const char *const navigation_help =
            !dashboard                     ? nullptr
            : Input::touch                 ? coretx::ui::tr::crsf_dashboard_touch_help
            : !crsf_.dashboard_interacting ? coretx::ui::tr::crsf_dashboard_help
                                           : coretx::ui::tr::crsf_dashboard_active_help;
        crsf_.menu.render(ExternalRFModule::config(), framebuffer_, Board::now_us(),
                          dashboard && crsf_.dashboard_interacting, navigation_help,
                          dashboard && Input::touch);
        if constexpr (Input::touch) {
            if (const auto point = input_.pressed(); point && dashboard)
                crsf_.menu.highlight(point->x, point->y, ExternalRFModule::config(), framebuffer_);
        }
        const auto input_error = input_notice();
        const auto notice = dashboard && (output_ != RadioOutput::external)
                                ? coretx::ui::tr::crsf_select_external
                            : input_error != nullptr ? input_error
                                                     : radio_message_;
        if (notice != nullptr)
            draw_notice(notice);
    }
    constexpr void render_multiswitch_page(const std::size_t slot,
                                           const ModelView &model) noexcept {
        const auto &page = diagnostics_.model().multiswitch[slot];
        const auto &state = crsf_.multiswitch;
        const bool operable = multiswitch_operable();
        framebuffer_.clear();
        render_multiswitch(page, slot, state.pages.switches(slot), state.selection(), framebuffer_,
                           Input::touch && operable);
        if (const auto pressed = input_.pressed_switch(); pressed && operable)
            highlight_multiswitch(*pressed, framebuffer_);
        framebuffer_.small_text(0, 52, model.name);
        const auto input_error = input_notice();
        const auto message = radio_notice();
        const auto notice =
            input_error != nullptr   ? input_error
            : model.error != nullptr ? model.error
            : (output_ == RadioOutput::external) && (ExternalRFModule::error() != CrsfError::none)
                ? coretx::ui::tr::crsf_link
            : message != nullptr           ? message
            : !diagnostics_.calibrated()   ? coretx::ui::tr::need_cal
            : !diagnostics_.valid()        ? coretx::ui::tr::wait_for_adc
            : !diagnostics_.output_valid() ? coretx::ui::tr::control_error
            : !operable                    ? coretx::ui::tr::crsf_select_external
            : Input::touch                 ? coretx::ui::tr::multiswitch_touch_help
            : state.select && page.controls[state.selected].condition().enabled
                ? coretx::ui::tr::multiswitch_bound_help
            : state.select ? coretx::ui::tr::multiswitch_select_help
                           : coretx::ui::tr::multiswitch_help;
        framebuffer_.small_text(0, 58, notice);
    }
    // The overview is drawn in sections so that sampling and RF output continue in between.
    constexpr void render_overview_page(const std::string_view model_name) noexcept {
        if ((overview_phase_ != 0) && (overview_model_ != storage_.models.active()))
            overview_phase_ = 0;
        overview_model_ = static_cast<std::uint8_t>(storage_.models.active());
        if (overview_phase_ == 0) {
            framebuffer_.clear();
            render_overview(diagnostics_, framebuffer_, telemetry_, output_, Board::now_us(),
                            model_name, nullptr, OverviewSection::header);
            overview_phase_ = 1;
            return;
        }
        if (overview_phase_ <= overview_channel_count) {
            render_overview(diagnostics_, framebuffer_, telemetry_, output_, Board::now_us(),
                            model_name, nullptr, OverviewSection::channel, overview_phase_ - 1U);
            ++overview_phase_;
            return;
        }
        render_overview(diagnostics_, framebuffer_, telemetry_, output_, Board::now_us(),
                        model_name, nullptr, OverviewSection::footer);
        overview_phase_ = 0;
        if (storage_.input.message() != nullptr)
            draw_notice(storage_.input.message());
        Display::begin_frame();
    }
    constexpr void render_dashboard_page(const ModelView &model) noexcept {
        render_dashboard(
            diagnostics_, error_message(Adc::error()), framebuffer_,
            {Probe::state(), Probe::error(), Probe::acknowledged(),
             storage_.radio.owns_memory() || model.saving
                 ? coretx::ui::tr::saving
                 : storage_.calibration.label(diagnostics_.calibrated()),
             Watchdog::resets(), storage_.input.message(), Board::name, Watchdog::last_fault()},
            telemetry_, output_, input_.message(), Board::now_us(), model.name, model.error);
        if ((diagnostics_.page() != Diagnostics::system_page) &&
            (storage_.input.message() != nullptr))
            draw_notice(storage_.input.message());
    }
    constexpr void draw_notice(const char *const notice) noexcept {
        framebuffer_.rectangle(0, 58, 128, 6, false);
        framebuffer_.small_text(0, 58, notice);
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
    [[no_unique_address]] Input input_{};
    [[no_unique_address]] std::conditional_t<Input::touch, DisabledFeature, TrimKeys> trim_keys_{};
    StorageSet<Memory, Probe> storage_{Board::sb_three_position};
    InputMenu input_menu_{};
    // Declared after storage_ so that it can refer to the model storage's active model.
    Diagnostics diagnostics_{Board::sb_three_position, storage_.models.data().model};
    Menu menu_{};
    [[no_unique_address]]
    std::conditional_t<has_external_rf, CrsfUi<ExternalRFModule, has_multiswitch>, DisabledFeature>
        crsf_{};
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
    Recovery recovery_{};
    bool recovery_selected_{}, recovery_drawn_{};
    [[no_unique_address]]
    std::conditional_t<has_power, Power, DisabledFeature> power_{};
};

} // namespace coretx::firmware
