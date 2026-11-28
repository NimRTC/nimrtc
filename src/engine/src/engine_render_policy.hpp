/**
 * @file src/engine/src/engine_render_policy.hpp
 * @brief The per-platform default render policy (v1-video-render-engine D2).
 *
 * ## What this file is
 *
 * The "auto" sink-backend table lives here, in the engine core, so every
 * host gets the same per-platform default without copying it.  It is the
 * render-side half of the device policy that D3 (v1-device-policy) owns
 * for capture: the two tables are deliberately **not** merged into
 * `engine_device_policy.hpp`, because a capture failure and a render
 * failure have different causes and different reasons ("no camera" vs
 * "no display").
 *
 * The file is **private** (src/engine/src/, compiled into nimrtc_engine
 * only): the table is a list of plugin ids, which the core library must
 * not know, and the video_sink module must not depend on.
 *
 * It is a plain function pair (no dependence on the engine, the plugins
 * or the core library) so the resolution rules are pinned by a
 * **deterministic** unit test rather than by driving a real renderer
 * (`src/engine/tests/test_engine_render_policy.cpp`).
 */

#pragma once

#include <string>
#include <string_view>

namespace nimrtc::engine::detail {

/** The render-side platform preference (v1-video-render-engine D2). */
struct RenderPolicy {
    /** The platform's preferred presentation backend id
     *  ("d3d11" / "metal" / "egl"; "" = no default on this platform). */
    std::string_view preferred_backend;
    /** The backend "auto" degrades to when the preferred one cannot run
     *  (always "headless" — the last rung of the ladder). */
    std::string_view fallback_backend;
};

/** The current platform's default render policy.  Never throws. */
RenderPolicy default_render_policy() noexcept;

/** Resolve "auto" down to one backend id.
 *
 *  @param policy        The platform preference.
 *  @param is_registered Registry-membership check, injected so the rule
 *                       can be unit-tested without a process-wide
 *                       registry (same shape as D3's
 *                       `resolve_auto_capture_backend`).
 *  @return The resolved backend id; "" when the policy names no
 *          preferred backend on this platform, or the preferred backend
 *          is not registered on this build.  An empty result is a
 *          *degrade*: the caller falls back to `policy.fallback_backend`
 *          and reports why.
 *
 *  Semantics mirror D3's `resolve_auto_capture_backend()`: "auto" is a
 *  **recommendation**, not a requirement.  A policy backend this build
 *  does not register is not an error — it degrades to the fallback
 *  rather than firing `kVideoSinkPluginMissing`.
 */
std::string resolve_auto_sink_backend(
    const RenderPolicy& policy,
    bool (*is_registered)(std::string_view id) noexcept) noexcept;

}  // namespace nimrtc::engine::detail
