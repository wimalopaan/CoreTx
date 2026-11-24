// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

#if !defined(CORETX_A7105) || !defined(CORETX_CRSF) || !defined(CORETX_I6S) ||                     \
    !defined(CORETX_EXTENDED_INPUTS) || !defined(CORETX_MULTISWITCH) ||                            \
    !defined(CORETX_SECURE_UI) || !defined(CORETX_I2C_FAST_MODE) ||                                \
    !defined(CORETX_CRSF_REMOTE_TIMEOUT_MS) || !defined(CORETX_CRSF_REMOTE_RETRIES) ||             \
    !defined(CORETX_CRSF_PARAMETER_IDS) || !defined(CORETX_CRSF_CACHE_BYTES) ||                    \
    !defined(CORETX_I6S_TOUCH_THRESHOLD) || !defined(CORETX_MODUL_TANKMIX)
#error "Define firmware features through the coretx CMake target"
#endif

namespace coretx::firmware {
struct DisabledFeature {};
} // namespace coretx::firmware

namespace coretx::firmware::features {
inline constexpr bool a7105 = CORETX_A7105 != 0;
inline constexpr bool crsf = CORETX_CRSF != 0;
inline constexpr bool multiswitch = crsf && (CORETX_MULTISWITCH != 0);
inline constexpr bool tankmix = CORETX_MODUL_TANKMIX != 0;
inline constexpr bool i6s = CORETX_I6S != 0;
inline constexpr bool extended_inputs = CORETX_EXTENDED_INPUTS != 0;
inline constexpr bool secure_ui = CORETX_SECURE_UI != 0;
inline constexpr bool i2c_fast_mode = CORETX_I2C_FAST_MODE != 0;
#if defined(CORETX_ADC_COHERENT_SNAPSHOTS) && CORETX_ADC_COHERENT_SNAPSHOTS
inline constexpr bool coherent_adc_snapshots = true;
#else
inline constexpr bool coherent_adc_snapshots = false;
#endif
} // namespace coretx::firmware::features

// Tuning values set through CMake cache variables; see BUILDING.adoc.
namespace coretx::firmware::tuning {
inline constexpr std::uint32_t crsf_remote_timeout_ms = CORETX_CRSF_REMOTE_TIMEOUT_MS;
inline constexpr std::uint8_t crsf_remote_retries = CORETX_CRSF_REMOTE_RETRIES;
// IDs 0..crsf_parameter_ids-1 fit the CRSF tree tables; devices with higher IDs report a
// capacity error. Each ID costs five bytes of RAM, the cache one byte per byte.
inline constexpr std::uint16_t crsf_parameter_ids = CORETX_CRSF_PARAMETER_IDS;
inline constexpr std::uint16_t crsf_cache_bytes = CORETX_CRSF_CACHE_BYTES;
inline constexpr std::uint8_t i6s_touch_threshold = CORETX_I6S_TOUCH_THRESHOLD;
static_assert((crsf_remote_timeout_ms >= 100) && (crsf_remote_timeout_ms <= 10'000));
static_assert(crsf_remote_retries <= 50);
static_assert((crsf_parameter_ids >= 32) && (crsf_parameter_ids <= 256));
static_assert((crsf_cache_bytes >= 512) && (crsf_cache_bytes <= 4096));
static_assert((i6s_touch_threshold >= 10) && (i6s_touch_threshold <= 60));
} // namespace coretx::firmware::tuning
