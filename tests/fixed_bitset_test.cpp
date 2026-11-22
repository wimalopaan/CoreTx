// SPDX-License-Identifier: GPL-2.0-or-later
#include "coretx/utility/fixed_bitset.hpp"

#include <cstdio>
#include <cstdlib>

namespace {

template <std::size_t Count> constexpr bool mutation_and_isolation() {
    coretx::FixedBitset<Count> flags{};
    const auto &read_only = flags;
    for (std::size_t i = 0; i < Count; ++i) {
        if (flags[i]) {
            return false;
        }
        flags.set(i);
        flags.set(i);
        for (std::size_t j = 0; j < Count; ++j) {
            if ((read_only[j] != (j == i)) || (read_only[j] != flags.test(j))) {
                return false;
            }
        }
        flags.reset(i);
        flags.reset(i);
    }
    for (std::size_t i = 0; i < Count; ++i) {
        flags.set(i);
    }
    auto copy = flags;
    copy.reset(Count - 1);
    if (copy[Count - 1] || !flags[Count - 1]) {
        return false;
    }
    for (std::size_t i = 0; i < Count; ++i) {
        if (!flags[i]) {
            return false;
        }
    }
    flags.clear();
    for (std::size_t i = 0; i < Count; ++i) {
        if (flags[i]) {
            return false;
        }
    }
    flags.set(Count - 1);
    return flags[Count - 1];
}

static_assert(mutation_and_isolation<1>());
static_assert(mutation_and_isolation<8>());
static_assert(mutation_and_isolation<32>());

} // namespace

int main(int argc, char **) {
    coretx::FixedBitset<32> flags{};
    // Keep the top-bit shift dynamic for sanitizer checks.
    const auto index = static_cast<std::size_t>(argc == 1 ? 31 : 0);
    flags.set(index);
    if (!flags[index] || !mutation_and_isolation<1>() || !mutation_and_isolation<8>() ||
        !mutation_and_isolation<32>()) {
        std::fputs("FAIL: FixedBitset mutation/isolation checks\n", stderr);
        return EXIT_FAILURE;
    }
    std::puts("FixedBitset mutation/isolation checks passed");
}
