/**
 * @file nimrtc/video_sync/video_sync.hpp
 * @brief The video-sync module's public entry point (v1.3 plan **Q6**).
 *
 * ## What this module is
 *
 * The **video half** of the A/V-sync contract.  The audio half shipped
 * in `v1.2.0` (audio plan WP-14, spec C-4); this module is the picture's
 * counterpart: the measured skew becomes an explicit **present / drop /
 * delay** decision.
 *
 * The policy itself is in `nimrtc/video_sync/render_clock.hpp`; this
 * header carries the module's *registration* entry point, so the
 * registry table (`src/core/src/pal_default_registrars.cpp`) can take
 * its address without including any concrete implementation.
 *
 * ## Why a module and not a field in the engine
 *
 * The engine's job is to *wire* the correction (Layout Invariant 4): it
 * measures the skew on the ADR-017 timeline and asks the clock what to
 * do.  The *policy* — the tolerance band, the drop / hold mapping, the
 * bounded-debt bookkeeping — is a pure function of the skew, so it is
 * testable without a live session, on every platform, including the CI
 * legs that have no display at all.
 *
 * @see docs/plan/v1.3-plan.md §3 Q6.
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).
#include <nimrtc/core/api_version.hpp>

namespace nimrtc::video_sync {

/**
 * @brief Populate the registry's video-sync slot.
 *
 * The entry point registers the built-in render clock under the id
 * "render-clock".  The engine resolves it through
 * `EngineConfig::video_sync_controller_name`.
 *
 * Idempotent.  Safe to call from multiple translation units.
 */
NIMRTC_API_STABLE void register_default_plugins() noexcept;

} // namespace nimrtc::video_sync
