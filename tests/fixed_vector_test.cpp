// SPDX-License-Identifier: GPL-2.0-or-later
#include "coretx/utility/fixed_vector.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace {

struct Entry {
    int value{7};
    int tag{9};
    friend constexpr bool operator==(const Entry &, const Entry &) noexcept = default;
};

constexpr bool insertion_and_erasure() {
    coretx::FixedVector<Entry, 4> values{};
    if (!values.try_push_back({11, 1}) || !values.try_push_back({22, 2}) ||
        !values.try_insert(0, values[1])) {
        return false;
    }
    constexpr std::array expected{Entry{22, 2}, Entry{11, 1}, Entry{22, 2}};
    if (!std::equal(values.begin(), values.end(), expected.begin(), expected.end()) ||
        !values.try_insert(2, {33, 3})) {
        return false;
    }
    const auto before = values;
    if (values.try_push_back({44, 4}) || values.try_insert(5, {55, 5}) || values.try_erase(4) ||
        values.try_resize(5) ||
        !std::equal(values.begin(), values.end(), before.begin(), before.end())) {
        return false;
    }
    if (!values.try_erase(1) || !values.try_erase(2)) {
        return false;
    }
    constexpr std::array remaining{Entry{22, 2}, Entry{33, 3}};
    return std::equal(values.begin(), values.end(), remaining.begin(), remaining.end());
}

constexpr bool resizing_and_iteration() {
    coretx::FixedVector<Entry, 4> values{};
    values.resize<3>();
    values[0] = {10, 1};
    values[1] = {20, 2};
    values[2] = {30, 3};
    if (!values.try_resize(1) || !values.try_resize(3) || (values[1] != Entry{}) ||
        (values[2] != Entry{})) {
        return false;
    }
    const auto &read_only = values;
    int total{};
    for (const auto &entry : read_only) {
        total += entry.value;
    }
    if (total != 24) {
        return false;
    }
    values.clear();
    if (!values.empty() || (values.begin() != values.end()) || values.try_erase(0) ||
        values.try_insert(1, {1, 1})) {
        return false;
    }
    values.resize<4>();
    return std::all_of(values.begin(), values.end(),
                       [](const Entry &entry) { return entry == Entry{}; });
}

constexpr bool counter_boundary() {
    coretx::FixedVector<int, 255> values{};
    values.resize<254>();
    if (!values.try_push_back(42) || values.try_push_back(43) || values.try_resize(256) ||
        (values.size() != 255) || (values[254] != 42)) {
        return false;
    }
    return values.try_erase(254) && values.try_push_back(73) && (values.size() == 255) &&
           (values[254] == 73);
}

static_assert(insertion_and_erasure());
static_assert(resizing_and_iteration());
static_assert(counter_boundary());

} // namespace

int main() {
    if (!insertion_and_erasure() || !resizing_and_iteration() || !counter_boundary()) {
        std::fputs("FAIL: FixedVector mutation/bounds checks\n", stderr);
        return EXIT_FAILURE;
    }
    std::puts("FixedVector mutation/bounds checks passed");
}
