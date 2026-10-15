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
const ITransportStackFactory* raw_udp_arq_stack_factory_singleton() noexcept;

// ---------------------------------------------------------------------------
// make_transport_instance — Slice 7.6 PR3 (v0.12.0) implementation.
//
// The single canonical entry point for translating the Selector's
// resolved (media_factory_id, control_factory_id) pair into a
// fully-constructed TransportSession. This replaces the pre-PR3
// engine path that built ICE / DTLS / RTP / SCTP via separate
// `reg.get_*_factory()` calls and stitched them together by hand.
//
// Why a free function (not a CapabilitySelector method)?
//   - Decouples the Selector's "rule engine" from the "stack
//     instantiation" responsibility. The Selector stays a pure
//     capability-flags → factory-ids mapper (testable in isolation).
//   - The engine is the composition root: it owns the lifecycle of
//     the resulting TransportSession. Putting instantiation in the
//     Selector would conflate "decide" with "build".
//   - The free-function form keeps the engine's call site
//     self-explanatory: `auto session = make_transport_instance(...);`
//     reads as a single step; the alternative — a Selector method
//     that takes resolver callbacks — splits the same logic across
//     two locations.
//
// Failure modes:
//   - Unknown media_factory_id → logs ERROR, returns session with
//     media_stack=null. The engine detects this and surfaces
//     kEngineInternal at open() time.
//   - Unknown control_factory_id (when non-empty) → logs WARN and
//     returns session with control_stack=null. The control stack is
//     optional, so this is non-fatal — but the engine still surfaces
//     a warning so misconfiguration is visible.
//   - Factory's create() returns null → logged and stack remains
//     null (defensive: every default factory's create() returns a
//     non-null ITransportStack; null indicates a factory-side bug).
// ---------------------------------------------------------------------------
nimrtc::TransportSession make_transport_instance(
    std::string_view media_factory_id,
    std::string_view control_factory_id,
    const nimrtc::StackConfig& cfg) noexcept {

    nimrtc::TransportSession session;
    auto& reg = core::PluginRegistry::instance();

    // ---- media stack ----------------------------------------------------
    if (media_factory_id.empty()) {
        // Empty media id is a programming error — the Selector always
        // returns a non-empty media_factory_id for every known rule
        // (rule 3 / webrtc-quic is the only branch that sets
        // media_factory_id="" and that's documented as v1.x scope).
        // Defensive: log and bail with empty session.
        core::log::Logger::instance().error(
            "transport::make_transport_instance: empty media_factory_id — "
            "Selector did not resolve a media stack. Check the §5.3 rule "
            "audit for the chosen strategy.");
        return session;
    }

    session.media_stack_id = std::string{media_factory_id};
    if (const ITransportStackFactory* f = reg.get_transport_stack(media_factory_id)) {
        session.media_stack = f->create(cfg);
        if (!session.media_stack) {
            core::log::Logger::instance().error(
                std::string{"transport::make_transport_instance: media factory '"} +
                std::string{media_factory_id} +
                "' returned null ITransportStack (factory-side bug).");
        } else {
            core::log::Logger::instance().info(
                std::string{"transport::make_transport_instance: media stack '"} +
                std::string{media_factory_id} + "' created (id='" +
                std::string{session.media_stack->id()} + "').");
        }
    } else {
        core::log::Logger::instance().error(
            std::string{"transport::make_transport_instance: media factory '"} +
            std::string{media_factory_id} +
            "' not registered (did nimrtc::transport::register_default_plugins() "
            "run before this call?).");
    }

    // ---- control stack (optional) ---------------------------------------
    if (control_factory_id.empty()) {
        // Strategy without high-freq control — e.g. browser-call,
        // sfu-relay, cross-network-peer. No control stack to build.
        return session;
    }

    session.control_stack_id = std::string{control_factory_id};
    if (const ITransportStackFactory* f = reg.get_transport_stack(control_factory_id)) {
        // Control stacks use the same StackConfig — the per-stack
        // backend fields (dtls_id, ice_id, etc.) are mostly unused by
        // the control plane (raw-udp-arq ignores them), but raw_id
        // carries the ARQ factory id which IS consulted by
        // RawUdpArqStack::start().
        session.control_stack = f->create(cfg);
        if (!session.control_stack) {
            core::log::Logger::instance().error(
                std::string{"transport::make_transport_instance: control factory '"} +
                std::string{control_factory_id} +
                "' returned null ITransportStack (factory-side bug).");
        } else {
            core::log::Logger::instance().info(
                std::string{"transport::make_transport_instance: control stack '"} +
                std::string{control_factory_id} + "' created (id='" +
                std::string{session.control_stack->id()} + "').");
        }
    } else {
        core::log::Logger::instance().warn(
            std::string{"transport::make_transport_instance: control factory '"} +
            std::string{control_factory_id} +
            "' not registered (control plane will be inactive for this strategy).");
    }

    return session;
}

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

    // ---- RawUdpArqStackFactory (id="raw-udp-arq") — Slice 7.6 ---------
    // The control-plane counterpart of WebRtcClassicStack. CapabilitySelector's
    // §5.3 rules 1b / 2 / 4 (high-freq-control variants) resolve this factory
    // for the control_stack slot. See docs/plan/transport-strategies.md §2.
    static const struct Registrar3 {
        Registrar3() {
            const ITransportStackFactory* f =
                raw_udp_arq_stack_factory_singleton();
            nimrtc::core::PluginRegistry::instance().register_transport_stack(
                std::string_view{f->id()}, f);
            nimrtc::core::log::Logger::instance().info(
                std::string("nimrtc::transport: RawUdpArqStackFactory "
                            "registered (id=\"") +
                std::string(f->id()) +
                "\"), via Slice 7.6; "
                "control-plane ARQ bypass; "
                "ICE/DTLS/RTP/SCTP accessors return null instances");
        }
    } s_registrar3;

    (void)s_registrar;
    (void)s_registrar2;
    (void)s_registrar3;
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
