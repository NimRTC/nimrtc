/**
 * @file src/engine/src/engine_impl.hpp
 * @brief PRIVATE engine internals: the `NimRTCEngine::Impl` state.
 *
 * NOT installed, NOT included by any public header (Layout Invariant 4).
 *
 * ## What this header holds (architecture remediation plan §4.3 / D1)
 *
 * The engine's internal state, shared by the engine translation units.
 * The header used to be a 1674-line catch-all that mixed three very
 * different things:
 *
 *   1. **free helper functions** — the SDP direction algebra, the
 *      payload-type parser, the base64 / hex decoders and the
 *      `a=rtcp-fb` / `a=rtcp-xr` parsers.  These are grouped by
 *      concern into `engine_sdp_helpers.hpp` and
 *      `engine_rtcp_helpers.hpp`.
 *   2. **the `Impl` state** — what this header is now: the per-session
 *      state the engine owns.
 *   3. **the `Impl` methods** — the ref-frame table's open-addressed
 *      hash table, which is self-contained and belongs with its own
 *      state; it now lives in `engine_ref_frame_table.hpp`.
 *
 * ## The state is grouped by concern
 *
 * What is left in `Impl` is the *engine's own* state — the composition
 * it owns (the SDP parser / munger, the DTLS handle, the SRTP context,
 * the transport session) plus a handful of counters that do not belong
 * to any one feature.  Everything that belongs to one feature lives in
 * a named collaborator next to this header:
 *
 *   | collaborator                    | header                        |
 *   |---------------------------------|-------------------------------|
 *   | `detail::AudioState`            | `engine_audio_state.hpp`      |
 *   | `detail::VideoState`            | `engine_video_state.hpp`      |
 *   | `detail::SessionState`          | `engine_session_state.hpp`    |
 *   | `detail::ScratchBuffers`        | `engine_scratch.hpp`          |
 *   | `detail::TimelineState`         | `engine_timeline_state.hpp`   |
 *   | `detail::XrState`               | `engine_xr_state.hpp`         |
 *   | `detail::RefFrameTable`         | `engine_ref_frame_table.hpp`  |
 *
 * The rule is simple: a member that only one feature reads or writes
 * belongs to that feature's collaborator, not to `Impl`.  The engine's
 * own composition stays here.
 *
 * The collaborators the plan §4.3 proposes (a `VideoPipeline`, an
 * `AudioPipeline`, an `RtpPacketizer`, …) are *not* introduced by this
 * change: each of them would move a coherent slice of behaviour *out*
 * of the engine, and doing that is a behavioural change that needs its
 * own review.  This change is the purely mechanical part — separating
 * the header's three kinds of content — so that the later moves are
 * reviewable in isolation.
 *
 * ## The `detail` namespace
 *
 * The few helpers that remain here (`trace`, `steady_now_us`,
 * `parse_net_feedback_policy`) are the ones that are used by *every*
 * engine translation unit and that carry no concern of their own.
 */
#pragma once

/**
 * @file src/engine/src/engine.cpp
 * @brief NimRTCEngine — wires ICE + SDP + RTP + JB + Audio3A + DTLS + SRTP + Opus.
 *
 * P1.1 implementation:
 *   - open(): create ICE transport via PluginRegistry (id="ice" → IceTransport),
 *     SDP parser/munger, Audio3A, Opus codec, DTLS session, SRTP context.
 *   - create_offer: emit RFC 8829-compliant audio-only offer with Opus payload,
 *     BUNDLE, rtcp-mux, ICE credentials, candidates, DTLS fingerprint.
 *   - process_remote_sdp: parse remote SDP, apply ICE credentials + candidates,
 *     configure DTLS role + peer fingerprint, emit answer.
 *   - tick: drain transport.recv() + drain_dtls(); demux by first byte.
 *   - send_audio: 3A → Opus encode → RTP packetise → SRTP encrypt → ICE.send.
 *
 * ICE-specific methods (state(), local_ufrag(), wait_for_gathering(),
 * gathered_local_candidates(), set_remote_description(), etc.) are accessed
 * through `plugins::IICETransport*` — the engine stores the resolved
 * IICETransport as `ice_t_` and calls through that virtual interface; no
 * `dynamic_cast<ice::IceTransport*>` happens at the engine layer (the only
 * remaining cast is a one-shot safety-net inside open()/pre_open() that
 * lets a generic ITransport plugin still satisfy the ICE-aware surface if
 * the user chose not to register an IICETransportFactory).
 */

#include "nimrtc/engine/engine.hpp"
#include "audio_routing.hpp"
#include "bandwidth_allocation.hpp"
#include "engine_audio_state.hpp"
#include "engine_rtcp_helpers.hpp"
#include "engine_ref_frame_table.hpp"
#include "engine_scratch.hpp"
#include "engine_sdp_helpers.hpp"
#include "engine_session_state.hpp"
#include "engine_timeline_state.hpp"
#include "engine_video_state.hpp"
#include "engine_xr_state.hpp"
#include "video_codec_config.hpp"
#include "video_negotiation.hpp"
#include "video_stats_bridge.hpp"

#include <array>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <vector>

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/time.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/core/engine_errors.hpp>   // P1#9 — module error-code constants
#include <nimrtc/engine/pal/engine_plugin_resolver.hpp>
#include <nimrtc/engine/pal/make_plugin.hpp>     // PAL Slice 1.5 — plugin
                                                // 5-step dance helper
#include <nimrtc/ice/ice.hpp>
#include <nimrtc/plugins/transport.hpp>
#include <nimrtc/plugins/audio3a.hpp>
#include <nimrtc/plugins/bwe.hpp>
#include <nimrtc/plugins/scheduler.hpp>
// Transport strategy + Selector (v0.12.0 Slice 7.6 PR3).
//   - transport_selector.hpp: TransportRequirements + CapabilitySelector
//   - transport_strategy.hpp: strategy_by_name() + the bridge
//     requirements_from_profile_input() that consumes a
//     ProfileTransportInput (transport-side mirror of
//     assembly::TransportSection) and returns the Selector's input.
#include <nimrtc/transport/transport_selector.hpp>
#include <nimrtc/transport/transport_strategy.hpp>
// Slice 7.6 PR3 (v0.12.0): TransportSession + ITransportStackFactory.
// The factory lookups (get_transport_stack by §5.3-rule-derived ids) are
// expressed inline in init_modules_once(); transport_plugin.hpp is NOT
// included to avoid a PUBLIC link dependency on make_transport_instance().
#include <nimrtc/transport/transport_stack.hpp>
#include <nimrtc/transport/transport_plugin.hpp>
// The H.264 codec module is *not* included here.  It used to be, behind
// `NIMRTC_HAS_H264`, purely so its `register_default_plugins()` symbol
// would be visible to the linker -- the engine uses no type from it.
// ADR-025 Decision 1 called that include "a link-time registration
// convenience, not a type dependency", and said it was scheduled for
// removal when the engine's default-registrar table was split out of
// `engine_impl.hpp` (remediation plan D1).  That split has landed:
// the registrar table now lives in `src/core/src/pal_default_registrars.cpp`
// and lists `nimrtc::h264::register_default_plugins` there, so the
// engine does not need the header to pull the symbol in.  The engine
// reaches the H.264 codec through the registry's video-codec slot.

// Concrete module headers — required by NimRTCEngine::Impl which lives in
// this translation unit.  The public header (engine.hpp) does NOT include
// any of these; consumers only need the plugin interfaces.
#include <nimrtc/sdp/session_description.hpp>
#include <nimrtc/rtp/packet.hpp>
#include <nimrtc/rtp/rtcp.hpp>          // RTCP demux (v1.2.0 M1, F1)
#include <nimrtc/rtp/timeline.hpp>     // ADR-017 cross-end timeline seam
#include <nimrtc/audio3a/audio3a.hpp>
// The video-quality statistics bridge (Phase 4.2 / D2).  The bridge makes
// the `nimrtc::video_stats` dependency *optional* — the module may be
// excluded from the build — while keeping the engine's call sites
// readable.  See `src/engine/src/video_stats_bridge.hpp`.

// Choose DTLS implementation via the PAL Slice 4 / Slice 8 seam.
// `engine.hpp::EngineConfig::dtls_name` selects the backend (empty
// = default = built-in wolfSSL factory at id="wolfssl"); the engine
// resolves the factory through `core::PluginRegistry::get_dtls_session(id)`
// at open() time, exactly as the Slice 8 typed registry hook requires
// (see `src/core/include/nimrtc/core/registry.hpp` §5.4).
//
// As of TPAL-4 (v0.11.0) the `Impl::dtls` field stores
// `unique_ptr<IDtlsSession>` so the engine never downcasts to the
// concrete backend — `DtlsSessionWolfSSL`, `DtlsSession` and any
// future 国密 / OpenSSL / BoringSSL / mbedTLS backend now implement
// the same surface (set_role(DtlsRole) / local_fingerprint() /
// feed_inbound() / take_outbound() / tick() / state() /
// is_connected() / srtp_keying_material() / set_peer_fingerprint()).
#include <nimrtc/dtls/dtls_session_iface.hpp>
#include <nimrtc/web_transport/web_transport_config.hpp>
#include <nimrtc/web_transport/web_transport_session_factory.hpp>
#include <nimrtc/web_transport/web_transport_session_iface.hpp>
// TPAL-4 cleanup: `DtlsSession::state_name()` is a static method on the
// concrete module class (used by the engine's state-transition logger
// below).  Including the concrete header here is allowed — Layout
// Invariant 4 constrains only `engine.hpp`, not the .cpp.  Keeping the
// include next to the seam header so reviewers see both in context.
#include <nimrtc/dtls/dtls.hpp>
#include <nimrtc/srtp/srtp.hpp>
// NOTE (v1.2.0, audio pipeline WP-7): the engine no longer includes
// `nimrtc/jb/jitter_buffer.hpp` nor `nimrtc/opus/opus.hpp`.  Both the
// receive pipeline and the encoder are reached through the plugin
// seams (`plugins::IAudioReceiver`, `plugins::ICodec`), which is
// exactly what constraint L4 asks for: the engine orchestrates plugins
// and never names a concrete module.

namespace nimrtc::engine {

namespace detail {

inline void trace(const char* fmt, ...) noexcept {
#if defined(_DEBUG) || defined(NIMRTC_TRACE)
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    core::log::Logger::instance().trace(std::string_view{buf});
#else
    (void)fmt;
#endif
}

/** The local monotonic clock, in microseconds.
 *
 *  This is the clock the timeline seam is defined in terms of: it never
 *  jumps (no NTP steps, no manual clock changes) and it is what the
 *  tracker's `local_recv_us_since_epoch` argument carries.  The wall
 *  clock is the *peer's* domain — the SR's NTP instant and the
 *  reconstructed capture timestamp live there — so the two are related
 *  through the monotonic origin, never mixed directly.
 *
 *  The steady clock's epoch is the boot time on every platform we
 *  support, so the reading is "microseconds since boot". */
inline std::int64_t steady_now_us() noexcept {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            core::SteadyClock::now().time_since_epoch()).count());
}

/** Map the host's configuration string to a signal-selection policy
 *  (ADR-018 Decision 2).
 *
 *  Unknown values fall back to the automatic default: the policy must
 *  never be the reason a session fails. */
inline plugins::NetFeedbackPolicy
parse_net_feedback_policy(std::string_view name) noexcept {
    if (name == "loss_rtt" || name == "loss-rtt" || name == "lossrtt") {
        return plugins::NetFeedbackPolicy::kLossRtt;
    }
    if (name == "require_transport_cc" || name == "require-transport-cc") {
        return plugins::NetFeedbackPolicy::kRequireTransportCc;
    }
    return plugins::NetFeedbackPolicy::kAuto;
}

}  // namespace detail

struct NimRTCEngine::Impl {
    // SDP: Parser/Munger are concrete classes.
    std::unique_ptr<sdp::Parser>    sdp_parser = std::make_unique<sdp::Parser>();
    std::unique_ptr<sdp::Munger>    sdp_munger = std::make_unique<sdp::Munger>();

    // Concrete fallback classes (when no plugin adapter is registered).
    std::unique_ptr<audio3a::IAudio3A> audio3a_concrete;

    // SRTP / DTLS / Opus — DTLS is now PAL Slice 4 seam-exposed (TPAL-4
    // v0.11.0).  `Impl::dtls` holds a NON-OWNING pointer to
    // `nimrtc::dtls::IDtlsSession` — the OWNING instance lives inside
    // the media `ITransportStack` (see `Impl::session` below, built by
    // `make_transport_instance()` in `init_modules_once()`).
    //
    // Why a weak_ptr (not `unique_ptr<IDtlsSession>` or raw pointer)?
    //   - Pre-Slice-7.6 PR3 (v0.12.0), the engine built DTLS itself
    //     via `reg.get_dtls_session("wolfssl")` and stored it in a
    //     `unique_ptr`. The Slice 7.6 refactor moves that construction
    //     into `WebRtcClassicStackFactory` (the "webrtc-classic" stack
    //     factory), which produces a `WebRtcClassicStack` that owns
    //     the DTLS session. The engine no longer builds DTLS
    //     independently — the stack's factory does, and the engine
    //     reaches the resulting session through the stack's
    //     `dtls_weak()` accessor.
    //   - Storing a `unique_ptr<dtls>` AND a stack-owned DTLS
    //     session would double-free on destruction.  Storing a raw
    //     pointer is safe (no ownership) but introduces the UAF class:
    //     if the engine dereferences the pointer after the stack dies,
    //     it reads freed memory.  weak_ptr resolves both: the stack
    //     keeps ownership via its internal `shared_ptr`, and the
    //     engine's `weak_ptr.lock()` returns nullptr once the stack
    //     releases that shared_ptr (i.e. on `close()`).
    //
    // Lifetime: `impl_->session.media_stack` is destroyed in
    // `delete impl_` (which `~NimRTCEngine` runs before its own
    // members are destroyed).
    //
    // v0.12.0 P0: is_open() guards prevent post-close UAF on the API
    // surface.  v0.13.0 (this change): changed to weak_ptr so
    // .lock() returns nullptr automatically after close(), eliminating
    // the UAF class entirely.  All callers now use:
    //   if (auto* d = impl_->dtls.lock().get()) { d->X(); }
    std::weak_ptr<nimrtc::dtls::IDtlsSession>  dtls;
    std::unique_ptr<srtp::SrtpContext> srtp;

    // ---- Resolved transport session (v0.12.0 Slice 7.6 PR3) ---------
    //
    // Built by `make_transport_instance()` in `init_modules_once()` from
    // the Selector's resolved (media_factory_id, control_factory_id).
    // The session owns the media and (optionally) control
    // ITransportStack instances. `ice_t_` is a raw pointer into this
    // session (see the comment above that field); `impl_->dtls` is a
    // weak_ptr into the same session (v0.13.0, see the comment above
    // the field).
    //
    // For the 4 browser-interop strategies (browser-call, agent-gateway,
    // sfu-relay, cross-network-peer):
    //   - `session.media_stack`  → WebRtcClassicStack (owns ICE / DTLS / RTP / SCTP)
    //   - `session.control_stack` → null (browser-call / sfu-relay / cross-network-peer)
    //                              RawUdpArqStack (agent-gateway — high-freq control)
    //
    // For teleop-ultra-low-latency (§5.3 Rule 2):
    //   - `session.media_stack`  → RawUdpArqStack (no DTLS — not browser-interop)
    //   - `session.control_stack` → RawUdpArqStack
    //   - `impl_->dtls` = nullptr (no DTLS in media path for this strategy)
    //   - Audio pipeline falls back to null-encoder; ICE/DTLS work via
    //     the control stack's raw-UDP ARQ
    TransportSession session;

    // ---- The DTLS / SRTP install state --------------------------------
    bool            dtls_active_inbound = false;
    bool            srtp_installed      = false;
    int             srtp_stats_drop     = 0;
    nimrtc::dtls::DtlsState last_dtls_state = nimrtc::dtls::DtlsState::Closed;

    // ---- Network feedback loop (v1.2.0 M1, F2/F6) ----------------------
    //
    // The loop is: inbound RTCP -> engine demux -> collector -> snapshot
    // -> IBwe::on_feedback().  These two counters make the loop
    // observable: `net_feedback_blocks` counts the RTCP blocks that were
    // routed to the collector, `bwe_feedback_samples` counts the
    // aggregated snapshots that were actually handed to the estimator.
    //
    // The last two fields are the de-duplication state: a snapshot is
    // fed only when it carries new information (its timestamp moved),
    // so the estimator sees one sample per feedback report rather than
    // one per engine tick.
    std::uint64_t net_feedback_blocks   = 0;
    std::uint64_t bwe_feedback_samples  = 0;
    core::TimePoint last_fed_feedback_ts{};
    bool           has_fed_feedback     = false;

    // True once the engine has reported that the transport-cc path was
    // required but is unavailable (ADR-018 Decision 2's "fail loudly"
    // mode).  The report is made once per session.
    bool           transport_cc_unavailable_reported = false;

    // ---- RTCP Extended Reports (v1.2.0, session plan X1) ---------------
    //
    // The number of inbound RTCP XR sub-packets that were routed to the
    // XR consumer.  It is the observable that proves the wiring is
    // closed: a peer that sends extended reports and a counter that
    // stays at zero mean the routing is broken.
    std::uint64_t xr_report_blocks = 0;

    // ---- The negotiated RTCP XR / TMMBR state --------------------------
    //
    // The RFC 3611 extended-report state and the RFC 5104 TMMBR / TMMBN
    // state, grouped into one collaborator: the negotiated flags are the
    // *authority* on what may be sent (RFC 3611 §5.1, RFC 5104 §7.2),
    // and they only make sense together with the state they gate.
    detail::XrState xr;

    // ---- Pacing split (v1.2.0 M3, P2) ----------------------------------
    //
    // The demand list of the bandwidth split, kept as a member so that
    // the per-tick split does not allocate a vector every time.  The
    // contents are rewritten (never reallocated) on each tick.
    std::vector<detail::BandwidthDemand> pacing_demands_;

    // ---- The feature-scoped state --------------------------------------
    //
    // Every member that belongs to exactly one feature lives in the
    // matching collaborator; see the table in the file header.
    detail::AudioState   audio_state;
    detail::VideoState   video_state;
    detail::SessionState session_state;
    detail::ScratchBuffers scratch;

    // ---- Cross-end timeline (ADR-017) ---------------------------------
    //
    // The timeline seam (extmap negotiation, outbound abs-send-time, the
    // outbound Sender Report, and the inbound SR / abs-send-time
    // refinement) lives in a dedicated collaborator so that this header
    // does not carry a dozen loose members for one feature.
    //
    // See `engine_timeline_state.hpp` for the field-by-field rationale.
    detail::TimelineState timeline;

    // ---- Outbound Sender Report state ----------------------------------
    // `sr_*` carry the values an SR body reports for the audio stream
    // (RFC 3550 §6.4.1 sender info).  Video is folded into the same SR via
    // report blocks only when a video sender is active; the audio stream is
    // the SR's own media source because it is always the first one out.
    std::chrono::steady_clock::time_point sr_last_sent{};
};

}  // namespace nimrtc::engine
