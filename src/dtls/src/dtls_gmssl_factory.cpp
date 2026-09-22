/**
 * @file src/dtls/src/dtls_gmssl_factory.cpp
 * @brief GmSSLDtlsFactory — GMSSL-backed IDtlsSessionFactory (ADR-013).
 *
 * Mirrors the wolfSSL-backed `WolfsslDtlsFactory` (id = "wolfssl") for
 * the GMSSL backend (id = "gmssl").  The factory is thin by design:
 * `create(DtlsConfig)` forwards to `new DtlsSessionGmSSL(cfg)`.
 *
 * ## Backward compatibility
 *
 * A NimRTC engine compiled with GMSSL support but running on a system
 * that lacks the GMSSL .dll / .so (e.g. a developer machine that has
 * only OpenSSL 3.x installed) will fail at `DtlsSessionGmSSL::open()`
 * with a shared-library resolution error or a symbol-not-found link
 * error before `create()` is even called.  This is intentional — the
 * factory itself does not carry a runtime availability check; the
 * integrator is expected to configure `cfg.dtls_name` based on what
 * they actually built.
 *
 * ## Thread safety
 *
 * `create()` is safe to call concurrently — each call returns a fresh
 * `DtlsSessionGmSSL` with its own `Impl` and GMSSL CTX/SSL objects.
 * The factory itself holds no mutable state.
 *
 * @see ADR-013 (docs/adr/ADR-013-gmssl-dtls-backend.md).
 */

#include <nimrtc/dtls/dtls_session_factory.hpp>
#include <nimrtc/dtls/dtls_gmssl_session.hpp>

#include <nimrtc/core/plugin_id.hpp>
#include <nimrtc/core/registry.hpp>

namespace nimrtc::dtls {

// ===========================================================================
// GmSSLDtlsFactory — second default DTLS backend (ADR-013).
//
// Registered by nimrtc::dtls::register_default_plugins() under id="gmssl".
// Select via EngineConfig::dtls_name = "gmssl" or by directly registering
// a factory with the same id into PluginRegistry.
// ===========================================================================

class GmSSLDtlsFactory final : public IDtlsSessionFactory {
public:
    // ---- IDtlsSessionFactory -----------------------------------------------

    std::string_view id() const noexcept override {
        return NIMRTC_PLUGIN_ID(kBackendId);
    }

    std::string_view display_name() const noexcept override {
        return "DTLS 1.2 (GMSSL backend, ECDHE-ECDSA-AES-GCM)";
    }

    std::unique_ptr<IDtlsSession>
    create(const DtlsConfig& cfg) const override {
        return std::make_unique<DtlsSessionGmSSL>(cfg);
    }

private:
    static constexpr const char* kBackendId = "gmssl";
};

// ===========================================================================
// Factory singleton accessor — used by dtls_plugin.cpp's registrar.
// ===========================================================================

const IDtlsSessionFactory* gmssl_factory_singleton() noexcept {
    static const GmSSLDtlsFactory s_factory{};
    return &s_factory;
}

// ===========================================================================
// Test-only accessors — mirrors the wolfSSL factory pattern.
// ===========================================================================

namespace test_only {

inline const IDtlsSessionFactory*&
registered_gmssl_factory_slot() noexcept {
    static const IDtlsSessionFactory* s_factory = nullptr;
    return s_factory;
}

const IDtlsSessionFactory* get_gmssl_factory() noexcept {
    if (const auto* f = nimrtc::core::PluginRegistry::instance()
                            .get_dtls_session("gmssl")) {
        return f;
    }
    return registered_gmssl_factory_slot();
}

void set_gmssl_factory_for_testing(const IDtlsSessionFactory* f) noexcept {
    registered_gmssl_factory_slot() = f;
}

} // namespace test_only

} // namespace nimrtc::dtls
