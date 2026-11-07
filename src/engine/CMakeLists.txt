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
if(NIMRTC_MODULE_VIDEO_SINK)
    target_link_libraries(nimrtc_engine PRIVATE nimrtc::video_sink)
endif()
if(NIMRTC_MODULE_VIDEO_PIPELINE)
    target_link_libraries(nimrtc_engine PRIVATE nimrtc::video_pipeline)
endif()
if(NIMRTC_MODULE_H264)
    target_link_libraries(nimrtc_engine PRIVATE nimrtc::h264)
endif()

target_include_directories(nimrtc_engine
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

add_library(nimrtc::engine ALIAS nimrtc_engine)
