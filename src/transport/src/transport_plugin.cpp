/**
 * @file src/transport/src/transport_plugin.cpp
 * @brief Default Selector + ITransportStackFactory registration entry
 *        points (Slice 7 + Slice 8).
 *
 * Slice 7 (transport-selection.md §6.4) — `register_default_selector()`
 * constructs a process-static `CapabilitySelector` so the Symbol has a
 * defined destructor location. The Selector registration does NOT
 * publish to `core::PluginRegistry` (the Selector is a separate role
 * from the Stack factory — it picks which factory id to use, but
 * doesn't build stacks itself).
 *
 * Slice 8 (v0.10.2) — `register_default_plugins()` publishes the
 * `CapabilitySelectorStackFactory` (id="default") through the typed
 * `core::PluginRegistry::register_transport_stack(...)` hook so the
 * Slice 8 engine integration has a non-null factory pointer to
 * resolve. Slice 7.5 will replace this with concrete
 * `WebRtcClassicStackFactory` + `RawUdpArqStackFactory` impls.
 *
 * Both entry points follow the Meyer's-singleton latch pattern (same
 * as `nimrtc::ice::register_default_plugins()` in ice.cpp) so the
 * symbols survive MSVC static-link stripping.
 */
#include <nimrtc/core/log.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/transport/transport_plugin.hpp>
#include <nimrtc/transport/transport_selector.hpp>
#include <nimrtc/transport/transport_stack.hpp>

namespace nimrtc::transport {

namespace detail {

// Forward declarations — defined in the respective .cpp files.
// We keep these internal so the module's public surface
// (transport_selector.hpp, transport_stack.hpp, transport_plugin.hpp) stays clean.
const ITransportStackFactory* default_stack_factory_singleton() noexcept;
const ITransportStackFactory* webrtc_classic_stack_factory_singleton() noexcept;

void do_register_default_selector() noexcept {
    // Process-static Selector instance — see Slice 7 header comment.
    // The Selector is intentionally not registered in
    // core::PluginRegistry because the registry holds *factories*, not
    // selectors. Consumers that need to select acquire the Selector
    // through this module's `register_default_selector()` entry point
    // (or by linking `nimrtc::transport` and constructing one directly).
    static const CapabilitySelector s_selector{};
    NIMRTC_LOG_INFO("transport: default selector registered "
                    "(id=\"default\", impl=CapabilitySelector)");
}

void do_register_default_plugins() noexcept {
    // Slice 8: publish the shell stack factory (id="default") through the
    // typed registry hook. Slice 7.5: also publish WebRtcClassicStackFactory
    // (id="webrtc-classic") so the CapabilitySelector's §5.3 rule 1 resolves
    // the real factory. Both ids coexist; the Selector picks which to use.

    // ---- Shell factory (id="default") — Slice 8 -----------------------------
    static const struct Registrar {
        Registrar() {
            const ITransportStackFactory* f =
                default_stack_factory_singleton();
            nimrtc::core::PluginRegistry::instance().register_transport_stack(
                std::string_view{f->id()}, f);
            nimrtc::core::log::Logger::instance().info(
                std::string("nimrtc::transport: shell stack factory "
                            "registered (id=\"") +
                std::string(f->id()) +
                "\", via Slice 8 typed registry hook; "
                "shell impl — real ICE/DTLS/RTP/SCTP composition lands "
                "in Slice 7.5)");
        }
    } s_registrar;

    // ---- WebRtcClassicStackFactory (id="webrtc-classic") — Slice 7.5 -----
    static const struct Registrar2 {
        Registrar2() {
            const ITransportStackFactory* f =
                webrtc_classic_stack_factory_singleton();
            nimrtc::core::PluginRegistry::instance().register_transport_stack(
                std::string_view{f->id()}, f);
            nimrtc::core::log::Logger::instance().info(
                std::string("nimrtc::transport: WebRtcClassicStackFactory "
                            "registered (id=\"") +
                std::string(f->id()) +
                "\"), via Slice 7.5; "
                "real ICE/DTLS/RTP/SCTP composition — "
                " SCTP stub until v0.11.0 adds usrsctp)");
        }
    } s_registrar2;

    (void)s_registrar;
    (void)s_registrar2;
}

} // namespace detail

void register_default_selector() noexcept {
    static const int once = []() {
        detail::do_register_default_selector();
        return 1;
    }();
    (void)once;
}

void register_default_plugins() noexcept {
    // Slice 8 — populates the typed registry slot for transport stacks.
    // Idempotent Meyer's-singleton latch.
    static const int once = []() {
        detail::do_register_default_plugins();
        return 1;
    }();
    (void)once;
}

} // namespace nimrtc::transport
