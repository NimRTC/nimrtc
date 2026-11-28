/**
 * @file src/engine/src/engine_render_policy.cpp
 * @brief Implementation of the default render policy (v1-video-render-engine D2).
 */

#include "engine_render_policy.hpp"

#include <string>
#include <string_view>

namespace nimrtc::engine::detail {

RenderPolicy default_render_policy() noexcept {
#if defined(_WIN32)
    return {.preferred_backend = "d3d11", .fallback_backend = "headless"};
#elif defined(__APPLE__)
    return {.preferred_backend = "metal", .fallback_backend = "headless"};
#elif defined(__linux__)
    return {.preferred_backend = "egl", .fallback_backend = "headless"};
#else
    // No desktop platform: "auto" cannot resolve to a presentation
    // backend, so it degrades to "headless" (no-platform-backend).
    return {.preferred_backend = "", .fallback_backend = "headless"};
#endif
}

std::string resolve_auto_sink_backend(
    const RenderPolicy& policy,
    bool (*is_registered)(std::string_view id) noexcept) noexcept {
    if (policy.preferred_backend.empty()) {
        // The policy names no default on this platform.  "auto" is a
        // recommendation, not a host requirement, so it degrades to the
        // fallback instead of failing open().
        return "";
    }
    if (is_registered(policy.preferred_backend)) {
        return std::string(policy.preferred_backend);
    }
    // The policy named a backend this build does not register.  Same
    // degrade rule: "auto" is a recommendation, not a requirement.
    return "";
}

}  // namespace nimrtc::engine::detail
