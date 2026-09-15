/**
 * @file nimrtc/dtls/dtls_session_factory.hpp
 * @brief IDtlsSessionFactory — DTLS session factory seam (PAL Slice 4).
 *
 * Per docs/plan/transport-selection.md §5.2 and §6.1, the DTLS seam
 * is built as an abstract factory + abstract product pair:
 *
 *   IDtlsSessionFactory::create(DtlsConfig) → std::unique_ptr<IDtlsSession>
 *
 * This matches the pattern already used by the ICE module
 * (`ice::IceTransportFactory : plugins::IICETransportFactory` —
 * src/modules/ice/include/nimrtc/ice/ice.hpp), so the registry lookup
 * shape is uniform across modules.
 *
 * ## Default factory
 *
 * The built-in factory wraps the existing wolfSSL-backed
 * `DtlsSessionWolfSSL` implementation.  Its `id()` is `"wolfssl"`.
 * The factory is registered by `nimrtc::dtls::register_default_plugins()`
 * — see `dtls_plugin.hpp` for the registration entry point.
 *
 * ## Replacement story
 *
 * A host application that wants a different DTLS backend
 * (OpenSSL, BoringSSL, mbedTLS, …) implements `IDtlsSession` for
 * that backend, registers an `IDtlsSessionFactory` via
 * `core::PluginRegistry::register_dtls(...)` (Slice 4 does not
 * introduce the registry slot — it stays at the seam level only;
 * Slice 8 / Slice 7 will add a registry slot when transport selection
 * goes through core::PluginRegistry), and selects the new factory by
 * id when building a TransportStack.
 *
 * @note P1 — interface added as part of PAL Slice 4 (v0.11.0).
 */

#ifndef NIMRTC_DTLS_SESSION_FACTORY_HPP
#define NIMRTC_DTLS_SESSION_FACTORY_HPP

#include <memory>
#include <string_view>

#include <nimrtc/dtls/dtls_session_iface.hpp>   // IDtlsSession, DtlsConfig

namespace nimrtc::dtls {

/**
 * @brief Factory contract for IDtlsSession backends.
 *
 * Implementations MUST be cheap to construct (registry looks them up
 * on every NimRTCEngine::open() so a heavy constructor would slow
 * startup).  Implementations MUST be safe to call `create()` from
 * multiple threads concurrently — each call returns a fresh session
 * instance.
 */
class IDtlsSessionFactory {
public:
    virtual ~IDtlsSessionFactory() = default;

    /**
     * @brief Stable backend identifier.
     *
     * Used as the lookup key in the transport-stack `StackConfig`
     * (`dtls_id` field, Slice 7) and in the JSON Profile `transport`
     * segment.  The built-in factory returns `"wolfssl"`.
     *
     * Empty string is reserved for "unset / unconfigured" and is not a
     * valid backend id; PAL Slice 3's `NIMRTC_PLUGIN_ID(name)`
     * validator will reject empty ids at compile time.
     *
     * @return std::string_view with program-lifetime storage (typically
     *         a string literal).  Must be non-empty.
     */
    virtual std::string_view id() const noexcept = 0;

    /**
     * @brief Short human-readable display name.
     *
     * Matches the `ice::IceTransportFactory::display_name()` /
     * `sctp::SctpStubFactory::display_name()` pattern; surfaces in
     * log lines and registry listings.
     */
    virtual std::string_view display_name() const noexcept = 0;

    /**
     * @brief Create a new IDtlsSession instance.
     *
     * The returned session is owned by the caller; pass it to the
     * transport stack / engine as a `unique_ptr`.  The factory may
     * mutate `cfg` (e.g. clone and customise role / SRTP profile);
     * callers should treat `cfg` as consumed-by-copy and not rely on
     * post-call contents.
     *
     * `const`-qualified to match the ICE factory contract
     * (`IceTransportFactory::create_ice() const`) — registries that
     * hold a `const IDtlsSessionFactory*` can still call create().
     *
     * @return A new IDtlsSession; never nullptr on a well-formed
     *         factory.  Throwing is acceptable (registry callers wrap
     *         in try/catch); the built-in wolfssl factory does not
     *         throw.
     */
    virtual std::unique_ptr<IDtlsSession> create(
        const DtlsConfig& cfg) const = 0;
};

} // namespace nimrtc::dtls

#endif // NIMRTC_DTLS_SESSION_FACTORY_HPP
