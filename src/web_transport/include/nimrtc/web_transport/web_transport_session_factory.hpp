/**
 * @file nimrtc/web_transport/web_transport_session_factory.hpp
 * @brief IWebTransportSessionFactory — WebTransport / QUIC session
 *        factory seam (v0.14.0 alpha.2, ADR-014).
 *
 * Per `docs/plan/v0.14-plan.md` §2.1.1 WT-REGISTRY and
 * `v1.0-plan.md` §10.4, the WebTransport seam is built as an
 * abstract factory + abstract product pair:
 *
 *   IWebTransportSessionFactory::create(WebTransportConfig)
 *       → std::unique_ptr<IWebTransportSession>
 *
 * This mirrors the pattern already used by the DTLS module
 * (`dtls::IDtlsSessionFactory : dtls::IDtlsSession` — see
 * `dtls_session_factory.hpp`), the SCTP module, and the
 * transport-stack module. The factory interface is part of the
 * alpha.2 seam; the msquic-backed implementation
 * (`WebTransportMsQuicSessionFactory`, id = `"msquic"`) lands in
 * v0.14.0-beta.1.
 *
 * ## Default factory
 *
 * At the v0.14.0 alpha.2 cut, **no factory is registered** —
 * `core::PluginRegistry::list_web_transports()` returns an
 * empty vector. The factory slot exists so the engine can
 * resolve a factory id from `EngineConfig::datachannel_backend`
 * at any point in the cycle; the production factory lands in
 * beta.1.
 *
 * v0.14.0 callers that try to use
 * `EngineConfig::datachannel_backend = "msquic"` (or any other
 * web_transport id) before beta.1 will see a runtime
 * `kErrNotReady` from `core::PluginRegistry::get_web_transport(id)`
 * with a clear log message pointing at the beta.1 milestone.
 *
 * ## Replacement story
 *
 * A host application that wants a different WebTransport backend
 * (e.g. quiche, see ADR-014 §Alternatives) implements
 * `IWebTransportSession` for that backend, registers an
 * `IWebTransportSessionFactory` via
 * `core::PluginRegistry::register_web_transport("quiche",
 * &factory)`, and selects the new factory by id when building a
 * `WebTransportDataChannel`.
 *
 * @note P0 — interface added as part of v0.14.0 alpha.2
 *       (the v1.0.0-alpha.2 pre-release).
 */

#ifndef NIMRTC_WEB_TRANSPORT_SESSION_FACTORY_HPP
#define NIMRTC_WEB_TRANSPORT_SESSION_FACTORY_HPP

// Stability classification: NIMRTC_API_UNSTABLE — see
// web_transport_session_iface.hpp for the rationale.
#include <memory>
#include <string_view>

#include <nimrtc/core/api_version.hpp>   // NIMRTC_API_UNSTABLE
#include <nimrtc/web_transport/web_transport_config.hpp>  // WebTransportConfig
#include <nimrtc/web_transport/web_transport_session_iface.hpp>  // IWebTransportSession

namespace nimrtc::web_transport {

/**
 * @brief Factory contract for IWebTransportSession backends.
 *
 * Implementations MUST be cheap to construct (registry looks
 * them up on every NimRTCEngine::open() so a heavy constructor
 * would slow startup). Implementations MUST be safe to call
 * `create()` from multiple threads concurrently — each call
 * returns a fresh session instance.
 */
class NIMRTC_API_UNSTABLE IWebTransportSessionFactory {
public:
    virtual ~IWebTransportSessionFactory() = default;

    /**
     * @brief Stable backend identifier.
     *
     * Used as the lookup key in the `EngineConfig::datachannel_backend`
     * field (lands in beta.1) and in any future
     * `core::PluginRegistry::get_web_transport(id)` lookup. The
     * built-in msquic factory returns `"msquic"` (lands in
     * beta.1).
     *
     * Empty string is reserved for "unset / unconfigured" and is
     * not a valid backend id.
     *
     * @return std::string_view with program-lifetime storage
     *         (typically a string literal). Must be non-empty.
     */
    virtual std::string_view id() const noexcept = 0;

    /**
     * @brief Short human-readable display name.
     *
     * Matches the `dtls::IDtlsSessionFactory::display_name()` /
     * `sctp::ISctpSocketFactory::display_name()` pattern;
     * surfaces in log lines and registry listings.
     */
    virtual std::string_view display_name() const noexcept = 0;

    /**
     * @brief Create a new IWebTransportSession instance.
     *
     * The returned session is owned by the caller; pass it to
     * the data-channel adapter / engine as a `unique_ptr`. The
     * factory may mutate `cfg` (e.g. clone and customise the
     * idle timeout); callers should treat `cfg` as
     * consumed-by-copy and not rely on post-call contents.
     *
     * `const`-qualified to match the DTLS / SCTP factory
     * contract — registries that hold a `const
     * IWebTransportSessionFactory*` can still call create().
     *
     * @return A new IWebTransportSession; never nullptr on a
     *         well-formed factory. Throwing is acceptable
     *         (registry callers wrap in try/catch).
     */
    virtual std::unique_ptr<IWebTransportSession> create(
        const WebTransportConfig& cfg) const = 0;
};

} // namespace nimrtc::web_transport

#endif // NIMRTC_WEB_TRANSPORT_SESSION_FACTORY_HPP
