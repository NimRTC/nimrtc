/**
 * @file src/modules/net_feedback/src/plugin.cpp
 * @brief Plugin adapter + PluginFactory for nimrtc::net_feedback.
 *
 * Wires the concrete `net_feedback::Collector` into the
 * `plugins::INetFeedback` seam and registers it under the id
 * "nimrtc_net_feedback".  The engine resolves that id through the
 * registry's net-feedback slot (see `EngineConfig::net_feedback_name`).
 *
 * Per ADR-001 + the MSVC static-link workaround (see audio3a / ice):
 * explicit-registration entry point `register_default_plugins()` forces
 * the .obj (with its registrar) into the consumer's link.
 */

#include <nimrtc/net_feedback/net_feedback.hpp>

#include <nimrtc/core/registry.hpp>

namespace nimrtc::net_feedback {

namespace {

/** Concrete factory producing `Collector` instances. */
class NetFeedbackPluginFactory final : public plugins::INetFeedbackFactory {
public:
    std::string_view id()           const noexcept override { return "nimrtc_net_feedback"; }
    std::string_view display_name() const noexcept override {
        return "Receiver-side network feedback collector (RR / REMB / transport-cc)";
    }
    plugins::INetFeedback* create() const override {
        return new Collector();
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Public registration entry point
// ---------------------------------------------------------------------------

namespace detail {

void do_register_default_plugins() noexcept {
    // static ⇒ address stable for process lifetime; runs once at first call.
    static const struct Registrar {
        Registrar() {
            static NetFeedbackPluginFactory s_factory{};
            nimrtc::core::PluginRegistry::instance().register_net_feedback(
                std::string_view{s_factory.id()}, &s_factory);
        }
    } s_registrar;
    (void)s_registrar;
}

} // namespace detail

// Non-inline (declared in net_feedback.hpp via registry.hpp forward
// declaration) so the symbol is guaranteed in nimrtc_net_feedback.lib for
// consumers that link via static lib + PluginRegistry.
void register_default_plugins() noexcept {
    static const int once = []() {
        detail::do_register_default_plugins();
        return 1;
    }();
    (void)once;
}

} // namespace nimrtc::net_feedback
