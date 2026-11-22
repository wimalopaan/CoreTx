// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/common/calibration_storage.hpp"
#include "boards/common/dashboard.hpp"
#include "boards/common/input_menu.hpp"
#include "boards/common/input_storage.hpp"
#include "boards/common/menu.hpp"
#include "boards/common/radio_errors.hpp"
#include "boards/common/sender_input.hpp"
#include "boards/common/telemetry_adapters.hpp"
#include "platform/stm32f072/lcd.hpp"
#if CORETX_CRSF
#include "boards/common/crsf_menu.hpp"
#include "platform/stm32f072/crsf.hpp"
#endif

namespace coretx::firmware::i6x {

enum class ApplicationState : std::uint8_t { starting, running, input_error, fatal };
enum class ApplicationFailure : std::uint8_t { none, not_initialized, hardware, lcd, stack };
struct NoMenu {};

template <typename Board, typename Input = PhysicalInput<typename Board::Keys>> class Application {
    inline static constexpr bool has_menu = (Board::name == "I6X") || (Board::name == "I6S");
    using Adc = typename Board::Adc;
    using Display = typename Board::Display;
    using Probe = typename Board::Probe;
    using Memory = typename Board::Memory;
    using Inputs = typename Board::Inputs;
#if CORETX_CRSF
    using Transmitter = typename Board::Transmitter;
#endif
#if CORETX_A7105
    using Radio = typename Board::Radio;
    using Internal = typename Board::Internal;
#endif
    using Watchdog = typename Board::Watchdog;

  public:
    [[nodiscard]] constexpr ApplicationState initialize() noexcept {
        state_ = ApplicationState::fatal;
        failure_ = ApplicationFailure::none;
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
#if CORETX_CRSF
            crsf_menu_ = {};
#if CORETX_MULTISWITCH
            multiswitch_ = {};
            multiswitch_select_ = false;
#endif
            crsf_dashboard_active_ = false;
            crsf_dashboard_saved_ = false;
            crsf_dashboard_interacting_ = false;
#endif
            framebuffer_.compact(false);
        }
        render();
        drawn_at_ = Board::now_us();
        return state_;
    }
    [[nodiscard]] constexpr ApplicationState service(TimestampUs now) noexcept {
        if (state_ == ApplicationState::fatal) {
            return state_;
        }
        if (!Board::stack_intact()) {
            return fail(ApplicationFailure::stack);
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
#if CORETX_A7105
            internal_.initialize(settings_.transmitter, settings_.receiver, now);
            internal_.set_channel_mode(settings_.channel_mode);
#endif
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
#if CORETX_MULTISWITCH
                multiswitch_ = {};
                multiswitch_select_ = false;
#endif
                settings_ = models_.data().radio;
#if CORETX_A7105
                internal_.deactivate();
                internal_.initialize(settings_.transmitter, settings_.receiver, Board::now_us());
                internal_.set_channel_mode(settings_.channel_mode);
#endif
                const auto selected_output = model_applied_ ? RadioOutput::off : settings_.output;
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
        Inputs::service(now, diagnostics_.take_click());
        display_ready_ = display_ready_ || !Display::busy();
#if CORETX_CRSF
        if constexpr (has_menu) {
#if CORETX_MULTISWITCH
            if (!multiswitch_slot())
                multiswitch_select_ = false;
#endif
            diagnostics_.constrain_page();
            sync_crsf_dashboard(now);
        }
#endif
        const auto key =
            input_.service(now, !Input::needs_display || display_ready_, multiswitch_input());
        // Keep RC evaluation out of nested UI handlers on the 2 KiB stack.
        diagnostics_.defer_refresh();
        handle(key, now);
        diagnostics_.flush_refresh();
        if constexpr (has_menu && !Input::touch) {
            const auto buttons = input_.trim_buttons();
            if (diagnostics_.phase() != CalibrationPhase::idle)
                trim_keys_.reset();
            else if (models_.editable() &&
                     trim_keys_.update(models_.data().model, diagnostics_.setup(), buttons, now)) {
                diagnostics_.refresh_model();
                Inputs::service(now, true);
                if (!models_.save_trims(now))
                    radio_message_ = models_.message();
            }
            diagnostics_.observe_trims(buttons);
        }
#if CORETX_CRSF
        if constexpr (has_menu) {
            sync_crsf_dashboard(now);
#if CORETX_MULTISWITCH
            multiswitch_.configure(diagnostics_.model());
            const auto slot = multiswitch_operable() ? multiswitch_slot() : std::nullopt;
            const auto held = Input::touch ? input_.held_switch()
                              : multiswitch_select_ && input_.ok_held()
                                  ? std::optional<std::size_t>{multiswitch_selected_}
                                  : std::nullopt;
            const auto tapped = Input::touch ? input_.tapped_switch() : multiswitch_tapped_;
            const auto *const controls =
                multiswitch_signal_operable() ? &diagnostics_.prepared().controls : nullptr;
            if (multiswitch_.update(slot, held, tapped, controls,
                                    diagnostics_.evaluation().frame.logical))
                Inputs::service(now, true);
            multiswitch_tapped_.reset();
#endif
        }
#endif
#if CORETX_A7105
        if (settings_applied_) {
            internal_.service(Board::now_us(), diagnostics_.evaluation().frame.channels,
                              (state_ == ApplicationState::running) && diagnostics_.output_valid(),
                              sampled_at_);
            if (internal_.take_receiver_changed()) {
                telemetry_.clear(telemetry::Source::afhds2a);
                settings_.receiver = internal_.session().receiver();
                save_radio();
            }
            if (output_ == RadioOutput::internal)
                publish_telemetry(internal_.telemetry(), telemetry_);
        }
#endif
#if CORETX_CRSF
        if (output_ == RadioOutput::external) {
            (void)Transmitter::service(
                Board::now_us(), diagnostics_.evaluation().frame.channels,
                (state_ == ApplicationState::running) && diagnostics_.output_valid(), sampled_at_,
                [&]() constexpr -> coretx::multiswitch::Pages * {
                    if constexpr (has_menu && features::multiswitch)
                        return &multiswitch_;
                    else
                        return nullptr;
                }());
            publish_telemetry(Transmitter::telemetry(), telemetry_, Board::now_us());
        }
#endif
        telemetry_.service(Board::now_us());
        if (!Display::busy() &&
            ((overview_phase_ != 0) || ((now - drawn_at_) >= redraw_interval))) {
            render();
            if (overview_phase_ == 0)
                drawn_at_ = Board::now_us();
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
#if CORETX_CRSF
    [[nodiscard]] const CrsfMenu &crsf_menu() const noexcept
        requires(has_menu)
    {
        return crsf_menu_;
    }
#endif
    [[nodiscard]] constexpr const ModelStorage<Memory> &models() const noexcept
        requires(has_menu)
    {
        return models_;
    }

  private:
    [[nodiscard]] constexpr MultiSwitchInput multiswitch_input() const noexcept {
#if CORETX_MULTISWITCH
        if constexpr (has_menu)
            if (const auto slot = multiswitch_slot())
                return {true, diagnostics_.model().multiswitch[*slot].momentary};
#endif
        return {};
    }
#if CORETX_CRSF
    [[nodiscard]] constexpr bool crsf_dashboard_page() const noexcept {
        if constexpr (has_menu)
            return diagnostics_.model().crsf_dashboard && !menu_.visible() &&
                   !input_menu_.visible() && (diagnostics_.phase() == CalibrationPhase::idle) &&
                   (diagnostics_.page() ==
                    (Diagnostics::page_count +
                     (features::multiswitch ? coretx::multiswitch::count(diagnostics_.model())
                                            : 0)));
        return false;
    }
    constexpr void sync_crsf_dashboard(const TimestampUs now) noexcept {
        if constexpr (has_menu) {
            const bool active = crsf_dashboard_page() && (output_ == RadioOutput::external);
            if (active == crsf_dashboard_active_) {
                if (active && !crsf_dashboard_interacting_)
                    crsf_menu_.handle(Key::none, Transmitter::config(), now);
                return;
            }
            crsf_dashboard_active_ = active;
            crsf_dashboard_interacting_ = false;
            if (active) {
                if (crsf_dashboard_saved_)
                    crsf_menu_.resume(Transmitter::config(), now);
                else
                    crsf_menu_.open(Transmitter::config(), now);
            } else {
                crsf_menu_.suspend(Transmitter::config());
                crsf_dashboard_saved_ = true;
            }
        }
    }
    [[nodiscard]] constexpr std::optional<std::size_t> multiswitch_slot() const noexcept {
        if constexpr (has_menu && features::multiswitch) {
            if (!menu_.visible() && !crsf_menu_.visible() && !input_menu_.visible() &&
                (diagnostics_.phase() == CalibrationPhase::idle) &&
                (diagnostics_.page() >= Diagnostics::page_count))
                return coretx::multiswitch::slot(diagnostics_.model(),
                                                 diagnostics_.page() - Diagnostics::page_count);
        }
        return std::nullopt;
    }
    [[nodiscard]] constexpr bool multiswitch_operable() const noexcept {
        if constexpr (has_menu && features::multiswitch)
            return multiswitch_signal_operable() && models_.editable();
        return false;
    }
    [[nodiscard]] constexpr bool multiswitch_signal_operable() const noexcept {
        if constexpr (has_menu && features::multiswitch)
            return (output_ == RadioOutput::external) && (state_ == ApplicationState::running) &&
                   diagnostics_.output_valid() && models_.loaded() &&
                   (input_.message() == nullptr) && (Transmitter::error() == CrsfError::none);
        return false;
    }
#endif
    constexpr void apply_inputs() noexcept {
        const bool extended = features::extended_inputs && diagnostics_.input_setup().extra_analog;
        if (extended != adc_extended_) {
            if (Adc::set_extended(extended) != AdcError::none) {
                radio_message_ = error_message(Adc::error());
                state_ = ApplicationState::input_error;
            } else
                adc_extended_ = extended;
        }
        Inputs::configure(diagnostics_.extra_digital_enabled());
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
#if CORETX_CRSF
        if constexpr (has_menu)
            crsf_menu_.close(Transmitter::config());
        crsf_dashboard_active_ = false;
        crsf_dashboard_saved_ = false;
        crsf_dashboard_interacting_ = false;
        Transmitter::power_off();
#endif
#if CORETX_A7105
        internal_.deactivate();
#endif
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
        if constexpr (features::i6s && features::extended_inputs) {
            if ((output == RadioOutput::external) && diagnostics_.extra_digital_enabled()) {
                radio_message_ = coretx::ui::tr::inputs_crsf_conflict;
                return;
            }
        }
        if (((output == RadioOutput::internal) && !features::a7105) ||
            ((output == RadioOutput::external) && !features::crsf)) {
            radio_message_ = coretx::ui::tr::radio_not_built;
            return;
        }
        output_ = output;
#if CORETX_CRSF
        if (output == RadioOutput::external)
            Transmitter::initialize(now);
        if constexpr (has_menu) {
            if ((output == RadioOutput::external) && models_.loaded() &&
                !Transmitter::set_baud(models_.data().baud_index, now))
                radio_message_ = coretx::ui::tr::bad_request;
        }
#endif
#if CORETX_A7105
        if (output == RadioOutput::internal) {
            if (internal_.error() != Afhds2aError::none) {
                internal_.initialize(settings_.transmitter, settings_.receiver, now);
                internal_.set_channel_mode(settings_.channel_mode);
            }
            internal_.activate();
        }
#endif
        (void)now;
    }
    constexpr void handle(const Key key, const TimestampUs now) noexcept {
        if (key != Key::none)
            overview_phase_ = 0;
#if CORETX_CRSF
        if constexpr (has_menu) {
            if (crsf_dashboard_page()) {
                auto &client = Transmitter::config();
                if (crsf_dashboard_interacting_) {
                    const bool leave =
                        (key == Key::cancel_long) ||
                        (Input::touch && (key == Key::ok_long) && !crsf_menu_.editing());
                    if (leave && (client.command() == coretx::crsf::config::Command::idle)) {
                        if (!client.pending_write()) {
                            crsf_menu_.finish_edit(client);
                            crsf_dashboard_interacting_ = false;
                        }
                    } else if ((key == Key::cancel) &&
                               (client.mode() == coretx::crsf::config::Mode::devices) &&
                               !crsf_menu_.editing()) {
                        crsf_dashboard_interacting_ = false;
                    } else
                        crsf_menu_.handle(key, client, now);
                    return;
                }
                if (key == Key::ok) {
                    if (output_ != RadioOutput::external)
                        radio_message_ = coretx::ui::tr::crsf_select_external;
                    else if (Transmitter::error() != CrsfError::none)
                        radio_message_ = coretx::ui::tr::crsf_link;
                    else
                        crsf_dashboard_interacting_ = true;
                    return;
                }
            } else if (crsf_menu_.visible()) {
                crsf_menu_.handle(key, Transmitter::config(), now);
                return;
            }
        }
#endif
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
#if CORETX_A7105
            if (((key == Key::cancel) || (key == Key::cancel_long)) &&
                internal_.session().binding()) {
                (void)internal_.bind(now);
                telemetry_.clear(telemetry::Source::afhds2a);
            }
#endif
#if CORETX_MULTISWITCH
            if (const auto slot = multiswitch_slot(); slot && !Input::touch) {
                if (multiswitch_select_) {
                    if ((key == Key::cancel) || (key == Key::cancel_long))
                        multiswitch_select_ = false;
                    else if ((key == Key::up) || (key == Key::down))
                        multiswitch_selected_ = static_cast<std::uint8_t>(
                            (multiswitch_selected_ + (key == Key::down ? 1U : 7U)) % 8U);
                    else if ((key == Key::ok) && multiswitch_operable())
                        multiswitch_tapped_ = multiswitch_selected_;
                    return;
                }
                if (key == Key::ok) {
                    if (multiswitch_operable()) {
                        multiswitch_select_ = true;
                        multiswitch_selected_ = 0;
                    } else
                        radio_message_ = coretx::ui::tr::crsf_select_external;
                    return;
                }
            }
#endif
            if ((key == Key::ok_long) && !menu_.visible() &&
                (diagnostics_.phase() == CalibrationPhase::idle)) {
                menu_.open();
#if CORETX_MULTISWITCH
                multiswitch_select_ = false;
#endif
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
#if CORETX_A7105
                    internal_.set_channel_mode(settings_.channel_mode);
#endif
                    save_radio();
                    break;
                case MenuCommand::bind:
#if CORETX_A7105
                    radio_message_ = output_ != RadioOutput::internal
                                         ? coretx::ui::tr::radio_select_internal
                                     : !internal_.bind(now) ? coretx::ui::tr::not_initialized
                                                            : nullptr;
                    if (radio_message_ == nullptr) {
                        telemetry_.clear(telemetry::Source::afhds2a);
                    }
#else
                    radio_message_ = coretx::ui::tr::radio_not_built;
#endif
                    break;
                case MenuCommand::baud:
                    models_.data().baud_index = menu_.requested_baud();
#if CORETX_CRSF
                    if ((output_ == RadioOutput::external) &&
                        !Transmitter::set_baud(models_.data().baud_index, now))
                        radio_message_ = coretx::ui::tr::bad_request;
#endif
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
                case MenuCommand::crsf_config:
#if CORETX_CRSF
                    if (output_ != RadioOutput::external)
                        radio_message_ = coretx::ui::tr::crsf_select_external;
                    else if (Transmitter::error() != CrsfError::none)
                        radio_message_ = coretx::ui::tr::crsf_link;
                    else
                        crsf_menu_.open(Transmitter::config(), now);
#else
                    radio_message_ = coretx::ui::tr::radio_not_built;
#endif
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
#if CORETX_A7105
        if ((key == Key::cancel) && internal_.session().binding()) {
            (void)internal_.bind(now);
            telemetry_.clear(telemetry::Source::afhds2a);
            return;
        }
#endif
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
        const char *message = radio_message_;
        bool binding{};
#if CORETX_A7105
        binding = internal_.session().binding();
        if (const auto error = internal_.error(); error != Afhds2aError::none) {
            const auto bus_error = Radio::info().error;
            message = (error == Afhds2aError::bus) && (bus_error != A7105Error::none)
                          ? error_message(bus_error)
                          : error_message(error);
        }
#endif
#if CORETX_CRSF
        if constexpr (has_menu) {
            if (crsf_menu_.visible() || crsf_dashboard_page()) {
                begin_menu_band();
                const bool dashboard = crsf_dashboard_page();
                const char *const navigation_help =
                    !dashboard                     ? nullptr
                    : !crsf_dashboard_interacting_ ? coretx::ui::tr::crsf_dashboard_help
                    : Input::touch                 ? coretx::ui::tr::crsf_dashboard_touch_help
                                                   : coretx::ui::tr::crsf_dashboard_active_help;
                crsf_menu_.render(Transmitter::config(), framebuffer_, Board::now_us(),
                                  dashboard && crsf_dashboard_interacting_, navigation_help);
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
#endif
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
                return models_.loaded() && models_.busy();
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
#if CORETX_MULTISWITCH
        if constexpr (has_menu) {
            if (const auto slot = multiswitch_slot()) {
                begin_menu_band();
                framebuffer_.clear();
                render_multiswitch(
                    diagnostics_.model().multiswitch[*slot], *slot, multiswitch_.switches(*slot),
                    multiswitch_select_ ? std::optional<std::size_t>{multiswitch_selected_}
                                        : std::nullopt,
                    framebuffer_, Input::touch && multiswitch_operable());
                framebuffer_.small_text(0, 52, model_name);
                const auto notice = input_.message() != nullptr ? input_.message()
                                    : error_message(Adc::error()) != nullptr
                                        ? error_message(Adc::error())
                                    : model_error != nullptr ? model_error
                                    : (output_ == RadioOutput::external) &&
                                            (Transmitter::error() != CrsfError::none)
                                        ? coretx::ui::tr::crsf_link
                                    : message != nullptr           ? message
                                    : !diagnostics_.calibrated()   ? coretx::ui::tr::need_cal
                                    : !diagnostics_.valid()        ? coretx::ui::tr::wait_for_adc
                                    : !diagnostics_.output_valid() ? coretx::ui::tr::control_error
                                    : !multiswitch_operable() ? coretx::ui::tr::crsf_select_external
                                    : Input::touch ? coretx::ui::tr::multiswitch_touch_help
                                    : multiswitch_select_ && diagnostics_.model()
                                                                 .multiswitch[*slot]
                                                                 .controls[multiswitch_selected_]
                                                                 .condition()
                                                                 .enabled
                                        ? coretx::ui::tr::multiswitch_bound_help
                                    : multiswitch_select_ ? coretx::ui::tr::multiswitch_select_help
                                                          : coretx::ui::tr::multiswitch_help;
                framebuffer_.small_text(0, 58, notice);
                finish_menu_band();
                return;
            }
        }
#endif
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
#if CORETX_CRSF
    [[no_unique_address]] std::conditional_t<has_menu, CrsfMenu, NoMenu> crsf_menu_{};
    [[no_unique_address]]
    std::conditional_t<has_menu && features::multiswitch, coretx::multiswitch::Pages, NoMenu>
        multiswitch_{};
#if CORETX_MULTISWITCH
    std::optional<std::size_t> multiswitch_tapped_{};
    std::uint8_t multiswitch_selected_{};
    bool multiswitch_select_{};
#endif
    bool crsf_dashboard_active_{};
    bool crsf_dashboard_saved_{};
    bool crsf_dashboard_interacting_{};
#endif
    RadioSettings settings_{};
    telemetry::Pool telemetry_{};
#if CORETX_A7105
    Internal internal_{};
#endif
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
    [[nodiscard]] constexpr bool model_owns_memory() const noexcept {
        if constexpr (has_menu)
            return models_.owns_memory();
        else
            return false;
    }
};

} // namespace coretx::firmware::i6x
