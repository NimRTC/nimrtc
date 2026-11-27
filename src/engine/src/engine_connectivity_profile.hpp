/**
 * @file src/engine/src/engine_connectivity_profile.hpp
 * @brief The connectivity profile: a named set of traversal defaults.
 *
 * Plan v1.4 D6.  A profile is **not** a configuration value the host
 * has to spell out — it is a named set of *defaults* that the engine
 * fills in for the keys the host left alone.  The host's explicit
 * settings always win.
 *
 * ## Why the profile is a policy and not a seam value
 *
 * The obvious alternative — "the profile picks the STUN server and the
 * TURN server" — is rejected because it would put *deployment detail*
 * into a file that is supposed to describe a *kind of session*.  The
 * profile says "STUN on"; the host says which server.  The one place
 * the profile does carry a concrete address is the built-in default
 * STUN server, and that is a deliberate exception (see
 * `default_stun_server()`): a host that wants a server-reflexive
 * candidate should not have to know the string `stun.l.google.com`,
 * and a host that does know it overrides the profile anyway.
 */
#ifndef NIMRTC_ENGINE_ENGINE_CONNECTIVITY_PROFILE_HPP
#define NIMRTC_ENGINE_ENGINE_CONNECTIVITY_PROFILE_HPP

#include <cstdint>
#include <string>
#include <string_view>

namespace nimrtc::engine::detail {

/** One connectivity profile's defaults.
 *
 *  The three fields are *intents*, not values:
 *
 *    - `stun` — gather server-reflexive candidates (RFC 8489);
 *    - `turn_tls` — how eagerly to use TURN over TLS (RFC 6062);
 *    - `tcp_fallback` — try TCP after UDP fails (RFC 6544,
 *      RFC 8656 §12).
 *
 *  `turn_tls` has three spellings because "use TURN/TLS" is not a
 *  single decision: `when_available` means "use it if the credentials
 *  and the transport are there", `always` means "insist on it, and say
 *  so when it is not available", and `never` means "plain TURN/UDP is
 *  fine".
 */
struct ConnectivityProfile {
    /** Whether to gather server-reflexive candidates. */
    bool stun = true;

    /** How eagerly to use TURN over TLS: "when_available" / "always" /
     *  "never". */
    std::string_view turn_tls = "when_available";

    /** Whether to try TCP after UDP fails. */
    bool tcp_fallback = true;
};

/** The profile named by `name`, or the `"auto"` profile if the name is
 *  unknown.
 *
 *  An unknown name is **not** an error: a profile is a convenience, and
 *  a typo should not stop a session from starting.  The caller is
 *  expected to log the substitution. */
ConnectivityProfile resolve_connectivity_profile(std::string_view name) noexcept;

/** Whether `name` is a profile this build knows. */
bool is_known_connectivity_profile(std::string_view name) noexcept;

/** The built-in default STUN server.
 *
 *  A public STUN server, so that a session with no explicit
 *  configuration still gets a server-reflexive candidate.  The engine
 *  only uses it when the host left `EngineConfig::stun_server_host`
 *  empty — a host that names its own server is unaffected.
 *
 *  When the server is unreachable the engine still works: the session
 *  simply has no server-reflexive candidate, and N4's
 *  `ice_failure_reason` says so.  A missing STUN server is a
 *  *reported fact*, not a failure. */
std::string_view default_stun_server() noexcept;

/** The default STUN port (the IANA-assigned STUN port, RFC 8489 §9). */
std::uint16_t default_stun_port() noexcept;

}  // namespace nimrtc::engine::detail

#endif  // NIMRTC_ENGINE_ENGINE_CONNECTIVITY_PROFILE_HPP
