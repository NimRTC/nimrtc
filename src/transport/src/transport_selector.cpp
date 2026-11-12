/**
 * @file src/transport/src/transport_selector.cpp
 * @brief CapabilitySelector — rule engine for §5.3 priority list.
 *
 * Slice 7 (transport-selection.md §5.3 + §6.4). This file contains the
 * full selector logic. The 4 priority rules from §5.3 are enumerated in
 * code comments in the same order as the doc so reviewers can verify
 * the rule set without running anything.
 *
 * ----------------------------------------------------------------------------
 * WHAT THIS FILE DOES NOT DO (intentional)
 * ----------------------------------------------------------------------------
 *   - It does NOT know about `core::PluginRegistry`.  The registry lookups
 *     are injected by the caller through the two resolver hooks, so the
 *     transport module stays independent of the plugin registry and the
 *     rule engine remains unit-testable without standing up real
 *     factories.
 *
 *   - The `webrtc-quic` branch (rule 3) is still a stub — it needs the
 *     msquic / ngtcp2 backends.  The id is recorded so the choice is
 *     visible, but no media stack is constructed for it.
 *
 *   The engine (src/engine/src/engine_transport.cpp) binds the resolvers
 *   to `core::PluginRegistry::get_transport_stack` and passes its
 *   `EngineConfig`-derived `StackConfig`, so the Selector produces live
 *   `ITransportStack` instances in production.
 * ----------------------------------------------------------------------------
 */
#include <nimrtc/core/log.hpp>
#include <nimrtc/transport/transport_selector.hpp>
#include <nimrtc/transport/transport_stack.hpp>

#include <utility>

namespace nimrtc {

// -----------------------------------------------------------------------------
// TransportSession member fns — fan out to whichever stacks are non-null.
// -----------------------------------------------------------------------------
void TransportSession::start() noexcept {
    if (media_stack) {
        NIMRTC_LOG_INFO("transport: starting media_stack ("
                        << media_stack_id << ")");
        media_stack->start();
    }
    if (control_stack) {
        NIMRTC_LOG_INFO("transport: starting control_stack ("
                        << control_stack_id << ")");
        control_stack->start();
    }
}

void TransportSession::close() noexcept {
    // Close control first so any in-flight raw-UDP ACKs get a chance to
    // drain before media tears down.
    if (control_stack) {
        control_stack->close();
    }
    if (media_stack) {
        media_stack->close();
    }
}

// -----------------------------------------------------------------------------
// CapabilitySelector
// -----------------------------------------------------------------------------
CapabilitySelector::CapabilitySelector(StackFactoryResolver factory_lookup,
                                       StackCreateResolver factory_create,
                                       const StackConfig& cfg) noexcept
    : factory_lookup_(std::move(factory_lookup)),
      factory_create_(std::move(factory_create)),
      stack_cfg_(cfg) {}

// -----------------------------------------------------------------------------
// §5.3 SELECTION RULE TABLE
// -----------------------------------------------------------------------------
// The code in select() below follows this table exactly. Each branch is
// annotated with the rule number from §5.3 to keep the doc ↔ code link
// auditable.
// -----------------------------------------------------------------------------
//
// Rule 1: needs_browser_interop == true
//         → media_stack    = webrtc-classic
//         → control_stack  = raw-udp-arq if needs_high_freq_control, else nullptr
//
// Rule 2: needs_browser_interop == false && needs_high_freq_control == true
//         → media_stack    = raw-udp-arq
//         → control_stack  = raw-udp-arq (independent stack)
//
// Rule 3: needs_browser_interop == false && prefer_quic == true
//         → media_stack    = webrtc-quic (Slice 7.x 二期)
//         → control_stack  = per needs_high_freq_control
//
// Rule 4: Default
//         → media_stack    = webrtc-classic
//         → control_stack  = per needs_high_freq_control
// -----------------------------------------------------------------------------

// Helper — try to instantiate a stack via the resolver callbacks; falls
// back to a "factory-id-only" TransportSession if resolvers are absent
// (Slice 7 default) or the factory is unknown. This keeps the rule
// engine testable today without standing up real factories.
static std::unique_ptr<ITransportStack>
try_create_stack(const ITransportStackFactory* factory,
                 CapabilitySelector::StackCreateResolver creator,
                 const StackConfig& cfg) {
    if (!factory || !creator) return nullptr;
    return creator(factory, cfg);
}

// Helper — set the trace fields so tests can assert which factory id
// the selector picked for each rule.
static void set_trace(CapabilitySelector::SelectionTrace& trace,
                      std::string_view media_id,
                      std::string_view control_id) {
    trace.media_factory_id   = std::string{media_id};
    trace.control_factory_id = std::string{control_id};
}

TransportSession CapabilitySelector::select(
    const TransportRequirements& req) const {

    // ---------------------------------------------------------------------
    // Rule resolution.
    //
    // The four §5.3 rules are enumerated in the comments below in the
    // same order as the doc, so a reviewer can verify the rule set
    // without running anything.  Each rule decides exactly two ids:
    // the media factory id and the control factory id.
    // ---------------------------------------------------------------------
    std::string media_id;
    std::string control_id;

    if (req.needs_browser_interop) {
        // Rule 1 — media_stack = webrtc-classic
        //          control_stack = raw-udp-arq if needs_high_freq_control
        media_id   = "webrtc-classic";
        control_id = req.needs_high_freq_control ? "raw-udp-arq" : "";
    } else if (req.needs_high_freq_control) {
        // Rule 2 — media_stack = raw-udp-arq
        //          control_stack = raw-udp-arq (independent stack)
        media_id   = "raw-udp-arq";
        control_id = "raw-udp-arq";
    } else if (req.prefer_quic) {
        // Rule 3 — media_stack = webrtc-quic (Slice 7.x 二期)
        //          control_stack = per needs_high_freq_control
        //
        // The QUIC factory does not exist yet, so the media stack stays
        // null.  The id is still recorded so the choice is visible in
        // the trace and in the engine's accessors.
        media_id   = "webrtc-quic";
        control_id = req.needs_high_freq_control ? "raw-udp-arq" : "";
        NIMRTC_LOG_WARN("transport: webrtc-quic factory not yet "
                        "implemented (Slice 7.x 二期); media_stack is null");
    } else {
        // Rule 4 — default: media_stack = webrtc-classic
        //          control_stack = per needs_high_freq_control
        media_id   = "webrtc-classic";
        control_id = req.needs_high_freq_control ? "raw-udp-arq" : "";
    }

    // ---- Explicit user override (EngineConfig::transport_stack_name) ----
    //
    // The user's choice wins over the rule's media id.  The control
    // plane is NOT affected: whether a separate control stack exists is
    // a property of the strategy, not of the media backend.
    if (!media_override_.empty()) {
        media_id = media_override_;
    }

    set_trace(last_trace_, media_id, control_id);

    return instantiate(media_id, control_id);
}

TransportSession CapabilitySelector::instantiate(
    std::string_view media_factory_id,
    std::string_view control_factory_id) const {

    TransportSession session;

    // The per-stack backend selection.  The engine injects its own
    // (ICE / DTLS / RTP / SCTP / raw-UDP ids from `EngineConfig`); the
    // default constructor leaves the struct's defaults in place.
    StackConfig cfg = stack_cfg_;

    // The ARQ control plane needs a raw-UDP factory id.  The engine may
    // name one explicitly (`EngineConfig::raw_udp_name`); when it does
    // not, the in-tree default ("arq") is used — that is the id the
    // `raw_udp` module registers.
    const std::string arq_raw_id = cfg.raw_id.empty() ? std::string{"arq"}
                                                      : cfg.raw_id;

    session.media_stack_id   = std::string{media_factory_id};
    session.control_stack_id = std::string{control_factory_id};

    // ---- Media stack ----------------------------------------------------
    //
    // An ARQ media stack (Rule 2) needs the raw-UDP factory; a
    // webrtc-classic media stack does not.
    if (media_factory_id == "raw-udp-arq") {
        cfg.raw_id = arq_raw_id;
    }
    {
        const ITransportStackFactory* factory =
            factory_lookup_ ? factory_lookup_(media_factory_id) : nullptr;
        session.media_stack =
            try_create_stack(factory, factory_create_, cfg);
    }

    // ---- Control stack (optional) ---------------------------------------
    //
    // The control plane is always ARQ, so it always gets the raw-UDP
    // factory id.
    if (!control_factory_id.empty()) {
        StackConfig ccfg = cfg;
        ccfg.raw_id = arq_raw_id;
        const ITransportStackFactory* ctrl_factory =
            factory_lookup_ ? factory_lookup_(control_factory_id) : nullptr;
        session.control_stack =
            try_create_stack(ctrl_factory, factory_create_, ccfg);
    }

    return session;
}

} // namespace nimrtc
