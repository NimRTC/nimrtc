/**
 * @file src/engine/src/engine_connectivity_profile.cpp
 * @brief The connectivity profile table and the built-in STUN default.
 *
 * Plan v1.4 D6.  See `engine_connectivity_profile.hpp` for why a
 * profile carries *intents* rather than addresses.
 */
#include "engine_connectivity_profile.hpp"

#include <array>
#include <string_view>

namespace nimrtc::engine::detail {

namespace {

/** The known profiles.
 *
 *  The table is deliberately small and explicit rather than loaded from
 *  a file: the profile decides *policy*, and policy that comes from a
 *  file is policy a host can change without the engine knowing — which
 *  is the opposite of what a "profile" is for.  The JSON template under
 *  `profiles/connectivity-profiles.json` documents the same table for
 *  hosts that want to read it; this table is the one the engine obeys.
 */
struct ProfileEntry {
    std::string_view name;
    ConnectivityProfile profile;
};

constexpr std::array<ProfileEntry, 3> kProfiles{{
    // The default.  Everything on, TURN/TLS used when it is available.
    {"auto", ConnectivityProfile{true, "when_available", true}},

    // A conference call: a short-lived session between peers that are
    // usually on reasonable networks.  TURN/TLS when it is there, but
    // plain TURN/UDP is an acceptable fallback.
    {"conference", ConnectivityProfile{true, "when_available", true}},

    // A remote-desktop session: long-lived, and the path matters more
    // than the first packet.  TURN/TLS is insisted on — the transport
    // is the security boundary of the relayed path — and TCP is tried
    // when UDP does not get through.
    {"remote-desktop", ConnectivityProfile{true, "always", true}},
}};

}  // namespace

bool is_known_connectivity_profile(std::string_view name) noexcept {
    for (const auto& entry : kProfiles) {
        if (entry.name == name) return true;
    }
    return false;
}

ConnectivityProfile resolve_connectivity_profile(std::string_view name) noexcept {
    for (const auto& entry : kProfiles) {
        if (entry.name == name) return entry.profile;
    }
    // An unknown name falls back to "auto", which is the profile that
    // means "on".  The caller logs the substitution: a profile is a
    // convenience, and refusing to start a session over a typo would be
    // a worse trade than running with the default policy.
    return resolve_connectivity_profile("auto");
}

std::string_view default_stun_server() noexcept {
    // A public STUN server (RFC 8489), so that a session with no
    // explicit configuration still gathers a server-reflexive candidate.
    // A host that names its own server overrides this entirely.
    return "stun.l.google.com";
}

std::uint16_t default_stun_port() noexcept {
    // The IANA-assigned STUN port (RFC 8489 §9).  The Google STUN
    // service also listens on 19302, which is what the in-tree
    // examples use; 3478 is the port every STUN deployment agrees on,
    // so it is the one a default should carry.
    return 3478;
}

}  // namespace nimrtc::engine::detail
