// SPDX-License-Identifier: GPL-2.0-or-later
#include "coretx/utility/time.hpp"

#include <concepts>
#include <cstdio>

namespace {

using coretx::DurationUs;
using coretx::TimestampUs;
using namespace coretx::literals;

template <typename A, typename B>
concept Addable = requires(A a, B b) { a + b; };
template <typename A, typename B>
concept Ordered = requires(A a, B b) { a < b; };

static_assert(!std::convertible_to<std::uint32_t, TimestampUs>);
static_assert(!std::convertible_to<TimestampUs, std::uint32_t>);
static_assert(!std::convertible_to<std::uint32_t, DurationUs>);
static_assert(!std::convertible_to<DurationUs, std::uint32_t>);
static_assert(!std::convertible_to<TimestampUs, DurationUs>);
static_assert(!std::convertible_to<DurationUs, TimestampUs>);
static_assert(!Addable<TimestampUs, TimestampUs>);
static_assert(!Ordered<TimestampUs, TimestampUs>);

constexpr bool wrapped_intervals() {
    const TimestampUs start{0xFFFF'FE0CU};
    const auto wrapped = start + 500_us;
    const auto elapsed = wrapped - start;
    return (wrapped == TimestampUs{}) && (elapsed == 500_us) && (elapsed < 1_ms) &&
           ((start - (start - 250_us)) == 250_us) && ((wrapped - 500_us) == start) &&
           (((start + (750_us + 250_us)) - start) == 1_ms) && ((3U * 250_us) == 750_us) &&
           ((((start + 2'750_us) - start) % 1_ms) == 750_us);
}

static_assert(wrapped_intervals());

} // namespace

int main() {
    if (!wrapped_intervals()) {
        std::fputs("Typed time arithmetic failed\n", stderr);
        return 1;
    }
}
