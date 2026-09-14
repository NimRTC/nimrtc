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

add_library(nimrtc_engine STATIC
    src/engine.cpp
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
    PRIVATE
        nimrtc::rtp            # RTP parser/builder (P1) — Impl in engine.cpp
        nimrtc::sdp            # SDP parser/munger (P1)
        nimrtc::jb             # JitterBuffer (P1)
        nimrtc::audio3a        # Audio 3A Null impl (P1)
        nimrtc::ice            # ICE transport (P1)
        nimrtc::dtls           # DTLS handshake (P1.1)
        nimrtc::srtp           # SRTP protect/unprotect (P1.1)
        nimrtc::bwe            # AIMD bandwidth estimator — resolved via
                               # PluginRegistry at open() and injected into ICE
        nimrtc::sched          # Strict-priority 5-queue scheduler — resolved via
                               # PluginRegistry at open(); tick() drains through ICE
        nimrtc_vendor_libjuice # ICE vendor (P1)
)

if(NIMRTC_MODULE_OPUS)
    target_link_libraries(nimrtc_engine PRIVATE nimrtc::opus)
    target_compile_definitions(nimrtc_engine PUBLIC NIMRTC_HAS_OPUS=1)
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
