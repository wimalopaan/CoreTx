// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cassert>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace coretx {

template <typename T, std::size_t Capacity> class FixedVector {
    static_assert((Capacity > 0) && (Capacity <= std::numeric_limits<std::uint8_t>::max()));
    static_assert(std::is_nothrow_default_constructible_v<T> &&
                  std::is_nothrow_copy_constructible_v<T> && std::is_nothrow_copy_assignable_v<T> &&
                  std::is_nothrow_move_assignable_v<T>);
    using Parameter = std::conditional_t<(sizeof(T) <= sizeof(std::uint32_t)), T, const T &>;

  public:
    [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }
    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] constexpr auto begin() noexcept { return elements_.begin(); }
    [[nodiscard]] constexpr auto begin() const noexcept { return elements_.begin(); }
    [[nodiscard]] constexpr auto end() noexcept { return begin() + size_; }
    [[nodiscard]] constexpr auto end() const noexcept { return begin() + size_; }

    // Like standard sequence containers, indexed access requires index < size().
    [[nodiscard]] constexpr T &operator[](const std::size_t index) noexcept {
        check_index(index);
        return elements_[index];
    }
    [[nodiscard]] constexpr const T &operator[](const std::size_t index) const noexcept {
        check_index(index);
        return elements_[index];
    }

    [[nodiscard]] constexpr bool try_resize(const std::size_t count) noexcept {
        if (count > Capacity) {
            return false;
        }
        for (auto i = count; i < size(); ++i) {
            elements_[i] = {};
        }
        size_ = static_cast<std::uint8_t>(count);
        return true;
    }

    template <std::size_t Count> constexpr void resize() noexcept {
        static_assert(Count <= Capacity);
        (void)try_resize(Count);
    }

    constexpr void clear() noexcept { resize<0>(); }

    [[nodiscard]] constexpr bool try_insert(const std::size_t index,
                                            const Parameter value) noexcept {
        if ((index > size()) || (size() == Capacity)) {
            return false;
        }
        // Snapshot before shifting: value may refer to an element in this list.
        const T copy = value;
        for (auto i = size(); i > index; --i) {
            elements_[i] = elements_[i - 1];
        }
        elements_[index] = copy;
        ++size_;
        return true;
    }

    [[nodiscard]] constexpr bool try_push_back(const Parameter value) noexcept {
        return try_insert(size(), value);
    }

    [[nodiscard]] constexpr bool try_erase(const std::size_t index) noexcept {
        if (index >= size()) {
            return false;
        }
        for (auto i = index; (i + 1) < size(); ++i) {
            elements_[i] = elements_[i + 1];
        }
        elements_[--size_] = {};
        return true;
    }

  private:
    constexpr void check_index([[maybe_unused]] const std::size_t index) const noexcept {
#if defined(__STDC_HOSTED__) && !__STDC_HOSTED__ && (defined(__GNUC__) || defined(__clang__))
        // Bare-metal assertions must not pull in libc diagnostics/syscalls.
        if (index >= size()) {
            __builtin_trap();
        }
#else
        assert(index < size());
#endif
    }

    std::array<T, Capacity> elements_{};
    std::uint8_t size_{};
};

} // namespace coretx
