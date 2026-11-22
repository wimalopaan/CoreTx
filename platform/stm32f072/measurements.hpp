// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#define RC_STACK_PATTERN 0xA5A5A5A5

#ifndef __ASSEMBLER__
#include <concepts>
#include <cstdint>

namespace coretx::firmware {

struct StackUsage {
    std::uint32_t used_bytes{};
    bool guard_intact{};
};

template <typename Word>
    requires(std::same_as<Word, std::uint32_t> || std::same_as<Word, volatile std::uint32_t>)
[[nodiscard]] constexpr StackUsage inspect_stack(const Word *const begin,
                                                 const std::uint32_t word_count) noexcept {
    std::uint32_t untouched{};
    while ((untouched < word_count) && (begin[untouched] == RC_STACK_PATTERN)) {
        ++untouched;
    }
    return {(word_count - untouched) * 4U, untouched >= 4U};
}

} // namespace coretx::firmware
#endif
