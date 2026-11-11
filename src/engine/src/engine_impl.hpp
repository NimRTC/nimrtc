/**
 * @file src/engine/src/engine_impl.hpp
 * @brief PRIVATE engine internals (v1.1 R1).
 *
 * Includes, internal helpers (nimrtc::engine::detail) and the
 * NimRTCEngine::Impl definition shared by the five engine TUs.
 * NOT installed, NOT included by any public header: the public
 * engine.hpp is untouched by R1 (L4).
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
#ifdef NIMRTC_HAS_H264
#include <nimrtc/h264/codec_plugin.hpp>
#endif

// Concrete module headers — required by NimRTCEngine::Impl which lives in
// this translation unit.  The public header (engine.hpp) does NOT include
// any of these; consumers only need the plugin interfaces.
#include <nimrtc/sdp/session_description.hpp>
#include <nimrtc/rtp/packet.hpp>
#include <nimrtc/rtp/rtcp.hpp>          // RTCP demux (v1.2.0 M1, F1)
#include <nimrtc/rtp/timeline.hpp>     // ADR-017 cross-end timeline seam
#include <nimrtc/audio3a/audio3a.hpp>

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

/** Parse a payload type out of an SDP `a=rtpmap:<fmt>` token.
 *
 *  The token is the format as it appeared in the SDP: a decimal number
 *  for both the static (0..95) and the dynamic (96..127) ranges.  A
 *  wildcard or a non-numeric token yields false.
 *
 *  Deliberately hand-rolled rather than `std::stoul`: the caller runs
 *  inside a `noexcept` function, where a thrown `std::invalid_argument`
 *  would terminate the process. */
inline bool parse_payload_type(std::string_view fmt, std::uint8_t& out) noexcept {
    if (fmt.empty() || fmt.size() > 3) return false;
    std::uint32_t v = 0;
    for (char c : fmt) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<std::uint32_t>(c - '0');
        if (v > 127) return false;
    }
    out = static_cast<std::uint8_t>(v);
    return true;
}

inline std::string make_session_id() {
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
    std::random_device rd;
    std::mt19937_64 mt(rd());
    return std::to_string(now) + std::to_string(mt() & 0xFFFF);
}

inline std::vector<std::uint8_t> base64_decode(std::string_view sv) {
    static const auto kIdx = []{
        std::array<int8_t, 256> t{};
        t.fill(-1);
        const char* a =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) t[static_cast<uint8_t>(a[i])] = static_cast<int8_t>(i);
        return t;
    }();
    std::vector<std::uint8_t> raw;
    raw.reserve((sv.size() * 3) / 4);
    int val = 0, bits = 0;
    for (char c : sv) {
        if (c == '=') break;
        // `char` is signed on x86_64 but UNSIGNED on aarch64 and on
        // macOS arm64, so a `c < 0` guard would be an always-false
        // comparison there -- rejected by -Werror=type-limits.  It is
        // also redundant: kIdx is a 256-entry table initialised to -1
        // and only the 64 base64 alphabet bytes are given a non-negative
        // index, so kIdx[byte] < 0 already rejects every non-alphabet
        // byte, including all bytes >= 0x80.  Iterate over the byte
        // value directly instead of the (signedness-dependent) char.
        const auto uc = static_cast<unsigned char>(c);
        const int8_t idx = kIdx[uc];
        if (idx < 0) continue;
        val = (val << 6) | idx;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            raw.push_back(static_cast<std::uint8_t>((val >> bits) & 0xff));
        }
    }
    return raw;
}

// Decode RFC 8122 DTLS fingerprint value = uppercase hex bytes separated by ':'.
// Example: "AA:BB:CC:..." -> {0xAA, 0xBB, 0xCC, ...}.  Tolerant of whitespace.
// Returns empty vector on malformed input.
inline std::vector<std::uint8_t> hex_colon_decode(std::string_view sv) {
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        return -1;
    };
    std::vector<std::uint8_t> out;
    out.reserve(sv.size() / 3 + 1);
    int hi = -1;
    for (char c : sv) {
        if (c == ':' || c == ' ' || c == '\t') {
            if (hi >= 0) return {};     // dangling half-byte
            continue;
        }
        int v = hex(c);
        if (v < 0) return {};
        if (hi < 0) hi = v;
        else {
            out.push_back(static_cast<std::uint8_t>((hi << 4) | v));
            hi = -1;
        }
    }
    if (hi >= 0) return {};            // dangling half-byte at end
    return out;
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

/** True when an `a=rtcp-fb` entry mentions the given feedback type.
 *
 *  The entry is the SDP text that follows "rtcp-fb:", i.e.
 *  "<pt> <type> [<type>…]" (RFC 4585 §4.1).  The payload type is
 *  scoped to its m-section, and browsers disagree about what they put
 *  there — Chrome uses the *media* payload type, not 205 — so the
 *  feedback *type* token is what identifies the format. */
inline bool rtcp_fb_has_type(std::string_view entry,
                             std::string_view type) noexcept {
    // Skip the payload-type token.
    std::size_t i = 0;
    while (i < entry.size() && entry[i] != ' ') ++i;
    while (i < entry.size()) {
        while (i < entry.size() && entry[i] == ' ') ++i;
        const std::size_t start = i;
        while (i < entry.size() && entry[i] != ' ') ++i;
        if (entry.compare(start, i - start, type) == 0) return true;
    }
    return false;
}

/** The feedback types of an `a=rtcp-fb` entry — everything after the
 *  payload-type token. */
inline std::string_view rtcp_fb_types(std::string_view entry) noexcept {
    const std::size_t sp = entry.find(' ');
    if (sp == std::string_view::npos) return {};
    std::size_t i = sp + 1;
    while (i < entry.size() && entry[i] == ' ') ++i;
    return entry.substr(i);
}

/** True when every feedback type of an `a=rtcp-fb` entry is one the
 *  engine supports.
 *
 *  RFC 3264 §6.1: an answer may only carry the feedback types the
 *  answerer supports, so the answer echoes the offer's set intersected
 *  with the supported one.  An entry that mixes a supported type with
 *  an unsupported one ("111 nack trr-int") is dropped as a whole — the
 *  engine cannot honour half of it. */
inline bool rtcp_fb_entry_supported(std::string_view entry) noexcept {
    std::size_t i = entry.find(' ');
    if (i == std::string_view::npos) return false;
    ++i;
    bool any = false;
    while (i < entry.size()) {
        while (i < entry.size() && entry[i] == ' ') ++i;
        const std::size_t start = i;
        while (i < entry.size() && entry[i] != ' ') ++i;
        const std::string_view tok = entry.substr(start, i - start);
        if (tok.empty()) continue;
        if (tok != "nack" && tok != "pli" && tok != "ccm" && tok != "fir" &&
            tok != "goog-remb" && tok != "transport-cc") {
            return false;
        }
        any = true;
    }
    return any;
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

    bool            dtls_active_inbound = false;
    bool            srtp_installed      = false;
    int             srtp_stats_drop     = 0;
    // ---- Video counters (v1.1 V1, cumulative since open()) ------------------
    std::uint64_t video_frames_sent       = 0;
    std::uint64_t video_packets_sent      = 0;
    std::uint64_t video_bytes_sent        = 0;
    std::uint64_t video_frames_received   = 0;
    std::uint64_t video_nack_seqs_queued  = 0;
    std::uint64_t video_keyframes_q       = 0;
    std::uint64_t video_feedback_applied  = 0;
    std::uint64_t video_keyframes_applied = 0;
    nimrtc::dtls::DtlsState last_dtls_state     = nimrtc::dtls::DtlsState::Closed;

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

    // ---- Ref-frame timeline map (v1.1 T1/T2, UNSTABLE) -------------------
    // Local registry: ref_frame id (== VideoSourceFrame::frame_seq) ->
    // capture_ts_us. Bounded ring (1024 entries); insertion-order deque
    // drives eviction so the map cannot grow without bound. Single-threaded
    // by engine contract (same as sr_tracker); no locking.
    static constexpr std::size_t kRefFrameMapCap = 1024;
    std::map<std::uint64_t, std::int64_t> ref_frame_to_ts;
    std::deque<std::uint64_t>             ref_frame_order;

    // ---- ICE flap -> DTLS rehandshake (v1.1 Dtls2, additive) ------------------
    // Tracks ICE state across tick() to detect NAT rebinding:
    // connected once -> disconnected (arm) -> connected again (trigger
    // IDtlsSession::rehandshake()). plugins::IceState mirrors
    // ice::IceState 1:1; no conversion needed.
    plugins::IceState last_ice_state = plugins::IceState::Disconnected;
    bool ice_connected_once = false;
    bool ice_rehandshake_armed = false;

    // Local SDP (after create_offer / process_remote_sdp).
    std::optional<sdp::SessionDescription> local_sdp;

    // ---- Video receive pipeline decode buffers (ping-pong I420) ------------
    // Allocated lazily on first decoded frame; reused across frames.
    // We keep two buffers so we can receive a new frame while the previous
    // one is still being rendered (the sink takes a view, not ownership).
    static constexpr std::size_t kDecodeBufferCount = 2;
    std::array<std::vector<std::uint8_t>, kDecodeBufferCount> video_decode_bufs;
    std::atomic<std::size_t> video_decode_buf_idx_{0};
    std::uint32_t video_decode_width_  = 0;
    std::uint32_t video_decode_height_ = 0;

    // ---- Video send pipeline state ------------------------------------------
    // Tracks whether the next encoded frame is a keyframe (forced by
    // force_keyframe() or signalled by the codec).  Consumed by the sender's
    // packet callback to set kVideoKeyframe priority on the marker packet.
    std::atomic<bool> video_keyframe_pending_{false};

    // ---- RTCP feedback state (v1.1 Q3) -------------------------------------
    //
    // The feedback loop has two halves:
    //
    //   outbound  receiver detects a loss -> the receiver plugin encodes a
    //             NACK / PLI -> the engine protects it (SRTCP) and sends it.
    //             The RTCP *sender* SSRC identifies us; the *media* SSRC is
    //             the peer's video stream the feedback is about.
    //
    //   inbound   the peer sends a NACK / PLI -> the engine hands the bytes
    //             to the sender plugin -> retransmission / forced IDR.
    //
    // `remote_video_ssrc_` is learned from the inbound RTP stream (the peer
    // picks its own SSRC; our config only carries the *expected* one), so
    // the NACK we send points at the right media source.
    std::uint32_t remote_video_ssrc_    = 0;
    /** Our own SSRC as the sender of the RTCP feedback (RFC 3550 §6.1). */
    std::uint32_t video_feedback_ssrc_  = 0;
    /** Feedback reports waiting to be put on the wire.  The receiver
     *  raises them from inside the ICE receive path, so the engine queues
     *  them and sends them from `tick()` — the same place the Sender
     *  Report is sent from.  Re-entering the transport's send() from
     *  inside its recv() would be a needless risk. */
    plugins::VideoFeedbackReport pending_video_feedback_{};
    bool                        has_pending_video_feedback_ = false;

    // ---- DTLS role negotiation ---------------------------------------------
    // Resolved in process_remote_sdp() from the peer's a=setup attribute.
    // Used to emit a matching a=setup on the answer SDP.  Empty until
    // process_remote_sdp() runs; defaults to "active" if unset.
    std::string dtls_local_setup;

    /** The DTLS role this engine ended up playing (RFC 5763 §5).
     *
     *  `true`  = DTLS **Client** — we send the ClientHello.
     *  `false` = DTLS **Server** — we wait for the peer's ClientHello.
     *
     *  The role decides two things:
     *
     *    1. which `a=setup` attribute we advertise in the answer, and
     *    2. which half of the DTLS-SRTP keying material is *ours*
     *       (RFC 5764 §4.2): the client uses `client_master_key` for its
     *       outbound stream, the server uses `server_master_key`.
     *
     *  It is resolved from the peer's `a=setup` attribute when we are the
     *  answerer, and from the answer's attribute when we are the offerer
     *  (see `process_remote_sdp()`). */
    bool dtls_is_client = false;

    /** The `o=` session id of the offer we produced, if any.
     *
     *  RFC 3264 §5.1: the answer carries the *offer's* session id, so the
     *  offerer can tell "this is the answer to my offer" from "this is a
     *  fresh offer" by comparing the ids.  That distinction is what lets
     *  `process_remote_sdp()` pick the right DTLS role (the answer commits
     *  the roles, an offer does not) and skip generating an answer for an
     *  answer.  Empty when we never produced an offer. */
    std::string local_session_id;

    // ---- send_audio scratch buffers (P0#4) --------------------------------
    // Reusable per-engine buffers for the audio capture→encode→RTP→SRTP
    // path.  Capacity grows monotonically across calls, avoiding the heap
    // allocations that a fresh `std::vector` per call would incur.
    //
    // `audio_codec_pkt` holds the raw codec (Opus) output for the in-flight
    // frame.  The outbound (SRTP-protected) packet is NOT kept here: it is
    // taken from the scheduler's packet pool and handed back to the pool
    // once the packet has been drained (P1 hardening, 2026-11-09).  Keeping
    // it in a member and MOVING it into the scheduler would leave the
    // member moved-from, so every packet would allocate a fresh buffer.
    std::vector<std::uint8_t> audio_codec_pkt;

    // Scratch buffer for the capture-side PCM.  The 3A stage processes audio
    // in place, but send_audio() receives the caller's buffer as `const
    // float*` — writing through a const_cast would be undefined behaviour and
    // would silently mutate the caller's data.  The engine therefore owns a
    // copy: the caller's buffer is read-only, the copy is what 3A mutates and
    // what the encoder consumes.
    std::vector<float> audio_pcm_buf;

    // ---- send_video scratch buffer (P0#4) ---------------------------------
    // Reusable buffer for the encoded H.264 frame.  Default reserve size
    // matches the worst-case 64 KiB IDR; resize grows monotonically.
    std::vector<std::uint8_t> video_enc_buf;

    // ---- Cross-end timeline (ADR-017) --------------------------------------
    //
    // `EngineConfig::timeline_enabled` turns this seam on.  Four things
    // hang off it (ADR-017 §"Decision 5" — the engine side):
    //
    //   1. extmap negotiation in create_offer() / process_remote_sdp()
    //      (`local_abs_send_time_id` / `remote_abs_send_time_id`);
    //   2. outbound abs-send-time on every RTP packet we build
    //      (`abs_send_time_origin_ntp64` anchors the encoder);
    //   3. outbound Sender Reports on `EngineConfig::sr_period_ms`
    //      (`sr_last_sent`, plus the per-stream RTP-TS / packet / octet
    //      counters the SR body carries);
    //   4. inbound SR + abs-send-time into `sr_tracker`, whose output is
    //      exposed through the `capture_ts_us_*` accessors.
    //
    // While the flag is false, every field here stays at its default and
    // no abs-send-time / SR is ever emitted (ADR-017 §"Decision 5").
    rtp::SenderReportTracker sr_tracker;

    /** The RTP header-extension id WE advertise for abs-send-time in our
     *  own SDP.  This is the id the *peer* will expect on the wire, so it
     *  is what outbound packets are built with.  0 = not advertised. */
    std::uint16_t local_abs_send_time_id = 0;

    /** The extension id the PEER advertised for its own abs-send-time.
     *  Inbound packets are parsed with this id.  0 = the peer never
     *  advertised the extension, so no inbound refinement is possible. */
    std::uint16_t remote_abs_send_time_id = 0;

    /** Monotonic-clock origin, shared by the abs-send-time encoder and
     *  `SenderReportTracker::set_monotonic_origin()`.
     *
     *  The value is the wall-clock instant (RFC 5905) that corresponds
     *  to a local **monotonic** reading of zero — i.e. the boot epoch.
     *  A monotonic sample of `m` µs therefore sits at the Unix instant
     *  `unix_us(origin) + m`, which is what both the encoder and the
     *  tracker's skew computation assume.
     *
     *  It is NOT "the wall clock at open()": that value would make the
     *  tracker's `origin + monotonic_reading` double-count the epoch
     *  (the tracker adds the monotonic reading to the origin) and every
     *  skew sample would be off by the whole Unix epoch. */
    rtp::Ntp64 monotonic_origin_ntp64{0};

    /** Local wall clock (µs since Unix epoch) at open() minus the steady
     *  clock reading taken at the same instant.  Lets the abs-send-time
     *  encoder convert a send-time monotonic stamp into Unix-anchored
     *  microseconds without a second clock read per packet.
     *
     *  This is the same quantity as `unix_us(monotonic_origin_ntp64)` —
     *  kept as a plain integer so the per-packet path does not have to
     *  re-derive it from the NTP64. */
    std::int64_t boot_epoch_offset_us = 0;

    /** Last reconstructed capture instant per inbound SSRC (µs since Unix
     *  epoch, on the *peer's* wall clock).  Ordered so `last_capture_ts`
     *  can report the most recently updated stream. */
    std::map<std::uint32_t, std::int64_t> capture_ts_us_by_ssrc;

    /** Local arrival instant (µs since Unix epoch) of the same frame, used
     *  to derive the one-way delay.  Parallel to the map above. */
    std::map<std::uint32_t, std::int64_t> arrival_us_by_ssrc;

    /** Capture instant + arrival of the most recent inbound frame across
     *  all SSRCs — backs `last_capture_ts_us()` / `last_one_way_delay_us()`. */
    std::optional<std::int64_t> last_capture_us;
    std::optional<std::int64_t> last_arrival_us;

    /** Inter-clock skew of the most recent inbound frame (µs) — the
     *  change since the last Sender Report of the offset between our
     *  clock and the peer's (RFC 3550 §8.1).  Backs
     *  `last_sender_recv_skew_us()`. */
    std::optional<std::int64_t> last_skew_us;

    /** Inter-clock skew per inbound SSRC — backs
     *  `sender_recv_skew_us_for_ssrc()`. */
    std::map<std::uint32_t, std::int64_t> skew_us_by_ssrc;

    /** SSRCs whose media clock rate has already been pinned on the tracker.
     *  Membership means "configured once" — the payload-type → clock-rate
     *  resolution in `handle_rtp()` runs only on the first packet of each
     *  SSRC, keeping the media hot path free of per-packet map work. */
    std::set<std::uint32_t> clock_rate_pinned_ssrcs;

    /** Payload type → media clock rate (Hz), taken from the *remote*
     *  SDP's `a=rtpmap:` lines.
     *
     *  The media clock rate is a property of the codec the *peer* sends
     *  with, so the peer's own description is the authoritative source
     *  (RFC 3550 §6.4.1: the Sender Report does not carry it).  A
     *  payload type may appear in several m-sections; the first
     *  declaration wins and a conflicting later one is logged and
     *  ignored — the RTP layer does not track which m-section an SSRC
     *  belongs to, so a per-m-line table could not be consulted
     *  anyway. */
    std::unordered_map<std::uint8_t, std::uint32_t> remote_clock_rate_hz;

    // ---- Inbound audio tracking (v1.2.0, audio pipeline WP-8) ---------
    //
    // The RTCP receiver report (RFC 3550 §6.4.2) needs the *sequence*
    // bookkeeping of the inbound audio stream: the extended highest
    // sequence number, the cumulative packet loss and the interarrival
    // jitter.  The receive pipeline does not track those — it counts
    // packets and plays out frames — so the engine keeps the numbers
    // here, next to the statistics it publishes (spec B-12 / B-13).
    //
    // The state follows RFC 3550 Appendix A.1: a *source* (SSRC) has
    // one sequence space, and a stream that restarts (a new SSRC, or a
    // large backward jump) resets it.
    struct AudioRxState {
        /** The SSRC of the inbound audio stream; 0 = nothing seen yet. */
        std::uint32_t ssrc = 0;

        /** The first sequence number of the current sequence space
         *  (RFC 3550's `base_seq`). */
        std::uint16_t base_seq = 0;

        /** The extended highest sequence number received
         *  (RFC 3550's `max_seq`): cycles in the high 16 bits. */
        std::uint32_t max_seq = 0;

        /** Number of packets received in the current sequence space. */
        std::uint64_t received = 0;

        /** The `expected` / `received` counters as of the last report
         *  (RFC 3550 A.3) — the fraction-lost computation is a
         *  *difference* between two intervals, so the previous values
         *  have to be remembered. */
        std::uint64_t expected_prior = 0;
        std::uint64_t received_prior = 0;

        /** Interarrival jitter, in RTP timestamp units
         *  (RFC 3550 §6.4.1). */
        std::uint32_t jitter = 0;

        /** The transit time (arrival − RTP timestamp) the last packet
         *  produced.  The jitter estimate is the EWMA of the
         *  *differences* of the transit times.
         *
         *  The value is a *signed* 32-bit number because the transit
         *  time is modular over 2^32 (RFC 3550 A.1): the difference
         *  between two transit times has to be taken modulo 2^32 as
         *  well, or a wrap of the RTP timestamp would look like a
         *  multi-second arrival gap. */
        std::int32_t  last_transit = 0;
        bool          have_transit = false;

        /** The middle 32 bits of the NTP timestamp of the last Sender
         *  Report received for this SSRC (the RR's LSR field), and
         *  the instant we received it (the DLSR field). */
        std::uint32_t last_sr_lsr = 0;
        std::int64_t  last_sr_recv_us = 0;
    } audio_rx;

    /** Our own SSRC as the sender of the RTCP reports (RFC 3550 §6.1).
     *
     *  The receiver report is sent by *us* as the receiver of the
     *  media, so it needs an SSRC of its own — distinct from the media
     *  SSRCs, which identify the streams rather than the reporter.
     *  0 until the first report is built. */
    std::uint32_t rtcp_sender_ssrc_ = 0;

    /** When the last receiver report went out (steady clock). */
    std::chrono::steady_clock::time_point rr_last_sent{};

    // ---- Negotiated payload-type routing (B-4) --------------------------
    //
    // The tables below are the *negotiated* routing: the payload types
    // the peer declared on the matching m-line of its description, and
    // which we therefore accept on the wire.  They replace the old rule
    // "anything that is not the video payload type is audio" (defect
    // F-8), which pushed telephone-event, RED and unsupported-codec
    // packets into the audio pipeline.
    //
    // Both tables are empty until a remote description has been
    // processed.  While they are empty the demux keeps its historical
    // behaviour, so a host that drives the engine without an SDP
    // exchange (the test seams, the loopback harness) is unaffected.
    std::map<std::uint8_t, detail::AudioPayloadRoute> audio_pt_routes;
    std::map<std::uint8_t, detail::AudioPayloadRoute> video_pt_routes;

    /** The payload type the negotiation selected for the inbound audio
     *  stream; 0 when nothing was selected. */
    std::uint8_t negotiated_audio_pt = 0;

    /** The Opus format parameters the peer sent for the selected
     *  payload type, parsed (B-4).  WP-7 feeds them into the receive
     *  pipeline's configuration. */
    detail::OpusFmtpParams negotiated_opus_fmtp;

    /** The format parameters the answer echoed back — the negotiated
     *  subset of `negotiated_opus_fmtp` (B-4). */
    std::string negotiated_opus_fmtp_text;

    /** The negotiation concluded that we cannot decode the peer's audio.
     *
     *  Set when every payload type the peer offered names an encoding
     *  that has no decoder plugin (A-9 / B-4).  The receive track is
     *  then disabled: inbound audio packets are counted and dropped
     *  instead of being handed to a decoder that does not exist. */
    bool audio_rx_disabled = false;

    /** Inbound audio packets dropped because the audio track is off.
     *
     *  The track is off when the host disabled it, when the negotiation
     *  found nothing decodable, or when the receive pipeline could not
     *  be created.  The packets are counted and never decoded. */
    std::uint64_t audio_rx_dropped_packets = 0;

    /** The `kAudioCodecUnsupported` error was already reported for this
     *  session.  The condition is reported exactly once (spec Part D). */
    bool audio_codec_unsupported_reported = false;

    /** Inbound audio packets dropped because their payload type is not
     *  in the negotiated routing table (`kAudioPayloadTypeUnknown`). */
    std::uint64_t audio_pt_unknown_count = 0;

    /** The extmap id the peer advertised for the ssrc-audio-level
     *  extension; 0 when the peer did not advertise it. */
    int remote_audio_level_ext_id = 0;

    /** The extmap id WE advertise for the ssrc-audio-level extension
     *  (B-14).  It is also the id our outbound packets will carry.
     *
     *  RFC 5285 §6 negotiates the id per direction.  When we answer a
     *  peer's offer we echo the id the peer advertised; when we are the
     *  offerer we keep the id we advertised and read the peer's echo
     *  back. */
    int local_audio_level_ext_id = 0;

    /** True once a remote description has been processed.
     *
     *  While this is false the demux keeps its historical behaviour
     *  ("not the video payload type ⇒ audio"), so a host that drives
     *  the engine without an SDP exchange — the test seams, the
     *  loopback harness — is unaffected by the routing table. */
    bool pt_routing_active = false;

    // ---- Outbound Sender Report state --------------------------------------
    // `sr_*` carry the values an SR body reports for the audio stream
    // (RFC 3550 §6.4.1 sender info).  Video is folded into the same SR via
    // report blocks only when a video sender is active; the audio stream is
    // the SR's own media source because it is always the first one out.
    std::chrono::steady_clock::time_point sr_last_sent{};
    std::uint32_t audio_ssrc          = 0;
    std::uint32_t audio_rtp_timestamp   = 0;
    std::uint32_t audio_packet_count    = 0;
    std::uint32_t audio_octet_count     = 0;
    std::uint32_t video_rtp_timestamp   = 0;
    std::uint32_t video_packet_count    = 0;
    std::uint32_t video_octet_count     = 0;

    // ---- Audio send state (A-6: per engine, never shared) -------------
    //
    // The SSRC, the RTP sequence number and the RTP timestamp used to
    // live in function-local `static std::atomic` variables inside
    // send_audio().  Two engines in one process therefore shared one
    // sequence space and one SSRC (defect F-12), and the SDP generator
    // had to hard-code the same value to stay in lockstep with the
    // packet path.  They are per-engine members now, and the SDP
    // generator reads `audio_ssrc` — the same member the packet path
    // stamps — so the two cannot drift apart again.
    std::uint16_t audio_seq = 0;
    std::uint32_t audio_ts  = 0;

    // ---- Frame accumulator (B-6) --------------------------------------
    //
    // send_audio() accepts any block size.  The processed PCM is
    // appended here and only whole frames are encoded, so the wire
    // format does not depend on how the host chunks its capture.
    // The buffer holds interleaved samples; `audio_accum_frames` counts
    // the per-channel samples already in it.
    std::vector<float> audio_accum;
    std::size_t audio_accum_frames = 0;

    // ---- Audio send counters (A-8) -----------------------------------
    //
    // An encode failure must never disappear: it is counted here and
    // reported through on_error (throttled to one report per second so
    // a persistently broken stream cannot flood the host's callback).
    std::uint64_t audio_frames_sent   = 0;
    std::uint64_t audio_bytes_sent    = 0;
    std::uint64_t audio_encode_errors = 0;
    std::uint64_t audio_samples_in    = 0;
    std::chrono::steady_clock::time_point audio_last_error_report{};

    // ---- Audio receive counters (WP-7) ---------------------------------
    //
    // The pipeline owns the detailed statistics (packets, bytes, lost,
    // jitter); these three count what the *engine* handed to the host,
    // which is what the engine-level statistics report (spec B-13).
    std::uint64_t audio_frames_received       = 0;
    std::uint64_t audio_samples_received       = 0;
    std::uint64_t audio_frames_concealed      = 0;
    std::uint64_t audio_frames_fec_recovered  = 0;

    // ---- WP-8: the inbound-audio bookkeeping for the RTCP receiver
    //      report (spec B-12). ----
    // <<WP8_MEMBERS>>

    // ---- WP-9: the cross-end capture timestamp of the inbound audio
    //      frames (spec B-17). ----
    // <<WP9_MEMBERS>>

    /** Wall-clock instant (µs since the Unix epoch) of the **last outbound
     *  audio packet**, sampled at the very moment its abs-send-time
     *  element was stamped.
     *
     *  The Sender Report pairs an NTP instant with an RTP timestamp, and
     *  for the pair to describe *one* point on the media timeline the two
     *  must refer to the same moment.  RFC 3550 §6.4.1 words the NTP
     *  field as "the wallclock time when the report was sent" while the
     *  RTP timestamp is that of the last packet sent — a pairing that is
     *  internally inconsistent by exactly the gap between the two events.
     *
     *  That gap is not a rounding detail: with a 1 s Sender-Report
     *  cadence it is up to a full second, and with continuous audio it
     *  is one frame interval.  A receiver that reconstructs the capture
     *  instant from the pair inherits the whole offset — every packet
     *  comes out systematically late, and the one-way delay reads a
     *  second too small.  The abs-send-time refinement does not rescue
     *  it either: the refinement adds the *elapsed* time since the
     *  anchor to the SR's NTP instant, so a late NTP instant stays late.
     *
     *  So the engine stamps the send instant on the packet itself and
     *  reuses it in the SR.  The SR's NTP field then denotes the exact
     *  instant of the packet whose RTP timestamp it carries — the same
     *  instant the abs-send-time field encodes, to within the 15.26 µs
     *  abs-send-time quantisation.  The peer's reconstruction is
     *  therefore free of the report-cadence bias, and the one-way delay
     *  measures the wire, not the report schedule.
     *
     *  Zero means "no packet sent yet"; the SR path already refuses to
     *  emit a report before the first packet. */
    std::int64_t audio_last_send_unix_us = 0;

    // Reusable buffer for the outbound SRTCP packet (avoid a per-tick
    // allocation; SR is 1 Hz by default so this is not hot, but tick() is
    // called at the transport cadence).
    std::vector<std::uint8_t> sr_send_buf;

    // ---- DataChannel DTLS-transport cookie (v1.x follow-up) ---------
    //
    // Placeholder for the physical SCTP-over-DTLS wiring that lands
    // in the **v1.x WebTransport / QUIC** re-design — see
    // `docs/plan/v0.11-plan.md` §2.2 (DC-1 / DC-2 / TPAL-5 deferred)
    // and §10 (forward direction). v0.11.0 falls back to the
    // in-process `tests/test_datachannel_engine` 5/5 PASS as the §7
    // substitute, made permanent.
    //
    // Typed as `nimrtc::dtls::IDtlsSession*` (non-owning) so the field
    // is self-describing — the v1.x wiring PR that swaps to
    // WebTransport will replace this with a tagged union over
    // {IDtlsSession*, IWebTransportSession*} rather than juggling a
    // raw `void*` cast.  Lifetime (v0.13.0): weak_ptr into the
    // media_stack's DTLS session; expired automatically after
    // close() tears down the stack.
    //
    // Initialised as an empty weak_ptr in init_modules_once() — at that point
    // `impl_->dtls` is a weak_ptr into `impl_->session.media_stack`. By the
    // time this method runs, `init_modules_once()` has built the session and
    // either set `impl_->dtls` to the stack's DTLS session (browser-interop
    // strategies) or left it expired (Rule 2 / raw-udp-arq media).
    // create_data_channel() then assigns impl_->dtls into this cookie
    // so the v1.x wiring code has a single, well-known place to look
    // for the underlying transport.  See also the TODO in
    // init_modules_once().
    //
    // v0.13.0 (this change): changed from raw pointer to weak_ptr, aligned
    // with Impl::dtls.  The cookie is informational — callers must .lock()
    // before use; they get nullptr after close() automatically.
    std::weak_ptr<nimrtc::dtls::IDtlsSession> datachannel_transport_cookie;

    // ---- WebTransport session ownership (v0.14.0-rc.1 follow-up) ----
    //
    // Created lazily by create_data_channel() when
    // `resolved_dc_factory_id_ == "web_transport"`, from
    // `config_.web_transport_name` + `web_transport_peer_host/port`.
    // Shared (not unique) so every channel created by this engine sits
    // on one QUIC connection. Channels hold a non-owning pointer into
    // it — callers must destroy channels before close() (same contract
    // as the non-owning ice_t_ handle). Torn down in close().
    std::shared_ptr<nimrtc::web_transport::IWebTransportSession> wt_session;

    // Last polled WebTransport session state (v0.15.0, product route).
    // Polled in tick(); transitions to kConnected/kFailed are reported
    // via on_data_state_("open"/"error") so engine integrators get the
    // same callback shape as the reliable-stream open path.
    // `std::uint8_t` (not the enum) so the header stays free of the
    // web_transport include; the .cpp maps it back to WebTransportState.
    std::uint8_t wt_last_state = 0;  // kConnecting

    // ---- Resolved transport strategy (v0.12.0 Slice 7.6 PR3) ---------
    //
    // Filled in by `init_transport_strategy()` (called from
    // `init_modules_once()` at the very top, before any module is
    // created).  The strategy name comes from `cfg.transport_strategy_name`
    // (or "browser-call" when empty); the factory ids come from
    // `CapabilitySelector::last_trace()`.  The ids are stored as
    // `std::string` (owned) because the Selector's trace returns
    // `std::string` values that are local to the call site — string_views
    // into the trace would dangle as soon as the local goes out of scope.
    std::string      resolved_strategy_name;       // owned
    std::string      resolved_media_factory_id;     // owned
    std::string      resolved_control_factory_id;   // owned (may be empty)

    uint32_t        last_open_rc = 0;
};

}  // namespace nimrtc::engine
