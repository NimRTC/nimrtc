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
 * WHAT THIS FILE DOES NOT DO (intentional, Slice-7.5 work)
 * ----------------------------------------------------------------------------
 *   - It does NOT look up real `WebRtcClassicStackFactory` /
 *     `RawUdpArqStackFactory` instances. Those land in Slice 7.5,
 *     which depends on Slice 4 (DTLS) / 5 (SCTP) / 6 (raw_udp) default
 *     impls being merged.
 *   - The resolver callbacks passed at construction time are the test
 *     seam. Production code passes nullptr for both and the selector
 *     returns TransportSession with `media_stack = nullptr` and the
 *     chosen id strings recorded in `last_trace_` so downstream code
 *     (or post-mortem logs) can see what would have been picked.
 *
 *   Once Slice 7.5 lands, the engine will pass registry-driven resolvers
 *   here. The contract change is purely additive.
 * ----------------------------------------------------------------------------
 */
#include <nimrtc/core/log.hpp>
#include <nimrtc/transport/transport_selector.hpp>
#include <nimrtc/transport/transport_stack.hpp>

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
                                       StackCreateResolver factory_create) noexcept
    : factory_lookup_(factory_lookup),
      factory_create_(factory_create) {}

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

    TransportSession session;

    // Default stack config: bind ICE/DTLS/RTP/SCTP to whatever the
    // profile / env vars say. The stack factory will read this when
    // it builds its concrete plugins.
    StackConfig cfg;
    // (Future: cfg.ice_id / dtls_id / sctp_id / rtp_id populated from
    // the Profile's `transport.media` section. Slice 7 leaves them at
    // their defaults; the JSON parsing layer lands with the engine.)

    // ------- Rule 1 -----------------------------------------------------
    if (req.needs_browser_interop) {
        // Rule 1 — media_stack = webrtc-classic
        set_trace(last_trace_, "webrtc-classic",
                  req.needs_high_freq_control ? "raw-udp-arq" : "");
        session.media_stack_id   = "webrtc-classic";
        session.control_stack_id = req.needs_high_freq_control
                                       ? "raw-udp-arq" : "";

        const ITransportStackFactory* factory =
            factory_lookup_ ? factory_lookup_("webrtc-classic") : nullptr;
        session.media_stack =
            try_create_stack(factory, factory_create_, cfg);

        if (req.needs_high_freq_control) {
            cfg.raw_id = "arq";
            const ITransportStackFactory* ctrl_factory =
                factory_lookup_ ? factory_lookup_("raw-udp-arq") : nullptr;
            session.control_stack =
                try_create_stack(ctrl_factory, factory_create_, cfg);
        }
        return session;
    }

    // ------- Rule 2 -----------------------------------------------------
    if (!req.needs_browser_interop && req.needs_high_freq_control) {
        // Rule 2 — media_stack + control_stack both = raw-udp-arq
        set_trace(last_trace_, "raw-udp-arq", "raw-udp-arq");
        session.media_stack_id   = "raw-udp-arq";
        session.control_stack_id = "raw-udp-arq";

        cfg.raw_id = "arq";

        const ITransportStackFactory* media_factory =
            factory_lookup_ ? factory_lookup_("raw-udp-arq") : nullptr;
        session.media_stack =
            try_create_stack(media_factory, factory_create_, cfg);

        const ITransportStackFactory* ctrl_factory =
            factory_lookup_ ? factory_lookup_("raw-udp-arq") : nullptr;
        session.control_stack =
            try_create_stack(ctrl_factory, factory_create_, cfg);
        return session;
    }

    // ------- Rule 3 -----------------------------------------------------
    if (!req.needs_browser_interop && req.prefer_quic) {
        // Rule 3 — media_stack = webrtc-quic (Slice 7.x 二期)
        set_trace(last_trace_, "webrtc-quic",
                  req.needs_high_freq_control ? "raw-udp-arq" : "");
        session.media_stack_id   = "webrtc-quic";
        session.control_stack_id = req.needs_high_freq_control
                                       ? "raw-udp-arq" : "";

        // TODO (Slice 7.x 二期): wire msquic / ngtcp2 + a real
        // WebRtcQuicStackFactory. Until then this branch logs and
        // returns an empty media_stack so the rule selection is still
        // visible in `last_trace_`.
        NIMRTC_LOG_WARN("transport: webrtc-quic factory not yet "
                        "implemented (Slice 7.x 二期); media_stack is null");
        if (req.needs_high_freq_control) {
            cfg.raw_id = "arq";
            const ITransportStackFactory* ctrl_factory =
                factory_lookup_ ? factory_lookup_("raw-udp-arq") : nullptr;
            session.control_stack =
                try_create_stack(ctrl_factory, factory_create_, cfg);
        }
        return session;
    }

    // ------- Rule 4 -----------------------------------------------------
    // Default: media_stack = webrtc-classic; control per
    // needs_high_freq_control.
    set_trace(last_trace_, "webrtc-classic",
              req.needs_high_freq_control ? "raw-udp-arq" : "");
    session.media_stack_id   = "webrtc-classic";
    session.control_stack_id = req.needs_high_freq_control
                                   ? "raw-udp-arq" : "";

    const ITransportStackFactory* factory =
        factory_lookup_ ? factory_lookup_("webrtc-classic") : nullptr;
    session.media_stack =
        try_create_stack(factory, factory_create_, cfg);

    if (req.needs_high_freq_control) {
        cfg.raw_id = "arq";
        const ITransportStackFactory* ctrl_factory =
            factory_lookup_ ? factory_lookup_("raw-udp-arq") : nullptr;
        session.control_stack =
            try_create_stack(ctrl_factory, factory_create_, cfg);
    }
    return session;
}

} // namespace nimrtc
