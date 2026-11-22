// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "coretx/storage/storage.hpp"

namespace coretx::host {

enum class FileError : std::uint8_t {
    none,
    not_open,
    open,
    lock,
    type,
    size,
    read,
    write,
    sync,
    verify,
    close
};

struct FileStatus {
    FileError code{FileError::none};
    int system_error{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return code == FileError::none;
    }
};

struct LoadResult {
    FileStatus file{};
    storage::Selection selection{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return file && selection; }
};

struct SaveResult {
    FileStatus file{};
    storage::Status codec{};
    std::uint32_t generation{};
    bool written{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return file && codec; }
};

class FileStorage {
  public:
    FileStorage() noexcept = default;
    FileStorage(const FileStorage &) = delete;
    FileStorage &operator=(const FileStorage &) = delete;
    ~FileStorage();

    [[nodiscard]] FileStatus open(const char *path) noexcept;
    [[nodiscard]] FileStatus close() noexcept;
    [[nodiscard]] LoadResult load(storage::State &state) noexcept;
    [[nodiscard]] SaveResult save(const storage::State &state) noexcept;

  private:
    struct Slots {
        std::array<storage::Record, 2> bytes{};
        std::array<std::size_t, 2> sizes{};
        FileStatus status{};

        [[nodiscard]] std::span<const std::uint8_t> view(std::size_t slot) const noexcept;
    };
    [[nodiscard]] Slots read() noexcept;
    [[nodiscard]] FileStatus synchronize() noexcept;

    int descriptor_{-1};
    int directory_{-1};
};

[[nodiscard]] const char *error_message(const FileError &error) noexcept;

} // namespace coretx::host
