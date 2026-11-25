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

// R1 (v1.1): core TU of the five-way engine.cpp split.
// Behaviour unchanged; see engine_impl.hpp.
#include "engine_impl.hpp"
#include "engine_device_policy.hpp"

#include <mutex>

namespace nimrtc::engine {

namespace {

/** Draw the next outbound audio SSRC (A-6).
 *
 *  The SSRC used to be the constant 0xDEADBEEF, which meant two engines
 *  in one process advertised and stamped the *same* SSRC — the peer could
 *  not tell the two streams apart.  It is now drawn from a per-process
 *  generator: the seed is taken once per process, and every engine draws
 *  its own value from it, so two engines never collide.
 *
 *  The generator is seeded from std::random_device, so the value is not
 *  predictable from the process start time alone. */
std::uint32_t next_random_audio_ssrc() noexcept {
    static std::mutex mtx;
    static std::mt19937 rng = [] {
        std::random_device rd;
        std::seed_seq seq{rd(), rd(), rd(), rd(),
                          static_cast<unsigned>(
                              std::chrono::high_resolution_clock::now()
                                  .time_since_epoch().count())};
        return std::mt19937(seq);
    }();
    std::lock_guard<std::mutex> lock(mtx);
    std::uniform_int_distribution<std::uint32_t> dist(1, 0xFFFFFFFFu);
    return dist(rng);
}

}  // namespace


// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

NimRTCEngine::NimRTCEngine(EngineConfig config)
    : impl_(new Impl()), config_(std::move(config)) {
    // A-6: the audio SSRC belongs to this engine.  An explicit value
    // (a test that wants a reproducible stream) wins; otherwise one is
    // drawn so that two engines in the same process never collide.
    impl_->audio_state.audio_ssrc = config_.audio_ssrc != 0
                            ? config_.audio_ssrc
                            : next_random_audio_ssrc();
    // Phase 3: the ref-frame registry is a fixed-size open-addressed
    // table.  It is sized once (in the table's own constructor) so that
    // recording a frame never allocates (the old `std::map` +
    // `std::deque` pair allocated twice per sent frame).
    // Phase 3: the outbound video scratch buffers are pre-sized to the
    // MTU so that the *first* keyframe -- which is larger than an
    // inter-frame -- does not have to grow them.  A keyframe's RTP
    // packet is bigger than an inter-frame's, so a scratch that started
    // empty would reallocate exactly once, on the first keyframe, and
    // the benchmark would report a non-zero allocation count even
    // though the steady state is allocation-free.
    {
        const std::size_t mtu = config_.video_sender_tuning.mtu;
        // The SRTP auth tag is appended after the RTP payload, so the
        // outbound buffer must hold the packet *plus* the tag.
        constexpr std::size_t kSrtpTagHeadroom = 16;
        const std::size_t outbound = mtu + kSrtpTagHeadroom;
        impl_->scratch.video_rtp_scratch.reserve(outbound);
        impl_->scratch.video_pkt_scratch.reserve(outbound);
    }
}


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
    return impl_ ? std::string_view{impl_->session_state.resolved_strategy_name}
                 : std::string_view{};
}

NimRTCEngine::TransportSessionView

NimRTCEngine::transport_strategy_session() const noexcept {
    if (!impl_) return {std::string_view{}, std::string_view{}};
    return {std::string_view{impl_->session_state.resolved_media_factory_id},
            std::string_view{impl_->session_state.resolved_control_factory_id}};
}

// The resolved media-plane p99 budget.  `init_transport_strategy()`
// stores it after applying the per-instance override in
// `cfg.transport_media_max_p99_ms`; before open() it is empty.
std::optional<int> NimRTCEngine::transport_media_max_p99_ms() const noexcept {
    if (!impl_) return std::nullopt;
    return impl_->session_state.resolved_media_max_p99_ms;
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
    // v1.2.0 (session capability plan D1): the RFC 4733 codec.
    // Resolution is not fatal — a missing codec only disables the DTMF
    // path, so the rest of the session still works.
    resolve_dtmf_codec();
    if (const uint32_t rc = init_datachannel_registry(); rc != 0) return rc;
    if (const uint32_t rc = init_dtls_handshake();       rc != 0) return rc;

    // ---- Session model (v1.2.0, session capability plan N1 / T1) -------
    //
    // The engine drives renegotiation through a pluggable session model
    // (`plugins::ISession`), resolved from the registry's session slot
    // with `EngineConfig::session_name`.  Resolution is not fatal: when
    // the model is missing the engine keeps its pre-v1.2.0 fixed media
    // list and the track API reports "no session".
    resolve_session_model();

    // ---- The remote-desktop control channel (plan G1 / G2) -------------
    //
    // The channel is resolved through the registry's control-channel
    // slot (`EngineConfig::control_channel_name`), so the engine never
    // includes a concrete module's header (Layout Invariant 4).  It is
    // resolved *after* the transport session exists, because the
    // transport selection (the DTLS app-data seam vs. the control
    // stack's raw-UDP path) depends on it.
    if (const uint32_t rc = resolve_control_channel(); rc != 0) return rc;

    // ---- The QoE collector (plan J3 / J6) -------------------------------
    //
    // The collector is resolved through the registry's qoe-collector
    // slot (`EngineConfig::qoe_collector_name`), so the engine never
    // includes a concrete module's header (Layout Invariant 4).  It is
    // **on by default**: a remote-desktop session is judged on exactly
    // these metrics, so a host should not have to name a plugin to get
    // them.  The opt-out is an empty `qoe_collector_name`.
    if (const uint32_t rc = resolve_qoe_collector(); rc != 0) return rc;

    // ---- The render clock (v1.3 Q6 / remote-desktop plan R3) -----------
    //
    // The A/V-sync *policy* lives in the `video_sync` module and is
    // reached through the registry's video-sync slot
    // (`EngineConfig::video_sync_controller_name`), so the engine never
    // includes a concrete module's header (Layout Invariant 4).  The
    // module is optional: a build without it presents every frame
    // unchanged, which is the pre-v1.3 behaviour.
    if (const uint32_t rc = resolve_video_sync_controller(); rc != 0) return rc;

    return 0;
}


uint32_t NimRTCEngine::open() noexcept {
    if (state_ != State::kConstructed) return core::kEngineNotReady;

    // ---- Audio clock check (A-7 / O-5) --------------------------------
    //
    // The RTP timestamp of an audio stream counts samples of the
    // *codec's* clock — 48 kHz for Opus, 8 kHz for G.711.  If the PCM
    // the host hands to send_audio() is sampled at a different rate,
    // every timestamp would be off by that ratio and the peer's jitter
    // buffer would play the stream at the wrong speed.
    //
    // There is no resampler in the stack, so the engine refuses to open
    // instead of emitting a stream on the wrong clock: a host that
    // configured the rate wrongly gets an explicit error rather than
    // audio that drifts.
    //
    // The rule is "the PCM rate must equal the codec's clock rate", so
    // Opus still requires 48 kHz (A-7) while G.711 still works at
    // 8 kHz.  A rate of 0 means "no audio configured" (the SFU profile
    // uses it) and is left alone.
    if (config_.pcm_sample_rate_hz != 0 &&
        config_.pcm_sample_rate_hz != config_.audio_codec.clock_rate) {
        impl_->session_state.last_open_rc = core::kAudioClockMismatch;
        core::log::Logger::instance().error(
            std::string("open(): pcm_sample_rate_hz=") +
            std::to_string(config_.pcm_sample_rate_hz) +
            " does not match the audio codec clock rate " +
            std::to_string(config_.audio_codec.clock_rate));
        if (on_error_) {
            on_error_(core::kAudioClockMismatch,
                      "open(): pcm_sample_rate_hz must equal the audio "
                      "codec clock rate");
        }
        return core::kAudioClockMismatch;
    }

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
        impl_->session_state.last_open_rc = core::kEngineInternal;
        if (on_error_) on_error_(core::kEngineInternal,
            "open(): ice_t_ is null (make_transport_instance returned null media_stack)");
        return core::kEngineInternal;
    }
    // ---- TURN credentials (v1.4 N7, ADR-032) ----------------------------
    //
    // Before candidate gathering starts, the engine resets the credentials
    // bookkeeping (a re-open starts each session clean — the ICE module's
    // own rejection latch clears here too) and pulls a credential set from
    // the host's provider when one is configured and no static TURN server
    // was added.  The pull runs before `ice_t_->open()` so the TURN server
    // is in the transport's config when the agent is built and gathering
    // begins (spec §二 4).
    impl_->session_state.turn_credentials.reset();
    // v1.4 N8: each session's path-change ledger starts from zero — and the
    // flat `Stats` projection with it, so a fresh open() does not report a
    // change that happened in a previous session.
    impl_->session_state.path_change.reset();
    impl_->session_state.last_path_change_reason.clear();
    impl_->session_state.last_path_change_new_pair.reset();
    refresh_turn_credentials();

    core::log::Logger::instance().debug("op8 calling ice_t_->open()");
    const auto ice_open_rc = ice_t_->open();
    if (ice_open_rc != plugins::kOk) {
        char err[64];
        std::snprintf(err, sizeof(err), "transport open failed rc=%d",
                      static_cast<int>(ice_open_rc));
        impl_->session_state.last_open_rc = core::kEngineInternal;
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
    //
    // The one *fatal* case is a codec plugin that does not identify its
    // codec family (see `IVideoCodec::kind()`): the engine cannot
    // derive the SDP `a=rtpmap:` encoding name from it, so the session
    // would advertise a malformed video m-line.  In that case the video
    // plugins stay down and `open()` fails.
    if (const uint32_t video_rc = init_video_plugins(); video_rc != plugins::kOk) {
        return video_rc;
    }

    // ---- Audio devices (remote-desktop plan D1) --------------------------
    //
    // The audio capture / playback devices are opened after the video
    // plugins.  An "auto" device that cannot open degrades to "no device"
    // with a warn and does not fail the session; an explicit id that
    // cannot open is a hard error, exactly like the video side (spec §五).
    if (const uint32_t audio_rc = init_audio_devices(); audio_rc != 0) {
        return audio_rc;
    }

    // ---- Session base state (v1.2.0, session capability plan N1) --------
    //
    // The video m-line only exists when the video pipeline resolved, so
    // the base session state is (re)built once the video plugins are
    // known.  The call is idempotent — it only adds what is missing.
    ensure_base_session_state();

    // ---- Cross-end timeline anchor (ADR-017) --------------------------------
    //
    // Sample both clocks once, back to back, so the monotonic origin
    // converts a steady-clock reading into a Unix-anchored instant
    // without a second clock read per packet.  Sampling both here
    // (rather than deriving one from the other) keeps the residual offset
    // inside one scheduling jitter (~µs), which is an order of magnitude
    // below the 15.26 µs abs-send-time quantisation.
    //
    // The origin is the wall clock at *monotonic zero* (the boot epoch),
    // NOT the wall clock at open(): `SenderReportTracker` computes our
    // own instant as `unix_us(origin) + monotonic_reading`, so an origin
    // that already carries "now" would add the epoch twice and make every
    // skew sample meaningless (ADR-017 §"Decision 4").
    //
    // Nothing is sent here: with `timeline_enabled = false` the tracker stays
    // empty, no extmap is advertised, and no SR is scheduled.
    {
        const auto steady_now = core::SteadyClock::now();
        const auto system_now = std::chrono::system_clock::now();
        const std::int64_t steady_us =
            std::chrono::duration_cast<std::chrono::microseconds>(
                steady_now.time_since_epoch()).count();
        const std::int64_t system_us =
            std::chrono::duration_cast<std::chrono::microseconds>(
                system_now.time_since_epoch()).count();
        impl_->timeline.boot_epoch_offset_us = system_us - steady_us;
        // The same instant expressed as an NTP64 (RFC 5905): the wall
        // clock that corresponds to a monotonic reading of zero.
        impl_->timeline.monotonic_origin_ntp64 = rtp::ntp64_from(
            std::chrono::system_clock::time_point{
                std::chrono::microseconds{impl_->timeline.boot_epoch_offset_us}});
        // Re-derive the offset *from the NTP64* so that the offset and
        // the origin denote exactly the same instant.
        //
        // The NTP64 fraction is 2^-32 s (0.233 µs), so converting the
        // wall clock into NTP64 and back truncates by up to 0.233 µs.
        // That is irrelevant for the arrival instant — but it would
        // *not* be irrelevant for the Sender Report, whose NTP field has
        // to agree with the abs-send-time element of the packet it
        // describes to the last microsecond.  The abs-send-time encoder
        // adds the monotonic reading to `unix_us(origin)`, so the engine's
        // own instants have to be built on that same basis: take the
        // offset from the origin, not the other way round.
        impl_->timeline.boot_epoch_offset_us = std::chrono::duration_cast<
            std::chrono::microseconds>(
            rtp::system_clock_from_ntp64(impl_->timeline.monotonic_origin_ntp64)
                .time_since_epoch())
                                           .count();
        impl_->timeline.sr_tracker.set_monotonic_origin(
            impl_->timeline.monotonic_origin_ntp64);
        impl_->timeline.sr_last_sent = steady_now;   // first SR fires one period in
        if (config_.timeline_enabled) {
            core::log::Logger::instance().info(
                "engine[timeline]: abs-send-time + Sender Reports enabled "
                "(sr_period_ms=" + std::to_string(config_.sr_period_ms) + ")");
        }
    }

    // v1.4 D7: every session starts from zero — the call-state machine is
    // reset, so a re-entrant open() never leaks a previous session's state.
    impl_->session_state.call_state.reset();
    state_ = State::kOpen;
    if (on_state_change_) on_state_change_("open");
    detail::trace("NimRTCEngine opened (DTLS, SRTP, Opus initialised)");
    // v1.4 D7: drive the first observation — the fresh machine's first
    // observation always reports kConnecting (acceptance 7).
    poll_call_state();
    return 0;
}


void NimRTCEngine::close() noexcept {
    if (state_ != State::kOpen) return;
    // Plan J3 / J6: the QoE collector is closed with the session — the
    // metrics are a property of the session, and a closed session's
    // reservoir is not worth keeping.
    impl_->qoe.close();
    if (ice_t_) ice_t_->close();
    // v1.4 N8: the session is over — drop the path-change ledger with it.
    impl_->session_state.path_change.reset();
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
    // v1.2.0 (audio pipeline WP-7): the audio receive pipeline is a
    // plugin; it is closed and released here.  The engine's own jitter
    // buffer and the concrete opus encoder/decoder are gone — the
    // pipeline and the codec seam own those roles now.
    if (audio_receiver_) {
        audio_receiver_->close();
        audio_receiver_.reset();
    }
    // Remote-desktop plan D1: the audio devices are closed with the
    // session, after the plugins that consume their PCM.
    close_audio_devices();
    audio3a_plugin_.reset();
    impl_->audio3a_concrete.reset();
    codec_plugin_.reset();
    impl_->srtp.reset();
    // ---- The data channels the host opened (plan Track S / S6) --------
    //
    // The engine keeps a shared handle on every channel it created (the
    // statistics need the backend's outbound counters), so the engine
    // closes them here rather than leaving them to the host's
    // destructor.  The entries stay in the statistics state: a reading
    // taken after close() still reports the last known numbers instead
    // of an empty list.
    {
        std::lock_guard<std::mutex> lock(impl_->data_channels.mutex);
        for (auto& entry : impl_->data_channels.channels) {
            if (!entry.backend) continue;
            entry.backend->close();
        }
    }
    // ---- WebTransport session teardown (v0.14.0-rc.1 follow-up) ----
    // Channels hold a non-owning pointer into wt_session — close it here
    // so no QUIC worker outlives the engine. Callers must destroy their
    // channels before close() returns (same contract as ice_t_).
    if (impl_->session_state.wt_session) {
        try {
            impl_->session_state.wt_session->close();
        } catch (...) {
        }
        impl_->session_state.wt_session.reset();
    }
    // weak_ptr reset (no implicit nullptr conversion in C++20)
    impl_->session_state.datachannel_transport_cookie.reset();
    state_ = State::kClosed;
    if (on_state_change_) on_state_change_("closed");

    // v1.4 D7: the call is over — report kClosed.  `stop()` / `hangup()` /
    // `close()` all land here, so the report fires exactly once (the
    // machine's close() judge reports nothing on a repeat).
    if (impl_->session_state.call_state.close()) {
        CallStateReport report;
        report.state = CallState::kClosed;
        fire_call_state(std::move(report));
    }
}


// ---------------------------------------------------------------------------
// v1.4 D7 — one call's lifecycle (start / stop / hangup)
// ---------------------------------------------------------------------------
//
// `stop()` is `close()` under its lifecycle name: the defined teardown order
// (QoE → ICE → audio plugins → codecs → video plugins → BWE → TransportSession)
// already lives in `close()` — D7 does not reorder it, it just names it and
// makes it testable (spec §四).  `hangup()` additionally carries a reason and
// notifies the peer *before* closing.

int NimRTCEngine::start(const EngineConfig& config) noexcept {
    config_ = config;
    return static_cast<int>(open());
}

void NimRTCEngine::stop() noexcept {
    close();
}

bool NimRTCEngine::hangup(std::string_view reason) noexcept {
    if (!is_open()) return false;
    // Best-effort: notify the peer of the hangup reason first, then close in
    // the defined order (spec §四 — hangup 先通知再关).  The reason is sent
    // as a control message; a missing / saturated control channel does not
    // stop the hangup.
    const auto* bytes =
        reinterpret_cast<const std::uint8_t*>(reason.data());
    (void)send_control_message(
        std::span<const std::uint8_t>(bytes, reason.size()));
    close();
    return true;
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
    return create_data_channel(
        label, plugins::DataChannelReliability::kReliableOrdered);
}

std::unique_ptr<plugins::IDataChannel>

NimRTCEngine::create_data_channel(
    std::string_view label,
    plugins::DataChannelReliability reliability) noexcept {
    // Gate 1: DataChannel disabled by config (resolved factory id empty).
    //
    // v0.14.0-rc.1: the resolved id is set by init_datachannel_registry()
    // and reflects the dispatch on `cfg.datachannel_backend` +
    // `cfg.datachannel_name` (with the v0.x legacy fallback for empty
    // `datachannel_backend`).  When the resolved id is empty, DataChannel
    // is disabled — `create_offer()` does not emit `m=application`,
    // process_remote_sdp() silently drops the peer's `m=application` line,
    // and create_data_channel() returns nullptr.
    if (resolved_dc_factory_id_.empty()) {
        if (on_error_) on_error_(core::kEngineInternal,
            "create_data_channel(): DataChannel is disabled (resolved factory "
            "id is empty; check cfg.datachannel_backend / cfg.datachannel_name)");
        return nullptr;
    }
    // Gate: engine must be open()ed (so impl_->dtls is built and the
    // registry slot has been probed by init_modules_once()).
    if (state_ != State::kOpen) {
        if (on_error_) on_error_(core::kEngineNotReady,
            "create_data_channel(): engine not open");
        return nullptr;
    }

    // WebTransport backend (v0.14.0-rc.1 follow-up): session-aware creation.
    //
    // The session is created lazily on the first call and owned by Impl
    // (shared_ptr) so every channel from this engine shares one QUIC
    // connection. The channel comes from
    // `IWebTransportSessionFactory::create_data_channel(session)` — the
    // seam-designed path (the msquic factory binds + attaches session
    // callbacks internally). Both open() overloads run: open(cfg) caches
    // the channel config, open() arms the stream/datagram path.
    if (resolved_dc_factory_id_ == "web_transport") {
        if (!impl_->session_state.wt_session) {
            // Case D forward-stall guard: interop answerer has empty
            // web_transport_peer_host (no QUIC peer configured). Creating a
            // client QUIC session with an empty hostname would enter
            // MsQuicConnectionStart on the tick thread and stall
            // engine->tick() -> ice.recv() so rx_queue backs up (DTLS
            // ClientHello never reaches feed_inbound). Fail fast instead;
            // the tick loop keeps draining DTLS/SRTP/RTP.
            if (config_.web_transport_peer_host.empty()) {
                if (on_error_) on_error_(core::kEngineInternal,
                    "create_data_channel(): web_transport_peer_host is empty "
                    "(no QUIC peer; skipping session creation so tick keeps draining DTLS)");
                return nullptr;
            }
            const web_transport::IWebTransportSessionFactory* wtf =
                pal::resolve_web_transport(config_.web_transport_name);
            if (!wtf) {
                if (on_error_) on_error_(core::kEngineInternal,
                    std::string("create_data_channel(): WebTransport session factory '")
                        .append(config_.web_transport_name)
                        .append("' not registered"));
                return nullptr;
            }
            web_transport::WebTransportConfig wcfg;
            wcfg.peer_hostname = config_.web_transport_peer_host;
            wcfg.peer_port = config_.web_transport_peer_port;
            std::unique_ptr<web_transport::IWebTransportSession> sess;
            try {
                sess = wtf->create(wcfg);
            } catch (...) {
                sess.reset();
            }
            if (!sess) {
                if (on_error_) on_error_(core::kEngineInternal,
                    "create_data_channel(): WebTransport session factory->create() failed");
                return nullptr;
            }
            impl_->session_state.wt_session =
                std::shared_ptr<web_transport::IWebTransportSession>(std::move(sess));
            core::log::Logger::instance().info(
                std::string("engine: WebTransport session created (factory '")
                    .append(wtf->id())
                    .append("', peer '")
                    .append(config_.web_transport_peer_host)
                    .append("')"));
        }
        const web_transport::IWebTransportSessionFactory* wtf =
            pal::resolve_web_transport(config_.web_transport_name);
        std::unique_ptr<plugins::IDataChannel> wch;
        if (wtf) {
            try {
                wch = wtf->create_data_channel(*impl_->session_state.wt_session);
            } catch (...) {
                wch.reset();
            }
        }
        if (!wch) {
            if (on_error_) on_error_(core::kEngineInternal,
                "create_data_channel(): WebTransport session factory->create_data_channel() returned nullptr");
            return nullptr;
        }
        // The engine keeps the backend alive (the statistics need its
        // outbound counters even after the host drops its handle) and
        // hands the host a proxy that counts the inbound traffic.
        // See `engine_datachannel_state.hpp`.
        std::shared_ptr<plugins::IDataChannel> backend{std::move(wch)};
        const std::uint32_t cid = next_data_channel_id_++;
        const std::string lbl{label};
        auto shared = impl_->data_channels.add(cid, lbl, reliability, backend);
        auto cbs = std::make_shared<detail::DataChannelCallbacks>();
        // WP-C1: the host's callbacks go through the shared block, so
        // the backend's late messages still find them.
        cbs->on_message = on_data_message_;
        cbs->on_state   = on_data_state_;
        if (on_data_message_ex_) {
            cbs->on_message_ex =
                [this, cid, lbl](const plugins::DataChannelMessage& m) noexcept {
                    if (!on_data_message_ex_) return;
                    plugins::DataChannelMessage out = m;
                    out.channel_id = cid;
                    if (out.label.empty()) out.label = lbl;
                    on_data_message_ex_(out);
                };
        }
        std::unique_ptr<plugins::IDataChannel> proxy =
            std::make_unique<detail::DataChannelProxy>(backend, shared, cbs);
        plugins::DataChannelConfig wdc_cfg;
        wdc_cfg.label       = std::string{label};
        wdc_cfg.reliability = reliability;
        wdc_cfg.ordered     = (reliability != plugins::DataChannelReliability::kUnreliable);
        wdc_cfg.priority    = config_.datachannel_priority;
        wdc_cfg.framing     = config_.datachannel_framing;
        // WP-D4: the partial-reliability budgets are per message and
        // reach the backend, so the ARQ can enforce them.
        wdc_cfg.max_retransmits =
            config_.datachannel_max_retransmits;
        wdc_cfg.max_packet_lifetime_ms =
            config_.datachannel_max_packet_lifetime_ms;
        proxy->open(wdc_cfg);
        proxy->open();
        return proxy;
    }

    const plugins::IDataChannelFactory* f =
        pal::resolve_datachannel(resolved_dc_factory_id_);
    if (!f) {
        if (on_error_) on_error_(core::kEngineInternal,
            std::string("create_data_channel(): factory '")
                .append(resolved_dc_factory_id_)
                .append("' not registered"));
        return nullptr;
    }

    std::unique_ptr<plugins::IDataChannel> ch{f->create()};
    if (!ch) {
        if (on_error_) on_error_(core::kEngineInternal,
            "create_data_channel(): factory->create() returned nullptr");
        return nullptr;
    }

    // The engine keeps the backend alive (the statistics need its
    // outbound counters even after the host drops its handle) and
    // hands the host a proxy that counts the inbound traffic.
    // See `engine_datachannel_state.hpp`.
    std::shared_ptr<plugins::IDataChannel> backend{std::move(ch)};
    const std::uint32_t cid = next_data_channel_id_++;
    const std::string lbl{label};
    auto shared = impl_->data_channels.add(cid, lbl, reliability, backend);
    auto cbs = std::make_shared<detail::DataChannelCallbacks>();

    // Install callbacks BEFORE open() — the IDataChannel contract
    // requires set_on_message / set_on_state to be called before open()
    // (per `plugins/datachannel.hpp` §"Thread safety").  The proxy
    // installs them on the backend; the host's own callbacks go into
    // the shared block the proxy forwards to.
    cbs->on_message = on_data_message_;
    cbs->on_state   = on_data_state_;
    // WP-C1: the identity-carrying callback (the engine assigns the id).
    if (on_data_message_ex_) {
        cbs->on_message_ex =
            [this, cid, lbl](const plugins::DataChannelMessage& m) noexcept {
                if (!on_data_message_ex_) return;
                plugins::DataChannelMessage out = m;
                out.channel_id = cid;
                if (out.label.empty()) out.label = lbl;
                on_data_message_ex_(out);
            };
    }
    std::unique_ptr<plugins::IDataChannel> proxy =
        std::make_unique<detail::DataChannelProxy>(backend, shared, cbs);

    // Default config: matches WebRTC's ordered+reliable channel
    // (the common case for control / telemetry).  The reliability-aware
    // overload (v0.15.0, WebTransport product route) forwards here.
    plugins::DataChannelConfig cfg;
    cfg.label       = std::string{label};
    cfg.reliability = reliability;
    cfg.ordered     = (reliability != plugins::DataChannelReliability::kUnreliable);
    cfg.priority    = config_.datachannel_priority;
    cfg.framing     = config_.datachannel_framing;
    // WP-D4: the partial-reliability budgets are per message and reach
    // the backend, so the ARQ can enforce them.
    cfg.max_retransmits = config_.datachannel_max_retransmits;
    cfg.max_packet_lifetime_ms = config_.datachannel_max_packet_lifetime_ms;
    proxy->open(cfg);

    // Arm the channel itself (the IPlugin lifecycle).  The
    // IDataChannel::open(cfg) call above only stores the config; the
    // IPlugin::open() call is what wires the backend (the SCTP recv
    // callbacks for the "sctp" backend) and marks the channel usable.
    //
    // Without this call the returned channel is a shell: every
    // send() fails with kErrNotReady and no inbound message ever
    // reaches the application.  A non-kOk return means the channel is
    // unusable — report it and hand back nullptr instead of a channel
    // that silently fails.
    const plugins::Status ch_rc = proxy->open();
    if (ch_rc != plugins::kOk) {
        if (on_error_) on_error_(core::kEngineInternal,
            "create_data_channel(): IDataChannel::open() failed for the "
            "resolved backend '" + std::string{resolved_dc_factory_id_} + "'");
        return nullptr;
    }

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
    impl_->session_state.datachannel_transport_cookie = impl_->dtls;

    return proxy;
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
    // v1.4 N7: an explicit server list wins over the credentials provider
    // (ADR-032 §3) — the provider is never consulted once a static server
    // has been added.
    impl_->session_state.has_static_turn_server = true;
    ice_t_->add_turn_server(host, port, username, password);
    return 0;
}

int NimRTCEngine::add_turn_server(std::string_view host, std::uint16_t port,
                                  std::string_view username,
                                  std::string_view password,
                                  plugins::TurnTransport transport) noexcept {
    if (!ice_t_) {
        core::log::Logger::instance().error(
            "add_turn_server: ICE transport not yet created — call pre_open() first");
        return -1;
    }
    impl_->session_state.has_static_turn_server = true;
    ice_t_->add_turn_server_with_transport(host, port, username, password, transport);
    return 0;
}


// ---------------------------------------------------------------------------
// TURN credentials — the provider seam (v1.4 N7, ADR-032)
// ---------------------------------------------------------------------------

void NimRTCEngine::refresh_turn_credentials() noexcept {
    if (impl_ == nullptr) return;
    // No provider configured: the host uses the static `add_turn_server()`
    // path and the engine does nothing (spec §四 14 / §四 15).
    if (config_.turn_credentials == nullptr) return;
    // Static TURN servers win over the provider: an explicit list is the
    // host's own configuration and is never overridden (spec §二 7).
    if (impl_->session_state.has_static_turn_server) return;

    TurnCredentials out;
    auto& tc = impl_->session_state.turn_credentials;
    const std::int64_t now_us = detail::steady_now_us();
    if (!tc.refresh(now_us, config_.turn_credentials,
                    config_.turn_credentials_ctx, &out)) {
        // Pull failed (provider false / empty host): the engine configures
        // no TURN and keeps going direct (spec §二 5 / §二 10).
        return;
    }
    // Apply only when the pulled credentials differ from the last-applied
    // ones — the ICE module re-reads its config when the agent is rebuilt
    // after a restart, so the same credentials must not be re-added (spec
    // §二 9).
    if (ice_t_ != nullptr && tc.credentials_changed()) {
        // v1-turn-tls-trust (ADR-037): the trust policy rides on the
        // credential set — "换中继就换信任锚" — through the additive 7-arg
        // plugin overload.  The ICE layer reads the knobs at agent
        // creation (`create_agent()`), which happens later at open().
        ice_t_->add_turn_server_with_transport(
            out.host, out.port, out.username, out.password, out.transport,
            out.turn_tls_verify_peer, out.turn_tls_ca_file);
        tc.note_applied();

        // Fail loudly, not silently (spec §四 13-15): a trust anchor the
        // host pointed at is missing or not a PEM bundle — the host must
        // see it, not discover it later as a mysteriously failing relay.
        // A broken anchor makes the verification fact "failed" and fires
        // `on_error_` naming the file and the problem.  The check is
        // TURN/TLS-only — for a UDP/TCP relay a ca_file is meaningless and
        // is ignored, exactly like `verify_peer == false` (spec §设计 一).
        const auto verdict = detail::check_turn_tls_ca_file(out.turn_tls_ca_file);
        if (out.turn_tls_verify_peer &&
            out.transport == plugins::TurnTransport::Tls &&
            verdict != detail::TurnTlsCaFileVerdict::kOk) {
            tc.note_turn_tls_anchor_failed();
            if (on_error_) {
                on_error_(core::kEngineInvalidParam,
                          detail::turn_tls_ca_file_error(out.turn_tls_ca_file,
                                                         verdict));
            }
        }
    }
}


std::string NimRTCEngine::turn_tls_peer_verification() const noexcept {
    if (impl_ == nullptr) return {};
    // The fact is computed from the credentials most recently applied
    // through the provider seam (see TurnCredentialsState); a host that
    // configured a static `add_turn_server()` list carries no trust policy
    // on that path, so the answer stays "" (engine.hpp documents this).
    return std::string(impl_->session_state.turn_credentials
                           .turn_tls_peer_verification());
}


// ---------------------------------------------------------------------------
// ICE restart (RFC 8445 §9.1.1.1)
// ---------------------------------------------------------------------------

bool NimRTCEngine::restart_ice() noexcept {
    if (!ice_t_) {
        core::log::Logger::instance().error(
            "restart_ice: ICE transport not yet created — call pre_open() first");
        return false;
    }
    // v1.4 N3: the manual restart shares the engine's reconnection budget
    // (ADR-031 §2.3).  A blocked manual restart returns false without
    // touching the transport.
    const std::int64_t now_us = detail::steady_now_us();
    auto& rec = impl_->session_state.ice_recovery;
    // v1.4 N8: record the pair before the restart so the report can say
    // "from where to where" (also when the restart is blocked — the
    // terminal report still carries the last pair, spec §七).
    impl_->session_state.path_change.note_before(current_pair());
    if (!rec.note_restart_attempted(now_us, "manual-blocked")) {
        report_ice_recovery_terminal("manual", "manual-blocked");
        return false;
    }
    const auto rc = ice_t_->restart();
    if (rc != plugins::kOk) {
        // The attempt already consumed its budget slot (spec §方案 语义 3).
        rec.note_restart_failed(now_us);
        core::log::Logger::instance().error(
            std::string{"restart_ice: ICE restart failed, status="} +
            plugins::status_string(rc));
        return false;
    }
    core::log::Logger::instance().info(
        "restart_ice: ICE restart done, new local ufrag=" + ice_t_->local_ufrag());
    fire_ice_recovery_report("manual", false, "");
    return true;
}

std::uint32_t NimRTCEngine::ice_restart_count() const noexcept {
    return ice_t_ ? ice_t_->restart_count() : 0u;
}

void NimRTCEngine::set_on_ice_restart(plugins::OnIceRestart cb) noexcept {
    on_ice_restart_ = std::move(cb);
}

void NimRTCEngine::set_on_ice_recovery(EngineIceRecoveryCallback cb) noexcept {
    on_ice_recovery_ = std::move(cb);
}

void NimRTCEngine::set_on_peer_restart(EnginePeerRestartCallback cb) noexcept {
    on_peer_restart_ = std::move(cb);
}

void NimRTCEngine::set_on_call_state(EngineCallStateCallback cb) noexcept {
    on_call_state_ = std::move(cb);
}

void NimRTCEngine::reset_ice_recovery_budget() noexcept {
    if (impl_ != nullptr) {
        // Clears the count and the terminal flag only — the sliding
        // window's origin is deliberately kept (ADR-031 §2.3), so the
        // host's "retry" stays bounded by the same 5-per-10-minutes rule.
        impl_->session_state.ice_recovery.reset_budget();
        impl_->session_state.ice_recovery_terminal_reported = false;
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

DevicePolicyInfo NimRTCEngine::default_device_policy() noexcept {
    const detail::DevicePolicy p = detail::default_device_policy();
    return {.screen_capture_backend = p.screen_capture_backend,
            .camera_capture_backend = p.camera_capture_backend,
            .audio_capture_backend = p.audio_capture_backend,
            .audio_playback_backend = p.audio_playback_backend};
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
    return impl_->security.srtp_installed;
}


uint32_t NimRTCEngine::last_open_rc() const noexcept {
    return impl_->session_state.last_open_rc;
}

}  // namespace nimrtc::engine
