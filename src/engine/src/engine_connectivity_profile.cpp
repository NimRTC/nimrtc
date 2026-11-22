/**
 * @file src/engine/src/engine_connectivity_profile.cpp
 * @brief The connectivity profile resolution (v1.4 plan Track D / D6).
 *
 * See `engine_connectivity_profile.hpp` for what the profile is (policy
 * defaults, not seam values) and how the engine-side resolution relates
 * to `profiles/connectivity/connectivity-profiles.json`.
 */
#include "engine_connectivity_profile.hpp"

namespace nimrtc::engine::detail {

ConnectivityProfile resolve_connectivity_profile(
    std::string_view name) noexcept {
    if (name == "remote-desktop") {
        // remote-desktop: STUN on, TURN/TLS always — the control plane of
        // a remote-desktop session tolerates the extra TLS round-trip far
        // better than it tolerates a blocked UDP control connection — and
        // TCP after UDP.
        return {.stun = true, .turn_tls = "always", .tcp_fallback = true};
    }
    // "auto" and "conference" share the same defaults; any other name
    // falls back to "auto" (D6: the policy is never the reason a session
    // fails).
    return {.stun = true, .turn_tls = "when_available", .tcp_fallback = true};
}

StunResolution resolve_stun_server(std::string_view profile,
                                   std::string_view explicit_host,
                                   std::uint16_t explicit_port) noexcept {
    if (!explicit_host.empty()) {
        // The host's explicit key wins over the profile.
        return {.host = std::string(explicit_host), .port = explicit_port};
    }
    const auto p = resolve_connectivity_profile(profile);
    if (p.stun) {
        return {.host = std::string(kDefaultStunServerHost),
                .port = kDefaultStunServerPort};
    }
    // STUN turned off by the profile — no built-in profile does this, but
    // the resolution honours it.
    return {};
}

}  // namespace nimrtc::engine::detail
