/**
 * @file src/modules/bwe/src/bwe_plugin.cpp
 * @brief Plugin adapter + PluginFactory for nimrtc::bwe.
 *
 * Wires the concrete `bwe::Bwe` (AIMD) implementation into the
 * `plugins::IBwe` seam and registers it under the id `"aimd"`.
 *
 * P3 will add a second factory for Goog-CC compatibility
 * (`"googcc"`); the registry's `get_bwe()` picks whichever id the
 * engine / scheduler asks for.
 *
 * Per ADR-001 + the MSVC static-link workaround (see audio3a / ice):
 * explicit-registration entry point `register_default_plugins()` forces
 * the .obj (with its registrar) into the consumer's link.
 */

#include <nimrtc/bwe/bwe.hpp>
#include <nimrtc/core/log.hpp>
#include <nimrtc/core/registry.hpp>

namespace nimrtc::bwe {

// ---------------------------------------------------------------------------
// AIMD factory
// ---------------------------------------------------------------------------

/** Concrete factory producing AIMD `Bwe` instances. */
class AimdPluginFactory final : public plugins::IBweFactory {
public:
    std::string_view id()          const noexcept override { return "aimd"; }
    std::string_view display_name()const noexcept override {
        return "AIMD bandwidth estimator (RFC 8698 §6 P1 default)";
    }
    plugins::IBwe* create(plugins::BweConfig config) const override {
        return new Bwe(config);
    }
};

// ---------------------------------------------------------------------------
// Public registration entry point
// ---------------------------------------------------------------------------

namespace detail {

void do_register_default_plugins() noexcept {
    // static ⇒ address stable for process lifetime; runs once at first call.
    static const struct Registrar {
        Registrar() {
            static AimdPluginFactory s_factory{};
            nimrtc::core::PluginRegistry::instance().register_bwe(
                std::string_view{s_factory.id()}, &s_factory);
        }
    } s_registrar;
    (void)s_registrar;
}

} // namespace detail

// Non-inline (declared in bwe.hpp via registry.hpp forward declaration) so
// the symbol is guaranteed in nimrtc_bwe.lib for consumers that link via
// static lib + PluginRegistry.
void register_default_plugins() noexcept {
    static const int once = []() {
        detail::do_register_default_plugins();
        return 1;
    }();
    (void)once;
}

} // namespace nimrtc::bwe
