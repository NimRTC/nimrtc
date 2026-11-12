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
if(NIMRTC_MODULE_VIDEO_CAPTURE)
    target_link_libraries(nimrtc_engine PRIVATE nimrtc::video_capture)
endif()
if(NIMRTC_MODULE_VIDEO_SINK)
    target_link_libraries(nimrtc_engine PRIVATE nimrtc::video_sink)
endif()
if(NIMRTC_MODULE_VIDEO_PIPELINE)
    target_link_libraries(nimrtc_engine PRIVATE nimrtc::video_pipeline)
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
