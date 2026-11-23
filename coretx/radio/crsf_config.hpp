// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "coretx/radio/crsf.hpp"

#include <optional>
#include <string_view>

namespace coretx::crsf::config {

using Frame = std::array<std::uint8_t, CRSF_MAX_PACKET_LEN>;
inline constexpr std::size_t visible_rows = 4;
inline constexpr std::size_t maximum_value_bytes = 56;
enum class Error : std::uint8_t {
    none,
    timeout,
    malformed,
    capacity,
    unsupported,
    verification,
    link
};
enum class Mode : std::uint8_t { closed, devices, parameters };
enum class Command : std::uint8_t { idle, starting, running, confirmation, cancelling };

struct Parameter {
    std::string_view name{};
    std::string_view text{};
    std::string_view unit{};
    std::span<const std::uint8_t> children{};
    std::int64_t value{};
    std::int64_t minimum{};
    std::int64_t maximum{};
    std::int64_t step{};
    std::uint8_t parent{};
    std::uint8_t type{};
    std::uint8_t decimals{};
    std::uint8_t timeout{};
    std::uint8_t string_limit{};
    bool hidden{};

    [[nodiscard]] constexpr bool numeric() const noexcept {
        return (type <= CRSF_INT32) || (type == CRSF_FLOAT);
    }
    [[nodiscard]] constexpr bool editable() const noexcept {
        return numeric() || (type == CRSF_TEXT_SELECTION) ||
               ((type == CRSF_STRING) && (text.size() < maximum_value_bytes));
    }
};

namespace detail {
class Reader {
  public:
    constexpr explicit Reader(const std::span<const std::uint8_t> bytes) noexcept : bytes_(bytes) {}
    [[nodiscard]] constexpr std::uint8_t byte() noexcept {
        if (position_ == bytes_.size()) {
            valid_ = false;
            return 0;
        }
        return bytes_[position_++];
    }
    [[nodiscard]] constexpr std::uint32_t integer(const unsigned width) noexcept {
        std::uint32_t value{};
        for (unsigned i = 0; i < width; ++i)
            value = (value << 8) | byte();
        return value;
    }
    [[nodiscard]] constexpr std::string_view text() noexcept {
        const auto start = position_;
        while ((position_ < bytes_.size()) && (bytes_[position_] != 0))
            ++position_;
        if (position_ == bytes_.size()) {
            valid_ = false;
            return {};
        }
        const auto size = position_++ - start;
        // byte and character views have the same representation.
        return {reinterpret_cast<const char *>(bytes_.data() + start), size};
    }
    [[nodiscard]] constexpr bool valid() const noexcept { return valid_; }
    [[nodiscard]] constexpr bool remaining() const noexcept { return position_ < bytes_.size(); }
    [[nodiscard]] constexpr std::size_t position() const noexcept { return position_; }

  private:
    std::span<const std::uint8_t> bytes_;
    std::size_t position_{};
    bool valid_{true};
};

[[nodiscard]] constexpr std::int64_t signed_integer(const std::uint32_t value,
                                                    const unsigned width) noexcept {
    if (width == 1)
        return std::bit_cast<std::int8_t>(static_cast<std::uint8_t>(value));
    if (width == 2)
        return std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(value));
    return std::bit_cast<std::int32_t>(value);
}
} // namespace detail

[[nodiscard]] inline std::optional<Parameter> parse(const std::span<const std::uint8_t> bytes,
                                                    const bool legacy_folder = false) noexcept {
    detail::Reader reader{bytes};
    Parameter result{};
    result.parent = reader.byte();
    const auto type = reader.byte();
    result.type = type & CRSF_FIELD_TYPE_MASK;
    result.hidden = (type & CRSF_FIELD_HIDDEN) != 0;
    result.name = reader.text();
    result.step = 1;
    if (result.type <= CRSF_INT32) {
        const unsigned width = 1U << (result.type / 2U);
        const bool signed_value = (result.type & 1U) != 0;
        const auto number = [&]() noexcept -> std::int64_t {
            const auto value = reader.integer(width);
            return signed_value ? detail::signed_integer(value, width) : std::int64_t{value};
        };
        result.value = number();
        result.minimum = number();
        result.maximum = number();
        (void)number();
        if (reader.remaining())
            result.unit = reader.text();
    } else {
        switch (result.type) {
        case CRSF_FLOAT:
            result.value = detail::signed_integer(reader.integer(4), 4);
            result.minimum = detail::signed_integer(reader.integer(4), 4);
            result.maximum = detail::signed_integer(reader.integer(4), 4);
            (void)reader.integer(4);
            result.decimals = reader.byte();
            result.step = detail::signed_integer(reader.integer(4), 4);
            if (reader.remaining())
                result.unit = reader.text();
            if ((result.decimals > 9) || (result.step <= 0))
                return {};
            break;
        case CRSF_TEXT_SELECTION: {
            result.text = reader.text();
            result.value = reader.byte();
            result.minimum = reader.byte();
            result.maximum = reader.byte();
            (void)reader.byte();
            std::size_t options = 1;
            for (const auto character : result.text)
                options += character == ';' ? 1U : 0U;
            if (result.maximum >= static_cast<std::int64_t>(options))
                return {};
            if (reader.remaining())
                result.unit = reader.text();
            break;
        }
        case CRSF_STRING:
            result.text = reader.text();
            result.string_limit = reader.byte();
            if (result.text.size() > result.string_limit)
                return {};
            break;
        case CRSF_INFO:
            result.text = reader.text();
            break;
        case CRSF_COMMAND:
            result.value = reader.byte();
            result.timeout = reader.byte();
            if (reader.remaining())
                result.text = reader.text();
            if (result.value > 6)
                return {};
            break;
        case CRSF_FOLDER: {
            if (legacy_folder && !reader.remaining())
                break;
            const auto start = reader.position();
            while (reader.remaining() && (reader.byte() != 0xFF)) {
            }
            if ((reader.position() == start) || (bytes[reader.position() - 1] != 0xFF))
                return {};
            result.children = bytes.subspan(start, reader.position() - start - 1);
            break;
        }
        default:
            break;
        }
    }
    if (!reader.valid() || ((result.numeric() || (result.type == CRSF_TEXT_SELECTION)) &&
                            ((result.minimum > result.maximum) || (result.value < result.minimum) ||
                             (result.value > result.maximum))))
        return {};
    return result;
}

struct Device {
    std::array<char, 24> name{};
    TimestampUs seen{};
    std::uint8_t address{};
    std::uint8_t parameters{};
    std::uint8_t name_length{};
};

// Capacity and remote-link timing of a client; the firmware derives them from its CMake options.
struct Limits {
    // IDs 0..parameter_ids-1 fit the tree tables; each ID costs five bytes of RAM.
    std::size_t parameter_ids{256};
    std::size_t cache_bytes{1280};
    // Read timeout and repetitions for devices behind the RF link; the TX module keeps 500 ms / 2.
    std::uint32_t remote_timeout_ms{1000};
    std::uint8_t remote_retries{9};
};

template <Limits L> class BasicClient {
  public:
    inline static constexpr std::size_t maximum_devices = 4;
    inline static constexpr std::size_t cache_slots = 16;
    inline static constexpr std::size_t parameter_ids = L.parameter_ids;
    inline static constexpr std::size_t cache_bytes = L.cache_bytes;
    inline static constexpr std::size_t maximum_parameter_bytes = 256;
    inline static constexpr std::size_t maximum_chunks_bytes = 512;
    static_assert((parameter_ids > 0) && (parameter_ids <= 256), "CRSF IDs are one byte");
    static_assert(cache_bytes >= maximum_parameter_bytes, "the cache must hold one parameter");

    void open(const TimestampUs now) noexcept {
        close();
        mode_ = Mode::devices;
        set_error(Error::none);
        device_count_ = 0;
        ping_at_ = now;
        ping_sent_ = false;
    }
    void close() noexcept {
        mode_ = Mode::closed;
        suspended_mode_ = Mode::closed;
        set_error(Error::none);
        waiting_ = false;
        write_pending_ = false;
        writing_ = false;
        command_ = Command::idle;
        pinned_ = 0;
        retry_ = 0;
        chunk_ = 0;
        assembly_size_ = 0;
    }
    void suspend() noexcept {
        const auto previous = mode_;
        close();
        suspended_mode_ = previous;
    }
    void resume(const TimestampUs now) noexcept {
        const auto previous = suspended_mode_;
        suspended_mode_ = Mode::closed;
        if (previous == Mode::closed) {
            open(now);
            return;
        }
        mode_ = previous;
        if (mode_ == Mode::devices) {
            ping_at_ = now;
            ping_sent_ = false;
        } else
            refresh_folder();
    }
    [[nodiscard]] Mode view_mode() const noexcept {
        return mode_ == Mode::closed ? suspended_mode_ : mode_;
    }
    void transport_lost() noexcept {
        if (mode_ != Mode::closed)
            set_error(Error::link);
        waiting_ = false;
        write_pending_ = false;
        writing_ = false;
        command_ = Command::idle;
    }
    void reset_stream() noexcept {
        if (waiting_ && !writing_) {
            chunk_ = 0;
            assembly_size_ = 0;
            waiting_ = false;
        }
    }
    [[nodiscard]] Mode mode() const noexcept { return mode_; }
    [[nodiscard]] Error error() const noexcept { return error_; }
    [[nodiscard]] std::optional<std::uint8_t> error_parameter() const noexcept {
        return error_parameter_;
    }
    [[nodiscard]] Command command() const noexcept { return command_; }
    [[nodiscard]] bool pending_write() const noexcept { return writing_ || write_pending_; }
    [[nodiscard]] std::size_t device_count() const noexcept { return device_count_; }
    [[nodiscard]] const Device &device(const std::size_t index) const noexcept {
        return devices_[index];
    }
    [[nodiscard]] const Device *selected_device() const noexcept {
        if (mode_ != Mode::parameters)
            return nullptr;
        for (std::size_t i = 0; i < device_count_; ++i)
            if (devices_[i].address == destination_)
                return &devices_[i];
        return nullptr;
    }
    [[nodiscard]] std::uint8_t folder() const noexcept { return focus_; }
    [[nodiscard]] std::uint8_t parent() const noexcept { return parent_[focus_]; }
    void pin(const std::uint8_t id) noexcept { pinned_ = id; }
    [[nodiscard]] std::optional<Parameter> parameter(const std::uint8_t id) const noexcept {
        for (const auto &slot : slots_)
            if ((slot.length != 0) && (slot.id == id))
                return parse(std::span{pool_}.subspan(slot.offset, slot.length));
        return {};
    }
    [[nodiscard]] bool failed(const std::uint8_t id) const noexcept {
        return flags_[id] == failed_flag;
    }
    [[nodiscard]] std::size_t count() const noexcept {
        std::size_t count{};
        for (unsigned id = first_[focus_], visited = 0; (id != 0) && (visited < 255);
             id = next_[id], ++visited)
            count += (flags_[id] & hidden_flag) == 0 ? 1U : 0U;
        return count;
    }
    [[nodiscard]] std::uint8_t item(std::size_t index) const noexcept {
        for (unsigned id = first_[focus_], visited = 0; (id != 0) && (visited < 255);
             id = next_[id], ++visited) {
            if ((flags_[id] & hidden_flag) != 0)
                continue;
            if (index-- == 0)
                return static_cast<std::uint8_t>(id);
        }
        return 0;
    }
    [[nodiscard]] std::optional<std::uint8_t> index(const std::uint8_t wanted) const noexcept {
        std::uint8_t index{};
        for (unsigned id = first_[focus_], visited = 0; (id != 0) && (visited < 255);
             id = next_[id], ++visited) {
            if ((flags_[id] & hidden_flag) == 0) {
                if (id == wanted)
                    return index;
                ++index;
            }
        }
        return {};
    }
    void window(const std::size_t first) noexcept {
        window_first_ = static_cast<std::uint8_t>(first);
        for (std::size_t i = 0; i < window_.size(); ++i)
            window_[i] = item(first + i);
    }
    [[nodiscard]] bool busy() const noexcept {
        return (mode_ != Mode::closed) && (error_ != Error::link) &&
               ((mode_ == Mode::devices) || waiting_ || write_pending_ || writing_ ||
                (command_ != Command::idle) || (needed() != 0xFFFF));
    }
    void select(const std::size_t index) noexcept {
        if (error_ == Error::link)
            return;
        if (index >= device_count_) {
            set_error(Error::malformed);
            return;
        }
        destination_ = devices_[index].address;
        total_ = devices_[index].parameters;
        mode_ = Mode::parameters;
        focus_ = 0;
        legacy_ = false;
        reset_tree();
    }
    void enter(const std::uint8_t id) noexcept {
        if ((mode_ != Mode::parameters) || (writing_ || write_pending_) ||
            (command_ != Command::idle) || (error_ == Error::link) || (id >= parameter_ids))
            return;
        focus_ = id;
        window(0);
        if (waiting_ && (loading_ != id) && (parent_[loading_] != id)) {
            waiting_ = false;
            chunk_ = 0;
            assembly_size_ = 0;
        }
    }
    void refresh() noexcept {
        if (writing_ || write_pending_ || (command_ != Command::idle) || (error_ == Error::link))
            return;
        const auto parent = parent_[focus_];
        // Definitions can change hidden fields, option lists and folder membership.
        // Rebuild the tree, with the displayed folder serviced before the root/BFS.
        reset_tree();
        parent_[focus_] = parent;
    }
    void refresh_folder() noexcept {
        if ((mode_ != Mode::parameters) || writing_ || write_pending_ ||
            (command_ != Command::idle) || (error_ == Error::link))
            return;
        waiting_ = false;
        chunk_ = 0;
        retry_ = 0;
        assembly_size_ = 0;
        set_error(Error::none);
        folder_only_ = true;
        for (unsigned id = 1; id < parent_.size(); ++id)
            if (member(static_cast<std::uint8_t>(id)) && (parent_[id] == focus_))
                invalidate_parameter(static_cast<std::uint8_t>(id));
        if (!(legacy_ && (focus_ == 0)))
            invalidate_parameter(focus_);
    }

    [[nodiscard]] bool write(const std::uint8_t id, const std::int64_t value) noexcept {
        if (error_ == Error::link)
            return false;
        const auto entry = parameter(id);
        if (!entry || (!entry->numeric() && (entry->type != CRSF_TEXT_SELECTION)) ||
            (value < entry->minimum) || (value > entry->maximum))
            return reject(Error::malformed, id);
        const unsigned width = entry->type == CRSF_TEXT_SELECTION ? 1U
                               : entry->type == CRSF_FLOAT        ? 4U
                                                                  : 1U << (entry->type / 2U);
        std::array<std::uint8_t, 4> data{};
        const auto raw = static_cast<std::uint32_t>(value);
        for (unsigned i = 0; i < width; ++i)
            data[i] = static_cast<std::uint8_t>(raw >> ((width - i - 1U) * 8U));
        return queue_write(id, std::span{data}.first(width), false);
    }
    [[nodiscard]] bool write(const std::uint8_t id, const std::string_view value) noexcept {
        if (error_ == Error::link)
            return false;
        const auto entry = parameter(id);
        if (!entry || (entry->type != CRSF_STRING) || (value.size() > entry->string_limit) ||
            (value.size() >= maximum_value_bytes))
            return reject(Error::capacity, id);
        std::array<std::uint8_t, maximum_value_bytes> data{};
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (value[i] == '\0')
                return reject(Error::malformed, id);
            data[i] = static_cast<std::uint8_t>(value[i]);
        }
        return queue_write(id, std::span{data}.first(value.size() + 1), false);
    }
    [[nodiscard]] bool start_command(const std::uint8_t id, const TimestampUs now = {}) noexcept {
        const auto entry = parameter(id);
        if (error_ == Error::link)
            return false;
        if (!entry || (entry->type != CRSF_COMMAND) || (command_ != Command::idle))
            return reject(Error::malformed, id);
        if ((entry->value == 2) || (entry->value == 3)) {
            waiting_ = false;
            chunk_ = 0;
            retry_ = 0;
            assembly_size_ = 0;
            loading_ = id;
            command_id_ = id;
            command_updated_at_ = now;
            command_polled_at_ = now;
            set_error(Error::none);
            command_poll_interval_ = entry->timeout == 0
                                         ? 250'000_us
                                         : DurationUs{std::uint32_t{entry->timeout} * 100'000U};
            command_ = entry->value == 3 ? Command::confirmation : Command::running;
            return true;
        }
        if (entry->value != 0)
            return reject(Error::verification, id);
        if (!command_write(id, 1))
            return false;
        command_ = Command::starting;
        return true;
    }
    [[nodiscard]] bool confirm_command() noexcept {
        if (error_ == Error::link)
            return false;
        if (command_ != Command::confirmation)
            return reject(Error::malformed);
        if (!command_write(command_id_, 4))
            return false;
        command_ = Command::starting;
        return true;
    }
    [[nodiscard]] bool cancel_command() noexcept {
        if (error_ == Error::link)
            return false;
        if (command_ == Command::idle)
            return reject(Error::malformed);
        if (!command_write(command_id_, 5))
            return false;
        command_ = Command::cancelling;
        return true;
    }
    [[nodiscard]] std::uint8_t command_id() const noexcept { return command_id_; }

    void service(const TimestampUs now) noexcept {
        if ((mode_ == Mode::closed) || (error_ == Error::link))
            return;
        if ((mode_ == Mode::devices) && ping_sent_ && ((now - ping_at_) >= 1'000'000_us)) {
            ping_sent_ = false;
            for (std::size_t i = 0; i < device_count_;) {
                if ((now - devices_[i].seen) >= 3'000'000_us)
                    devices_[i] = devices_[--device_count_];
                else
                    ++i;
            }
        }
        if (waiting_ && ((now - requested_at_) >= timeout())) {
            waiting_ = false;
            // An unacknowledged value write is not sent again; its parameter is read back
            // instead (same timeout and retries as reads) and the reply verifies the value.
            if ((!writing_ || (command_ == Command::idle)) && (retry_ < retries())) {
                ++retry_;
            } else if (!writing_ && (loading_ == 0) && !legacy_ && (total_ >= parameter_ids)) {
                finish_error(Error::capacity);
            } else if (!writing_ && (loading_ == 0) && !legacy_) {
                legacy_ = true;
                legacy_next_ = 1;
                flags_[0] = loaded_flag | CRSF_FOLDER;
                first_[0] = 0;
                chunk_ = 0;
                retry_ = 0;
                assembly_size_ = 0;
            } else {
                const bool written = writing_;
                const auto id = loading_;
                if (!writing_)
                    flags_[loading_] = failed_flag;
                if (legacy_ && !writing_ && (loading_ == legacy_next_))
                    ++legacy_next_;
                writing_ = false;
                command_ = Command::idle;
                chunk_ = 0;
                retry_ = 0;
                assembly_size_ = 0;
                if (written)
                    refresh();
                set_error(Error::timeout, id);
            }
        }
        if (((command_ == Command::running) || (command_ == Command::cancelling)) && !waiting_ &&
            !write_pending_ && (chunk_ == 0) &&
            ((now - command_polled_at_) >= command_poll_interval_)) {
            if ((now - command_updated_at_) >= 8'000'000_us) {
                set_error(Error::timeout, command_id_);
                command_ = Command::idle;
            } else
                (void)command_write(command_id_, 6);
        }
    }

    // Called only when the UART DMA is idle; the supplied buffer is then owned by DMA.
    [[nodiscard]] std::size_t next_frame(const TimestampUs now, Frame &frame) noexcept {
        service(now);
        if ((mode_ == Mode::closed) || (error_ == Error::link) || waiting_)
            return 0;
        if (mode_ == Mode::devices) {
            if (ping_sent_)
                return 0;
            ping_sent_ = true;
            ping_at_ = now;
            return encode(frame, CRSF_FRAMETYPE_DEVICE_PING, CRSF_ADDRESS_BROADCAST, {});
        }
        if (write_pending_) {
            write_pending_ = false;
            writing_ = true;
            waiting_ = true;
            requested_at_ = now;
            command_polled_at_ = now;
            frame[5] = loading_;
            std::copy_n(write_data_.begin(), write_size_, frame.begin() + 6);
            return encode(frame, CRSF_FRAMETYPE_PARAMETER_WRITE, destination_,
                          std::span{frame}.subspan(5, write_size_ + 1U));
        }
        if (!writing_ && (command_ == Command::idle)) {
            const auto id = (retry_ != 0) || (chunk_ != 0) ? std::uint16_t{loading_} : needed();
            if (id == 0xFFFF)
                return 0;
            loading_ = static_cast<std::uint8_t>(id);
        } else if ((chunk_ == 0) && ((retry_ == 0) || (command_ != Command::idle)))
            return 0;
        frame[5] = loading_;
        frame[6] = chunk_;
        waiting_ = true;
        requested_at_ = now;
        return encode(frame, CRSF_FRAMETYPE_PARAMETER_READ, destination_,
                      std::span{frame}.subspan(5, 2));
    }
    [[nodiscard]] std::size_t next_frame_size() const noexcept {
        if ((mode_ == Mode::closed) || (error_ == Error::link) || waiting_)
            return 0;
        if (mode_ == Mode::devices)
            return ping_sent_ ? 0 : 6;
        if (write_pending_)
            return write_size_ + 7U;
        return ((!writing_ && (command_ == Command::idle) && (needed() != 0xFFFF)) ||
                (chunk_ != 0) || (retry_ != 0))
                   ? 8
                   : 0;
    }
    void defer_frame() noexcept {
        waiting_ = false;
        if (mode_ == Mode::devices)
            ping_sent_ = false;
        else if (writing_) {
            writing_ = false;
            write_pending_ = true;
        }
    }

    void on_frame(const std::span<const std::uint8_t> frame, const TimestampUs now) noexcept {
        if ((mode_ == Mode::closed) || (error_ == Error::link) || (frame.size() < 6) ||
            ((frame[3] != CRSF_ADDRESS_RADIO_TRANSMITTER) && (frame[3] != CRSF_ADDRESS_BROADCAST)))
            return;
        const auto data = frame.subspan(5, frame.size() - 6);
        if ((frame[2] == CRSF_FRAMETYPE_DEVICE_INFO) && (mode_ == Mode::devices)) {
            observe_device(frame[4], data, now);
            return;
        }
        const bool command_update = (frame[2] == CRSF_FRAMETYPE_PARAMETER_SETTINGS_ENTRY) &&
                                    (command_ != Command::idle) &&
                                    (command_ != Command::starting) && !write_pending_;
        if ((mode_ != Mode::parameters) || (frame[4] != destination_) || data.empty() ||
            (data[0] != loading_) || (!waiting_ && !command_update))
            return;
        if ((frame[2] == CRSF_FRAMETYPE_PARAMETER_WRITE) && writing_ &&
            (command_ == Command::idle)) {
            if ((data.size() != (write_size_ + 1U)) ||
                !std::equal(data.begin() + 1, data.end(), write_data_.begin())) {
                written(Error::verification);
            } else
                written();
            return;
        }
        if ((frame[2] != CRSF_FRAMETYPE_PARAMETER_SETTINGS_ENTRY) || (data.size() < 2))
            return;
        if ((chunk_ != 0) && (data[1] != remaining_))
            return;
        const auto payload = data.subspan(2);
        if ((payload.size() > (assembly_.size() - assembly_size_)) ||
            ((chunk_ == 255) && (data[1] != 0))) {
            finish_error(Error::capacity);
            return;
        }
        std::copy(payload.begin(), payload.end(), assembly_.begin() + assembly_size_);
        assembly_size_ = static_cast<std::uint16_t>(assembly_size_ + payload.size());
        waiting_ = false;
        retry_ = 0;
        if (data[1] != 0) {
            ++chunk_;
            remaining_ = static_cast<std::uint8_t>(data[1] - 1U);
            return;
        }
        const auto bytes = std::span{assembly_}.first(assembly_size_);
        if ((bytes.size() >= 2) && ((bytes[1] & CRSF_FIELD_TYPE_MASK) == CRSF_OUT_OF_RANGE) &&
            !writing_ && ((loading_ == 0) || legacy_)) {
            if ((loading_ != 0) && (member(loading_) || (loading_ == focus_))) {
                finish_error(Error::malformed);
                return;
            }
            if (total_ >= parameter_ids) {
                finish_error(Error::capacity);
                return;
            }
            flags_[0] = loaded_flag | CRSF_FOLDER;
            legacy_next_ = loading_ == 0 ? 1U : std::uint16_t{total_} + 1U;
            legacy_ = true;
            chunk_ = 0;
            assembly_size_ = 0;
            return;
        }
        const auto entry = parse(bytes, legacy_);
        if (!entry) {
            finish_error(Error::malformed);
            return;
        }
        if (writing_ || command_update) {
            if (command_ != Command::idle) {
                if (entry->type != CRSF_COMMAND) {
                    finish_error(Error::verification);
                    return;
                }
                command_status(*entry, bytes, now);
            } else {
                // Older ELRS/TBS implementations acknowledge values with a full entry.
                if (!matches_write(*entry)) {
                    written(Error::verification);
                    return;
                }
                written();
            }
            return;
        }
        if ((loading_ == 0) && (entry->type != CRSF_FOLDER)) {
            finish_error(Error::malformed);
            return;
        }
        if (!legacy_ && (loading_ != 0) &&
            ((entry->parent == loading_) || (entry->parent == 0xFF))) {
            finish_error(Error::malformed);
            return;
        }
        if ((!legacy_ && (loading_ != 0) && (entry->parent >= parameter_ids)) ||
            std::any_of(entry->children.begin(), entry->children.end(),
                        [](const auto child) { return child >= parameter_ids; })) {
            finish_error(Error::capacity);
            return;
        }
        if (legacy_) {
            const auto parent = entry->parent == 0xFF ? std::uint8_t{0} : entry->parent;
            if ((parent == loading_) || (parent > total_)) {
                finish_error(Error::malformed);
                return;
            }
            for (unsigned ancestor = parent, depth = 0; ancestor != 0; ++depth) {
                if ((ancestor == loading_) || (depth == 255)) {
                    finish_error(Error::malformed);
                    return;
                }
                ancestor = parent_[ancestor];
            }
            parent_[loading_] = parent;
            append_child(parent, loading_);
            if (loading_ == legacy_next_)
                ++legacy_next_;
        } else if ((loading_ != 0) && member(loading_) && (entry->parent != parent_[loading_])) {
            finish_error(Error::malformed);
            return;
        }
        if ((loading_ != 0) && !member(loading_))
            parent_[loading_] = entry->parent;
        if (entry->type == CRSF_FOLDER) {
            if (!set_children(loading_, entry->children)) {
                finish_error(Error::malformed);
                return;
            }
            enqueue(loading_);
        }
        flags_[loading_] =
            static_cast<std::uint8_t>(loaded_flag | (entry->hidden ? hidden_flag : 0U) |
                                      (entry->type < 31 ? entry->type : 31));
        if (!cache(loading_, bytes, *entry)) {
            finish_error(Error::capacity);
            return;
        }
        if (!entry->editable() && (entry->type != CRSF_INFO) && (entry->type != CRSF_FOLDER) &&
            (entry->type != CRSF_COMMAND))
            set_error(Error::unsupported, loading_);
        chunk_ = 0;
        assembly_size_ = 0;
        window(window_first_);
    }

  private:
    struct Slot {
        std::uint16_t offset{};
        std::uint16_t length{};
        std::uint8_t id{};
    };
    inline static constexpr std::uint8_t loaded_flag = 0x20;
    inline static constexpr std::uint8_t hidden_flag = 0x40;
    inline static constexpr std::uint8_t failed_flag = loaded_flag | 31;

    void set_error(const Error error, const std::optional<std::uint8_t> parameter = {}) noexcept {
        error_ = error;
        error_parameter_ = error == Error::none ? std::nullopt : parameter;
    }
    [[nodiscard]] bool reject(const Error error,
                              const std::optional<std::uint8_t> parameter = {}) noexcept {
        set_error(error, parameter);
        return false;
    }
    // Parameter read timeouts. Only the TX module answers over the wire: 0.5 s per request,
    // 3 requests. Every other device (receiver, ...) answers over the RF link, where replies wait
    // for telemetry slots and are often lost: each request is repeated after
    // Limits::remote_timeout_ms (firmware: CORETX_CRSF_REMOTE_TIMEOUT_MS, default 1 s), up to
    // Limits::remote_retries times (CORETX_CRSF_REMOTE_RETRIES, default 9), so a missing reply is
    // reported only after 10 unanswered requests (10 s).
    // Late replies to an earlier request are still accepted. Writes are never repeated: a missing
    // write acknowledgement starts read-backs of the parameter with the same timeout and retries,
    // and the read value must match the written one.
    [[nodiscard]] bool remote() const noexcept {
        return destination_ != CRSF_ADDRESS_CRSF_TRANSMITTER;
    }
    [[nodiscard]] DurationUs timeout() const noexcept {
        return remote() ? DurationUs{L.remote_timeout_ms * 1'000U} : 500'000_us;
    }
    [[nodiscard]] std::uint8_t retries() const noexcept {
        return remote() ? L.remote_retries : std::uint8_t{2};
    }
    void reset_tree() noexcept {
        folder_only_ = false;
        flags_.fill(0);
        first_.fill(0);
        next_.fill(0);
        parent_.fill(0);
        members_.fill(0);
        slots_.fill({});
        pinned_ = 0;
        window_.fill(0);
        pool_size_ = 0;
        queue_head_ = 0;
        queue_size_ = 0;
        window_first_ = 0;
        waiting_ = false;
        retry_ = 0;
        chunk_ = 0;
        assembly_size_ = 0;
        set_error(Error::none);
        if (legacy_) {
            flags_[0] = loaded_flag | CRSF_FOLDER;
            legacy_next_ = 1;
        }
        enqueue(0);
    }
    [[nodiscard]] bool member(const std::uint8_t id) const noexcept {
        return (members_[id / 8U] & (1U << (id % 8U))) != 0;
    }
    void mark_member(const std::uint8_t id) noexcept {
        members_[id / 8U] |= static_cast<std::uint8_t>(1U << (id % 8U));
    }
    void append_child(const std::uint8_t folder, const std::uint8_t id) noexcept {
        if (member(id))
            return;
        mark_member(id);
        if (first_[folder] == 0)
            first_[folder] = id;
        else {
            auto last = first_[folder];
            for (unsigned visited = 0; (next_[last] != 0) && (visited < 255); ++visited)
                last = next_[last];
            next_[last] = id;
        }
    }
    [[nodiscard]] bool set_children(const std::uint8_t folder,
                                    const std::span<const std::uint8_t> children) noexcept {
        std::array<std::uint8_t, (parameter_ids + 7) / 8> seen{};
        for (const auto child : children) {
            if ((child == 0) || (child >= parameter_ids) || (child == folder) ||
                ((seen[child / 8U] & (1U << (child % 8U))) != 0) ||
                (member(child) && (parent_[child] != folder)))
                return false;
            seen[child / 8U] |= static_cast<std::uint8_t>(1U << (child % 8U));
            auto ancestor = folder;
            for (unsigned depth = 0; ancestor != 0; ++depth) {
                if ((ancestor == child) || (depth == 255))
                    return false;
                ancestor = parent_[ancestor];
            }
        }
        if (!legacy_) {
            first_[folder] = 0;
            std::uint8_t previous{};
            for (const auto child : children) {
                if (folder_only_ && (folder == focus_))
                    invalidate_parameter(child);
                mark_member(child);
                parent_[child] = folder;
                if (previous == 0)
                    first_[folder] = child;
                else
                    next_[previous] = child;
                next_[child] = 0;
                previous = child;
            }
        }
        return true;
    }
    void enqueue(const std::uint8_t id) noexcept {
        for (unsigned i = 0; i < queue_size_; ++i)
            if (queue_[(queue_head_ + i) % queue_.size()] == id)
                return;
        if (queue_size_ == queue_.size()) {
            set_error(Error::capacity, id);
            return;
        }
        queue_[(queue_head_ + queue_size_) % queue_.size()] = id;
        ++queue_size_;
    }
    [[nodiscard]] std::uint16_t missing(const std::uint8_t folder) const noexcept {
        if ((flags_[folder] & loaded_flag) == 0)
            return folder;
        if ((flags_[folder] & 31U) != CRSF_FOLDER)
            return 0xFFFF;
        for (unsigned id = first_[folder], visited = 0; (id != 0) && (visited < 255);
             id = next_[id], ++visited)
            if ((flags_[id] & loaded_flag) == 0)
                return static_cast<std::uint16_t>(id);
        return 0xFFFF;
    }
    [[nodiscard]] std::uint16_t needed() const noexcept {
        if ((mode_ != Mode::parameters) || writing_ || write_pending_ ||
            (command_ != Command::idle))
            return 0xFFFF;
        if ((flags_[focus_] & loaded_flag) == 0)
            return focus_;
        if (!failed(focus_) && !(legacy_ && (focus_ == 0)) && !parameter(focus_))
            return focus_;
        for (const auto id : window_)
            if ((id != 0) && !failed(id) && !parameter(id))
                return id;
        if (const auto id = missing(focus_); id != 0xFFFF)
            return id;
        if (folder_only_)
            return 0xFFFF;
        if (legacy_ && (legacy_next_ <= total_))
            return legacy_next_;
        for (unsigned i = 0; i < queue_size_; ++i) {
            const auto folder = queue_[(queue_head_ + i) % queue_.size()];
            if (const auto id = missing(folder); id != 0xFFFF)
                return id;
        }
        return 0xFFFF;
    }
    [[nodiscard]] bool protected_slot(const std::uint8_t id) const noexcept {
        return (id == focus_) || ((id == command_id_) && (command_ != Command::idle)) ||
               ((pinned_ != 0) && (id == pinned_)) ||
               (std::find(window_.begin(), window_.end(), id) != window_.end());
    }
    void evict(const std::size_t index) noexcept {
        const auto removed = slots_[index];
        if (removed.length == 0)
            return;
        std::move(pool_.begin() + removed.offset + removed.length, pool_.begin() + pool_size_,
                  pool_.begin() + removed.offset);
        pool_size_ = static_cast<std::uint16_t>(pool_size_ - removed.length);
        for (auto &slot : slots_)
            if (slot.offset > removed.offset)
                slot.offset = static_cast<std::uint16_t>(slot.offset - removed.length);
        slots_[index] = {};
    }
    void invalidate_parameter(const std::uint8_t id) noexcept {
        flags_[id] = static_cast<std::uint8_t>(flags_[id] & ~loaded_flag);
        for (std::size_t i = 0; i < slots_.size(); ++i)
            if ((slots_[i].length != 0) && (slots_[i].id == id))
                evict(i);
    }
    [[nodiscard]] bool cache(const std::uint8_t id, const std::span<const std::uint8_t> bytes,
                             const Parameter &entry) noexcept {
        const auto size = entry.type == CRSF_FOLDER
                              ? std::min(entry.name.size(), std::size_t{32}) + 4
                              : bytes.size();
        if ((entry.type != CRSF_FOLDER) && (size > maximum_parameter_bytes))
            return false;
        for (std::size_t i = 0; i < slots_.size(); ++i)
            if ((slots_[i].length != 0) && (slots_[i].id == id))
                evict(i);
        std::size_t free = slots_.size();
        while (true) {
            for (std::size_t i = 0; i < slots_.size(); ++i)
                if (slots_[i].length == 0) {
                    free = i;
                    break;
                }
            if ((free != slots_.size()) && (size <= (pool_.size() - pool_size_)))
                break;
            bool removed{};
            for (std::size_t i = 0; i < slots_.size(); ++i) {
                const auto candidate = (victim_ + i) % slots_.size();
                if ((slots_[candidate].length != 0) && !protected_slot(slots_[candidate].id)) {
                    evict(candidate);
                    victim_ = static_cast<std::uint8_t>((candidate + 1) % slots_.size());
                    removed = true;
                    break;
                }
            }
            if (!removed)
                return false;
        }
        auto output = pool_.begin() + pool_size_;
        if (entry.type == CRSF_FOLDER) {
            *output++ = entry.parent;
            *output++ =
                static_cast<std::uint8_t>(entry.type | (entry.hidden ? CRSF_FIELD_HIDDEN : 0));
            for (std::size_t i = 0; i < (size - 4); ++i)
                *output++ = static_cast<std::uint8_t>(entry.name[i]);
            *output++ = 0;
            *output = 0xFF;
        } else
            std::copy(bytes.begin(), bytes.end(), output);
        slots_[free] = {pool_size_, static_cast<std::uint16_t>(size), id};
        pool_size_ = static_cast<std::uint16_t>(pool_size_ + size);
        return true;
    }
    [[nodiscard]] bool queue_write(const std::uint8_t id, const std::span<const std::uint8_t> bytes,
                                   const bool command) noexcept {
        if (error_ == Error::link)
            return false;
        if ((mode_ != Mode::parameters) || writing_ || write_pending_ ||
            (!command && (command_ != Command::idle)))
            return reject(Error::verification, id);
        waiting_ = false;
        loading_ = id;
        assembly_size_ = 0;
        chunk_ = 0;
        retry_ = 0;
        std::copy(bytes.begin(), bytes.end(), write_data_.begin());
        write_size_ = static_cast<std::uint8_t>(bytes.size());
        write_pending_ = true;
        set_error(Error::none);
        return true;
    }
    [[nodiscard]] bool command_write(const std::uint8_t id, const std::uint8_t value) noexcept {
        if (value == 5) {
            waiting_ = false;
            writing_ = false;
            write_pending_ = false;
        }
        const std::array data{value};
        if (!queue_write(id, data, true))
            return false;
        command_id_ = id;
        return true;
    }
    [[nodiscard]] bool matches_write(const Parameter &entry) const noexcept {
        if (entry.type == CRSF_STRING) {
            if ((entry.text.size() + 1) != write_size_)
                return false;
            for (std::size_t i = 0; i < entry.text.size(); ++i)
                if (static_cast<std::uint8_t>(entry.text[i]) != write_data_[i])
                    return false;
            return true;
        }
        if (!entry.numeric() && (entry.type != CRSF_TEXT_SELECTION))
            return false;
        std::uint32_t value{};
        for (unsigned i = 0; i < write_size_; ++i)
            value = (value << 8) | write_data_[i];
        const auto decoded =
            (entry.type == CRSF_FLOAT) || ((entry.type <= CRSF_INT32) && ((entry.type & 1U) != 0))
                ? detail::signed_integer(value, write_size_)
                : std::int64_t{value};
        return decoded == entry.value;
    }
    void written(const Error error = Error::none) noexcept {
        const auto id = loading_;
        writing_ = false;
        waiting_ = false;
        refresh();
        set_error(error, id);
    }
    void command_status(const Parameter &entry, const std::span<const std::uint8_t> bytes,
                        const TimestampUs now) noexcept {
        writing_ = false;
        chunk_ = 0;
        assembly_size_ = 0;
        if (!cache(loading_, bytes, entry)) {
            finish_error(Error::capacity);
            return;
        }
        command_updated_at_ = now;
        command_poll_interval_ =
            entry.timeout == 0 ? 250'000_us : DurationUs{std::uint32_t{entry.timeout} * 100'000U};
        if (entry.value == 3)
            command_ = Command::confirmation;
        else if (entry.value == 2)
            command_ = command_ == Command::cancelling ? Command::cancelling : Command::running;
        else if (entry.value == 0) {
            command_ = Command::idle;
            refresh();
        } else {
            finish_error(Error::malformed);
        }
    }
    void finish_error(const Error error) noexcept {
        set_error(error, loading_);
        flags_[loading_] = failed_flag;
        waiting_ = false;
        writing_ = false;
        command_ = Command::idle;
        chunk_ = 0;
        retry_ = 0;
        assembly_size_ = 0;
        if (legacy_ && (loading_ == legacy_next_))
            ++legacy_next_;
    }
    void observe_device(const std::uint8_t address, const std::span<const std::uint8_t> bytes,
                        const TimestampUs now) noexcept {
        detail::Reader reader{bytes};
        const auto name = reader.text();
        if (!reader.valid() ||
            ((bytes.size() - reader.position()) < DEVICE_INFORMATION_FIXED_PAYLOAD_LENGTH) ||
            (address == CRSF_ADDRESS_BROADCAST) || (address == CRSF_ADDRESS_RADIO_TRANSMITTER)) {
            set_error(Error::malformed);
            return;
        }
        std::size_t slot{};
        while ((slot < device_count_) && (devices_[slot].address != address))
            ++slot;
        if (slot == device_count_) {
            if (device_count_ == devices_.size()) {
                set_error(Error::capacity);
                return;
            }
            ++device_count_;
        }
        auto &device = devices_[slot];
        device.address = address;
        device.parameters = bytes[reader.position() + 12];
        device.seen = now;
        device.name_length = static_cast<std::uint8_t>(std::min(name.size(), device.name.size()));
        std::copy_n(name.begin(), device.name_length, device.name.begin());
    }
    [[nodiscard]] static std::size_t encode(Frame &frame, const std::uint8_t type,
                                            const std::uint8_t destination,
                                            const std::span<const std::uint8_t> payload) noexcept {
        const auto size = payload.size() + 6;
        frame[0] = CRSF_ADDRESS_CRSF_TRANSMITTER;
        frame[1] = static_cast<std::uint8_t>(size - 2);
        frame[2] = type;
        frame[3] = destination;
        frame[4] = CRSF_ADDRESS_RADIO_TRANSMITTER;
        if (payload.data() != (frame.data() + 5))
            std::copy(payload.begin(), payload.end(), frame.begin() + 5);
        frame[size - 1] = crc8(std::span{frame}.subspan(2, size - 3));
        return size;
    }

    // Complete tree metadata survives cache eviction; links are indexed by wire parameter ID.
    std::array<std::uint8_t, parameter_ids> parent_{}; // Parent folder ID; 0 denotes the root.
    std::array<std::uint8_t, parameter_ids> first_{};  // First child ID; 0: empty child list.
    std::array<std::uint8_t, parameter_ids> next_{};   // Next sibling ID in device order; 0 ends.
    // Type and loaded/hidden/failed state; loaded does not imply a resident cache entry.
    std::array<std::uint8_t, parameter_ids> flags_{};
    // Folder IDs in BFS discovery order; completed entries remain here but are skipped.
    std::array<std::uint8_t, parameter_ids> queue_{};
    // One bit per ID: already linked into the tree, independently of its loading state.
    std::array<std::uint8_t, (parameter_ids + 7) / 8> members_{};
    // Cache directory: parameter ID, byte offset and length within pool_.
    std::array<Slot, cache_slots> slots_{};
    // Evictable raw definitions; folder child lists live in the tree links instead.
    std::array<std::uint8_t, cache_bytes> pool_{};
    // Reassembles one parameter response from its CRSF chunks before parsing/caching.
    std::array<std::uint8_t, maximum_chunks_bytes> assembly_{};
    // Write value/command payload retained for transmission and value acknowledgement checks.
    std::array<std::uint8_t, maximum_value_bytes> write_data_{};
    // Bounded discovery list, including names, addresses and device-information timestamps.
    std::array<Device, maximum_devices> devices_{};
    // Visible parameter IDs: prioritized for loading and protected from cache eviction.
    std::array<std::uint8_t, visible_rows> window_{};
    TimestampUs requested_at_{};
    TimestampUs ping_at_{};
    TimestampUs command_updated_at_{};
    TimestampUs command_polled_at_{};
    DurationUs command_poll_interval_{};
    std::uint16_t queue_head_{};
    std::uint16_t queue_size_{};
    std::uint16_t pool_size_{};
    std::uint16_t assembly_size_{};
    std::uint16_t legacy_next_{};
    std::uint8_t window_first_{};
    std::uint8_t destination_{};
    std::uint8_t device_count_{};
    std::uint8_t focus_{};
    std::uint8_t total_{};
    std::uint8_t loading_{};
    std::uint8_t chunk_{};
    std::uint8_t remaining_{};
    std::uint8_t retry_{};
    std::uint8_t write_size_{};
    std::uint8_t victim_{};
    std::uint8_t command_id_{};
    std::uint8_t pinned_{};
    Mode mode_{};
    Mode suspended_mode_{};
    Error error_{};
    std::optional<std::uint8_t> error_parameter_{};
    Command command_{};
    bool legacy_{};
    bool folder_only_{};
    bool ping_sent_{};
    bool waiting_{};
    bool writing_{};
    bool write_pending_{};
};

// The table and cache sizes are configurable; the firmware link checks the RAM budget.

} // namespace coretx::crsf::config
