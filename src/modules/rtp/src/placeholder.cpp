// modules/rtp/src/placeholder.cpp
// =============================================================================
// P0 placeholder. Implementation lands in P1 (Agent 1).
// This file exists so the static library has at least one translation unit,
// which CMake requires.
// =============================================================================

#include <nimrtc/core/log.hpp>
#include <nimrtc/rtp/packet.hpp>

namespace nimrtc::rtp {

// Reserved namespace for future definitions. Do not export anything here
// without NIMRTC_API once the export header is wired up.

} // namespace nimrtc::rtp
