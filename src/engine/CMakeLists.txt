# engine/CMakeLists.txt
# =============================================================================
# NimRTCEngine — the top-level composition layer.
#
# This is the primary public target for application code. It links all modules
# together and provides the single entry point (NimRTCEngine) documented in
# the user-facing API.
#
# Consumers only need:
#   target_link_libraries(my_app PRIVATE nimrtc::engine)
# =============================================================================

# R1 (v1.1): engine.cpp was a single ~4.5 kLOC TU; it is now five
# responsibility-grouped TUs sharing private engine_impl.hpp.
# Public header untouched (L4); behaviour unchanged.
add_library(nimrtc_engine STATIC
    src/engine.cpp
    src/engine_transport.cpp
    src/engine_media.cpp
    src/engine_signaling.cpp
    src/engine_timeline.cpp
    # v1.2.0 (audio pipeline): the RTCP receiver report for the inbound
    # audio stream (WP-8, spec B-12) and the cross-end capture-timestamp
    # lookup for the audio frames (WP-9, spec B-17).  Both are private
    # engine TUs — they add no public symbol beyond what engine.hpp
    # already declares.
    src/engine_rtcp.cpp
    src/engine_capture.cpp
    src/engine_audio_frames.cpp
    # v1.2.0 (session capability plan N1–N5 / T1–T4): the session /
    # track / transceiver surface.  The engine drives a pluggable
    # `plugins::ISession` model through this TU and turns it into the
    # SDP media list.
    src/engine_session.cpp
    # v1.2.0 (session capability plan D1–D4): the RFC 4733 DTMF plane.
    # The telephone-event codec is resolved through the registry's DTMF
    # slot here, and the send / receive paths of the in-band event
    # stream live in this TU.
    src/engine_dtmf.cpp
    # v1.2.0 (session capability plan X2–X5): the RFC 3611 RTCP XR
    # report and the RFC 5104 TMMBR / TMMBN plane.  This is the one
    # engine TU that links the XR / TMMBR parsers and builders (the
    # public header stays free of concrete module includes —
    # Layout Invariant 4).
    src/engine_rtcp_xr.cpp
    # v1.3 (remote-desktop plan A10): the capture → engine pump.  The
    # engine pops frames from the capture device's bounded queue and
    # drives them through the encoder ON THE TICK THREAD, so a capture
    # plug-in plus an encoder produce an outbound RTP stream without the
    # host ever calling send_video().  RD-6: no new thread — the
    # capture device owns its own, the engine only polls it.
    src/engine_video_pump.cpp
    # Remote-desktop plan D1: the audio device → outbound pump plus the
    # audio device open/close helpers.  The pump polls the audio source
    # device's bounded queue on the tick thread and feeds the same
    # outbound chain the host drives with send_audio() — without the
    # host.  RD-6: no new thread.
    src/engine_audio_device_pump.cpp
    # Remote-desktop plan G1 / G2 / G6: the remote-desktop control
    # channel.  This TU owns the channel's engine-side plumbing — the
    # registry resolution (the seam), the transport selection (the DTLS
    # app-data seam vs. the control stack's raw-UDP path), the outbound
    # flush and the inbound delivery.  The channel itself is a private
    # collaborator (`engine_control_seam.hpp`).
    src/engine_control.cpp
    src/engine_qoe.cpp
    src/engine_qoe_seam.cpp
    src/engine_control_seam.cpp
    # Remote-desktop plan E5 / E6 / E7 / E9: the receive-side render
    # state — the render clock (E5), the input echo (E6), the
    # presentation (E7) and the displayed-frame record (E9).
    src/engine_render_state.cpp
    # Remote-desktop plan H1 / H2 / H4: the multi-stream video plane.
    # The default stream keeps the engine's own plugin members; every
    # extra stream owns its own codec / sender / receiver / sink /
    # source, resolved through the registry by id (H2).  The per-stream
    # statistics are filled here (H4).
    src/engine_video_streams.cpp
    # Architecture remediation plan §4.3 (D1) + ADR-025 Decision 4: the
    # RTP packet builder.  This TU is the ONE place the engine names a
    # concrete `rtp::` type for *outbound* packets, so the engine's
    # exemption from Layout Invariant 4 has a visible boundary.
    src/rtp_packetizer.cpp
    # v1.2.0 (plan Track S / S5): the transport-plane statistics
    # projection -- the ICE agent's state and nominated candidate pair,
    # the DTLS session's state and role, the SRTP counters and the
    # round-trip time the feedback path measured.  It lives in its own
    # TU so the transport surface has ONE visible owner, and so the
    # engine's other TUs do not grow the projection's vocabulary.
    src/engine_transport_stats.cpp
    # v1.3 plan Q4 / video plan T2: the receive-side feedback bound.
    # The NACK cap, the global key-frame rate limit and the pending
    # report's age live in one private collaborator, so the engine's
    # receive-side feedback has ONE visible owner.
    src/engine_feedback_guard.cpp
    # v1-device-policy D3: the per-platform default device policy.  The
    # platform backend table that used to live in the remote-desktop
    # example now lives here, in the engine core (private TU — the table
    # is a list of plugin ids, which the public engine header must not
    # grow).
    src/engine_device_policy.cpp
    # v1-video-render-engine D2: the per-platform default render policy.
    # The "auto" sink-backend table lives here, private to the engine (the
    # table is a list of plugin ids, which the public engine header must
    # not grow).  It is the render-side sibling of engine_device_policy.cpp.
    src/engine_render_policy.cpp
    # v1.4 (v3-n4-d6, D6): the connectivity profile.  A private TU — the
    # profile's policy defaults are engine-internal knowledge that must
    # not leak into the public engine header (Layout Invariant 4).
    src/engine_connectivity_profile.cpp
    # v1.4 (v1-ice-recovery, N3): the ICE-recovery state machine.  A
    # private TU — the engine's reconnection policy (grace window, backoff,
    # restart budget, terminal state) is engine-internal knowledge driven
    # by the tick thread through `plugins::IceState` / `IICETransport`.
    src/engine_ice_recovery.cpp
    # v1.4 (v1-turn-credentials, N7): the TURN-credentials provider seam.
    # A private TU — the pull bookkeeping (consecutive failures, the
    # rejection latch, change detection) is engine-internal knowledge that
    # must not leak into the public engine header (Layout Invariant 4).
    src/engine_turn_credentials.cpp
    # v1.4 (v1-path-change-report, N8): the path-change ledger.  A private
    # TU — the "which pair was the previous one" bookkeeping and the pure
    # reason classification live here, so the three report points (N3
    # restart, manual restart, peer restart) share one judgment instead of
    # each carrying its own.
    src/engine_path_change.cpp

    # Remote-desktop plan A10: the capture -> engine pump.  The engine
    # pops the capture device's bounded queue on its tick thread and
    # drives the encoder with it, so the host does not have to pull
    # frames and call send_video() by hand.  The pump lives in
    # `engine_video_pump.cpp` (above) together with its zero-copy
    # counterpart; the engine's public header stays free of concrete
    # module includes -- Layout Invariant 4.
)

nimrtc_apply_options(nimrtc_engine)
# All concrete module deps are PRIVATE — engine.hpp no longer includes
# any concrete module header (Layout Invariant 4 + ADR-001).  Consumers
# linking only nimrtc::engine still get the full composition via
# `core::register_all_default_plugins()`, which is invoked internally
# by NimRTCEngine::open().
target_link_libraries(nimrtc_engine
    PUBLIC
        nimrtc::core           # base types + PluginRegistry (PUBLIC so callers
                               # can call register_all_default_plugins() too)
        # PAL Slice 2: explicitly link the OBJECT library that holds
        # `kDefaultRegistrars[]` and `kDefaultRegistrarCount`.  `nimrtc::core`
        # is INTERFACE-only, so it propagates only include paths; consumers
        # must link `nimrtc_core_objects` directly to pull in the symbols.
        nimrtc_core_objects
    PRIVATE
        nimrtc::rtp            # RTP parser/builder (P1) — Impl in engine.cpp
        nimrtc::sdp            # SDP parser/munger (P1)
        # v1.3 plan Q3 (video plan T1): the RFC 4588 retransmission
        # *payload format*.  The engine needs it for two things: the
        # `apt=` SDP parameter (which binds the retransmission payload
        # type to the one it protects) and the `rtx/90000` encoding
        # name.  It is the same kind of dependency as `rtp` and `sdp` —
        # a wire contract that is not replaceable (ADR-025 / the L4
        # exception in AGENTS.md), not a pluggable implementation.
        nimrtc::rtx
        nimrtc::jb             # JitterBuffer (P1) — the *plugin* that the
                               # audio receive pipeline injects through the
                               # IJB seam (spec B-3); the engine itself no
                               # longer owns a jitter buffer (WP-7).
        nimrtc::audio3a        # Audio 3A Null impl (P1)
        nimrtc::ice            # ICE transport (P1)
        nimrtc::dtls           # DTLS handshake (P1.1)
        nimrtc::srtp           # SRTP protect/unprotect (P1.1)
        nimrtc::bwe            # AIMD bandwidth estimator — resolved via
                               # PluginRegistry at open() and injected into ICE
        nimrtc::sched          # Strict-priority 5-queue scheduler — resolved via
                               # PluginRegistry at open(); tick() drains through ICE
        # Slice 8 (v0.10.2): the four new typed registry hooks (DTLS
        # seam / SCTP / raw_udp / transport-stack) are populated by
        # these modules' register_default_plugins() entry points.
        # Linking them here guarantees the symbols in
        # `kDefaultRegistrars[]` resolve at link time, and the
        # Slice 8 registry lookups in init_modules_once() (DTLS in
        # particular) return non-null factory pointers.
        nimrtc::dtls_seam      # DTLS factory id="wolfssl" (Slice 4/8)
        nimrtc::sctp           # SCTP factory id="stub"   (Slice 5/8)
        nimrtc::raw_udp        # raw-UDP factory id="arq" (Slice 6/8)
        nimrtc::transport      # transport-stack factory id="default" (Slice 7/8)
                               # Also pulls in TransportSession + ITransportStackFactory
                               # (used by engine via get_transport_stack() lookups).
        # v0.14.0-beta.1 (ADR-014): WebTransport / QUIC seam factory.
        # `pal_default_registrars.cpp` references
        # `nimrtc::web_transport::register_default_plugins()` unconditionally,
        # so any consumer linking `nimrtc::engine` must have the
        # `nimrtc::web_transport` symbol resolved. Linking PRIVATE follows
        # the same Layout Invariant 5 / MSVC-linker-quirk pattern as the
        # other Slice 8 modules above — it guarantees the static-init
        # symbol in `web_transport_plugin.cpp` is included in
        # `nimrtc_engine.lib` and is therefore resolvable by every
        # downstream executable (tests/ examples/ chrome_interop_harness/).
        nimrtc::web_transport
        nimrtc_vendor_libjuice # ICE vendor (P1)
)

if(NIMRTC_MODULE_OPUS)
    target_link_libraries(nimrtc_engine PRIVATE nimrtc::opus)
    target_compile_definitions(nimrtc_engine PUBLIC NIMRTC_HAS_OPUS=1)
endif()

# v1.2.0 (audio pipeline, WP-7): the audio receive pipeline.
#
# The engine reaches the pipeline only through `plugins::IAudioReceiver`
# — it never includes this module's header (Layout Invariant 4).  The
# link dependency exists for two reasons:
#
#   1. the module's registrar entry point (in `kDefaultRegistrars[]`)
#      must resolve at link time, so the pipeline is registered when
#      `core::register_all_default_plugins()` runs;
#   2. the pipeline's own registration lives in the module's static
#      initialiser, which the MSVC linker would otherwise drop (the
#      Layout Invariant 5 / MSVC-linker-quirk pattern used by the
#      Slice 8 entries above).
#
# When the module is not built, the audio receive pipeline cannot be
# resolved and the engine reports `kAudioReceiverMissing` — there is
# deliberately no fallback receiver (spec B-16).
if(NIMRTC_MODULE_AUDIO_RX)
    target_link_libraries(nimrtc_engine PRIVATE nimrtc::audio_rx)
endif()

# v1.4 (remote-desktop plan D1): the audio *device* module.
#
# The engine reaches the devices only through `plugins::IAudioSource` /
# `plugins::IAudioSink` and the registry's audio-source / audio-sink
# slots — it never includes this module's header (Layout Invariant 4).
# The link dependency exists so the module is part of the engine's
# composition and its registrar symbol is resolvable by every consumer:
# `pal_default_registrars.cpp` carries the guarded
# `&nimrtc::audio_device::register_default_plugins` entry (the
# `kDefaultRegistrars[]` table), which materialises once the
# `NIMRTC_HAS_AUDIO_DEVICE` mirror lands in `src/core/CMakeLists.txt`.
# Until then hosts / tests register the backends explicitly by calling
# `nimrtc::audio_device::register_default_plugins()` (the documented
# "call the module-specific entry point directly" pattern, registry.hpp).
#
# When the module is not built, "auto" audio resolution degrades to "no
# device" (spec §五) and an explicit audio device id reports
# `kEngineInternal` — there is no audio-device fallback backend.
if(NIMRTC_MODULE_AUDIO_DEVICE)
    target_link_libraries(nimrtc_engine PRIVATE nimrtc::audio_device)
endif()

# Network-feedback collector seam (v1.2.0 M1, F2): the engine routes the
# receiver-side RTCP (RR / REMB / transport-cc) into the collector and
# hands the aggregated snapshot to the bandwidth estimator.  The engine
# reaches the collector only through `plugins::INetFeedback` — it never
# includes this module's header (Layout Invariant 4).  The link dependency
# exists so the module's registrar entry point (in `kDefaultRegistrars[]`)
# resolves at link time and its static initialiser is not stripped
# (same pattern as the audio_rx entry above).
#
# When the module is not built the engine simply has no collector: the
# bandwidth estimate stays at its configured initial value.
if(NIMRTC_MODULE_NET_FEEDBACK)
    target_link_libraries(nimrtc_engine PRIVATE nimrtc::net_feedback)
endif()

# Session-model seam (v1.2.0, session capability plan N1 / T1): the
# engine resolves the session model through the registry's session slot
# (`EngineConfig::session_name`) and drives it through
# `plugins::ISession` — it never includes this module's header
# (Layout Invariant 4).  The link dependency exists so the module's
# registrar entry point (in `kDefaultRegistrars[]`) resolves at link
# time and its static initialiser is not stripped (same pattern as the
# audio_rx / net_feedback entries above).
#
# When the module is not built the engine simply has no session model:
# the offer keeps the pre-v1.2.0 fixed media list and the track API
# reports "no session".
if(NIMRTC_MODULE_SESSION)
    target_link_libraries(nimrtc_engine PRIVATE nimrtc::session)
endif()

# DTMF seam (v1.2.0, session capability plan D1 / D2): the engine
# resolves the RFC 4733 codec through the registry's DTMF slot
# (`EngineConfig::dtmf_codec_name`) and drives it through
# `plugins::IDtmfCodec` — it never includes this module's header
# (Layout Invariant 4).  The link dependency exists so the module's
# registrar entry point (in `kDefaultRegistrars[]`) resolves at link
# time (same pattern as the session / net_feedback entries above).
#
# When the module is not built the engine simply has no DTMF codec:
# `dtmf_enabled` is then inert, and the pre-existing drop path for a
# telephone-event payload stays in place (decision D4).
if(NIMRTC_MODULE_DTMF)
    target_link_libraries(nimrtc_engine PRIVATE nimrtc::dtmf)
endif()

# Video plugin seam (R3-Batch): the engine resolves video_source / sink /
# receiver / sender from core::PluginRegistry, but does not link any
# concrete video_* module header. Consumers that want engine integration
# must ensure video_source::register_default_plugins() etc. are invoked
# before any engine open() — see nimrtc::core::register_all_default_plugins().
#
# The plugin adapters live in their respective modules; the engine pulls
# in the plugin interface headers (under nimrtc::plugins) via PUBLIC
# propagation of nimrtc::core (which already links nimrtc::plugins).
# The optional module dependencies below are marked PRIVATE because the
# engine itself never references concrete types — they're only required
# at link time so the static initialisers in `register_default_plugins()`
# are not stripped (Layout Invariant 5 + MSVC-linker quirk; see
# `video_source::register_default_plugins()` header for details).
if(NIMRTC_MODULE_VIDEO_SOURCE)
    target_link_libraries(nimrtc_engine PRIVATE nimrtc::video_source)
endif()
# Remote-desktop plan A1: the capture-device module.  Same linker-strip
# reason as the video_source entry above.
#
# Remote-desktop plan A10: the engine's capture pump lives in
# `engine_video_pump.cpp` and polls the capture device's bounded
# queue directly, through the `plugins::IVideoCapture` seam.  The
# device-to-frame-stream translation is the pump's own job, so the
# engine depends on the module only for the device it drives.
if(NIMRTC_MODULE_VIDEO_CAPTURE)
    target_link_libraries(nimrtc_engine PRIVATE nimrtc::video_capture)
endif()
if(NIMRTC_MODULE_VIDEO_SINK)
    target_link_libraries(nimrtc_engine PRIVATE nimrtc::video_sink)
endif()
# v1.3 Q6 (remote-desktop plan R3): the render-clock module.  The engine
# never includes its header -- the correction policy is reached through the
# `plugins::IVideoSyncController` seam (Layout Invariant 4 / ADR-025).
# The link exists so the module's registrar entry point (in
# `kDefaultRegistrars[]`) resolves at link time (same pattern as the
# rd_qoe entry above).
if(NIMRTC_MODULE_VIDEO_SYNC)
    target_link_libraries(nimrtc_engine PRIVATE nimrtc::video_sync)
endif()
if(NIMRTC_MODULE_VIDEO_PIPELINE)
    target_link_libraries(nimrtc_engine PRIVATE nimrtc::video_pipeline)
endif()

# Remote-desktop plan D1: the video rate controller module.  The engine
# resolves the controller through the registry's video-bitrate-controller
# slot and pushes its level into the encoder's parameter set (D4), but the
# *registration* itself lives in the module — the engine only names the
# plugin id.  The link is PRIVATE for the same linker-strip reason as the
# video_source / video_capture entries above.
if(NIMRTC_MODULE_VIDEO_BITRATE_CONTROL)
    target_link_libraries(nimrtc_engine PRIVATE nimrtc::video_bitrate_control)
endif()

# Remote-desktop plan Track G (G3 / G4 / G7): the remote-desktop input
# module.  The engine never includes its header (the event vocabulary
# belongs to the module; the engine's echo seam uses plain engine types
# — Layout Invariant 4 / ADR-025).  The link exists so the module's
# registrar entry point (in `kDefaultRegistrars[]`) resolves at link
# time (same pattern as the video_source / video_capture entries
# above).
if(NIMRTC_MODULE_RD_INPUT)
    target_link_libraries(nimrtc_engine PRIVATE nimrtc::rd_input)
endif()

# Remote-desktop plan Track J (J3 / J6): the remote-desktop QoE-metrics
# module.  The engine never includes its header — the metrics are reached
# through the `plugins::IQoeCollector` seam (Layout Invariant 4 /
# ADR-025).  The link exists so the module's registrar entry point (in
# `kDefaultRegistrars[]`) resolves at link time (same pattern as the
# rd_input entry above).
if(NIMRTC_MODULE_RD_QOE)
    target_link_libraries(nimrtc_engine PRIVATE nimrtc::rd_qoe)
endif()

# The codec-specific SDP attribute module (architecture remediation plan
# D2 / Phase 4.2).  The engine builds and parses the `a=fmtp:` body for
# each video codec family through this module, so the format parameters
# have exactly ONE authority — the module that also *parses* them.
#
# Before this dependency existed the engine carried a second, hand-written
# copy of the same table (`engine/src/video_codec_config.hpp`), which could
# disagree with the module's parser: a fix to one never reached the other.
#
# The engine still reaches the module through its *header* (not through a
# plugin seam) — the format parameters are not replaceable, exactly like
# RTP and SDP (see ADR-025 / the L4 exception in AGENTS.md).
if(NIMRTC_MODULE_VIDEO_SDP_ATTRIBUTES)
    target_link_libraries(nimrtc_engine PRIVATE nimrtc::video_sdp_attributes)
endif()

# The video-quality statistics module (architecture remediation plan D2 /
# Phase 4.2).  The engine drives the `IVideoStatsSink` from the receive
# pipeline (frame received → decoded → rendered), the jitter buffer (drops)
# and the feedback path (NACK / PLI), and exposes the resulting snapshot
# through `Engine::Stats`.
#
# Like the SDP-attributes module above, this is a *helper* rather than a
# replaceable plugin: the counters it keeps have no wire representation and
# no alternative implementation could disagree with them.  The engine
# reaches it through its header (the same L4 exception ADR-025 records for
# RTP / SDP).
if(NIMRTC_MODULE_VIDEO_STATS)
    target_link_libraries(nimrtc_engine PRIVATE nimrtc::video_stats)
    # The bridge (`src/engine/src/video_stats_bridge.hpp`) compiles the real
    # sink when this is defined, and compiles to no-ops when it is not.
    target_compile_definitions(nimrtc_engine PRIVATE NIMRTC_HAS_VIDEO_STATS=1)
endif()
if(NIMRTC_MODULE_H264)
    target_link_libraries(nimrtc_engine PRIVATE nimrtc::h264)
endif()
# v1.3 (video plan C4 / C8): the H.265 / AV1 codec modules.  The engine
# reaches them through the registry's video-codec slot (the same seam the
# H.264 module uses), so the link is what makes the *symbol* available.
if(NIMRTC_MODULE_H265)
    target_link_libraries(nimrtc_engine PRIVATE nimrtc::h265)
endif()
if(NIMRTC_MODULE_AV1)
    target_link_libraries(nimrtc_engine PRIVATE nimrtc::av1)
endif()
# Remote-desktop plan B11: the VP9 codec module.  Same seam as the
# H.264 / H.265 / AV1 modules above — the engine reaches it through the
# registry's video-codec slot, so the link is what makes the *symbol*
# available.
if(NIMRTC_MODULE_VP9)
    target_link_libraries(nimrtc_engine PRIVATE nimrtc::vp9)
endif()

# The video pixel storage (nimrtc::video_frame) — send_video() stages the
# caller's VideoSourceFrame into a VideoFrameBuffer so the codec can read
# the planes.  The engine references the concrete types here (it builds
# the buffer), so this is a real dependency, not a link-time-only one.
if(NIMRTC_MODULE_VIDEO_FRAME)
    target_link_libraries(nimrtc_engine PRIVATE nimrtc::video_frame)
endif()

target_include_directories(nimrtc_engine
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

add_library(nimrtc::engine ALIAS nimrtc_engine)

# v1-device-policy (D3 / D4): the engine's own deterministic tests — the
# device-policy resolution rules and the capture-reconnect backoff machine
# are engine-private, so their tests live inside the engine rather than in
# the top-level tests/ directory.
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
