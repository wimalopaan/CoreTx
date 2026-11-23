// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "coretx/storage/storage.hpp"
#include "platform/stm32f072/eeprom.hpp"

#include <algorithm>
#include <bit>
#include <span>

namespace coretx::firmware {

enum class ProbeSelection : std::uint8_t { wait, selected, missing, multiple };

// Selects the single EEPROM found by the bus probe; several devices are ambiguous.
template <typename Memory>
[[nodiscard]] constexpr ProbeSelection select_memory(const StorageProbeResult probe) noexcept {
    if ((probe.state == StorageProbeState::idle) || (probe.state == StorageProbeState::scanning))
        return ProbeSelection::wait;
    if (probe.state != StorageProbeState::found)
        return ProbeSelection::missing;
    if (std::popcount(probe.addresses) != 1)
        return ProbeSelection::multiple;
    Memory::select(static_cast<std::uint8_t>(StorageProbeResult::first_address +
                                             std::countr_zero(probe.addresses)));
    return ProbeSelection::selected;
}

// The EEPROM transfer shared by the record stores (calibration, radio, inputs, models).
//
// read():  reads a record page by page.
// write(): journaled write of a record slot:
//          1. invalidate - clear the first byte, so an interrupted write leaves an invalid slot,
//          2. body       - write everything behind the commit area page by page,
//          3. commit     - write the first commit_bytes (1 byte or the first page),
//          4. verify     - read the record back; it must have the CRC computed before writing.
//          Without invalidation only commit and verify run (single-page records).
// erase(): only the invalidation step.
//
// The buffer must stay alive until the transfer completes; verification reads into it, so after a
// successful write it still holds the written record.
template <typename Memory> class SlotTransfer {
  public:
    enum class Result : std::uint8_t { busy, done, failed, mismatch };

    constexpr void read(const std::uint16_t address,
                        const std::span<std::uint8_t> buffer) noexcept {
        start(Step::read, address, buffer);
    }
    constexpr void write(const std::uint16_t address, const std::span<std::uint8_t> buffer,
                         const std::size_t commit_bytes, const bool invalidate = true) noexcept {
        commit_ = static_cast<std::uint16_t>(commit_bytes);
        expected_ = coretx::storage::crc32(buffer);
        start(invalidate ? Step::invalidate : Step::commit, address, buffer);
    }
    constexpr void erase(const std::uint16_t address) noexcept {
        start(Step::invalidate, address, {});
    }
    [[nodiscard]] constexpr bool active() const noexcept { return step_ != Step::idle; }

    // Advances the transfer; failed leaves the cause in Memory::error() (none: request rejected).
    [[nodiscard]] constexpr Result service(const TimestampUs now) noexcept {
        if (pending_) {
            Memory::service(now);
            if (Memory::busy())
                return Result::busy;
            pending_ = false;
            if (Memory::error() != I2cError::none)
                return stop(Result::failed);
            switch (step_) {
            case Step::read:
            case Step::body:
            case Step::verify:
                offset_ = static_cast<std::uint16_t>(offset_ + count_);
                if (offset_ == buffer_.size()) {
                    if (step_ == Step::read)
                        return stop(Result::done);
                    if (step_ == Step::verify)
                        return stop(coretx::storage::crc32(buffer_) == expected_
                                        ? Result::done
                                        : Result::mismatch);
                    step_ = Step::commit;
                    offset_ = 0;
                }
                break;
            case Step::invalidate:
                if (buffer_.empty())
                    return stop(Result::done);
                step_ = commit_ < buffer_.size() ? Step::body : Step::commit;
                offset_ = commit_;
                break;
            case Step::commit:
                step_ = Step::verify;
                offset_ = 0;
                break;
            case Step::idle:
                return Result::done;
            }
        } else if (step_ == Step::idle)
            return Result::done;
        const auto address = static_cast<std::uint16_t>(address_ + offset_);
        count_ = static_cast<std::uint16_t>(std::min<std::size_t>(
            Memory::page_size - (address % Memory::page_size), buffer_.size() - offset_));
        bool started{};
        switch (step_) {
        case Step::read:
        case Step::verify:
            started = Memory::read(address, buffer_.subspan(offset_, count_));
            break;
        case Step::invalidate:
            started = Memory::write(address_, invalid_);
            break;
        case Step::body:
            started = Memory::write(address, buffer_.subspan(offset_, count_));
            break;
        case Step::commit:
            started = Memory::write(address_, buffer_.first(commit_));
            break;
        case Step::idle:
            break;
        }
        if (!started)
            return stop(Result::failed);
        pending_ = true;
        return Result::busy;
    }

  private:
    enum class Step : std::uint8_t { idle, read, invalidate, body, commit, verify };
    constexpr void start(const Step step, const std::uint16_t address,
                         const std::span<std::uint8_t> buffer) noexcept {
        step_ = step;
        address_ = address;
        buffer_ = buffer;
        offset_ = 0;
        pending_ = false;
    }
    [[nodiscard]] constexpr Result stop(const Result result) noexcept {
        step_ = Step::idle;
        pending_ = false;
        return result;
    }
    inline static constexpr std::array<std::uint8_t, 1> invalid_{0};
    std::span<std::uint8_t> buffer_{};
    std::uint32_t expected_{};
    std::uint16_t address_{};
    std::uint16_t offset_{};
    std::uint16_t count_{};
    std::uint16_t commit_{};
    Step step_{};
    bool pending_{};
};

} // namespace coretx::firmware
