/**
 * @file src/dtls/src/dtls_wolfssl_factory.cpp
 * @brief WolfsslDtlsFactory — default IDtlsSessionFactory (PAL Slice 4).
 *
 * Wraps the existing wolfSSL-backed `DtlsSessionWolfSSL` (defined in
 * src/modules/dtls/src/dtls_wolfssl_session.cpp) as the default DTLS
 * backend under the seam ID "wolfssl".
 *
 * ## Why a thin factory
 *
 * The factory is deliberately a single-purpose wrapper around
 * `new DtlsSessionWolfSSL(cfg)`.  All the heavy lifting — context
 * setup, cert loading, fingerprint computation, SRTP keying export —
 * stays inside the existing concrete class.  This keeps the Slice 4
 * diff small (the seam + factory is ~50 LOC of new code, plus the
 * IDtlsSession method implementations added to DtlsSessionWolfSSL)
 * while still standing up the full seam surface that Slice 7 / Slice 8
 * will route through.
 *
 * ## Thread safety
 *
 * `create()` is safe to call concurrently — each call returns a
 * fresh `DtlsSessionWolfSSL` with its own `Impl` and wolfSSL CTX.  The
 * factory itself holds no mutable state.
 */

#include <nimrtc/dtls/dtls_session_factory.hpp>
#include <nimrtc/dtls/dtls_wolfssl_session.hpp>

#include <nimrtc/core/plugin_id.hpp>

namespace nimrtc::dtls {

// ===========================================================================
// WolfsslDtlsFactory — default factory for the DTLS seam.
//
// Registered by nimrtc::dtls::register_default_plugins() under the id
// "wolfssl".  See dtls_plugin.cpp for the registration entry point.
// ===========================================================================

class WolfsslDtlsFactory final : public IDtlsSessionFactory {
public:
    // ---- IDtlsSessionFactory ------------------------------------------------

    std::string_view id() const noexcept override {
        // Stable backend id used as the lookup key in the transport
        // stack's StackConfig::dtls_id (Slice 7) and in the JSON
        // Profile `transport` segment (Slice 7).  Non-empty by
        // construction so PAL Slice 3's NIMRTC_PLUGIN_ID("wolfssl")
        // compile-time id-literal validator accepts it.
        //
        // PAL Slice 3: route through the compile-time-unique
        // `PluginIdTag<__LINE__>` wrapper so this callsite is greppable
        // and the wrapper type is distinct from any other factory id.
        return NIMRTC_PLUGIN_ID(kBackendId);
    }

    std::string_view display_name() const noexcept override {
        // Human-readable name surfaced in log lines and registry
        // listings — matches the `IceTransportFactory::display_name()`
        // / `SctpStubFactory::display_name()` pattern.
        return "DTLS 1.2 (wolfSSL backend, ECDHE-ECDSA-AES-GCM)";
    }

    std::unique_ptr<IDtlsSession> create(const DtlsConfig& cfg) const override {
        // Hand off to the existing wolfSSL-backed concrete class.
        // The factory owns nothing; the returned session's `Impl`
        // (held by unique_ptr) owns the wolfSSL CTX and SSL objects.
        //
        // DtlsSessionWolfSSL now inherits IDtlsSession (Slice 4
        // refactor of src/modules/dtls/include/nimrtc/dtls/dtls_wolfssl_session.hpp),
        // so the upcast to std::unique_ptr<IDtlsSession> is implicit.
        //
        // No-throw guarantee: the wolfssl constructor does not throw
        // (only heap-allocates an Impl).  The actual wolfSSL_CTX
        // creation happens later in DtlsSessionWolfSSL::open(), so a
        // wolfSSL_Init failure here would surface as state() == Failed
        // and (optionally) an on_handshake_complete callback firing
        // with kErrInternal.
        return std::make_unique<DtlsSessionWolfSSL>(cfg);
    }

private:
    // Backend id stored as a static literal — stable lifetime for the
    // returned std::string_view (id() returns this directly).  Kept
    // private so callers can't mutate it.
    static constexpr const char* kBackendId = "wolfssl";
};

// ===========================================================================
// Factory-of-factory accessor (used by dtls_plugin.cpp's Registrar).
//
// Returns a `const IDtlsSessionFactory*` pointing to the singleton
// `WolfsslDtlsFactory`.  The pointer has program lifetime (Meyer
// singleton), so it is safe to cache at module-init time and look up
// later.
//
// Production callers should NOT use this — they go through
// `register_default_plugins()` and (in Slice 7) core::PluginRegistry.
// This accessor exists so the Registrar in dtls_plugin.cpp can publish
// the factory pointer through the test-only slot without exposing
// `WolfsslDtlsFactory`'s definition across translation units.
// ===========================================================================

const IDtlsSessionFactory* wolfssl_factory_singleton() noexcept {
    static const WolfsslDtlsFactory s_factory{};
    return &s_factory;
}

// ===========================================================================
// Test-only accessor (not part of the public API; production code MUST NOT
// use this).  Mirrors `nimrtc::sctp::test_only::get_stub_factory()` —
// keeps the public seam surface (only `register_default_plugins()`) clean
// while still letting tests resolve the factory pointer without going
// through a registry.
//
// Slice 7 will route the canonical lookup through
// `core::PluginRegistry::register_dtls(...)` / `get_dtls(id)`; for
// Slice 4 the test path is the seam-local accessor.
// ===========================================================================

namespace test_only {

// Internal pointer (set once by the Registrar in dtls_plugin.cpp).
// Mutable only inside dtls_plugin.cpp's Registrar; tests should never
// touch this directly.
inline const IDtlsSessionFactory*& registered_factory_slot() noexcept {
    static const IDtlsSessionFactory* s_factory = nullptr;
    return s_factory;
}

/** Returns the registered factory pointer, or nullptr if
 *  `register_default_plugins()` has not been called yet.
 *  Test-only — production code MUST NOT call this. */
const IDtlsSessionFactory* get_wolfssl_factory() noexcept {
    return registered_factory_slot();
}

/** Test-only registration hook (called by the Registrar in
 *  dtls_plugin.cpp).  Production code MUST NOT call this. */
void set_wolfssl_factory_for_testing(const IDtlsSessionFactory* f) noexcept {
    registered_factory_slot() = f;
}

} // namespace test_only

} // namespace nimrtc::dtls
