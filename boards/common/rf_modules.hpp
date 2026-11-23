// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "boards/common/crsf_client.hpp"
#include "coretx/features.hpp"
#include "platform/stm32f072/crsf.hpp"
#if CORETX_A7105
#include "platform/stm32f072/radio_timer.hpp"
#endif

namespace coretx::firmware {

template <typename Io, typename Assignment, bool Enabled = features::a7105>
struct InternalRFModules;

template <typename Io, typename Assignment> struct InternalRFModules<Io, Assignment, false> {
    using RadioTimer = void;
    using InternalRFModule = void;
    using Radio = void;
};

#if CORETX_A7105
template <typename Io, typename Assignment> struct InternalRFModules<Io, Assignment, true> {
    using RadioTimer = firmware::RadioTimer<Io, InterruptGuard, Assignment>;
    using InternalRFModule = TimedAfhds2a<A7105<Io, Assignment::radio>, RadioTimer>;
    using Radio = typename InternalRFModule::Radio;
};
#endif

template <typename Io, typename Assignment, bool Enabled = features::crsf>
struct ExternalRFModules {
    using ExternalRFModule = void;
};

template <typename Io, typename Assignment> struct ExternalRFModules<Io, Assignment, true> {
    using ExternalRFModule = Crsf<Io, Assignment, CrsfClient>;
};

template <typename Io, typename Assignment> struct RfModules : InternalRFModules<Io, Assignment> {
    using ExternalRFModule = typename ExternalRFModules<Io, Assignment>::ExternalRFModule;
};

} // namespace coretx::firmware
