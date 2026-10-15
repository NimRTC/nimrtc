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

#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
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
#ifdef NIMRTC_HAS_H264
#include <nimrtc/h264/codec_plugin.hpp>
#endif

// Concrete module headers — required by NimRTCEngine::Impl which lives in
// this translation unit.  The public header (engine.hpp) does NOT include
// any of these; consumers only need the plugin interfaces.
#include <nimrtc/sdp/session_description.hpp>
#include <nimrtc/rtp/packet.hpp>
#include <nimrtc/jb/jitter_buffer.hpp>
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
// TPAL-4 cleanup: `DtlsSession::state_name()` is a static method on the
// concrete module class (used by the engine's state-transition logger
// below).  Including the concrete header here is allowed — Layout
// Invariant 4 constrains only `engine.hpp`, not the .cpp.  Keeping the
// include next to the seam header so reviewers see both in context.
#include <nimrtc/dtls/dtls.hpp>
#include <nimrtc/srtp/srtp.hpp>
#ifdef NIMRTC_HAS_OPUS
#include <nimrtc/opus/opus.hpp>
#endif

namespace nimrtc::engine {

namespace {

void trace(const char* fmt, ...) noexcept {
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

std::string make_session_id() {
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
    std::random_device rd;
    std::mt19937_64 mt(rd());
    return std::to_string(now) + std::to_string(mt() & 0xFFFF);
}

std::vector<std::uint8_t> base64_decode(std::string_view sv) {
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
        if (c < 0 || kIdx[static_cast<uint8_t>(c)] < 0) continue;
        val = (val << 6) | kIdx[static_cast<uint8_t>(c)];
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
std::vector<std::uint8_t> hex_colon_decode(std::string_view sv) {
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

} // anonymous namespace

// ---------------------------------------------------------------------------
// NimRTCEngine::Impl — all concrete-module state lives here.
//
// Layout Invariant 4 forbids NimRTCEngine from depending on concrete
// module headers in its public API.  engine.hpp forward-declares the
// concrete types it exposes in the public surface (e.g. dtls::DtlsState)
// and the accessors that need them are out-of-line; the rest of the
// concrete state (JitterBuffers, Audio3A fallback, SRTP, Opus, DTLS, SDP)
// lives in this Impl struct compiled together with engine.cpp.
// ---------------------------------------------------------------------------
struct NimRTCEngine::Impl {
    // SDP: Parser/Munger are concrete classes.
    std::unique_ptr<sdp::Parser>    sdp_parser = std::make_unique<sdp::Parser>();
    std::unique_ptr<sdp::Munger>    sdp_munger = std::make_unique<sdp::Munger>();

    // Jitter buffers — keyed by SSRC.
    std::map<std::uint32_t, std::unique_ptr<jb::JitterBuffer>> jitter_buffers;

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
#ifdef NIMRTC_HAS_OPUS
    std::unique_ptr<opus::Encoder>     opus_encoder;
    std::unique_ptr<opus::Decoder>     opus_decoder;
#endif

    bool            dtls_active_inbound = false;
    bool            srtp_installed      = false;
    int             srtp_stats_drop     = 0;
    nimrtc::dtls::DtlsState last_dtls_state     = nimrtc::dtls::DtlsState::Closed;

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

    // ---- DTLS role negotiation ---------------------------------------------
    // Resolved in process_remote_sdp() from the peer's a=setup attribute.
    // Used to emit a matching a=setup on the answer SDP.  Empty until
    // process_remote_sdp() runs; defaults to "active" if unset.
    std::string dtls_local_setup;

    // ---- send_audio scratch buffers (P0#4) --------------------------------
    // Reusable per-engine buffers for the audio capture→encode→RTP→SRTP
    // path.  Capacity grows monotonically across calls, avoiding the heap
    // allocations that a fresh `std::vector` per call would incur.
    //
    // `audio_codec_pkt` holds the raw codec (Opus) output for the in-flight
    // frame.  `audio_send_buf` is the SRTP-protected RTP packet that goes
    // out the wire (or into the scheduler queue).  Both are members rather
    // than `static thread_local` so the engine owns the lifetime and so a
    // future move to per-stream send queues stays trivial.
    std::vector<std::uint8_t> audio_codec_pkt;
    std::vector<std::uint8_t> audio_send_buf;

    // ---- send_video scratch buffer (P0#4) ---------------------------------
    // Reusable buffer for the encoded H.264 frame.  Default reserve size
    // matches the worst-case 64 KiB IDR; resize grows monotonically.
    std::vector<std::uint8_t> video_enc_buf;

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

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

NimRTCEngine::NimRTCEngine(EngineConfig config)
    : impl_(new Impl()), config_(std::move(config)) {}

NimRTCEngine::~NimRTCEngine() {
    if (state_ == State::kOpen) {
        close();
    }
    delete impl_;
    impl_ = nullptr;
}

// ---------------------------------------------------------------------------
// Transport strategy accessors (v0.12.0 Slice 7.6 PR3)
//
// Out-of-line because they dereference `Impl` (pImpl idiom) — Impl is
// forward-declared in the public header, so an inline definition there
// would not compile.  The body is intentionally trivial: read the
// strings `init_transport_strategy()` (called from `init_modules_once()`)
// wrote into `impl_`, and hand them back as string_views / a view struct.
// ---------------------------------------------------------------------------
std::string_view NimRTCEngine::transport_strategy_name() const noexcept {
    return impl_ ? std::string_view{impl_->resolved_strategy_name}
                 : std::string_view{};
}

NimRTCEngine::TransportSessionView
NimRTCEngine::transport_strategy_session() const noexcept {
    if (!impl_) return {std::string_view{}, std::string_view{}};
    return {std::string_view{impl_->resolved_media_factory_id},
            std::string_view{impl_->resolved_control_factory_id}};
}

void NimRTCEngine::init_bwe_scheduler() noexcept {
    // ---- BWE plugin -------------------------------------------------------
    // Created and opened here; injected into ice_t_ via set_bwe().
    // If bwe_name is empty, BWE is skipped (passthrough — "transport" profile).
    if (!config_.bwe_name.empty() && !bwe_) {
        // Failure policy: Warn (matches pre-Slice-1.5 behaviour — BWE is
        // optional, so a missing or broken plugin degrades to "no BWE"
        // with a diagnostic log line, not an on_error_ fire).
        (void)pal::make_plugin<&pal::resolve_bwe>(
            config_.bwe_name,
            bwe_,
            config_.bwe_config,
            {.err_open_failed = core::kEngineInternal,  // unused (Warn)
             .err_missing = core::kEngineInternal,      // unused (Warn)
             .open_fail_mode = pal::PluginFailureMode::Warn,
             .missing_mode = pal::PluginFailureMode::Warn,
             .on_error = on_error_});
    }

    // ---- Scheduler plugin ------------------------------------------------
    // Created and opened here; injected into ice_t_ via set_scheduler().
    // If scheduler_config.enabled is false, scheduler is skipped (direct ICE send).
    if (config_.scheduler_config.enabled && !scheduler_) {
        (void)pal::make_plugin<&pal::resolve_scheduler>(
            config_.scheduler_name,
            scheduler_,
            config_.scheduler_config,
            {.err_open_failed = core::kEngineInternal,
             .err_missing = core::kEngineInternal,
             .open_fail_mode = pal::PluginFailureMode::Warn,
             .missing_mode = pal::PluginFailureMode::Warn,
             .on_error = on_error_});
    }

    // ---- Inject into ICE transport ---------------------------------------
    // ice_t_ is set by the time init_bwe_scheduler() is called
    // (pre_open / open both create it first).
    if (ice_t_) {
        ice_t_->set_bwe(bwe_.get());
        ice_t_->set_scheduler(scheduler_.get());
    }
}

void NimRTCEngine::shutdown_bwe_scheduler() noexcept {
    auto close_plugin = [](auto& p) {
        if (p) p->close();
        p.reset();
    };
    close_plugin(bwe_);
    close_plugin(scheduler_);
    if (ice_t_) {
        ice_t_->set_bwe(nullptr);
        ice_t_->set_scheduler(nullptr);
    }
}

    uint32_t NimRTCEngine::pre_open() noexcept {
    if (state_ != State::kConstructed) return core::kEngineNotReady;
    core::log::Logger::instance().debug("pre_open: starting");

    // Defer ice_t_->open() until a later open() call so the caller can
    // call set_remote_ice() between pre_open() and open() (this is what
    // the loopback test relies on to fix the ICE role conflict).
    const uint32_t rc = init_modules_once();
    if (rc != 0) return rc;

    // State stays kConstructed — open() will call ice_t_->open()
    // and set state to kOpen when invoked.
    return 0;
}

// ---------------------------------------------------------------------------
// init_modules_once() — thin orchestrator (P0 refactor, see engine.hpp).
//
// Splits the pre-refactor ~460-line monolith into 5 phase helpers:
//   1. init_transport_strategy()  — validate + Selector
//   2. init_transport_session()   — TransportSession build + ICE pre-open config
//   3. init_bwe_scheduler()       — already-extracted (PAL Slice 1.5 era)
//   4. init_audio_modules()       — audio3a + codec (Silent fallback paths)
//   5. init_datachannel_registry()— DataChannel factory verification
//   6. init_dtls_handshake()      — SRTP context + DTLS open + ICE callbacks
//
// Phase ordering matches the pre-refactor inline order exactly; behaviour
// is unchanged.
// ---------------------------------------------------------------------------
uint32_t NimRTCEngine::init_modules_once() noexcept {
    core::register_all_default_plugins();
    // Note: `core::PluginRegistry::instance()` is consulted indirectly
    // through `make_transport_instance()` (which looks up the media and
    // control factories by id) and directly through
    // `init_bwe_scheduler()` and `init_video_plugins()` for BWE /
    // scheduler / video plugins. We don't capture a local `reg` here
    // because every consumer in the helpers below gets the registry
    // through its own helper.

    if (const uint32_t rc = init_transport_strategy();   rc != 0) return rc;
    if (const uint32_t rc = init_transport_session();    rc != 0) return rc;
    init_bwe_scheduler();
    init_audio_modules();
    init_datachannel_registry();
    if (const uint32_t rc = init_dtls_handshake();       rc != 0) return rc;

    return 0;
}

// ---------------------------------------------------------------------------
// Phase 1 — transport strategy validation + CapabilitySelector.
//
// Resolve the strategy name and apply the CapabilitySelector BEFORE any
// module is created.  This is the single place where the engine validates
// the strategy id and records the resolved (media_factory_id,
// control_factory_id) pair.
//
// Failure modes:
//   - Unknown strategy name (non-empty, not in the 5-id catalog)
//     → std::invalid_argument from `strategy_by_name()`.  We catch it
//     and convert to kEngineInvalidParam (0x1001) so callers see a
//     uniform non-zero return code (the engine API does not throw).
//   - Selector returning an empty media_stack (the CapabilitySelector
//     default ctor returns empty stacks when no resolver is injected) →
//     kEngineInternal.  This shouldn't happen with a well-formed
//     strategy but is caught defensively.
//
// On success, the resolved pair is stored in `impl_` and surfaced
// through the `transport_strategy_session()` accessor.
// ---------------------------------------------------------------------------
uint32_t NimRTCEngine::init_transport_strategy() noexcept {
    {
        // Resolve the strategy name.  `strategy_by_name("")` returns the
        // browser-call default, so empty `cfg.transport_strategy_name`
        // is handled silently.
        std::string_view resolved_name;
        try {
            // requirements_from_profile_input() throws on unknown names.
            // The base call below has no per-instance overrides — it
            // returns the strategy's canonical TransportRequirements.
            ProfileTransportInput in;
            in.strategy = config_.transport_strategy_name;
            (void)requirements_from_profile_input(in);
            resolved_name = config_.transport_strategy_name.empty()
                ? std::string_view{"browser-call"}
                : config_.transport_strategy_name;
        } catch (const std::invalid_argument& e) {
            impl_->last_open_rc = core::kEngineInvalidParam;
            const std::string msg = std::string{"transport_strategy_name: "} + e.what();
            core::log::Logger::instance().error(msg);
            if (on_error_) on_error_(core::kEngineInvalidParam, msg);
            return core::kEngineInvalidParam;
        }
        impl_->resolved_strategy_name = resolved_name;

        // Apply the Selector.  We construct a default CapabilitySelector
        // (no resolver hooks), which in "rule-only mode" returns empty
        // stacks but records the chosen factory ids in `last_trace()`.
        // Those ids are the §5.3-rule-derived factory names — exactly
        // what the engine's accessors need to expose.
        CapabilitySelector selector;
        TransportRequirements req;
        try {
            req = requirements_from_profile_input(
                ProfileTransportInput{resolved_name});
        } catch (const std::invalid_argument&) {
            // Unreachable — we just validated the name above.  Keep the
            // catch as a defensive belt-and-braces; if it ever fires
            // it indicates a logic bug, not a config error.
            impl_->last_open_rc = core::kEngineInternal;
            return core::kEngineInternal;
        }
        TransportSession session = selector.select(req);
        const auto trace = selector.last_trace();

        // The Selector's trace carries the §5.3-rule-derived factory
        // ids.  An empty trace.media_factory_id means the Selector
        // could not resolve a media stack (rule 3 / `webrtc-quic` is
        // not yet implemented; the `webrtc-quic-call` strategy lands
        // in v1.x per transport-strategies.md §6 #1).
        //
        // We copy the trace strings into impl_->resolved_*_factory_id
        // (owned `std::string` storage) because `trace` is a local that
        // goes out of scope at the end of this block; views into it
        // would dangle.
        if (trace.media_factory_id.empty()) {
            // Fall back to the legacy v0.10.x media-stack id
            // ("webrtc-classic") for v0.12.0 strategies.  This
            // preserves the engine's pre-PR3 media-stack path while
            // keeping the strategy recorded.  Slice 7.x 2nd-half work
            // (v0.13+) will replace this with the Selector's actual
            // choice.
            impl_->resolved_media_factory_id = "webrtc-classic";
        } else {
            impl_->resolved_media_factory_id = trace.media_factory_id;
        }
        impl_->resolved_control_factory_id = trace.control_factory_id;

        // (void)session — the rule-only Selector returns empty stacks.
        // We log the trace instead so diagnostics still see what the
        // Selector would have chosen in production.
        (void)session;
        core::log::Logger::instance().info(
            std::string{"transport: strategy '"} +
            impl_->resolved_strategy_name +
            "' resolved — media_factory_id='" +
            impl_->resolved_media_factory_id +
            "', control_factory_id='" +
            impl_->resolved_control_factory_id + "'");
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Phase 2 — TransportSession build + ICE pre-open configuration.
//
// Slice 7.6 PR3 replaces the pre-PR3 manual ICE + DTLS construction
// path with a TransportSession that owns the media_stack and
// control_stack.  The factory lookups are expressed inline here (rather
// than via a helper in the transport module) to keep the engine's
// link surface unchanged for existing consumers — nimrtc::transport is
// still linked, but the PUBLIC link is sufficient; no new API
// boundary is required.
//
// StackConfig defaults mirror what the pre-PR3 engine set on each
// plugin individually.  The §5.3 factory ids (webrtc-classic /
// raw-udp-arq) are stored in impl_->resolved_media_factory_id /
// control_factory_id.
//
// Rule 2 caveat (teleop-ultra-low-latency):
//   §5.3 Rule 2 maps "no browser interop + high-freq control" to
//   media=raw-udp-arq.  That stack has no DTLS.  Audio falls back to
//   null-encoder; ICE/connectivity work via raw-UDP ARQ.
// ---------------------------------------------------------------------------
uint32_t NimRTCEngine::init_transport_session() noexcept {
    {
        StackConfig scfg;
        scfg.ice_id  = config_.transport_name;
        scfg.dtls_id = config_.dtls_name.empty()
                           ? std::string{"wolfssl"}
                           : config_.dtls_name;
        // The sctp_id and rtp_id use the factory defaults; leave them
        // empty so the factory uses its own defaults.

        impl_->session.media_stack_id   = impl_->resolved_media_factory_id;
        impl_->session.control_stack_id = impl_->resolved_control_factory_id;

        auto& reg = core::PluginRegistry::instance();

        // Media stack — must succeed.
        const ITransportStackFactory* media_f =
            reg.get_transport_stack(impl_->resolved_media_factory_id);
        if (!media_f) {
            impl_->last_open_rc = core::kEngineInternal;
            const std::string msg =
                std::string{"engine: media stack factory '"} +
                impl_->resolved_media_factory_id +
                "' not registered (did "
                "nimrtc::transport::register_default_plugins() run "
                "before engine construction?).";
            core::log::Logger::instance().error(msg);
            if (on_error_) on_error_(core::kEngineInternal, msg);
            return core::kEngineInternal;
        }
        impl_->session.media_stack = media_f->create(scfg);
        if (!impl_->session.media_stack) {
            impl_->last_open_rc = core::kEngineInternal;
            const std::string msg =
                std::string{"engine: media stack factory '"} +
                impl_->resolved_media_factory_id +
                "' returned null ITransportStack.";
            core::log::Logger::instance().error(msg);
            if (on_error_) on_error_(core::kEngineInternal, msg);
            return core::kEngineInternal;
        }

        // Control stack — optional (null for strategies without
        // high-freq control).
        if (!impl_->resolved_control_factory_id.empty()) {
            const ITransportStackFactory* ctrl_f =
                reg.get_transport_stack(impl_->resolved_control_factory_id);
            if (ctrl_f) {
                impl_->session.control_stack = ctrl_f->create(scfg);
                if (!impl_->session.control_stack) {
                    core::log::Logger::instance().warn(
                        std::string{"engine: control stack factory '"} +
                        impl_->resolved_control_factory_id +
                        "' returned null — control plane inactive for this strategy.");
                }
            } else {
                core::log::Logger::instance().warn(
                    std::string{"engine: control stack factory '"} +
                    impl_->resolved_control_factory_id +
                    "' not registered — control plane inactive for this strategy.");
            }
        }

        // Extract handles into the owned session.
        //   - ice_t_: raw pointer (out of v0.13.0 scope; still safe because
        //     the engine never outlives the session's media_stack — see
        //     ~NimRTCEngine's delete impl_ ordering).
        //   - impl_->dtls: weak_ptr (v0.13.0; auto-expires after close()).
        ice_t_      = &impl_->session.media_stack->ice();
        impl_->dtls = impl_->session.media_stack->dtls_weak();   // weak_ptr — auto-expires after close()

        // Rule 2 warning: raw-udp-arq media has no DTLS.
        if (!impl_->dtls.lock()) {
            core::log::Logger::instance().warn(
                std::string{"engine: strategy '"} +
                impl_->resolved_strategy_name +
                "' uses media_factory_id='" +
                impl_->resolved_media_factory_id +
                "' with no DTLS (Rule 2 — raw-udp-arq media). "
                "Audio pipeline uses null-encoder; "
                "DTLS-dependent features (SRTP, DataChannel) inactive.");
        }

        core::log::Logger::instance().info(
            std::string{"engine: TransportSession built — "
                        "media='"} +
            impl_->resolved_media_factory_id +
            "', control='" +
            impl_->resolved_control_factory_id +
            "'");
    }

    // ---- Apply pre-open ICE configuration ---------------------------------
    // set_bind_address / set_stun_server / set_local_port_range are safe
    // on null-instance ICE returns from RawUdpArqStack (they are no-ops).
    ice_t_->set_bind_address(config_.local_bind_address);
    ice_t_->set_stun_server(config_.stun_server_host, config_.stun_server_port);
    ice_t_->set_local_port_range(config_.local_port_range_begin,
                                  config_.local_port_range_end);
    // TURN servers are added via NimRTCEngine::add_turn_server() *before*
    // open() — they're applied to ice_t_ at that time, so no loop here.

    return 0;
}

// ---------------------------------------------------------------------------
// Phase 4 — audio3a + codec plugin creation.
//
// Original order: set_callbacks() BEFORE open().  We preserve that
// ordering via make_plugin's post_create hook.  The audio3a plugin's
// on_error callback bridges into the engine's on_error_ via a
// captured-this lambda.
//
// Failure policy:
//   - audio3a — silent on missing (concrete NullAudio3A fallback below
//     covers the gap), Error on open fail (callers want to know the
//     configured plugin misbehaved even though we fall back).
//   - codec — Silent on missing / open fail / create returns null.  The
//     engine falls back to the statically-linked opus module
//     (NIMRTC_HAS_OPUS).  Callers do not need an error notification
//     because the fallback is "the same codec from a different path".
//     This matches the pre-Slice-1.5 behaviour.
//
// Returns void — failures surface through `on_error_` and the
// `unique_ptr` slot is reset to `nullptr` so the fallback path runs.
// ---------------------------------------------------------------------------
void NimRTCEngine::init_audio_modules() noexcept {
    // ---- Audio3A --------------------------------------------------------
    (void)pal::make_plugin<&pal::resolve_audio3a>(
        config_.audio3a_name,
        audio3a_plugin_,
        /*config=*/nullptr,                // IAudio3AFactory::create()
                                           // takes no config (see
                                           // nullptr_t branch in helper)
        {.err_open_failed = core::kAudio3APluginOpenFailed,
         .open_fail_mode = pal::PluginFailureMode::Error,
         .missing_mode = pal::PluginFailureMode::Silent,  // missing → silent fallback
         .on_error = on_error_},
        /*post_create=*/[this](plugins::IAudio3A& p) {
            p.set_callbacks(
                [](bool) {}, [](float) {},
                [this](std::uint32_t err, std::string_view msg) {
                    if (on_error_) on_error_(err, msg);
                });
        });
    if (!audio3a_plugin_) {
        impl_->audio3a_concrete = std::make_unique<audio3a::NullAudio3A>();
        audio3a::Config a3a;
        a3a.sample_rate_hz    = config_.pcm_sample_rate_hz;
        a3a.capture_channels = config_.pcm_channels;
        a3a.render_channels   = config_.pcm_channels;
        impl_->audio3a_concrete->init(a3a);
    }

    // ---- Codec ---------------------------------------------------------
    {
        plugins::CodecConfig codec_cfg{};
        codec_cfg.sample_rate_hz = config_.pcm_sample_rate_hz;
        codec_cfg.channels       = config_.pcm_channels;
        codec_cfg.bitrate_bps    = 64000;
        codec_cfg.complexity     = 10;
        codec_cfg.fec_enabled    = true;
        codec_cfg.dtx_enabled    = false;
        codec_cfg.vad_enabled    = false;
        codec_cfg.payload_type   = config_.audio_codec.payload_type;
        codec_cfg.name           = config_.codec_name;
        (void)pal::make_plugin<&pal::resolve_codec>(
            config_.codec_name,
            codec_plugin_,
            std::move(codec_cfg),
            {.open_fail_mode = pal::PluginFailureMode::Silent,
             .missing_mode = pal::PluginFailureMode::Silent,
             .on_error = on_error_});
        if (!codec_plugin_) {
#ifdef NIMRTC_HAS_OPUS
            opus::EncoderConfig ec;
            ec.sample_rate_hz = config_.pcm_sample_rate_hz;
            ec.channels       = config_.pcm_channels;
            impl_->opus_encoder = std::make_unique<opus::Encoder>(ec);
            opus::DecoderConfig dc;
            dc.sample_rate_hz = config_.pcm_sample_rate_hz;
            dc.channels       = config_.pcm_channels;
            impl_->opus_decoder = std::make_unique<opus::Decoder>(dc);
#endif
        }
    }
}

// ---------------------------------------------------------------------------
// Phase 5 — DataChannel factory verification.
//
// Verify the IDataChannelFactory registered under
// `cfg.datachannel_name` resolves via PluginRegistry.  The engine
// does NOT auto-create any channel — callers explicitly invoke
// `create_data_channel(label)` after open() succeeds (mirrors the JS
// `peer.createDataChannel()` API).
//
// When `datachannel_name.empty()` the engine skips DataChannel
// entirely: `create_offer()` won't emit `m=application`, the answer
// side silently drops the peer's `m=application` line, and
// `create_data_channel()` returns nullptr.
//
// The factory lookup is intentionally lazy — we don't construct any
// IDataChannel at init time.  The default `SctpDataChannelFactory`
// registers at static-init time via `register_default_plugins()`,
// so by the time `init_modules_once()` runs, the factory is
// already in the registry.
//
// NOTE: SCTP-over-DTLS wiring deferred to the **v1.x WebTransport /
// QUIC** re-design — see `docs/plan/v0.11-plan.md` §2.2 (DC-1 /
// DC-2 / TPAL-5 deferred) and §10 (forward direction).  v0.11.0
// falls back to the in-process `tests/test_datachannel_engine`
// 5/5 PASS as the §7 substitute, made permanent.  Chrome is moving
// DataChannel to WebTransport over QUIC; v1.x will redesign the
// data plane on top of that path (likely msquic, MIT-licensed,
// Windows-first, cross-platform CI).
//
// The full DataChannel lifecycle requires plumbing the DTLS
// transport's outbound datagram path into the SCTP socket:
//   1. Once DTLS reaches `Connected`, derive the SRTP / DTLS-SRTP
//      keying material and feed it into the SCTP socket via the
//      `ISctpSocket::set_dtls_keys()` hook.
//   2. Hook the DTLS transport's outbound (encrypted) side to
//      the SCTP socket's `send_datagram()` — that's where
//      SCTP-over-DTLS packets are emitted onto the wire.
//   3. Hook the DTLS transport's inbound (decrypted) datagrams
//      into `ISctpSocket::feed_inbound_datagram()` so usrsctp
//      can drain its receive queue.
//   This skeleton only reserves the (void*) transport pointer
// placeholder (`Impl::datachannel_transport_cookie`) — the v1.x
// wiring PR replaces it with the physical plumbing on top of
// WebTransport / QUIC.
//
// Returns void — a missing factory just logs a warning and
// `create_data_channel()` will return `nullptr` on the caller side.
// ---------------------------------------------------------------------------
void NimRTCEngine::init_datachannel_registry() noexcept {
    if (!config_.datachannel_name.empty()) {
        const plugins::IDataChannelFactory* dcf =
            pal::resolve_datachannel(config_.datachannel_name);
        if (dcf) {
            // Don't auto-create — channels are created on demand via
            // create_data_channel().  Just verify the registry slot
            // resolves.  We log the resolved id so misconfiguration
            // (typo'd `cfg.datachannel_name`) surfaces immediately
            // instead of at first send.
            core::log::Logger::instance().info(
                std::string("engine: DataChannel plugin '")
                    .append(dcf->id())
                    .append("' ready (factory resolved via "
                            "core::PluginRegistry::get_datachannel)"));
        } else {
            core::log::Logger::instance().warn(
                std::string("engine: DataChannel plugin '")
                    .append(config_.datachannel_name)
                    .append("' not registered (create_data_channel() will "
                            "return nullptr)"));
        }
    }
}

// ---------------------------------------------------------------------------
// Phase 6 — SRTP context creation + DTLS open + ICE callbacks.
//
// Slice 7.6 PR3 (v0.12.0): DTLS session ownership moved from the
// engine (direct `reg.get_dtls_session()` + `factory->create()`) to
// `WebRtcClassicStackFactory` inside `make_transport_instance()`.
// `impl_->dtls` is a weak_ptr into `impl_->session.media_stack`
// (see the field comment on `Impl::dtls`).
//
// v0.13.0: changed from raw pointer to weak_ptr so `.lock()` returns
// nullptr automatically after close(), eliminating the UAF class.
//
// The factory's `create()` already built a default DtlsConfig with
// role=Server / srtp_profile=Aes128CmSha1_80 — matching what the
// pre-PR3 engine set here.  The DTLS role is still a placeholder;
// `process_remote_sdp()` resolves the real role from the peer's
// `a=setup` attribute (RFC 5763 §5) and calls `dtls->set_role()`
// before the handshake.  See P0#1 (DTLS role negotiation).
//
// Rule 2 caveat (teleop-ultra-low-latency):
//   `impl_->dtls` may be expired (weak_ptr, no lock) when the media stack is
//   raw-udp-arq (no DTLS in the media plane).  In that case we
//   skip DTLS open and surface a warning earlier in this function.
//   Note: weak_ptr::lock() == nullptr for both "never set" and "already destroyed".
//
// DTLS-missing-but-required gate (regression for test_dtls_seam
//   `cfg_dtls_name_unknown_factory_yields_open_failure`):
//   When the resolved media stack REQUIRES DTLS (webrtc-classic
//   for all 4 browser-interop strategies) but the user-supplied
//   `dtls_name` doesn't resolve to a registered factory, the
//   stack's factory returns the stack with dtls_=null (defensive
//   — see webrtc_classic_stack.cpp create()'s "DTLS factory not
//   found" branch).  The engine must NOT silently proceed without
//   DTLS in that case — it must surface kDtlsOpenFailed (the same
//   error the pre-PR3 path produced when `reg.get_dtls_session()`
//   returned null).  Rule 2 (raw-udp-arq media) is the only case
//   where null DTLS is OK; the strategy id is checked here to
//   distinguish "DTLS-less by design" from "DTLS-less by
//   misconfiguration".
// ---------------------------------------------------------------------------
uint32_t NimRTCEngine::init_dtls_handshake() noexcept {
    // ---- SRTP ----------------------------------------------------------
    impl_->srtp = std::make_unique<srtp::SrtpContext>();
    core::log::Logger::instance().debug(
        "engine: srtp_ created, about to open dtls_");

    // ---- DTLS ----------------------------------------------------------
    if (auto* d = impl_->dtls.lock().get()) {
        auto open_rc = d->open();
        if (!open_rc.ok()) {
            impl_->last_open_rc = core::kDtlsOpenFailed;
            core::log::Logger::instance().error(
                std::string("engine: dtls_->open() failed: ") +
                open_rc.error().message());
            if (on_error_) on_error_(core::kDtlsOpenFailed,
                "DTLS open failed: " + open_rc.error().message());
            return core::kDtlsOpenFailed;
        }
        core::log::Logger::instance().debug("engine: dtls_->open() returned ok");
    } else if (impl_->resolved_media_factory_id == "raw-udp-arq") {
        // Rule 2: no DTLS in media path (raw-udp-arq media stack).
        // The strategy block above already warned; here we just log
        // at debug level so the trace shows the session was built
        // without DTLS.
        core::log::Logger::instance().debug(
            "engine: dtls_->open() skipped — "
            "Rule 2 strategy has no DTLS in media path");
    } else {
        // Media stack requires DTLS (webrtc-classic) but dtls_ is
        // null.  This means the user-supplied cfg.dtls_name (or the
        // default "wolfssl") didn't resolve in core::PluginRegistry.
        // The pre-PR3 contract was to fail with kEngineInternal in
        // that case (test_dtls_seam regresses this).  Preserve that
        // contract: a misconfigured DTLS backend is a hard error,
        // not a silent degradation.  The error code matches the
        // pre-Slice-8 "transport plugin not found" path that the
        // test pins.
        impl_->last_open_rc = core::kEngineInternal;
        const std::string msg =
            std::string{"engine: DTLS plugin '"} +
            config_.dtls_name +
            "' not registered (required by media stack '" +
            impl_->resolved_media_factory_id +
            "' — no DTLS in the resolved stack). Check "
            "nimrtc::dtls::register_default_plugins() ran before "
            "engine construction.";
        core::log::Logger::instance().error(msg);
        if (on_error_) on_error_(core::kEngineInternal, msg);
        return core::kEngineInternal;
    }

    // ---- ICE recv / error callbacks ------------------------------------
    ice_t_->set_callbacks(
        [this](plugins::BufferView bv) { on_transport_recv(bv); },
        [this](plugins::Status err, std::string_view msg) {
            if (on_error_) on_error_(static_cast<std::uint32_t>(err), msg);
        });

    return 0;
}

uint32_t NimRTCEngine::open() noexcept {
    if (state_ != State::kConstructed) return core::kEngineNotReady;

    // If pre_open() already created the modules, skip recreation and go straight
    // to opening the transport (which triggers gather_candidates with the
    // correct role set by set_remote_ice()).
    if (!ice_t_) {
        // Normal path: modules not yet created — run the full init via the
        // shared helper. pre_open() takes the same path.
        const uint32_t rc = init_modules_once();
        if (rc != 0) return rc;
    } else {
        core::log::Logger::instance().debug(
            "op7b transport pre_open→open");
    }

    // ---- Session start triggers ICE gather_candidates() (Slice 7.6 PR3) ----
    //
    // The TransportSession owns ICE (inside media_stack). `ice_t_` is a raw
    // non-owning pointer into the session; calling `ice_t_->open()` directly
    // reaches the real ICE transport and preserves the error-code path
    // (pre-PR3 behaviour). The session's own `start()` method is void; we
    // call `ice_t_->open()` here to keep the `!= kOk` error check.
    //
    // At this point, if `set_remote_ice()` was called before `open()`, the
    // ICE transport knows the peer's credentials and will be CONTROLLED.
    if (!ice_t_) {
        // Guard: if `make_transport_instance()` returned a null media_stack
        // (unknown factory), `init_modules_once()` already returned an error
        // and we would not reach here. This is a belt-and-suspenders check.
        impl_->last_open_rc = core::kEngineInternal;
        if (on_error_) on_error_(core::kEngineInternal,
            "open(): ice_t_ is null (make_transport_instance returned null media_stack)");
        return core::kEngineInternal;
    }
    core::log::Logger::instance().debug("op8 calling ice_t_->open()");
    const auto ice_open_rc = ice_t_->open();
    if (ice_open_rc != plugins::kOk) {
        char err[64];
        std::snprintf(err, sizeof(err), "transport open failed rc=%d",
                      static_cast<int>(ice_open_rc));
        impl_->last_open_rc = core::kEngineInternal;
        core::log::Logger::instance().error(std::string(err));
        if (on_error_) on_error_(core::kEngineInternal, err);
        ice_t_ = nullptr;  // raw pointer — null after failure
        return core::kEngineInternal;
    }
    core::log::Logger::instance().debug("op7 transport open ok");

    // ---- Video plugins (R3-Batch) ---------------------------------------
    // Resolves all four video_* plugins from core::PluginRegistry and
    // opens them. Failures are non-fatal at engine level (R3 scope: just
    // expose the accessors); warnings go through on_error_.
    init_video_plugins();

    state_ = State::kOpen;
    if (on_state_change_) on_state_change_("open");
    trace("NimRTCEngine opened (DTLS, SRTP, Opus initialised)");
    return 0;
}

void NimRTCEngine::set_video_sink(std::unique_ptr<plugins::IVideoSink> sink) noexcept {
    if (video_sink_) {
        video_sink_->close();
        video_sink_.reset();
    }
    video_sink_ = std::move(sink);
    if (video_sink_) {
        if (video_sink_->open() != plugins::kOk) {
            if (on_error_) on_error_(core::kVideoSinkOpenFailed, "video_sink open failed");
            video_sink_.reset();
        }
    }
}

void NimRTCEngine::set_video_source(
    std::unique_ptr<plugins::IVideoSource> source) noexcept {
    if (video_source_) {
        video_source_->close();
        video_source_.reset();
    }
    video_source_ = std::move(source);
    if (video_source_) {
        if (video_source_->open() != plugins::kOk) {
            if (on_error_) on_error_(core::kVideoSourceOpenFailed, "video_source open failed");
            video_source_.reset();
        }
    }
}

void NimRTCEngine::init_video_plugins() noexcept {
    // PHASE 1 — create all plugins (order: sink, source, receiver, sender,
    // codec). The 5-step factory dance for each plugin is consolidated
    // into pal::make_plugin() (PAL Slice 1.5); see make_plugin.hpp.
    // PHASE 2 — wire pipeline callbacks (down below the make_plugin
    // blocks).  Splitting the function into two phases ensures that by
    // the time any callback fires, every plugin slot is already filled.

    // ---- Video Sink --------------------------------------------------------
    if (!video_sink_) {
        // PAL Slice 1.5 — replaces the 5-step "factory → create → null-check
        // → open → report" dance with one helper call. The Error/Error
        // policy matches the original on_error_ path.
        (void)pal::make_plugin<&pal::resolve_video_sink>(
            config_.video_sink_name,
            video_sink_,
            plugins::VideoSinkConfig{},                  // default config
            {.err_open_failed = core::kVideoSinkOpenFailed,
             .err_missing = core::kVideoSinkPluginMissing,
             .open_fail_mode = pal::PluginFailureMode::Error,
             .missing_mode = pal::PluginFailureMode::Error,
             .on_error = on_error_});
    }

    // ---- Video Source ------------------------------------------------------
    if (!video_source_) {
        plugins::VideoSourceConfig cfg{};
        cfg.width  = 640;
        cfg.height = 480;
        cfg.fps    = 30;
        cfg.ssrc   = config_.video_sender_tuning.ssrc;
        (void)pal::make_plugin<&pal::resolve_video_source>(
            config_.video_source_name,
            video_source_,
            std::move(cfg),
            {.err_open_failed = core::kVideoSourceOpenFailed,
             .err_missing = core::kVideoSourcePluginMissing,
             .open_fail_mode = pal::PluginFailureMode::Error,
             .missing_mode = pal::PluginFailureMode::Error,
             .on_error = on_error_});
    }

    // ---- Video Receiver ----------------------------------------------------
    if (!video_receiver_) {
        plugins::VideoReceiverConfig cfg{};
        cfg.codec        = plugins::VideoCodecKind::kH264;
        cfg.ssrc         = config_.video_receiver_tuning.ssrc;
        cfg.payload_type = config_.video_receiver_tuning.payload_type;
        cfg.expect_fu_a  = config_.video_receiver_tuning.expect_fu_a;
        cfg.max_inflight_frames =
            config_.video_receiver_tuning.max_inflight_frames;
        cfg.max_jitter_buffer_ms =
            config_.video_receiver_tuning.max_jitter_buffer_ms;
        cfg.emit_nacks  = config_.video_receiver_tuning.emit_nacks;
        (void)pal::make_plugin<&pal::resolve_video_receiver>(
            config_.video_receiver_name,
            video_receiver_,
            std::move(cfg),
            {.err_open_failed = core::kVideoReceiverOpenFailed,
             .err_missing = core::kVideoReceiverPluginMissing,
             .open_fail_mode = pal::PluginFailureMode::Error,
             .missing_mode = pal::PluginFailureMode::Error,
             .on_error = on_error_});
    }

    // ---- Video Sender ------------------------------------------------------
    if (!video_sender_) {
        plugins::VideoSenderConfig cfg{};
        cfg.codec        = plugins::VideoCodecKind::kH264;
        cfg.ssrc         = config_.video_sender_tuning.ssrc;
        cfg.payload_type = config_.video_sender_tuning.payload_type;
        cfg.mtu          = config_.video_sender_tuning.mtu;
        cfg.initial_seq  = config_.video_sender_tuning.initial_seq;
        cfg.packetization_mode =
            plugins::VideoPacketizationMode::kNonInterleaved;
        (void)pal::make_plugin<&pal::resolve_video_sender>(
            config_.video_sender_name,
            video_sender_,
            std::move(cfg),
            {.err_open_failed = core::kVideoSenderOpenFailed,
             .err_missing = core::kVideoSenderPluginMissing,
             .open_fail_mode = pal::PluginFailureMode::Error,
             .missing_mode = pal::PluginFailureMode::Error,
             .on_error = on_error_});
    }

    // ---- Video Codec (H.264) -----------------------------------------------
    if (!config_.video_codec_name.empty() && !video_codec_) {
        plugins::VideoCodecConfig cfg{};
        cfg.width = 640;
        cfg.height = 480;
        cfg.fps = 30;
        cfg.bitrate_bps = 1'000'000;
        cfg.keyframe_interval = 60;
        cfg.pixel_format = plugins::VideoPixelFormat::kI420;
        cfg.codec = plugins::VideoCodecKind::kH264;
        cfg.payload_type = config_.video_receiver_tuning.payload_type;
        cfg.name = config_.video_codec_name;
        (void)pal::make_plugin<&pal::resolve_video_codec>(
            config_.video_codec_name,
            video_codec_,
            std::move(cfg),
            {.err_open_failed = core::kVideoCodecOpenFailed,
             .err_missing = core::kVideoCodecPluginMissing,
             .open_fail_mode = pal::PluginFailureMode::Error,
             .missing_mode = pal::PluginFailureMode::Error,
             .on_error = on_error_});
    }

    // ========================================================================
    // PHASE 2 — wire all pipeline callbacks (all plugins now exist)
    // ========================================================================

    // ---- Video send pipeline: source → codec → sender → RTP+SRTP → scheduler
    //
    // Tracking state: the codec's force_keyframe() causes the NEXT encoded
    // frame to be a keyframe. We track it in impl_->video_keyframe_pending_
    // (atomic bool) so it survives across send_video() calls.
    //
    // The video_source frame callback is NOT wired to the encode path
    // here (IVideoCodec / IVideoSender are NOT thread-safe; the source
    // cadence thread would race with any concurrent user thread).
    // Users drive video through send_video() — see engine.hpp.

    if (video_sender_) {
        // video_sender packet callback: build RTP header → SRTP protect → enqueue
        video_sender_->set_packet_callback(
            [this](plugins::BufferView payload,
                                          std::uint32_t rtp_ts,
                                          std::uint16_t seq,
                                          bool marker) {
                // ---- Build 12-byte RTP header ---------------------------------
                // byte 0: V=2 P=0 X=0 CC=0 → 0x80
                // byte 1: M=X(1) PT(7)
                std::uint8_t pt = config_.video_sender_tuning.payload_type;
                std::uint8_t hdr[12] = {
                    0x80,                             // version, no extensions
                    static_cast<std::uint8_t>((marker ? 0x80 : 0x00) | (pt & 0x7F)),
                    static_cast<std::uint8_t>((seq >> 8) & 0xFF),
                    static_cast<std::uint8_t>(seq & 0xFF),
                    static_cast<std::uint8_t>((rtp_ts >> 24) & 0xFF),
                    static_cast<std::uint8_t>((rtp_ts >> 16) & 0xFF),
                    static_cast<std::uint8_t>((rtp_ts >>  8) & 0xFF),
                    static_cast<std::uint8_t>(rtp_ts & 0xFF),
                    static_cast<std::uint8_t>((config_.video_sender_tuning.ssrc >> 24) & 0xFF),
                    static_cast<std::uint8_t>((config_.video_sender_tuning.ssrc >> 16) & 0xFF),
                    static_cast<std::uint8_t>((config_.video_sender_tuning.ssrc >>  8) & 0xFF),
                    static_cast<std::uint8_t>(config_.video_sender_tuning.ssrc & 0xFF),
                };

                // ---- Assemble full RTP packet ----------------------------------
                std::vector<std::uint8_t> rtp_pkt;
                rtp_pkt.reserve(12 + payload.size());
                rtp_pkt.insert(rtp_pkt.end(), hdr, hdr + 12);
                rtp_pkt.insert(rtp_pkt.end(),
                               payload.data(), payload.data() + payload.size());

                // ---- SRTP protect (if DTLS is connected) ----------------------
                if (impl_->srtp_installed && impl_->srtp) {
                    auto* sess = impl_->srtp->get_session(
                        config_.video_sender_tuning.ssrc, /*outgoing=*/true);
                    if (!sess) {
                        // SRTP not ready — drop this packet silently.
                        return;
                    }
                    core::ByteSpan span(rtp_pkt.data(), rtp_pkt.size());
                    auto enc = sess->protect_rtp(span,
                        config_.video_sender_tuning.ssrc, rtp_ts);
                    if (!enc) return;
                    rtp_pkt.assign(enc.value().data(),
                                   enc.value().data() + enc.value().size());
                }

                // ---- Enqueue to scheduler (move, P0#3) or send directly -------
                plugins::Priority prio = plugins::Priority::kVideo;
                if (marker && impl_->video_keyframe_pending_.load(std::memory_order_acquire)) {
                    prio = plugins::Priority::kVideoKeyframe;
                    impl_->video_keyframe_pending_.store(false, std::memory_order_release);
                }

                if (scheduler_) {
                    scheduler_->enqueue_owned(prio,
                                              std::move(rtp_pkt),
                                              plugins::Addr{});
                } else if (ice_t_) {
                    plugins::BufferView bv{rtp_pkt.data(), rtp_pkt.size()};
                    ice_t_->send(bv, {});
                }
            });
    }

    // ---- Video receive pipeline: receiver → codec → sink
    //
    // On each assembled frame from the receiver:
    //   1. Decode via video_codec_
    //   2. Render via video_sink_
    //   3. Fire on_video_frame_ (NAL delivery, for existing users)
    // -------------------------------------------------------------------------
    if (video_receiver_) {
        video_receiver_->set_frame_callback(
            [this](const plugins::EncodedVideoFrame& enc,
                   plugins::TimestampUs /*now_us*/) {
                // Fire the NAL-level callback (existing behaviour).
                if (on_video_frame_) {
                    on_video_frame_(enc.payload.data(),
                                   enc.payload.size(),
                                   enc.is_keyframe);
                }

                // Decode + render if the codec and sink are available.
                if (!video_codec_ || !video_sink_) return;

                const std::uint32_t w = enc.payload.size() > 0 ? 640 : 0;
                const std::uint32_t h = 480;

                // Lazily allocate / resize decode buffers to the frame size.
                // Stride is aligned to 16 bytes.
                auto align16 = [](std::uint32_t v) -> std::uint32_t {
                    return (v + 15u) & ~15u;
                };
                const std::uint32_t stride_y = align16(w);
                const std::uint32_t stride_uv = align16(w / 2);
                const std::size_t y_size = static_cast<std::size_t>(stride_y) * h;
                const std::size_t uv_size = static_cast<std::size_t>(stride_uv) * (h / 2);
                const std::size_t total = y_size + uv_size + uv_size;

                // Use a ping-pong buffer: alternate between two allocations.
                std::size_t buf_idx = impl_->video_decode_buf_idx_.fetch_add(1)
                                      % 2;
                auto& buf = impl_->video_decode_bufs[buf_idx];
                if (buf.size() < total) buf.resize(total);

                // Set plane pointers into the flat buffer.
                std::uint8_t* planes[3] = {
                    buf.data(),
                    buf.data() + y_size,
                    buf.data() + y_size + uv_size,
                };

                // Decode.
                plugins::VideoFrame raw_out{};
                raw_out.info.capture_ts_us = enc.info.capture_ts_us;
                raw_out.info.frame_seq     = enc.info.frame_seq;
                raw_out.info.rtp_timestamp = enc.info.rtp_timestamp;

                if (video_codec_->decode(enc, raw_out, planes) != plugins::kOk) {
                    return;
                }

                // Render to sink.
                plugins::VideoSourceFrame sink_frame{};
                sink_frame.width   = w;
                sink_frame.height  = h;
                sink_frame.format  = plugins::VideoPixelFormat::kI420;
                sink_frame.stride_y = static_cast<std::int32_t>(stride_y);
                sink_frame.stride_u = static_cast<std::int32_t>(stride_uv);
                sink_frame.stride_v = static_cast<std::int32_t>(stride_uv);
                sink_frame.plane_y  = planes[0];
                sink_frame.plane_u  = planes[1];
                sink_frame.plane_v  = planes[2];
                sink_frame.buffer_size = total;
                sink_frame.capture_ts_us = enc.info.capture_ts_us;
                sink_frame.frame_seq     = enc.info.frame_seq;
                sink_frame.rtp_timestamp = enc.info.rtp_timestamp;

                video_sink_->render(sink_frame);
            });
    }
}

void NimRTCEngine::shutdown_video_plugins() noexcept {
    auto close_all = [](auto& p) {
        if (p) p->close();
        p.reset();
    };
    stop_video();   // stop video source if running
    close_all(video_source_);
    close_all(video_sink_);
    close_all(video_receiver_);
    close_all(video_sender_);
    close_all(video_codec_);
}

void NimRTCEngine::close() noexcept {
    if (state_ != State::kOpen) return;
    if (ice_t_) ice_t_->close();
    if (audio3a_plugin_) audio3a_plugin_->close();
    if (codec_plugin_) codec_plugin_->close();
    shutdown_video_plugins();
    shutdown_bwe_scheduler();
    // ---- Tear down the TransportSession (Slice 7.6 PR3) ----------------
    //
    // `impl_->session` owns the media_stack and (optionally)
    // control_stack. Their `close()` methods drive ICE / DTLS / RTP /
    // SCTP / raw-UDP teardown in the right order. The session's own
    // close() also nulls `session.media_stack` / `session.control_stack`,
    // so after this call `ice_t_` becomes dangling and `impl_->dtls`
    // (weak_ptr into those stacks) auto-expires — but no code dereferences
    // them after close() returns (P0 v0.12.0 is_open() guards prevent API
    // surface use; v0.13.0 weak_ptr eliminates the UAF class entirely).
    //
    // `~NimRTCEngine` later calls `delete impl_` which destroys the
    // session and any remaining owned resources. Order is critical:
    // the engine must call this `close()` BEFORE the destructor runs
    // (which the destructor does itself when `state_ == State::kOpen`).
    impl_->session.close();
    ice_t_ = nullptr;
    // weak_ptr: no explicit reset needed — auto-expires when stack dies
    impl_->jitter_buffers.clear();
    audio3a_plugin_.reset();
    impl_->audio3a_concrete.reset();
    codec_plugin_.reset();
#ifdef NIMRTC_HAS_OPUS
    impl_->opus_encoder.reset();
    impl_->opus_decoder.reset();
#endif
    impl_->srtp.reset();
    // weak_ptr reset (no implicit nullptr conversion in C++20)
    impl_->datachannel_transport_cookie.reset();
    state_ = State::kClosed;
    if (on_state_change_) on_state_change_("closed");
}

// ---------------------------------------------------------------------------
// create_data_channel — creates a new IDataChannel via the factory resolved
// from `core::PluginRegistry::instance().get_datachannel(datachannel_name)`.
//
// P2/TPAL-5 wiring contract:
//   - Returns nullptr when datachannel_name is empty or the factory is
//     missing (so callers can detect a misconfigured engine without
//     needing a separate "is_available()" probe).
//   - The returned IDataChannel is open()ed with a default config
//     (label = @p label, reliability = kReliableOrdered, priority = 128,
//     ordered = true) — matches WebRTC's default ordered/reliable
//     channel (RFC 8831 §6.2).
//   - on_data_message_ / on_data_state_ are installed BEFORE open() so the
//     callback pipeline is hot the moment the channel transitions to
//     "open".
//   - DTLS-transport wiring is deferred to the **v1.x WebTransport /
//     QUIC** re-design — see `docs/plan/v0.11-plan.md` §2.2 (DC-1 /
//     DC-2 / TPAL-5 deferred) and §10 (forward direction).  For now
//     `impl_->dtls` is a weak_ptr into the session's media_stack;
//     `datachannel_transport_cookie` also holds a weak_ptr (v0.13.0).
//     See the comment block in init_modules_once() for the full scope.
//     v0.11.0 satisfies the data-plane DoD gate via the §7 fallback
//     (in-process `test_datachannel_engine` 5/5 PASS), made permanent.
// ---------------------------------------------------------------------------
std::unique_ptr<plugins::IDataChannel>
NimRTCEngine::create_data_channel(std::string_view label) noexcept {
    // Gate: DataChannel disabled by config.
    if (config_.datachannel_name.empty()) {
        if (on_error_) on_error_(core::kEngineInternal,
            "create_data_channel(): cfg.datachannel_name is empty");
        return nullptr;
    }
    // Gate: engine must be open()ed (so impl_->dtls is built and the
    // registry slot has been probed by init_modules_once()).
    if (state_ != State::kOpen) {
        if (on_error_) on_error_(core::kEngineNotReady,
            "create_data_channel(): engine not open");
        return nullptr;
    }

    const plugins::IDataChannelFactory* f =
        pal::resolve_datachannel(config_.datachannel_name);
    if (!f) {
        if (on_error_) on_error_(core::kEngineInternal,
            std::string("create_data_channel(): factory '")
                .append(config_.datachannel_name)
                .append("' not registered"));
        return nullptr;
    }

    std::unique_ptr<plugins::IDataChannel> ch{f->create()};
    if (!ch) {
        if (on_error_) on_error_(core::kEngineInternal,
            "create_data_channel(): factory->create() returned nullptr");
        return nullptr;
    }

    // Install callbacks BEFORE open() — the IDataChannel contract
    // requires set_on_message / set_on_state to be called before open()
    // (per `plugins/datachannel.hpp` §"Thread safety").
    if (on_data_message_) ch->set_on_message(on_data_message_);
    if (on_data_state_)   ch->set_on_state(on_data_state_);

    // Default config: matches WebRTC's ordered+reliable channel
    // (the common case for control / telemetry).  Per-channel QoS
    // can be added later via a new overload that takes an explicit
    // DataChannelConfig — out of scope for this P2 skeleton.
    plugins::DataChannelConfig cfg;
    cfg.label       = std::string{label};
    cfg.reliability = plugins::DataChannelReliability::kReliableOrdered;
    cfg.ordered     = true;
    cfg.priority    = 128;
    ch->open(cfg);

    // NOTE: SCTP-over-DTLS wiring deferred to the **v1.x WebTransport
    // / QUIC** re-design — see `docs/plan/v0.11-plan.md` §2.2 (DC-1 /
    // DC-2 / TPAL-5 deferred) and §10 (forward direction).  Stash the
    // DTLS transport handle as a placeholder so the v1.x wiring PR
    // has a single, well-known place to look for the underlying
    // transport.  v0.13.0: the cookie is typed as
    // `std::weak_ptr<IDtlsSession>` (was `IDtlsSession*` raw pointer
    // pre-v0.13.0; now auto-expires after close()).  The v1.x
    // redesign will replace this field with a tagged union over
    // {IDtlsSession*, IWebTransportSession*}.
    //
    // For now, the cookie is informational — the channel's own open()
    // succeeds, but its send() / on_message callbacks will be no-ops
    // until the v1.x wiring PR lands.  v0.11.0 satisfies the
    // data-plane DoD gate via the §7 fallback (in-process
    // `test_datachannel_engine` 5/5 PASS), made permanent.
    impl_->datachannel_transport_cookie = impl_->dtls;

    return ch;
}

std::string NimRTCEngine::create_offer() noexcept {
    if (ice_t_) ice_t_->wait_for_gathering(1500);

    sdp::SessionDescription sdp;
    sdp.version = 0;
    sdp.origin_username         = "-";
    sdp.origin_session_id      = make_session_id();
    sdp.origin_session_version = "1";
    sdp.origin_address          = "0.0.0.0";
    sdp.session_name            = "-";
    sdp.bundle_mids = {"0"};
    sdp.msid_semantic_token = "WMS";

    sdp::MediaDescription audio;
    audio.type            = sdp::MediaType::Audio;
    audio.port            = 9;
    audio.protocol        = "UDP/TLS/RTP/SAVPF";
    // Build the format string key from the configurable payload type.
    const std::string pt = std::to_string(config_.audio_codec.payload_type);
    audio.formats         = {pt};
    audio.direction       = sdp::Direction::SendRecv;
    audio.mid             = "0";
    audio.rtcp_mux_value  = "rtcp-mux";
    audio.ice_ufrag       = ice_t_ ? ice_t_->local_ufrag() : "";
    audio.ice_pwd         = ice_t_ ? ice_t_->local_password() : "";
    audio.ice_options     = "trickle";
    audio.dtls_setup      = "actpass";

    if (auto d = impl_->dtls.lock()) {
        audio.dtls_fingerprint_algo  = "sha-256";
        audio.dtls_fingerprint_value = d->local_fingerprint().hex_colon;
    }

    sdp::MediaDescription::RtpMap codec_map;
    codec_map.encoding   = config_.audio_codec.encoding;
    codec_map.clock_rate = config_.audio_codec.clock_rate;
    codec_map.channels   = config_.audio_codec.channels;
    audio.rtpmap[pt]     = codec_map;
    if (!config_.audio_codec.fmtp.empty())
        audio.fmtp[pt] = config_.audio_codec.fmtp;

    // Helper: append ICE candidates stripped of any prefix the libjuice
    // collector added (the munger re-emits "a=candidate:" itself).
    auto push_candidates_into = [&](sdp::MediaDescription& m) {
        if (!ice_t_) return;
        auto gathered = ice_t_->gathered_local_candidates();
        constexpr std::string_view kFullPrefix = "a=candidate:";
        constexpr std::string_view kHalfPrefix = "candidate:";
        for (auto& c : gathered) {
            if (c.size() >= kFullPrefix.size() &&
                c.substr(0, kFullPrefix.size()) == kFullPrefix) {
                m.candidates.push_back(std::string(c.substr(kFullPrefix.size())));
            } else if (c.size() >= kHalfPrefix.size() &&
                       c.substr(0, kHalfPrefix.size()) == kHalfPrefix) {
                m.candidates.push_back(std::string(c.substr(kHalfPrefix.size())));
            } else {
                m.candidates.push_back(c);
            }
        }
    };
    push_candidates_into(audio);

    sdp.media.push_back(std::move(audio));

    // ---- Video m-line (RFC 8829 §5, H.264 via RFC 6184) -----------------
    // Emit a video m-line only when at least one video plugin was resolved
    // (video_receiver_ / video_sender_ / video_codec_).  The default H.264
    // codec plugin is decoder-only — for encode we'd need a real codec
    // plugin — but the SDP/SDP-answer plumbing is correct regardless so
    // that browsers see the negotiation succeed even on a one-way demo.
    if (video_receiver_ || video_sender_) {
        sdp::MediaDescription video;
        video.type     = sdp::MediaType::Video;
        video.port     = 9;
        video.protocol = "UDP/TLS/RTP/SAVPF";
        const std::string vpt =
            std::to_string(config_.video_receiver_tuning.payload_type);
        video.formats         = {vpt};
        video.direction       = sdp::Direction::SendRecv;
        video.mid             = "1";           // second BUNDLE mid
        video.rtcp_mux_value  = "rtcp-mux";
        video.ice_ufrag       = ice_t_ ? ice_t_->local_ufrag() : "";
        video.ice_pwd         = ice_t_ ? ice_t_->local_password() : "";
        video.ice_options     = "trickle";
        video.dtls_setup      = "actpass";

        if (auto d = impl_->dtls.lock()) {
            video.dtls_fingerprint_algo  = "sha-256";
            video.dtls_fingerprint_value = d->local_fingerprint().hex_colon;
        }

        // a=rtpmap:<vpt> H264/90000  (RFC 6184 §5.6)
        sdp::MediaDescription::RtpMap video_map;
        video_map.encoding   = "H264";
        video_map.clock_rate = 90000;
        video_map.channels   = 1;             // H.264 clock is 90 kHz, no audio channels
        video.rtpmap[vpt]    = video_map;

        // a=fmtp:<vpt> profile-level-id=42E01F;packetization-mode=1;
        //                  sprop-parameter-sets=Z0LAHtkA,aM4G4g==
        // Baseline profile, level 3.1 — matches the synthetic SPS/PPS our
        // H.264 stub encoder emits (see h264::StubEncoder::encode).  When
        // a real codec plugin replaces the stub, the codec should override
        // these via a getter or config (out of scope for this round).
        if (video_codec_) {
            video.fmtp[vpt] =
                "profile-level-id=42E01F;"
                "packetization-mode=1;"
                "level-asymmetry-allowed=1;"
                "sprop-parameter-sets=Z0LAHtkA,aM4G4g==";
        } else {
            video.fmtp[vpt] =
                "profile-level-id=42E01F;packetization-mode=1";
        }

        // a=rtcp-fb:<vpt> nack pli  (RFC 4585 §6.2.1 + WebRTC §5.2)
        // NACK + Picture Loss Indication are the two feedbacks Chrome's
        // SFU expects on a recvonly/sendrecv video track.  We add both
        // unconditionally so the answer side mirrors them verbatim.
        video.rtcp_fb.push_back(vpt + " nack");
        video.rtcp_fb.push_back(vpt + " nack pli");
        video.rtcp_fb.push_back(vpt + " goog-remb");

        // a=ssrc:<video_ssrc> cname:nimrtc-video  (RFC 5576 §4.1)
        // WebRTC browsers count inbound-rtp only when the answer SDP
        // carries an a=ssrc line that matches the wire SSRC.  Emit it
        // here on the offer side as well so we have it for renegotiation.
        char ssrc_line[96];
        std::snprintf(ssrc_line, sizeof(ssrc_line),
                      "%u cname:nimrtc-video",
                      static_cast<unsigned>(config_.video_sender_tuning.ssrc));
        video.extra_attrs.emplace_back("ssrc", ssrc_line);

        push_candidates_into(video);
        sdp.media.push_back(std::move(video));
        // Make sure both mids are in the BUNDLE group.
        sdp.bundle_mids = {"0", "1"};
    }

    // ---- DataChannel m=application line (RFC 8831 / RFC 8260) ----------
    //
    // P2 simplification (RFC 8831 §5 "SCTP over DTLS"):
    //   We emit the canonical `m=application ... UDP/DTLS/SCTP <port>`
    //   line + an `a=sctp-port:<port>` attribute when
    //   `cfg.datachannel_name` is non-empty.  This is the bare minimum
    //   Chrome needs to populate its peer connection's SCTP transport
    //   and run DCEP (Data Channel Establishment Protocol, RFC 8832).
    //
    //   We deliberately do NOT emit `a=dcep` attributes or full DCEP
    //   handshake support — DCEP handshake is a **v1.x WebTransport /
    //   QUIC** re-design (see `docs/plan/v0.11-plan.md` §2.2 / §10).
    //   v0.11.0 emits only the `m=application` line + `a=sctp-port:<port>`,
    //   which is the minimum Chrome needs to populate its peer
    //   connection's SCTP transport.
    //
    //   Chrome tolerates this minimal form: as long as both sides
    //   advertise `m=application` with the same SCTP port and the
    //   DTLS fingerprint matches, the SCTP association is negotiated
    //   in-band once usrsctp starts emitting INIT chunks.
    if (!config_.datachannel_name.empty()) {
        sdp::MediaDescription app;
        app.type     = sdp::MediaType::Application;
        app.port     = 9;
        // Per RFC 8260 §6 / RFC 8831 §5: `UDP/DTLS/SCTP` is the
        // canonical protocol token for SCTP-over-DTLS.  The legacy
        // `DTLS/SCTP` token from RFC 8261 is also seen in the wild;
        // emitting both via the protocol string is fine — the parser
        // round-trips whichever the peer picked.
        app.protocol = "UDP/DTLS/SCTP";
        // RFC 8831 §6.1: the format list is empty for SCTP
        // (the payload type is implicit).  We keep it as an empty
        // list rather than a placeholder — the munger emits
        // `m=application 9 UDP/DTLS/SCTP` with no trailing formats.
        app.formats.clear();
        app.direction       = sdp::Direction::SendRecv;
        // Third BUNDLE mid — keep mids 0=audio, 1=video, 2=application
        // to match the wire order.
        app.mid             = "2";
        app.rtcp_mux_value  = "";    // rtcp-mux is RTP-specific; not valid for SCTP.
        app.ice_ufrag       = ice_t_ ? ice_t_->local_ufrag() : "";
        app.ice_pwd         = ice_t_ ? ice_t_->local_password() : "";
        app.ice_options     = "trickle";
        app.dtls_setup      = "actpass";

        if (auto d = impl_->dtls.lock()) {
            app.dtls_fingerprint_algo  = "sha-256";
            app.dtls_fingerprint_value = d->local_fingerprint().hex_colon;
        }

        // a=sctp-port:<port>  (RFC 8831 §6.1 — mandatory for SCTP).
        // Using extra_attrs keeps the munger's a= emission order stable
        // (extra_attrs come after rtcp-fb / extmap).
        app.extra_attrs.emplace_back(
            "sctp-port", std::to_string(config_.sctp_port));

        // a=max-message-size:<bytes>  (RFC 8831 §6.2 — optional, but
        // Chrome emits it; matching the peer's value avoids
        // `Bundled SCTP negotiation` warnings in chrome://webrtc-internals).
        // 6556 matches Chrome's default since M85.
        app.extra_attrs.emplace_back("max-message-size", "6556");

        push_candidates_into(app);
        sdp.media.push_back(std::move(app));
        // Update BUNDLE mids — the application mid always appends.
        // We rebuild from sdp.media to keep the order in sync (audio=0,
        // video=1, application=2).  When video is disabled, the audio mid
        // is still 0 and the application mid slides to 1 to keep the
        // BUNDLE group contiguous.
        sdp.bundle_mids.clear();
        for (const auto& m : sdp.media) {
            if (!m.mid.empty()) sdp.bundle_mids.push_back(m.mid);
        }
    }

    auto sdp_str = impl_->sdp_munger->to_sdp(sdp);
    if (!sdp_str) {
        if (on_error_) on_error_(core::kEngineSdpCorrupt, "SDP munger failed");
        return {};
    }
    impl_->local_sdp = std::move(sdp);
    return sdp_str.value();
}

std::optional<std::string>
NimRTCEngine::process_remote_sdp(std::string_view remote_sdp) noexcept {
    // P0 (v0.12.0): guard against post-close use.  Same rationale as
    // on_transport_recv(): after close() the underlying dtls session is
    // gone but a delayed signaling message may still call this entry
    // point and deref impl_->dtls->set_role() / set_peer_fingerprint()
    // below.  Mirrors the DataChannel guard pattern at line 1341.
    if (state_ != State::kOpen) return std::nullopt;
    auto parsed = impl_->sdp_parser->parse(remote_sdp);
    if (!parsed) {
        if (on_error_) on_error_(core::kEngineSdpCorrupt, "SDP parse failed");
        return std::nullopt;
    }

    const auto& remote = parsed.value();
    core::log::Logger::instance().info(
        std::string("process_remote_sdp: parsed ok; media.size=") + std::to_string(remote.media.size())
        + " extra_attrs.size=" + std::to_string(remote.extra_attrs.size()));

    // Apply ICE credentials + candidates through the plugin seam.
    //
    // RFC 8829 / WebRTC BUNDLE: when the SDP uses BUNDLE, ice-ufrag/ice-pwd
    // often live at the *session* level (since there is effectively one
    // transport), not at the media level.  Try session-level first, then
    // fall back to media-level — whichever has credentials wins.
    std::string ice_block;

    // 1. Session-level fallback (Chrome BUNDLE answer).  The concrete
    //    sdp::SessionDescription stores session-level a= attributes in
    //    extra_attrs (when cur_media was nullopt at parse time).
    std::string sess_ufrag, sess_pwd;
    for (const auto& [k, v] : remote.extra_attrs) {
        if      (k == "ice-ufrag" && !v.empty()) sess_ufrag = v;
        else if (k == "ice-pwd"   && !v.empty()) sess_pwd   = v;
    }
    auto flush_ufrag_pwd = [&](const std::string& ufrag,
                               const std::string& pwd) {
        if (ufrag.empty() || pwd.empty()) return false;
        ice_block += "a=ice-ufrag:" + ufrag + "\n";
        ice_block += "a=ice-pwd:"   + pwd   + "\n";
        return true;
    };

    bool wrote_creds = false;
    if (!sess_ufrag.empty() || !sess_pwd.empty()) {
        wrote_creds = flush_ufrag_pwd(sess_ufrag, sess_pwd);
    }
    core::log::Logger::instance().info(
        std::string("process_remote_sdp: sess_ufrag='") + sess_ufrag
        + "' sess_pwd.len=" + std::to_string(sess_pwd.size())
        + " wrote_creds=" + (wrote_creds ? "1" : "0"));

    // 2. Media-level candidates (always appended).  Use the *first* media
    //    block that has credentials; otherwise use session credentials.
    //
    // Note: `sdp::SessionDescription::media[i]` is the concrete MediaDescription
    // (NOT the plugins::SdpMedia wrapper), which exposes dedicated ice_ufrag,
    // ice_pwd and candidates fields.  This is the engine's internal parser
    // (impl_->sdp_parser), not the ISDP plugin.
    for (const auto& m : remote.media) {
        core::log::Logger::instance().info(
            std::string("process_remote_sdp: media ice_ufrag='") + m.ice_ufrag
            + "' ice_pwd.len=" + std::to_string(m.ice_pwd.size())
            + " candidates=" + std::to_string(m.candidates.size())
            + " dtls_setup='" + m.dtls_setup + "'");
        if (!wrote_creds) {
            wrote_creds = flush_ufrag_pwd(m.ice_ufrag, m.ice_pwd);
        }
        for (const auto& c : m.candidates) {
            ice_block += "a=candidate:" + c + "\n";
        }
        // Don't break: subsequent media blocks may carry more candidates.
    }

    core::log::Logger::instance().info(
        std::string("process_remote_sdp: ice_block.size=") + std::to_string(ice_block.size())
        + " ice_t_=" + (ice_t_ ? "yes" : "no")
        + " wrote_creds=" + (wrote_creds ? "1" : "0"));

    if (wrote_creds && !ice_block.empty() && ice_t_) {
        ice_t_->set_remote_description(ice_block);
    }

    // Configure DTLS role + peer fingerprint from the FIRST m-line we
    // recognise (audio / video / application).  The DTLS session is
    // BUNDLE-shared, so all m= lines use the same DTLS role + fingerprint
    // — we only need to read the offer's setup once.
    for (const auto& rm : remote.media) {
        if (rm.type != sdp::MediaType::Audio &&
            rm.type != sdp::MediaType::Video &&
            rm.type != sdp::MediaType::Application) {
            continue;
        }
        if (auto d = impl_->dtls.lock()) {
        // DTLS role is determined by the remote peer's `a=setup` attribute
        // (RFC 5763 §5):
        //
        //   remote "actpass" → the peer did not commit to a role; we are the
        //                      answerer and we decide.  WebRTC convention
        //                      (libwebrtc / Chrome): answerer is the DTLS
        //                      Client (sends ClientHello).  We therefore
        //                      become Client and advertise "active".
        //   remote "active"  → the peer will send ClientHello; we are Server
        //                      (passive), advertise "passive".
        //   remote "passive" → the peer is waiting for our ClientHello;
        //                      we are Client (active), advertise "active".
        //   remote ""        → missing setup attribute; treat as "actpass"
        //                      (we answerer, become Client, advertise "active").
        //
        // The resulting local setup attribute is also captured for the
        // answer SDP below.
        std::string peer_setup = rm.dtls_setup;
        if (peer_setup.empty()) peer_setup = "actpass";

        // DEBUG-TEMP: force NimRTC to be the DTLS SERVER (passive) for
        // Chrome interop.  Recent Chrome 124+ BoringSSL rejects every
        // variant of anonymous client response to CertificateRequest
        // (empty list → handshake_failure, omitted → unexpected_message,
        // self-signed cert → certificate_unknown) — but as the SERVER we
        // never receive a CertReq so the question doesn't arise.  This
        // is purely a workaround for the Chrome-side rejection of RFC
        // 5246 §7.4.6 anonymous clients; restore the original logic
        // once Chrome's BoringSSL relaxes this.
        nimrtc::dtls::DtlsRole desired_role  = nimrtc::dtls::DtlsRole::Server;
        std::string    local_setup_attr = "passive";
        (void)peer_setup;
        d->set_role(desired_role);
        impl_->dtls_local_setup = local_setup_attr;
        core::log::Logger::instance().info(
            std::string("process_remote_sdp: dtls role resolved peer_setup='")
            + peer_setup + "' local_setup='" + local_setup_attr
            + "' role=" + (desired_role == nimrtc::dtls::DtlsRole::Client ? "Client" : "Server"));

            if (!rm.dtls_fingerprint_algo.empty() &&
                !rm.dtls_fingerprint_value.empty()) {
                // RFC 8122: peer fingerprint value is colon-separated UPPER hex,
                // NOT base64.  Use hex_colon_decode; fall back to base64 if the
                // value clearly isn't hex (older peers / future deprecation).
                auto bytes = hex_colon_decode(rm.dtls_fingerprint_value);
                if (bytes.empty()) {
                    bytes = base64_decode(rm.dtls_fingerprint_value);
                }
                d->set_peer_fingerprint(rm.dtls_fingerprint_algo, std::move(bytes));
            }
        }
        break;
    }

    if (ice_t_) ice_t_->wait_for_gathering(1500);

    sdp::SessionDescription ans;
    ans.version = 0;
    ans.origin_username         = "-";
    ans.origin_session_id      = make_session_id();
    ans.origin_session_version = "1";
    ans.origin_address          = "0.0.0.0";
    ans.session_name            = "-";
    ans.bundle_mids             = remote.bundle_mids;
    ans.msid_semantic_token     = "WMS";

    for (const auto& rm : remote.media) {
        // Support audio (RTP), video (RTP/H.264), and application (data
        // channel, DTLS/SCTP).  Skipping other types avoids leaking
        // unknown m= sections into the answer.
        if (rm.type != sdp::MediaType::Audio &&
            rm.type != sdp::MediaType::Video &&
            rm.type != sdp::MediaType::Application) {
            continue;
        }
        sdp::MediaDescription am;
        am.type = rm.type;
        am.port = 9;
        am.protocol = rm.protocol.empty() ? "UDP/TLS/RTP/SAVPF" : rm.protocol;
        am.formats  = rm.formats;
        // RFC 3264 §6.1: the answer's media direction is the answerer's
        // capability intersected with the offer's direction.  For our
        // demo the answerer always has SendRecv capability, so the
        // answer direction reduces to the inverse of the offer for
        // asymmetric offers:
        //   offer sendrecv -> answer sendrecv
        //   offer sendonly -> answer recvonly
        //   offer recvonly -> answer sendonly
        //   offer inactive -> answer inactive
        am.direction = ([&]() {
            using D = sdp::Direction;
            switch (rm.direction) {
                case D::SendRecv: return D::SendRecv;
                case D::SendOnly: return D::RecvOnly;
                case D::RecvOnly: return D::SendOnly;
                case D::Inactive: return D::Inactive;
            }
            return D::SendRecv;
        })();
        // Preserve the remote mid when present (BUNDLE); fall back to a
        // positional assignment by index for the legacy single-m-line case.
        am.mid      = rm.mid.empty() ? std::to_string(remote.media.size()) : rm.mid;
        // rtcp-mux is RTP-specific — invalid on an SCTP m-line.  The
        // munger emits `a=rtcp-mux` whenever the field is non-empty;
        // we set it only for audio/video to avoid spurious lines
        // on the SCTP m-line.
        am.rtcp_mux_value =
            (rm.type == sdp::MediaType::Application) ? "" : "rtcp-mux";
        am.ice_ufrag = local_ufrag();
        am.ice_pwd   = local_password();
        am.dtls_setup = impl_->dtls_local_setup.empty()
                            ? "active"
                            : impl_->dtls_local_setup;
        core::log::Logger::instance().info(
            std::string("process_remote_sdp: answer SDP setup='") + am.dtls_setup +
            "' (peer wanted '" + rm.dtls_setup + "')");
        if (auto d = impl_->dtls.lock()) {
            am.dtls_fingerprint_algo  = "sha-256";
            am.dtls_fingerprint_value = d->local_fingerprint().hex_colon;
        }

        // For audio: carry rtpmap/fmtp from offer.
        if (rm.type == sdp::MediaType::Audio) {
            am.rtpmap = rm.rtpmap;
            am.fmtp   = rm.fmtp;
        }

        // For video: carry rtpmap/fmtp/rtcp-fb from offer verbatim.  The
        // remote codec params (profile-level-id, packetization-mode, etc.)
        // are what the remote decoder expects to see in our answer; the
        // engine doesn't renegotiate them.  We also append the a=ssrc
        // line so Chrome's inbound-rtp counter advances on the wire SSRC
        // we emit.
        if (rm.type == sdp::MediaType::Video) {
            am.rtpmap   = rm.rtpmap;
            am.fmtp     = rm.fmtp;
            am.rtcp_fb  = rm.rtcp_fb;
            char ssrc_line[96];
            std::snprintf(ssrc_line, sizeof(ssrc_line),
                          "%u cname:nimrtc-video",
                          static_cast<unsigned>(config_.video_sender_tuning.ssrc));
            am.extra_attrs.emplace_back("ssrc", ssrc_line);
        }

        // For data channel (application): carry sctp-port / max-message-size
        // attributes verbatim.  We use `extra_attrs` (the catch-all
        // a= bucket in `MediaDescription`) for `sctp-port` because the
        // parser only stores known SDP attributes in dedicated fields;
        // `sctp-port` isn't a dedicated field on `MediaDescription`
        // yet (P2 follow-up).  The munger re-emits
        // `a=sctp-port:<port>` from extra_attrs verbatim, which is
        // exactly what Chrome's SCTP-over-DTLS stack needs to see.
        if (rm.type == sdp::MediaType::Application) {
            for (auto& [fmt, params] : rm.fmtp) {
                // params looks like "max-message-size=1073741823"
                am.fmtp[fmt] = params;
            }
            // Echo any peer-supplied `a=sctp-port:` value.  When the
            // peer didn't include one, fall back to the engine's own
            // configured port so the answer never omits the attribute
            // (RFC 8831 §6.1 calls sctp-port mandatory for SCTP).
            bool found_sctp_port = false;
            for (const auto& [k, v] : rm.extra_attrs) {
                if (k == "sctp-port" && !v.empty()) {
                    am.extra_attrs.emplace_back(k, v);
                    found_sctp_port = true;
                }
            }
            if (!found_sctp_port) {
                am.extra_attrs.emplace_back(
                    "sctp-port", std::to_string(config_.sctp_port));
            }
            // Likewise for max-message-size — echo if present, default
            // to 6556 (Chrome M85+) when absent.
            bool found_max_msg = false;
            for (const auto& [k, v] : rm.extra_attrs) {
                if (k == "max-message-size" && !v.empty()) {
                    am.extra_attrs.emplace_back(k, v);
                    found_max_msg = true;
                }
            }
            if (!found_max_msg) {
                am.extra_attrs.emplace_back("max-message-size", "6556");
            }
        }

        if (ice_t_) {
            auto gathered = ice_t_->gathered_local_candidates();
            for (auto& c : gathered) {
                // libjuice emits candidates with the "a=candidate:" prefix.
                // The munger adds its own "a=candidate:" so strip the prefix.
                constexpr std::string_view kFullPrefix = "a=candidate:";
                constexpr std::string_view kHalfPrefix = "candidate:";
                if (c.size() >= kFullPrefix.size() &&
                    c.substr(0, kFullPrefix.size()) == kFullPrefix) {
                    am.candidates.push_back(std::string(c.substr(kFullPrefix.size())));
                } else if (c.size() >= kHalfPrefix.size() &&
                           c.substr(0, kHalfPrefix.size()) == kHalfPrefix) {
                    am.candidates.push_back(std::string(c.substr(kHalfPrefix.size())));
                } else {
                    am.candidates.push_back(c);
                }
            }
        }

        ans.media.push_back(std::move(am));
    }

    // Rebuild BUNDLE mids from the answer's media sections so a remote
    // multi-m-line BUNDLE offer produces a matching multi-m-line answer.
    if (!ans.media.empty()) {
        ans.bundle_mids.clear();
        for (const auto& m : ans.media) {
            if (!m.mid.empty()) ans.bundle_mids.push_back(m.mid);
        }
        if (ans.bundle_mids.empty()) {
            ans.bundle_mids = {"0"};
        }
    }

    auto ans_str = impl_->sdp_munger->to_sdp(ans);
    if (!ans_str) {
        if (on_error_) on_error_(core::kEngineSdpCorrupt, "SDP munger failed");
        return std::nullopt;
    }
    return ans_str.value();
}

uint32_t NimRTCEngine::send_audio(const float* pcm_samples, std::size_t num_samples) noexcept {
    if (!is_open()) return core::kEngineNotReady;
    if (!pcm_samples || num_samples == 0) return core::kEngineInvalidParam;
    // Gate on DTLS being Connected — otherwise RTP packets would reach the
    // peer over plaintext UDP (no SRTP key), or get sent into a half-open
    // session that the peer has already torn down (e.g. after a fatal
    // DTLS alert).  Without this check, demo-p2p would continue logging
    // "audio frames sent=N rc=0x0" after the handshake has transitioned
    // to Failed.
    auto d_check = impl_->dtls.lock();
    if (!d_check || !d_check->is_connected()) {
        return core::kEngineNotReady;
    }

    // 3A — prefer plugin adapter; fall back to concrete NullAudio3A.
    if (audio3a_plugin_) {
        // Plugin API takes raw (ptr, samples, channels) — same shape we have.
        audio3a_plugin_->process_capture(
            const_cast<float*>(pcm_samples),
            num_samples,
            config_.pcm_channels);
    } else if (impl_->audio3a_concrete) {
        audio3a::Frame frame;
        frame.samples        = const_cast<float*>(pcm_samples);
        frame.num_samples    = num_samples;
        frame.num_channels   = config_.pcm_channels;
        frame.sample_rate_hz = config_.pcm_sample_rate_hz;
        impl_->audio3a_concrete->process_capture(frame);
    }

    // Codec — prefer plugin adapter; fall back to concrete opus::Encoder.
    // Reuse Impl::audio_codec_pkt (P0#4) — capacity grows monotonically.
    const std::size_t needed = num_samples * sizeof(float) + 64;
    if (impl_->audio_codec_pkt.size() < needed) {
        impl_->audio_codec_pkt.resize(needed);
    }
    std::size_t codec_len = 0;
    if (codec_plugin_) {
        codec_len = codec_plugin_->encode(pcm_samples, num_samples,
                                         impl_->audio_codec_pkt.data(),
                                         impl_->audio_codec_pkt.size());
#ifdef NIMRTC_HAS_OPUS
    } else if (impl_->opus_encoder) {
        codec_len = impl_->opus_encoder->encode(pcm_samples, num_samples,
                                         impl_->audio_codec_pkt.data(),
                                         impl_->audio_codec_pkt.size());
#endif
    }
    if (codec_len == 0) return 0;

    static std::atomic<std::uint32_t> seq{0};
    static std::atomic<std::uint32_t> ssrc_val{0xDEADBEEF};
    static std::atomic<std::uint32_t> ts_val{0};
    rtp::PacketBuilder pb;
    std::uint32_t curr_ssrc = ssrc_val.load();
    std::uint16_t seq_num = static_cast<std::uint16_t>(seq.fetch_add(1));
    std::uint32_t cur_ts  = ts_val.fetch_add(static_cast<std::uint32_t>(num_samples));
    pb.set_ssrc(curr_ssrc)
      .set_seq(seq_num)
      .set_timestamp(cur_ts)
      .set_payload_type(config_.audio_codec.payload_type)
      .set_marker(false)
      .set_payload(core::ByteSpan(impl_->audio_codec_pkt.data(), codec_len));
    auto pkt_buf = pb.build();
    if (pkt_buf.empty()) return 0;

    // SRTP protect (or pass-through when not yet keyed).
    // Reuse Impl::audio_send_buf (P0#4) — one allocation ever, then in-place.
    core::ByteSpan out_payload(pkt_buf.data(), pkt_buf.size());
    if (impl_->srtp_installed && impl_->srtp) {
        auto* sess = impl_->srtp->get_session(curr_ssrc, /*outgoing=*/true);
        if (!sess) {
            // SRTP installed but no session for this SSRC — drop.
            return 0;
        }
        auto enc = sess->protect_rtp(out_payload, curr_ssrc, cur_ts);
        if (!enc) return 0;
        impl_->audio_send_buf.assign(enc.value().data(),
                                     enc.value().data() + enc.value().size());
    } else {
        impl_->audio_send_buf.assign(out_payload.data(),
                                     out_payload.data() + out_payload.size());
    }

    // ---- Scheduler: enqueue for priority-drain (move, P0#3) --------------
    // The scheduler takes ownership of audio_send_buf via enqueue_owned.
    // After this call, audio_send_buf is in MOVED-FROM state; we must not
    // touch it.  The scheduler will release it on drain.
    if (scheduler_) {
        scheduler_->enqueue_owned(plugins::Priority::kAudio,
                                  std::move(impl_->audio_send_buf),
                                  plugins::Addr{});
        return 0;
    }

    // Fallback: no scheduler — send directly through ICE (transport profile).
    plugins::BufferView bv{impl_->audio_send_buf.data(),
                           impl_->audio_send_buf.size()};
    const auto rc = ice_t_->send(bv, {});
    // Clear so the next call starts fresh.
    impl_->audio_send_buf.clear();
    impl_->audio_send_buf.shrink_to_fit();   // release backing if needed
    return rc;
}

plugins::Status NimRTCEngine::start_video() noexcept {
    if (!is_open()) return plugins::kErrNotReady;
    if (video_source_) {
        video_source_->start();
        return plugins::kOk;
    }
    return plugins::kErrNotReady;
}

void NimRTCEngine::stop_video() noexcept {
    if (video_source_) {
        video_source_->stop();
    }
}

plugins::Status NimRTCEngine::send_video(
    const plugins::VideoSourceFrame& frame) noexcept {
    if (!is_open()) return plugins::kErrNotReady;
    if (!video_codec_ || !video_sender_) return plugins::kErrNotReady;

    // Translate plugins::VideoSourceFrame → plugins::VideoFrame for the codec.
    // P0: only the metadata is propagated; the actual pixel storage is
    // owned by `frame.plane_y/u/v` and must outlive the encode call.
    plugins::VideoFrame raw{};
    raw.info.capture_ts_us = frame.capture_ts_us;
    raw.info.frame_seq     = frame.frame_seq;
    raw.info.rtp_timestamp = frame.rtp_timestamp;

    // Encode — reuse Impl::video_enc_buf (P0#4).  Reserve a typical IDR
    // size on first use; subsequent calls within the same capacity reuse
    // the storage and only pay the per-byte conversion cost inside the
    // codec.  H.264 frames can spike above 64 KiB (high-bitrate 1080p60)
    // — when that happens we resize and pay one reallocation.
    constexpr std::size_t kInitialVideoEncCap = 64 * 1024;
    if (impl_->video_enc_buf.capacity() < kInitialVideoEncCap) {
        impl_->video_enc_buf.reserve(kInitialVideoEncCap);
    }
    plugins::EncodedVideoFrame encoded{};
    const auto enc_rc = video_codec_->encode(raw,
                                             impl_->video_enc_buf.data(),
                                             impl_->video_enc_buf.capacity(),
                                             encoded);
    if (enc_rc != plugins::kOk) {
        // Codec may need a larger buffer than reserved (e.g. very high
        // bitrate IDR).  Resize to the codec's expected capacity and retry
        // exactly once to avoid an unbounded loop on a misbehaving codec.
        const std::size_t needed = encoded.payload.size();
        if (needed == 0 || needed > impl_->video_enc_buf.capacity() * 4) {
            return plugins::kErrInternal;  // give up on pathological sizes
        }
        impl_->video_enc_buf.reserve(needed);
        if (video_codec_->encode(raw,
                                 impl_->video_enc_buf.data(),
                                 impl_->video_enc_buf.capacity(),
                                 encoded) != plugins::kOk) {
            return plugins::kErrInternal;
        }
    }

    // Mark keyframe so the sender packet callback gives it kVideoKeyframe
    // priority (consumed on the marker=true packet).
    if (encoded.is_keyframe) {
        impl_->video_keyframe_pending_.store(true, std::memory_order_release);
    }

    // Packetize (the sender's packet callback handles RTP header assembly
    // + SRTP protect + scheduler enqueue).
    return video_sender_->push_frame(encoded, frame.rtp_timestamp);
}

int NimRTCEngine::tick() noexcept {
    if (!is_open()) return 0;

    // ---- Push BWE estimate into the scheduler (P2/P3 wiring) -------------
    // estimate() is cheap (reads an atomic snapshot in the AIMD impl) and
    // tick is on the engine thread, so no extra locks.  We skip the push
    // when target_bps == 0 so the scheduler keeps its previous budget
    // rather than reading "0 = total sender pause" on a transient BWE
    // startup glitch.  Both pointers may be null in the "transport" profile.
    if (bwe_ && scheduler_) {
        const std::uint32_t target_bps = bwe_->estimate().target_bitrate_bps;
        if (target_bps > 0) {
            scheduler_->on_bwe_update(target_bps);
        }
    }

    // ---- Drain the scheduler (if enabled) ---------------------------------
    // The scheduler holds outbound packets (audio / video / control) and
    // dispatches them in priority order.  drain_with() invokes our callback
    // for each packet; we send it through ICE.
    if (scheduler_) {
        scheduler_->drain_with(64, [this](plugins::Priority,
                                          core::ByteSpan data) noexcept -> bool {
            if (!ice_t_) return false;
            plugins::BufferView bv{data.data(), data.size()};
            ice_t_->send(bv, {});
            return true;
        });
    }

    // Only drain DTLS outbound once ICE has selected a candidate pair;
    // otherwise libjuice drops the UDP datagram ("Send while ICE is not
    // connected") and the handshake never starts.
    if (is_ice_connected()) {
        drain_dtls();
    }
    return ice_t_->recv();
}

const char* NimRTCEngine::ice_state_string() const noexcept {
    if (!ice_t_) return "closed";
    switch (ice_t_->state()) {
        case plugins::IceState::Disconnected: return "disconnected";
        case plugins::IceState::Gathering:    return "gathering";
        case plugins::IceState::Connecting:   return "connecting";
        case plugins::IceState::Connected:    return "connected";
        case plugins::IceState::Completed:    return "completed";
        case plugins::IceState::Failed:       return "failed";
    }
    return "unknown";
}

std::string NimRTCEngine::local_ufrag() const noexcept {
    return ice_t_ ? ice_t_->local_ufrag() : std::string{};
}

std::string NimRTCEngine::local_password() const noexcept {
    return ice_t_ ? ice_t_->local_password() : std::string{};
}

std::string NimRTCEngine::local_dtls_fingerprint_sha256_base64() const noexcept {
    // Pointer stability: dtls_ (weak_ptr) locks into a shared_ptr here.
    // The shared_ptr keeps the session alive for the lifetime of this
    // call, so the returned reference from local_fingerprint() is safe.
    auto d = impl_ ? impl_->dtls.lock() : std::shared_ptr<dtls::IDtlsSession>{};
    if (!d) return {};
    const auto& fp = d->local_fingerprint();
    if (fp.bytes.empty()) return {};
    static const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((fp.bytes.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 3 <= fp.bytes.size()) {
        std::uint32_t v = (std::uint32_t(fp.bytes[i]) << 16)
                        | (std::uint32_t(fp.bytes[i + 1]) <<  8)
                        |  std::uint32_t(fp.bytes[i + 2]);
        out.push_back(kAlphabet[(v >> 18) & 0x3f]);
        out.push_back(kAlphabet[(v >> 12) & 0x3f]);
        out.push_back(kAlphabet[(v >>  6) & 0x3f]);
        out.push_back(kAlphabet[ v        & 0x3f]);
        i += 3;
    }
    if (i < fp.bytes.size()) {
        std::uint32_t v = std::uint32_t(fp.bytes[i]) << 16;
        if (i + 1 < fp.bytes.size()) v |= std::uint32_t(fp.bytes[i + 1]) << 8;
        out.push_back(kAlphabet[(v >> 18) & 0x3f]);
        out.push_back(kAlphabet[(v >> 12) & 0x3f]);
        if (i + 1 < fp.bytes.size()) {
            out.push_back(kAlphabet[(v >> 6) & 0x3f]);
        } else {
            out.push_back('=');
        }
        out.push_back('=');
    }
    return out;
}

void NimRTCEngine::on_transport_recv(const plugins::BufferView& pkt) noexcept {
    // P0 (v0.12.0): guard against post-close use.  The ICE recv callback
    // can race with close(): a buffered packet may arrive on the transport
    // thread after close() has torn down the media_stack.  Without this
    // guard we'd hit a use-after-free in d->state() / feed_inbound()
    // below.  Mirrors the send_audio() pattern at line 2119.
    //
    // v0.13.0: impl_->dtls is a weak_ptr; .lock() returns nullptr
    // automatically after close(), so the inner if (auto d = ...) check
    // below catches the closed-engine case.  This is_open() guard
    // remains as a defence-in-depth short-circuit that avoids the
    // weak_ptr::lock() atomic increment on every non-DTLS packet.
    if (!is_open()) return;
    if (pkt.empty() || pkt.size() < 1) return;
    std::uint8_t b0 = pkt.data()[0];
    bool looks_like_dtls = (b0 == 22 || b0 == 23 || b0 == 21);
    if (looks_like_dtls) {
        auto d = impl_->dtls.lock();
        if (!d) return;  // weak_ptr expired — engine is closed; drop packet
        core::log::Logger::instance().info(
            std::string("engine[dtls]: rx dtls ct=") + std::to_string(b0) +
            " len=" + std::to_string(pkt.size()) +
            " state=" + std::string(nimrtc::dtls::DtlsSession::state_name(d->state())));
        std::span<const std::uint8_t> bytes(pkt.data(), pkt.size());
        // The ITransport::RecvCallback interface carries only the payload, no
        // source address.  For DTLS over ICE there is only one peer at a time
        // (the selected ICE candidate pair), so we derive the peer address from
        // the ICE transport's current selected remote endpoint.  This is correct
        // for both the initial ClientHello and all subsequent handshake records.
        //
        // NOTE: `ice_t_->remote_addr()` returns a text-encoded "host:port"
        // string packed into a plugins::Addr structure.  The DtlsAddr uses
        // plain "host" + port fields — the parsing below extracts those fields.
        nimrtc::dtls::DtlsAddr from;
        if (ice_t_) {
            plugins::Addr raw = ice_t_->remote_addr();
            // Text encoding: "host\0port\0" packed in raw.data[0..len-1].
            std::string_view sv(reinterpret_cast<const char*>(raw.data), raw.len);
            std::size_t colon = sv.find(':');
            if (colon != std::string_view::npos) {
                from.host.assign(sv.data(), colon);
                auto port_str = sv.substr(colon + 1);
                if (!port_str.empty()) {
                    // NOLINTNEXTLINE(cert-env33-c): port is always numeric here
                    int p = std::atoi(std::string(port_str).c_str());
                    from.port = static_cast<std::uint16_t>(p);
                }
            }
        }
        d->feed_inbound(bytes, from);
        // Only flush outbound DTLS records once ICE has selected a pair,
        // mirroring the gate in tick().  Without this, the first inbound
        // ClientHello (which arrives as soon as the peer's ICE connectivity
        // check reaches us) enqueues a HelloVerifyRequest that libjuice
        // silently drops with "Send while ICE is not connected"; the peer
        // never sees our cookie and keeps retransmitting its cookie-less
        // ClientHello forever.
        if (is_ice_connected()) {
            drain_dtls();
        }
        return;
    }
    // CCS (content type 20) isn't recognised as DTLS above; process it so
    // the receiving side can complete its state machine.
    if (b0 == nimrtc::dtls::kDtlsChangeCipherSpec) {
        auto d = impl_->dtls.lock();
        if (!d) return;  // weak_ptr expired — engine is closed; drop packet
        std::span<const std::uint8_t> bytes(pkt.data(), pkt.size());
        // Same peer-address derivation as the DTLS path above.
        nimrtc::dtls::DtlsAddr from;
        if (ice_t_) {
            plugins::Addr raw = ice_t_->remote_addr();
            std::string_view sv(reinterpret_cast<const char*>(raw.data), raw.len);
            std::size_t colon = sv.find(':');
            if (colon != std::string_view::npos) {
                from.host.assign(sv.data(), colon);
                auto port_str = sv.substr(colon + 1);
                if (!port_str.empty()) {
                    int p = std::atoi(std::string(port_str).c_str()); // NOLINT(cert-env33-c)
                    from.port = static_cast<std::uint16_t>(p);
                }
            }
        }
        d->feed_inbound(bytes, from);
        if (is_ice_connected()) {
            drain_dtls();
        }
        return;
    }
    std::vector<std::uint8_t> plaintext;
    core::ByteSpan view(pkt.data(), pkt.size());
    bool tried_srtp = false;
    if (impl_->srtp_installed && impl_->srtp) {
        auto* sess = impl_->srtp->get_session(0, /*outgoing=*/false);
        if (sess) {
            auto r = sess->unprotect_rtp(view, nullptr, nullptr);
            if (r) {
                view = r.value();
                tried_srtp = true;
            } else {
                ++impl_->srtp_stats_drop;
                return;
            }
        }
    }
    rtp::Parser rp;
    auto parsed = rp.parse(view);
    if (parsed) {
        handle_rtp(parsed.value());
        return;
    }
    if (!tried_srtp) {
        core::log::Logger::instance().debug(
            "engine: non-RTP and non-DTLS packet (likely STUN handled by libjuice)");
    }
}

int NimRTCEngine::drain_dtls() noexcept {
    auto d = impl_->dtls.lock();
    if (!d) return 0;

    // Drive the wolfSSL retransmit timer (RFC 6347 §4.2.4).  Without
    // this pump, a stalled handshake (e.g. lost HelloVerifyRequest
    // cookie exchange) hangs forever.  tick() is a no-op once the
    // session reaches Connected/Failed/Closed.
    d->tick();

    auto recs = d->take_outbound();
    int sent = 0;
    for (auto& rec : recs) {
        plugins::BufferView bv{rec.bytes.data(), rec.bytes.size()};
        if (ice_t_->send(bv, {}) == plugins::kOk) ++sent;
    }

    // Trace DTLS state transitions for debuggability.  Uses a per-engine
    // member (not a static local) so two engines in the same process
    // (e.g. the loopback test) log their transitions independently.
    const auto new_state = d->state();
    if (new_state != impl_->last_dtls_state) {
        core::log::Logger::instance().info(
            "engine[dtls]: state transition " +
            std::string(nimrtc::dtls::DtlsSession::state_name(impl_->last_dtls_state)) + " → " +
            std::string(nimrtc::dtls::DtlsSession::state_name(new_state)));
        impl_->last_dtls_state = new_state;
    }

    if (new_state == nimrtc::dtls::DtlsState::Connected) {
        // SRTP key installation happens here — this is the critical
        // handshake boundary the P1 interop tests verify.
        core::log::Logger::instance().info(
            "engine[dtls→srtp]: DTLS reached Connected, "
            "calling maybe_install_srtp_keys()");
        maybe_install_srtp_keys();
    }
    return sent;
}

void NimRTCEngine::maybe_install_srtp_keys() noexcept {
    // Idempotency: only install once per DTLS connection.
    if (impl_->srtp_installed) {
        core::log::Logger::instance().debug(
            "engine[dtls→srtp]: already installed, skipping");
        return;
    }
    auto d = impl_->dtls.lock();
    if (!d) {
        core::log::Logger::instance().debug(
            "engine[dtls→srtp]: dtls_ is null, skipping");
        return;
    }
    if (!impl_->srtp) {
        core::log::Logger::instance().debug(
            "engine[dtls→srtp]: srtp_ is null, skipping");
        return;
    }

    // Pre-condition: DTLS MUST be Connected (RFC 5764 §4.2 — keying material
    // is only available after the Finished message).  Re-check inside this
    // function to avoid races between drain_dtls()'s state read and our own.
    const auto dtls_state = d->state();
    if (dtls_state != nimrtc::dtls::DtlsState::Connected) {
        core::log::Logger::instance().debug(
            "engine[dtls→srtp]: dtls_state=" +
            std::to_string(static_cast<int>(dtls_state)) +
            " (not Connected, skipping key install)");
        return;
    }

    auto km = d->srtp_keying_material();
    if (!km) {
        core::log::Logger::instance().warn(
            "engine[dtls→srtp]: dtls reports Connected but no keying material");
        return;
    }

    // RFC 5764 §4.2 — DTLS-SRTP key derivation produces:
    //   client_master_key || client_master_salt || server_master_key || server_master_salt
    // Each side uses its OWN direction's keys for outbound, peer's for inbound.
    // NimRTC uses client_master_key for inbound (we are the controlled/answerer).
    std::vector<std::uint8_t> ck(km->client_master_key.begin(),
                                 km->client_master_key.end());
    std::vector<std::uint8_t> cs(km->client_master_salt.begin(),
                                 km->client_master_salt.end());

    auto& log = core::log::Logger::instance();
    log.info("engine[dtls→srtp]: derived keying material:");
    log.info("  - profile        = 0x" + std::to_string(static_cast<int>(km->profile)));
    log.info("  - key_len        = " + std::to_string(ck.size()));
    log.info("  - salt_len       = " + std::to_string(cs.size()));
    log.info("  - lifetime       = " + std::to_string(km->lifetime));
    if (!ck.empty()) {
        log.info("  - master_key[0..7] = " + [&]{
            std::string s;
            for (std::size_t i = 0; i < std::min<std::size_t>(8, ck.size()); ++i) {
                char buf[8]; std::snprintf(buf, sizeof(buf), "%02x", ck[i]);
                s += buf;
                if (i + 1 < std::min<std::size_t>(8, ck.size())) s += ":";
            }
            return s;
        }());
        log.info("  - master_salt[0..7] = " + [&]{
            std::string s;
            for (std::size_t i = 0; i < std::min<std::size_t>(8, cs.size()); ++i) {
                char buf[8]; std::snprintf(buf, sizeof(buf), "%02x", cs[i]);
                s += buf;
                if (i + 1 < std::min<std::size_t>(8, cs.size())) s += ":";
            }
            return s;
        }());
    }

    // Install the PEER's keys for INBOUND (decrypting packets from Chrome).
    impl_->srtp->derive_keys_for_remote(ck, cs, srtp::CryptoSuite::Aes128CmSha1_80);

    // Install NimRTC's OWN (server) keys for OUTBOUND (encrypting packets to Chrome).
    // RFC 5764 §4.2: each side uses its OWN direction's keys for outbound.
    std::vector<std::uint8_t> sk(km->server_master_key.begin(),
                                 km->server_master_key.end());
    std::vector<std::uint8_t> ss(km->server_master_salt.begin(),
                                 km->server_master_salt.end());
    if (!sk.empty() && !ss.empty()) {
        impl_->srtp->derive_keys_for_local(sk, ss, srtp::CryptoSuite::Aes128CmSha1_80);
    } else {
        core::log::Logger::instance().warn(
            "engine[dtls→srtp]: server_master_key/salt missing — outbound SRTP will fail");
    }

    // Mark installed; subsequent calls will short-circuit.
    impl_->srtp_installed = true;

    log.info("engine[dtls→srtp]: SRTP keys INSTALLED ✓ "
             "(state=" + std::string(state_ == State::kOpen ? "Open" : "Other") +
             ") — RTP from peer will now be unprotectable");
}

uint32_t NimRTCEngine::feed_srtp_inbound(const std::uint8_t* srtp_packet, std::size_t len) noexcept {
    if (!srtp_packet || len == 0 || !impl_->srtp) return core::kEngineInvalidParam;
    auto sess = impl_->srtp->get_session(0, /*outgoing=*/false);
    if (!sess) return 0;
    core::ByteSpan span(srtp_packet, len);
    auto un = sess->unprotect_rtp(span, nullptr, nullptr);
    return un ? 0 : 0xDEAD;
}

bool NimRTCEngine::set_remote_ice(std::string_view ice_block) noexcept {
    // If the transport already exists, apply immediately through the plugin seam.
    if (ice_t_) {
        return ice_t_->set_remote_description(ice_block) == plugins::kOk;
    }
    return false;
}

int NimRTCEngine::add_turn_server(std::string_view host, std::uint16_t port,
                                  std::string_view username,
                                  std::string_view password) noexcept {
    if (!ice_t_) {
        core::log::Logger::instance().error(
            "add_turn_server: ICE transport not yet created — call pre_open() first");
        return -1;
    }
    ice_t_->add_turn_server(host, port, username, password);
    return 0;
}

void NimRTCEngine::handle_rtp(const rtp::PacketView& pv) noexcept {
    // ---- Demux by RTP payload type --------------------------------------
    // The audio_codec PT is configured in EngineConfig; the video codec PT
    // lives in the video_receiver/video_sender tuning. We first check
    // whether this packet matches the video PT (and a video_receiver
    // plugin is wired up) — if so, push it through the video receiver
    // pipeline; otherwise treat it as audio.
    if (video_receiver_ &&
        pv.payload_type == config_.video_receiver_tuning.payload_type) {
        plugins::VideoRtpPacket vp;
        vp.ssrc          = pv.ssrc;
        vp.seq           = pv.seq;
        vp.rtp_timestamp = pv.timestamp;
        vp.payload_type  = pv.payload_type;
        vp.marker        = pv.marker;
        vp.recv_ts_us    = plugins::TimestampUs{
            static_cast<std::int64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    core::SteadyClock::now().time_since_epoch()).count())};
        // BufferView = core::ByteSpan = std::span<const uint8_t>
        vp.payload       = pv.payload;
        video_receiver_->push_rtp(vp, vp.recv_ts_us);
        video_receiver_->tick(vp.recv_ts_us);
        return;
    }

    // ---- Audio path (existing behaviour) --------------------------------
    auto& jb = impl_->jitter_buffers[pv.ssrc];
    if (!jb) {
        jb::Config jc;
        jc.initial_delay = core::Milliseconds{config_.jb_initial_delay_ms};
        jc.min_delay     = core::Milliseconds{config_.jb_min_delay_ms};
        jc.max_delay     = core::Milliseconds{config_.jb_max_delay_ms};
        jb = std::make_unique<jb::JitterBuffer>(jc);
    }
    jb->push(pv, core::SteadyClock::now(), std::nullopt);
    if (auto frame = jb->pop(core::SteadyClock::now())) {
        if (on_audio_frame_ && !frame->packets.empty()) {
            const auto& p = frame->packets.front();
            if (p.payload.data() != nullptr && p.payload.size() > 0) {
                on_audio_frame_(reinterpret_cast<const float*>(p.payload.data()),
                                p.payload.size() / sizeof(float));
            }
        }
    }
}

void NimRTCEngine::default_on_error(uint32_t err, std::string_view msg) noexcept {
    core::log::Logger::instance().error(
        "ERROR 0x" + ([err, msg] {
            char hex[8];
            std::snprintf(hex, sizeof(hex), "%04X", static_cast<unsigned>(err));
            return std::string(hex) + ": " + std::string(msg);
        })());
}

// ---------------------------------------------------------------------------
// Out-of-line inspection accessors
//
// These used to be `inline` in the public header.  They reference concrete
// module state (DTLS, SRTP, ICE plugin) and therefore pull in concrete
// module headers — keeping them out-of-line lets engine.hpp stay free of
// those headers (Layout Invariant 4).
// ---------------------------------------------------------------------------

nimrtc::dtls::DtlsState NimRTCEngine::dtls_state() const noexcept {
    auto d = impl_->dtls.lock();
    return d ? d->state() : nimrtc::dtls::DtlsState::Closed;
}

bool NimRTCEngine::dtls_connected() const noexcept {
    auto d = impl_->dtls.lock();
    return d && d->is_connected();
}

bool NimRTCEngine::is_ice_connected() const noexcept {
    if (!ice_t_) return false;
    const auto s = ice_t_->state();
    return s == plugins::IceState::Connected ||
           s == plugins::IceState::Completed;
}

bool NimRTCEngine::srtp_installed() const noexcept {
    return impl_->srtp_installed;
}

uint32_t NimRTCEngine::last_open_rc() const noexcept {
    return impl_->last_open_rc;
}

NimRTCEngine::Stats NimRTCEngine::stats() const noexcept {
    return Stats{impl_->srtp_stats_drop};
}

} // namespace nimrtc::engine
