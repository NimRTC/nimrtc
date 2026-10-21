/**
 * @file nimrtc/datachannel/datachannel.hpp
 * @brief DataChannel module — P1 placeholder.
 *
 * This header is the module-level public API for the datachannel module.
 * In P1, no concrete implementation is provided — only the plugin interface
 * in `nimrtc/plugins/datachannel.hpp` is used by consumers.
 *
 * P2: replace this header with the usrsctp-backed SCTP implementation.
 *
 * @note P1 placeholder — interface defined in nimrtc/plugins/datachannel.hpp.
 */

#pragma once

#include <nimrtc/core/api_version.hpp>   // NIMRTC_API_STABLE

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.
// P1: the module is a thin shim. The concrete IDataChannel implementation
// lives in nimrtc/plugins/datachannel.hpp (the plugin interface).
// P2: include the SCTP implementation header here.
namespace nimrtc::datachannel {

// P1: No-op namespace. All interface types are in nimrtc::plugins.
// (P2) Concrete channel types go here, e.g.:
//   class SctpDataChannel : public plugins::IDataChannel { ... };

} // namespace nimrtc::datachannel
