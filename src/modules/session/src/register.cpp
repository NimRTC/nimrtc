/**
 * @file src/modules/session/src/register.cpp
 * @brief Default-plugin registration for nimrtc::session.
 *
 * v1.2.0 — `docs/plan/session-capability-plan.md` N1 / T1.
 *
 * Registers the concrete `session::SessionModel` behind the
 * `plugins::ISession` seam under the id "session".  The engine
 * resolves it through the registry's session slot
 * (`core::PluginRegistry::get_session(EngineConfig::session_name)`)
 * and never names this module's headers (Layout Invariant 4).
 *
 * The registration follows the same pattern as the other modules
 * (see `src/modules/net_feedback/src/plugin.cpp`): a function-local
 * static registrar whose address is stable for the lifetime of the
 * process, so the factory pointer handed to the registry stays valid.
 */

#include <nimrtc/session/session_model.hpp>

#include <nimrtc/core/registry.hpp>

namespace nimrtc::session {

namespace {

/** Concrete factory producing `SessionModel` instances. */
class SessionModelFactory final : public plugins::ISessionFactory {
public:
    std::string_view id()           const noexcept override { return "session"; }
    std::string_view display_name() const noexcept override {
        return "NimRTC session model (tracks + transceivers, renegotiation)";
    }
    plugins::ISession* create()      const override { return new SessionModel(); }
};

} // namespace

// ---------------------------------------------------------------------------
// Public registration entry point
// ---------------------------------------------------------------------------

namespace detail {

void do_register_default_plugins() noexcept {
    static const struct Registrar {
        Registrar() {
            static SessionModelFactory s_factory{};
            nimrtc::core::PluginRegistry::instance().register_session(
                std::string_view{s_factory.id()}, &s_factory);
        }
    } s_registrar;
    (void)s_registrar;
}

} // namespace detail

void register_default_plugins() noexcept {
    static const int once = []() {
        detail::do_register_default_plugins();
        return 1;
    }();
    (void)once;
}

} // namespace nimrtc::session
