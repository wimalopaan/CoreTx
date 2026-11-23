// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "coretx/features.hpp"
#include "coretx/radio/crsf_config.hpp"

namespace coretx::firmware {

// The CRSF configuration client sized and timed by the CMake options.
using CrsfClient = crsf::config::BasicClient<crsf::config::Limits{
    tuning::crsf_parameter_ids, tuning::crsf_cache_bytes, tuning::crsf_remote_timeout_ms,
    tuning::crsf_remote_retries}>;

} // namespace coretx::firmware
