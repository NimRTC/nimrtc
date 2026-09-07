/**
 * @file src/modules/sched/src/scheduler_plugin.cpp
 * @brief Plugin adapter + PluginFactory for nimrtc::sched.
 *
 * Wires the concrete `sched::Scheduler` (strict-priority 5-queue + BWE)
 * implementation into the `plugins::IScheduler` seam and registers it
 * under the id `"strict_priority"`.
 *
 * P2 will add a second factory for a weighted-fair scheduler (e.g. for
 * SFU fan-out); the registry's `get_scheduler()` picks whichever id
 * the engine asks for.
 *
 * Per ADR-001 + the MSVC static-link workaround (see audio3a / ice):
 * explicit-registration entry point `register_default_plugins()` forces
 * the .obj (with its registrar) into the consumer's link.
 */

#include <nimrtc/sched/sched.hpp>
#include <nimrtc/core/log.hpp>
#include <nimrtc/core/registry.hpp>

namespace nimrtc::sched {

// ---------------------------------------------------------------------------
// StrictPriority factory
// ---------------------------------------------------------------------------

/** Concrete factory producing strict-priority `Scheduler` instances. */
class StrictPriorityPluginFactory final : public plugins::ISchedulerFactory {
public:
    std::string_view id()          const noexcept override {
        return "strict_priority";
    }
    std::string_view display_name()const noexcept override {
        return "Strict-priority 5-queue + BWE (NimRTC D8 / §8.3 default)";
    }
    plugins::IScheduler* create(plugins::SchedulerConfig config) const override {
        return new Scheduler(config);
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
            static StrictPriorityPluginFactory s_factory{};
            nimrtc::core::PluginRegistry::instance().register_scheduler(
                std::string_view{s_factory.id()}, &s_factory);
        }
    } s_registrar;
    (void)s_registrar;
}

} // namespace detail

// Non-inline (declared via registry.hpp forward declaration) so the
// symbol is guaranteed in nimrtc_sched.lib for consumers that link via
// static lib + PluginRegistry.
void register_default_plugins() noexcept {
    static const int once = []() {
        detail::do_register_default_plugins();
        return 1;
    }();
    (void)once;
}

} // namespace nimrtc::sched
