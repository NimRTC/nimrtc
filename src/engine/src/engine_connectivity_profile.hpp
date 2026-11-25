/**
 * @file src/engine/src/engine_connectivity_profile.hpp
 * @brief PRIVATE engine collaborator: the connectivity profile (D6).
 *
 * The v1.4 plan Track D / D6 connectivity profile: a named set of policy
 * defaults for STUN / TURN / TCP fallback, carried on
 * `EngineConfig::connectivity_profile` (default "auto", which is *on*).
 *
 * This header is PRIVATE: it is not installed and no public header includes
 * it (Layout Invariant 4).
 *
 * ## What the profile is — and is not
 *
 * The profile carries **policy defaults, not seam values** (spec
 * §接口约定 2): a key the host sets explicitly — the STUN server, the TURN
 * servers added through `add_turn_server()` — always wins over the
 * profile.  The profile only fills in the keys the host left unset.
 *
 * The templates live in `profiles/connectivity/connectivity-profiles.json`, which is
 * deliberately **not** on the engine's profile loading path and not
 * managed by `profiles/schema/` (spec §接口约定 3).  This file is the
 * engine-side resolution of the *same* three profiles; keeping the two in
 * lockstep is pinned by the slice's acceptance test.
 */
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace nimrtc::engine::detail {

/** The built-in default STUN server the connectivity profiles fall back
 *  to when the host set no STUN server.  A well-known public STUN server
 *  (no credentials needed). */
inline constexpr std::string_view kDefaultStunServerHost = "stun.l.google.com";
inline constexpr std::uint16_t kDefaultStunServerPort = 19302;

/** The policy defaults a connectivity profile name resolves to.
 *
 *  Mirrors one entry of `profiles/connectivity/connectivity-profiles.json`. */
struct ConnectivityProfile {
    bool stun = true;
    /** "when_available" — TURN/TLS is preferred when the build can speak
     *  it and the host asked for it; "always" — the profile insists on
     *  TURN/TLS. */
    std::string turn_tls = "when_available";
    bool tcp_fallback = true;
};

/** Resolve a connectivity profile name to its policy defaults.
 *
 *  Recognized names: "auto", "conference", "remote-desktop".  An unknown
 *  name falls back to "auto" — the policy is never the reason a session
 *  fails (D6). */
ConnectivityProfile resolve_connectivity_profile(std::string_view name) noexcept;

/** The STUN server the engine should configure, given the profile and the
 *  host's explicit STUN key.
 *
 *  An explicit host STUN server always wins (the profile carries policy
 *  defaults, not seam values); the profile's default is used only when
 *  the host set none and the profile turns STUN on.  An empty host means
 *  "no STUN" (only possible when the profile turns STUN off — no built-in
 *  profile does). */
struct StunResolution {
    std::string host;
    std::uint16_t port = 0;
};

StunResolution resolve_stun_server(std::string_view profile,
                                   std::string_view explicit_host,
                                   std::uint16_t explicit_port) noexcept;

}  // namespace nimrtc::engine::detail
