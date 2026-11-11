/**
 * @file src/modules/dtmf/src/register.cpp
 * @brief Default-plugin registration for nimrtc::dtmf.
 *
 * v1.2.0 — `docs/plan/session-capability-plan.md` D1 / D2.
 *
 * Registers the concrete `dtmf::TelephoneEventCodec` behind the
 * `plugins::IDtmfCodecFactory` seam under the id "dtmf".  The engine
 * resolves it through the registry's DTMF slot
 * (`core::PluginRegistry::get_dtmf(EngineConfig::dtmf_codec_name)`)
 * and never names this module's headers (Layout Invariant 4).
 *
 * The registration follows the same pattern as the other modules
 * (see `src/modules/net_feedback/src/plugin.cpp`): a function-local
 * static registrar whose address is stable for the lifetime of the
 * process, so the factory pointer handed to the registry stays valid.
 */

#include <nimrtc/dtmf/dtmf.hpp>

#include <nimrtc/core/registry.hpp>

namespace nimrtc::dtmf {

namespace {

/** Concrete factory producing `TelephoneEventCodec` instances. */
class TelephoneEventFactory final : public plugins::IDtmfCodecFactory {
public:
    std::string_view id()           const noexcept override { return "dtmf"; }
    std::string_view display_name() const noexcept override {
        return "NimRTC RFC 4733 telephone-event codec (DTMF)";
    }
    plugins::IDtmfCodec* create()   const override {
        return new TelephoneEventCodec();
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Public registration entry point
// ---------------------------------------------------------------------------

namespace detail {

void do_register_default_plugins() noexcept {
    static const struct Registrar {
        Registrar() {
            static TelephoneEventFactory s_factory{};
            nimrtc::core::PluginRegistry::instance().register_dtmf(
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

} // namespace nimrtc::dtmf
