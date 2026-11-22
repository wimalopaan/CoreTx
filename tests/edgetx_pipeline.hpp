// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "coretx/core/core.hpp"

namespace edgetx_reference {

// Host-only, single-threaded adapter. Preserves EdgeTX input state between calls.
// Only validated models in CoreTX's supported subset may be supplied.
class Pipeline {
  public:
    [[nodiscard]] coretx::Frame evaluate(const coretx::Model &model,
                                         const coretx::Controls &controls);

  private:
    std::array<std::int16_t, coretx::input_count> inputs_{};
    std::array<std::int8_t, coretx::input_count> trim_origins_{};
    std::array<bool, coretx::input_count> trim_reversed_{};
};

} // namespace edgetx_reference
