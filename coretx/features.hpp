// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#if !defined(CORETX_A7105) || !defined(CORETX_CRSF) || !defined(CORETX_I6S) ||                     \
    !defined(CORETX_EXTENDED_INPUTS) || !defined(CORETX_MULTISWITCH) ||                            \
    !defined(CORETX_SECURE_UI) || !defined(CORETX_I2C_FAST_MODE)
#error "Define firmware features through the coretx CMake target"
#endif

namespace coretx::firmware {
struct DisabledFeature {};
} // namespace coretx::firmware

namespace coretx::firmware::features {
inline constexpr bool a7105 = CORETX_A7105 != 0;
inline constexpr bool crsf = CORETX_CRSF != 0;
inline constexpr bool multiswitch = crsf && (CORETX_MULTISWITCH != 0);
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
