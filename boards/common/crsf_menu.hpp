// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/common/crsf_client.hpp"
#include "coretx/ui/input.hpp"
#include "coretx/ui/translation.hpp"
#include "coretx/ui/ui.hpp"

namespace coretx::firmware {

namespace crsf_menu_detail {
namespace cfg = coretx::crsf::config;
namespace tr = coretx::ui::tr;
inline constexpr int header_y = 0;
inline constexpr int list_y = 12;
inline constexpr int row_height = 10;
// Touch hit areas of the Back/OK buttons: larger than the drawn 32x10 boxes, because taps near
// the panel edge are reported a few pixels too low or beside them.
enum class Button : std::uint8_t { none, back, ok };
[[nodiscard]] constexpr Button button(const std::uint8_t x, const std::uint8_t y) noexcept {
    if (y < list_y - 1)
        return x < 40 ? Button::back : x >= 88 ? Button::ok : Button::none;
    if (y < 15)
        return x < 36 ? Button::back : x >= 92 ? Button::ok : Button::none;
    return Button::none;
}
inline constexpr int value_right = coretx::ui::Framebuffer::width - 1;
inline constexpr int value_character_width = 4;
inline constexpr int help_y = 58;
inline constexpr std::size_t label_columns = 12;
inline constexpr std::size_t value_columns = 13;
inline constexpr std::string_view characters =
    " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.:/;";

[[nodiscard]] inline const char *error(const cfg::Error error) noexcept {
    switch (error) {
    case cfg::Error::none:
        return nullptr;
    case cfg::Error::timeout:
        return tr::crsf_timeout;
    case cfg::Error::malformed:
        return tr::crsf_invalid;
    case cfg::Error::capacity:
        return tr::crsf_limit;
    case cfg::Error::unsupported:
        return tr::crsf_unsupported;
    case cfg::Error::verification:
        return tr::crsf_verify;
    case cfg::Error::link:
        return tr::crsf_link;
    }
    return tr::crsf_invalid;
}
[[nodiscard]] inline std::string_view head(const std::string_view text,
                                           const std::size_t size) noexcept {
    return {text.data(), std::min(text.size(), size)};
}
[[nodiscard]] inline std::string_view option(const std::string_view options,
                                             const std::int64_t value) noexcept {
    std::size_t start{};
    for (std::int64_t i = 0; i < value; ++i) {
        const auto end = options.find(';', start);
        if (end == std::string_view::npos)
            return {};
        start = end + 1;
    }
    const auto end = options.find(';', start);
    return {options.data() + start, (end == std::string_view::npos ? options.size() : end) - start};
}
[[nodiscard]] inline std::string_view number(std::array<char, 24> &buffer, const std::int64_t value,
                                             const std::uint8_t decimals) noexcept {
    auto magnitude = static_cast<std::uint32_t>(value < 0 ? -value : value);
    std::size_t start = buffer.size();
    unsigned digits{};
    do {
        if ((decimals != 0) && (digits == decimals))
            buffer[--start] = '.';
        buffer[--start] = static_cast<char>('0' + magnitude % 10);
        magnitude /= 10;
        ++digits;
    } while ((magnitude != 0) || (digits <= decimals));
    if (value < 0)
        buffer[--start] = '-';
    return {buffer.data() + start, buffer.size() - start};
}
} // namespace crsf_menu_detail

class CrsfMenu {
  public:
    [[nodiscard]] bool visible() const noexcept { return visible_; }
    [[nodiscard]] bool editing() const noexcept { return editing_; }
    void finish_edit(CrsfClient &client) noexcept {
        editing_ = false;
        client.pin(0);
    }
    void suspend(CrsfClient &client) noexcept {
        visible_ = false;
        finish_edit(client);
        if ((client.mode() != coretx::crsf::config::Mode::closed) && !client.pending_write() &&
            (client.command() == coretx::crsf::config::Command::idle) &&
            (client.error() == coretx::crsf::config::Error::none))
            client.suspend();
    }
    void resume(CrsfClient &client, const TimestampUs now) noexcept {
        visible_ = true;
        notice_ = nullptr;
        if (client.mode() == coretx::crsf::config::Mode::closed)
            client.resume(now);
        client.window(first_);
    }
    void open(CrsfClient &client, const TimestampUs now) noexcept {
        *this = {};
        visible_ = true;
        client.open(now);
    }
    void close(CrsfClient &client) noexcept {
        visible_ = false;
        editing_ = false;
        client.close();
    }
    void handle(const Key key, CrsfClient &client, const TimestampUs now) noexcept {
        namespace cfg = coretx::crsf::config;
        if (!visible_)
            return;
        if (client.error() == cfg::Error::link) {
            if ((key == Key::cancel) || (key == Key::cancel_long))
                close(client);
            return;
        }
        if (client.command() != cfg::Command::idle) {
            if (((key == Key::cancel) || (key == Key::cancel_long)) &&
                (client.command() != cfg::Command::cancelling))
                (void)client.cancel_command();
            else if ((key == Key::ok) && (client.command() == cfg::Command::confirmation))
                (void)client.confirm_command();
            return;
        }
        if (client.pending_write()) {
            if (key == Key::cancel_long)
                close(client);
            return;
        }
        if (editing_) {
            const auto row = client.index(editing_id_);
            if (!row) {
                editing_ = false;
                client.pin(0);
                notice_ = crsf_menu_detail::tr::crsf_invalid;
                return;
            }
            selected_ = *row;
            if (selected_ < first_)
                first_ = selected_;
            if (selected_ >= (first_ + cfg::visible_rows))
                first_ = static_cast<std::uint8_t>(selected_ - cfg::visible_rows + 1);
            client.window(first_);
            edit(key, client);
            return;
        }
        if (key != Key::none)
            notice_ = nullptr;
        const bool devices = client.view_mode() != cfg::Mode::parameters;
        const auto count = devices ? client.device_count() : client.count();
        if (selected_ >= count)
            selected_ = count == 0 ? 0 : static_cast<std::uint8_t>(count - 1);
        if ((key == Key::cancel_long) || ((key == Key::cancel) && devices)) {
            close(client);
            return;
        }
        if (key == Key::cancel) {
            if (client.folder() == 0)
                client.open(now);
            else
                client.enter(client.parent());
            selected_ = 0;
            first_ = 0;
        } else if ((client.error() == cfg::Error::link) && (key != Key::none)) {
            notice_ = crsf_menu_detail::tr::crsf_link;
        } else if ((key == Key::up) && (selected_ != 0))
            --selected_;
        else if ((key == Key::down) && ((std::size_t{selected_} + 1) < count))
            ++selected_;
        else if ((key == Key::ok_long) && !devices)
            client.refresh();
        else if ((key == Key::ok) && (count != 0)) {
            if (devices) {
                client.select(selected_);
                selected_ = 0;
                first_ = 0;
            } else {
                const auto id = client.item(selected_);
                const auto parameter = client.parameter(id);
                if (!parameter) {
                    notice_ = client.failed(id) ? crsf_menu_detail::tr::crsf_invalid
                                                : crsf_menu_detail::tr::crsf_loading;
                } else if (parameter->type == coretx::crsf::CRSF_FOLDER) {
                    client.enter(id);
                    selected_ = 0;
                    first_ = 0;
                } else if (parameter->type == coretx::crsf::CRSF_COMMAND) {
                    (void)client.start_command(id, now);
                } else if (parameter->editable()) {
                    editing_ = true;
                    editing_id_ = id;
                    client.pin(id);
                    editing_type_ = parameter->type;
                    value_ = parameter->value;
                    position_ = 0;
                    length_ = static_cast<std::uint8_t>(parameter->text.size());
                    if (editing_type_ == coretx::crsf::CRSF_STRING) {
                        text_.fill(0);
                        std::copy(parameter->text.begin(), parameter->text.end(), text_.begin());
                    }
                } else
                    notice_ = parameter->type == coretx::crsf::CRSF_INFO
                                  ? crsf_menu_detail::tr::crsf_read_only
                                  : crsf_menu_detail::tr::crsf_unsupported;
            }
        }
        if (selected_ < first_)
            first_ = selected_;
        if (selected_ >= (first_ + cfg::visible_rows))
            first_ = static_cast<std::uint8_t>(selected_ - cfg::visible_rows + 1);
        client.window(first_);
    }
    void tap(const std::uint8_t x, const std::uint8_t y, CrsfClient &client,
             const TimestampUs now) noexcept {
        namespace cfg = coretx::crsf::config;
        namespace md = crsf_menu_detail;
        if (!visible_)
            return;
        const auto button = md::button(x, y);
        const bool confirm = editing_ || (client.command() == cfg::Command::confirmation);
        if ((button == md::Button::back) || ((button == md::Button::ok) && confirm) ||
            (y < md::list_y - 1)) {
            if (button == md::Button::back) {
                if (client.error() == cfg::Error::link) {
                    finish_edit(client);
                    client.open(now);
                    selected_ = first_ = 0;
                    notice_ = nullptr;
                } else if ((client.view_mode() == cfg::Mode::parameters) ||
                           (client.command() != cfg::Command::idle))
                    handle(Key::cancel, client, now);
            } else if ((button == md::Button::ok) && confirm)
                handle(editing_ ? Key::ok_long : Key::ok, client, now);
            return;
        }
        const auto row = static_cast<std::size_t>((y - (md::list_y - 1)) / md::row_height);
        const auto index = first_ + row;
        const auto count =
            client.view_mode() == cfg::Mode::parameters ? client.count() : client.device_count();
        if ((row >= cfg::visible_rows) || (index >= count))
            return;
        if ((editing_ || client.pending_write() || (client.command() != cfg::Command::idle)) &&
            (index != selected_))
            return;
        selected_ = static_cast<std::uint8_t>(index);
        handle(Key::ok, client, now);
    }
    // Marks the element that tap() would activate for the current touch point.
    void highlight(const std::uint8_t x, const std::uint8_t y, const CrsfClient &client,
                   coretx::ui::Framebuffer &display) const noexcept {
        namespace cfg = coretx::crsf::config;
        namespace md = crsf_menu_detail;
        if (!visible_)
            return;
        const auto button = md::button(x, y);
        const bool confirm = editing_ || (client.command() == cfg::Command::confirmation);
        if ((button == md::Button::back) || ((button == md::Button::ok) && confirm) ||
            (y < md::list_y - 1)) {
            if (button == md::Button::back)
                display.invert(0, 0, 32, 10);
            else if ((button == md::Button::ok) && confirm)
                display.invert(96, 0, 32, 10);
            return;
        }
        const auto row = static_cast<std::size_t>((y - (md::list_y - 1)) / md::row_height);
        const auto count =
            client.view_mode() == cfg::Mode::parameters ? client.count() : client.device_count();
        if ((row < cfg::visible_rows) && ((first_ + row) < count))
            display.invert(0, md::list_y - 1 + static_cast<int>(row) * md::row_height, 128,
                           md::row_height);
    }
    void render(const CrsfClient &client, coretx::ui::Framebuffer &display, const TimestampUs now,
                const bool active_header = false, const char *const navigation_help = nullptr,
                const bool touch = false) const noexcept {
        namespace md = crsf_menu_detail;
        namespace cfg = coretx::crsf::config;
        display.compact(false);
        display.clear();
        const bool devices = client.view_mode() != cfg::Mode::parameters;
        const auto folder = client.parameter(client.folder());
        const auto device = client.selected_device();
        const auto title = !devices && (client.folder() != 0) && folder ? folder->name
                           : !devices && (client.folder() == 0) && device
                               ? std::string_view{device->name.data(), device->name_length}
                               : std::string_view{md::tr::crsf_devices};
        if (active_header)
            display.rectangle(0, md::header_y, coretx::ui::Framebuffer::width, md::row_height);
        const bool confirm =
            touch && (editing_ || (client.command() == cfg::Command::confirmation));
        if (touch) {
            display.rectangle(0, 0, 32, 10);
            display.small_text(2, 2, md::tr::touch_back, false);
            if (confirm) {
                display.rectangle(96, 0, 32, 10);
                display.small_text(106, 2, "OK", false);
            }
        }
        display.text(touch ? 35 : 0, md::header_y, md::head(title, touch ? (confirm ? 9 : 14) : 19),
                     !active_header);
        if (client.busy() && !confirm) {
            constexpr std::string_view spinner = "|/-\\";
            display.text(122, md::header_y, {spinner.data() + (now.count() / 200'000U) % 4U, 1},
                         !active_header);
        }
        const auto count = devices ? client.device_count() : client.count();
        if (count == 0)
            display.small_text(0, md::list_y,
                               devices                    ? md::tr::crsf_search
                               : folder || !client.busy() ? md::tr::menu_empty
                                                          : md::tr::crsf_loading);
        for (std::size_t row = first_; (row < count) && (row < (first_ + cfg::visible_rows));
             ++row) {
            const int y = md::list_y + static_cast<int>(row - first_) * md::row_height;
            const bool ink = row != selected_;
            if (!ink)
                display.rectangle(0, y - 1, 128, md::row_height);
            if (devices) {
                const auto &device = client.device(row);
                display.text(1, y, md::head({device.name.data(), device.name_length}, 20), ink);
                continue;
            }
            const auto id = client.item(row);
            const auto parameter = client.parameter(id);
            if (!parameter) {
                display.number(25, y, id, ink);
                const auto text =
                    md::head(client.failed(id) ? md::tr::crsf_invalid : "...", md::value_columns);
                display.small_text(md::value_right -
                                       static_cast<int>(text.size()) * md::value_character_width,
                                   y + 2, text, ink);
                continue;
            }
            display.text(1, y, md::head(parameter->name, md::label_columns), ink);
            std::array<char, 24> buffer{};
            std::array<char, md::value_columns> numeric_text{};
            const bool string_edit =
                editing_ && (editing_id_ == id) && (editing_type_ == coretx::crsf::CRSF_STRING);
            const std::size_t start =
                string_edit ? (position_ / md::value_columns) * md::value_columns : 0;
            const auto value = editing_ && (editing_id_ == id) ? value_ : parameter->value;
            auto text = parameter->numeric() ? md::number(buffer, value, parameter->decimals)
                        : parameter->type == coretx::crsf::CRSF_TEXT_SELECTION
                            ? md::option(parameter->text, value)
                        : parameter->type == coretx::crsf::CRSF_FOLDER ? ">"
                        : parameter->type == coretx::crsf::CRSF_COMMAND
                            ? parameter->text.empty() ? "*" : parameter->text
                        : parameter->type == coretx::crsf::CRSF_STRING
                            ? string_edit ? std::string_view{text_.data() + start, length_ - start}
                                          : parameter->text
                        : parameter->type == coretx::crsf::CRSF_INFO ? parameter->text
                                                                     : "?";
            if (parameter->numeric() && !parameter->unit.empty() &&
                ((text.size() + 1) < md::value_columns)) {
                std::copy(text.begin(), text.end(), numeric_text.begin());
                const auto unit_size =
                    std::min(parameter->unit.size(), md::value_columns - text.size() - 1);
                numeric_text[text.size()] = ' ';
                std::copy_n(parameter->unit.begin(), unit_size,
                            numeric_text.begin() + static_cast<std::ptrdiff_t>(text.size()) + 1);
                text = {numeric_text.data(), text.size() + 1 + unit_size};
            }
            text = md::head(text, md::value_columns);
            const auto columns = text.size() + (string_edit && (position_ == length_) ? 1U : 0U);
            const int x = md::value_right - static_cast<int>(columns) * md::value_character_width;
            display.small_text(x, y + 2, text, ink);
            if (string_edit)
                display.rectangle(x + static_cast<int>(position_ - start) *
                                          md::value_character_width,
                                  y + md::row_height - 2, 3, 1, ink);
        }
        const auto error = md::error(client.error());
        const auto help = error != nullptr                                 ? error
                          : notice_ != nullptr                             ? notice_
                          : client.command() == cfg::Command::confirmation ? md::tr::crsf_confirm
                          : client.command() != cfg::Command::idle         ? md::tr::crsf_running
                          : client.pending_write()                         ? md::tr::saving
                          : touch && navigation_help != nullptr            ? navigation_help
                          : editing_ && (editing_type_ == coretx::crsf::CRSF_STRING)
                              ? md::tr::menu_name_help
                          : editing_                   ? md::tr::menu_help
                          : navigation_help != nullptr ? navigation_help
                                                       : md::tr::crsf_help;
        if ((error != nullptr) && client.error_parameter()) {
            std::array<char, 24> buffer{};
            const auto id = md::number(buffer, *client.error_parameter(), 0);
            std::array<char, 32> message{};
            const auto prefix = md::head(error, message.size() - id.size() - 2);
            std::copy(prefix.begin(), prefix.end(), message.begin());
            message[prefix.size()] = ' ';
            message[prefix.size() + 1] = '#';
            std::copy(id.begin(), id.end(),
                      message.begin() + static_cast<std::ptrdiff_t>(prefix.size()) + 2);
            display.small_text(0, md::help_y, {message.data(), prefix.size() + 2 + id.size()});
        } else
            display.small_text(0, md::help_y, help);
    }

  private:
    void edit(const Key key, CrsfClient &client) noexcept {
        namespace c = coretx::crsf;
        const auto parameter = client.parameter(editing_id_);
        if (!parameter) {
            editing_ = false;
            notice_ = crsf_menu_detail::tr::crsf_invalid;
            return;
        }
        if ((key == Key::cancel) || (key == Key::cancel_long)) {
            editing_ = false;
            client.pin(0);
            return;
        }
        const bool string = editing_type_ == c::CRSF_STRING;
        if ((!string && ((key == Key::ok) || (key == Key::ok_long))) ||
            (string && (key == Key::ok_long))) {
            const bool written =
                string ? client.write(editing_id_, std::string_view{text_.data(), length_})
                       : client.write(editing_id_, value_);
            if (written) {
                editing_ = false;
                client.pin(0);
            }
            return;
        }
        if (string && (key == Key::ok)) {
            const auto limit = std::min<std::size_t>(parameter->string_limit,
                                                     coretx::crsf::config::maximum_value_bytes - 1);
            if ((position_ + 1U) < limit)
                position_ =
                    static_cast<std::uint8_t>(std::min<std::size_t>(position_ + 1U, length_));
            return;
        }
        if ((key != Key::up) && (key != Key::down))
            return;
        if (string) {
            if (parameter->string_limit == 0)
                return;
            const auto characters = crsf_menu_detail::characters;
            const auto current =
                position_ < length_ ? characters.find(text_[position_]) : characters.size();
            const auto index = key == Key::up ? (current + 1) % (characters.size() + 1)
                               : current == 0 ? characters.size()
                                              : std::min(current - 1, characters.size());
            if (index == characters.size()) {
                length_ = position_;
                std::fill(text_.begin() + position_, text_.end(), '\0');
            } else {
                text_[position_] = characters[index];
                length_ = static_cast<std::uint8_t>(std::max<unsigned>(length_, position_ + 1U));
            }
        } else if (editing_type_ == c::CRSF_TEXT_SELECTION) {
            value_ = key == Key::up ? value_ >= parameter->maximum ? parameter->minimum : value_ + 1
                     : value_ <= parameter->minimum ? parameter->maximum
                                                    : value_ - 1;
        } else
            value_ = std::clamp(value_ + (key == Key::up ? parameter->step : -parameter->step),
                                parameter->minimum, parameter->maximum);
    }

    std::array<char, coretx::crsf::config::maximum_value_bytes> text_{};
    std::int64_t value_{};
    const char *notice_{};
    std::uint8_t selected_{};
    std::uint8_t first_{};
    std::uint8_t editing_id_{};
    std::uint8_t editing_type_{};
    std::uint8_t position_{};
    std::uint8_t length_{};
    bool visible_{};
    bool editing_{};
};

} // namespace coretx::firmware
