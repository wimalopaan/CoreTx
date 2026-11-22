// SPDX-License-Identifier: GPL-2.0-or-later
#include "platform/desktop/file_storage.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace coretx::host {

FileStorage::~FileStorage() {
    if (const auto status = close(); !status) {
        std::fprintf(stderr, "Storage close: %s\n", std::strerror(status.system_error));
    }
}

FileStatus FileStorage::open(const char *const path) noexcept {
    if ((path == nullptr) || (*path == '\0')) {
        return {FileError::open, EINVAL};
    }
    if ((descriptor_ >= 0) || (directory_ >= 0)) {
        return {FileError::open, EBUSY};
    }
    const std::string_view name{path};
    const auto slash = name.find_last_of('/');
    const std::string parent{slash == std::string_view::npos ? std::string_view{"."}
                             : slash == 0                    ? std::string_view{"/"}
                                                             : name.substr(0, slash)};
    const auto basename = slash == std::string_view::npos ? path : path + slash + 1;
    directory_ = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_ < 0) {
        return {FileError::open, errno};
    }
    descriptor_ = ::openat(directory_, basename, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    FileStatus status{};
    if (descriptor_ < 0) {
        status = {FileError::open, errno};
    } else {
        struct stat info{};
        if (::fstat(descriptor_, &info) != 0) {
            status = {FileError::read, errno};
        } else if (!S_ISREG(info.st_mode)) {
            status = {FileError::type};
        } else if (::flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
            status = {FileError::lock, errno};
        } else if (info.st_size > static_cast<off_t>(2 * storage::slot_size)) {
            status = {FileError::size};
        }
    }
    if (!status) {
        if (const auto cleanup = close(); !cleanup) {
            std::fprintf(stderr, "Storage cleanup: %s\n", std::strerror(cleanup.system_error));
        }
    }
    return status;
}

FileStatus FileStorage::close() noexcept {
    FileStatus status{};
    if ((descriptor_ >= 0) && (::close(std::exchange(descriptor_, -1)) != 0)) {
        status = {FileError::close, errno};
    }
    if ((directory_ >= 0) && (::close(std::exchange(directory_, -1)) != 0) && status) {
        status = {FileError::close, errno};
    }
    return status;
}

std::span<const std::uint8_t> FileStorage::Slots::view(const std::size_t slot) const noexcept {
    return {bytes[slot].data(), sizes[slot]};
}

FileStorage::Slots FileStorage::read() noexcept {
    Slots result{};
    if (descriptor_ < 0) {
        result.status = {FileError::not_open};
        return result;
    }
    struct stat info{};
    if (::fstat(descriptor_, &info) != 0) {
        result.status = {FileError::read, errno};
        return result;
    }
    if (info.st_size > static_cast<off_t>(2 * storage::slot_size)) {
        result.status = {FileError::size};
        return result;
    }
    for (std::size_t slot = 0; slot < 2; ++slot) {
        auto &size = result.sizes[slot];
        while (size < storage::slot_size) {
            const auto count =
                ::pread(descriptor_, result.bytes[slot].data() + size, storage::slot_size - size,
                        static_cast<off_t>(slot * storage::slot_size + size));
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                result.status = {FileError::read, errno};
                return result;
            }
            if (count == 0) {
                break;
            }
            size += static_cast<std::size_t>(count);
        }
    }
    return result;
}

LoadResult FileStorage::load(storage::State &state) noexcept {
    const auto slots = read();
    if (!slots.status) {
        return {slots.status, {}};
    }
    auto selection = storage::select(slots.view(0), slots.view(1));
    if (selection) {
        const auto decoded = storage::decode(slots.view(selection.slot), state);
        if (!decoded) {
            selection.status = decoded.status;
        }
    }
    return {{}, selection};
}

FileStatus FileStorage::synchronize() noexcept {
    if ((::fsync(descriptor_) != 0) || (::fsync(directory_) != 0)) {
        return {FileError::sync, errno};
    }
    return {};
}

SaveResult FileStorage::save(const storage::State &state) noexcept {
    if (const auto status = storage::validate(state); !status) {
        return {{}, status};
    }
    const auto slots = read();
    if (!slots.status) {
        return {slots.status};
    }
    const auto selection = storage::select(slots.view(0), slots.view(1));
    if (!selection && (selection.status.code != storage::Error::empty)) {
        return {{}, selection.status};
    }
    storage::Record record{};
    std::uint32_t generation = selection ? selection.generation : 0;
    if (const auto status = storage::encode(state, generation, record); !status) {
        return {{}, status};
    }
    std::size_t target{};
    if (selection) {
        // A prior failed sync may have left a valid record only in the OS cache.
        if (const auto status = synchronize(); !status) {
            return {status};
        }
        target = 1U - selection.slot;
        const auto other_error = selection.slots[target].code;
        if (((other_error == storage::Error::none) || (other_error == storage::Error::empty)) &&
            std::equal(record.begin(), record.end(), slots.bytes[selection.slot].begin())) {
            return {{}, {}, generation, false};
        }
        ++generation;
        if (const auto status = storage::encode(state, generation, record); !status) {
            return {{}, status};
        }
    }
    std::size_t written{};
    while (written < record.size()) {
        const auto count = ::pwrite(descriptor_, record.data() + written, record.size() - written,
                                    static_cast<off_t>(target * storage::slot_size + written));
        if ((count < 0) && (errno == EINTR)) {
            continue;
        }
        if (count <= 0) {
            return {{FileError::write, count < 0 ? errno : EIO}};
        }
        written += static_cast<std::size_t>(count);
    }
    if (const auto status = synchronize(); !status) {
        return {status};
    }
    const auto verify = read();
    if (!verify.status) {
        return {verify.status};
    }
    if ((verify.sizes[target] != record.size()) || (verify.bytes[target] != record)) {
        return {{FileError::verify}};
    }
    return {{}, {}, generation, true};
}

const char *error_message(const FileError &error) noexcept {
    switch (error) {
    case FileError::none:
        return "no error";
    case FileError::not_open:
        return "storage file is not open";
    case FileError::open:
        return "cannot open storage file or parent directory";
    case FileError::lock:
        return "storage file is already in use";
    case FileError::type:
        return "storage must be a regular file";
    case FileError::size:
        return "storage file exceeds the two-slot format";
    case FileError::read:
        return "cannot read storage";
    case FileError::write:
        return "cannot write storage";
    case FileError::sync:
        return "cannot durably synchronize storage";
    case FileError::verify:
        return "storage write verification failed";
    case FileError::close:
        return "cannot close storage";
    }
    return "unknown storage file error";
}

} // namespace coretx::host
