# modules/CMakeLists.txt
# ----------------------------------------------------------------------------
# Container directory: each module is its own subdirectory and may be
# enabled/disabled independently. Modules are isolated: they declare their
# own dependencies via target_link_libraries, and may NOT depend on each
# other (except where a documented cross-module header is required, e.g.
# `src/modules/jb` may include `src/modules/rtp/include/nimrtc/rtp/packet.hpp`).
#
# All modules link nimrtc::plugins (header-only) so they can:
#   - implement plugin interfaces (ITransport, IRTP, etc.)
#   - register themselves with PluginRegistry
#
# NOTE: NIMRTC_MODULE_* cache variables are *declared* at the TOP LEVEL
# (CMakeLists.txt) — not here — so that src/core/CMakeLists.txt, which
# runs earlier in the dependency order, can read them and propagate
# NIMRTC_HAS_*=1 into pal_default_registrars.cpp via nimrtc_core_objects.
# This file only consumes the cache values via the if(add_subdirectory)
# guards below.

# -----------------------------------------------------------------------------
# Module targets — order is irrelevant; each is self-contained.
# -----------------------------------------------------------------------------
if(NIMRTC_MODULE_RTP)
    add_subdirectory(rtp)
endif()

if(NIMRTC_MODULE_SDP)
    add_subdirectory(sdp)
endif()

# Shared adaptive JB strategy (v1.3 Q2) — consumed by jb and video_jb.
# NOTE: jb_strategy must come before jb: nimrtc_jb links
# nimrtc::jb_strategy, so the target has to exist first.
if(NIMRTC_MODULE_JB_STRATEGY)
    add_subdirectory(jb_strategy)
endif()

if(NIMRTC_MODULE_JB)
    if(NOT NIMRTC_MODULE_JB_STRATEGY)
        message(FATAL_ERROR
            "NIMRTC_MODULE_JB requires NIMRTC_MODULE_JB_STRATEGY "
            "(shared RFC 3550 jitter estimator)")
    endif()
    add_subdirectory(jb)
endif()

if(NIMRTC_MODULE_BWE)
    add_subdirectory(bwe)
endif()

# v1.2.0 M3 (P1): the token-bucket pacer.  It must come before sched:
# nimrtc_sched links nimrtc::pacing, so the target has to exist first.
# This is the documented cross-module dependency pattern (same shape as
# modules/jb using modules/jb_strategy).
if(NIMRTC_MODULE_PACING)
    add_subdirectory(pacing)
endif()

if(NIMRTC_MODULE_SCHED)
    if(NOT NIMRTC_MODULE_PACING)
        message(FATAL_ERROR
            "NIMRTC_MODULE_SCHED requires NIMRTC_MODULE_PACING "
            "(the token-bucket pacer that shapes the outbound traffic)")
    endif()
    add_subdirectory(sched)
endif()

# v1.2.0 M1 (F2): the receiver-side network-feedback collector.  It links
# nimrtc::rtp for the RTCP wire formats — the documented cross-module
# dependency pattern (same shape as modules/jb using modules/rtp).
if(NIMRTC_MODULE_NET_FEEDBACK)
    add_subdirectory(net_feedback)
endif()

if(NIMRTC_MODULE_AUDIO3A)
    add_subdirectory(audio3a)
endif()

if(NIMRTC_MODULE_ICE)
    add_subdirectory(ice)
endif()

if(NIMRTC_MODULE_SRTP)
    add_subdirectory(srtp)
endif()

if(NIMRTC_MODULE_DTLS)
    add_subdirectory(dtls)
endif()

if(NIMRTC_MODULE_OPUS)
    add_subdirectory(opus)
endif()

# v1.2.0 (audio pipeline): the audio receive pipeline. Self-contained
# module -- it depends on nimrtc::core and nimrtc::plugins only; the
# jitter buffer and the decoder are injected through the plugin seams.
if(NIMRTC_MODULE_AUDIO_RX)
    add_subdirectory(audio_rx)
endif()

if(NIMRTC_MODULE_SESSION)
    add_subdirectory(session)
endif()

# v1.2.0 (session capability plan D1 / D2): the RFC 4733 telephone-event
# codec.  Self-contained -- it depends on nimrtc::core and nimrtc::plugins
# only.
if(NIMRTC_MODULE_DTMF)
    add_subdirectory(dtmf)
endif()

if(NIMRTC_MODULE_ASSEMBLY)
    add_subdirectory(assembly)
endif()

if(NIMRTC_MODULE_DATACHANNEL)
    add_subdirectory(datachannel)
endif()

# -----------------------------------------------------------------------------
# Video modules — must come after core, before any video consumer.
# -----------------------------------------------------------------------------
if(NIMRTC_MODULE_VIDEO_FRAME)
    add_subdirectory(video_frame)
endif()

if(NIMRTC_MODULE_VIDEO_PAYLOAD)
    add_subdirectory(video_payload)
endif()

if(NIMRTC_MODULE_VIDEO_JB)
    if(NOT NIMRTC_MODULE_JB_STRATEGY)
        message(FATAL_ERROR
            "NIMRTC_MODULE_VIDEO_JB requires NIMRTC_MODULE_JB_STRATEGY "
            "(shared RFC 3550 jitter estimator)")
    endif()
    add_subdirectory(video_jb)
endif()

# v1.3 (video plan C4 / C8): the shared NVIDIA codec module.  It owns the
# NVENC / NVDEC driver integration and exposes it through
# codec-parameterised entry points, so the H.264, H.265 and AV1 codec
# modules share one copy of the driver code instead of triplicating it.
# NOTE: nvcodec must come before h264 — nimrtc_h264 links nimrtc::nvcodec,
# so the target has to exist first.  This is the documented cross-module
# dependency pattern (same shape as modules/jb using modules/jb_strategy).
if(NIMRTC_MODULE_NVCODEC)
    add_subdirectory(nvcodec)
endif()

if(NIMRTC_MODULE_H264)
    add_subdirectory(h264)
endif()

# v1.3 (video plan C4 / C8): the H.265 codec module.  It links
# nimrtc::nvcodec — the shared lower-level module that owns the NVIDIA
# driver integration (NVENC / NVDEC) and exposes it through
# codec-parameterised entry points.  This is the documented
# cross-module dependency pattern (same shape as modules/jb using
# modules/jb_strategy).
if(NIMRTC_MODULE_H265 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/h265/CMakeLists.txt")
    if(NOT NIMRTC_MODULE_NVCODEC)
        message(FATAL_ERROR
            "NIMRTC_MODULE_H265 requires NIMRTC_MODULE_NVCODEC "
            "(the shared NVIDIA NVENC / NVDEC backend module)")
    endif()
    add_subdirectory(h265)
endif()

# v1.3 (video plan C6 / C8): the AV1 codec module.  Same cross-module
# dependency on nimrtc::nvcodec as the H.265 module above.
if(NIMRTC_MODULE_AV1 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/av1/CMakeLists.txt")
    if(NOT NIMRTC_MODULE_NVCODEC)
        message(FATAL_ERROR
            "NIMRTC_MODULE_AV1 requires NIMRTC_MODULE_NVCODEC "
            "(the shared NVIDIA NVENC / NVDEC backend module)")
    endif()
    add_subdirectory(av1)
endif()

# -----------------------------------------------------------------------------
# Video R2 modules — must each be self-contained, no cross-module deps
# unless explicitly listed in the module's own CMakeLists.txt.
# Each add_subdirectory is guarded by EXISTS so a not-yet-implemented
# module directory doesn't break the build; flip the option to ON once
# the module gains its CMakeLists.txt and sources.
# -----------------------------------------------------------------------------
if(NIMRTC_MODULE_VIDEO_SDP_ATTRIBUTES)
    add_subdirectory(video_sdp_attributes)
endif()

if(NIMRTC_MODULE_VIDEO_SOURCE)
    add_subdirectory(video_source)
endif()

if(NIMRTC_MODULE_VIDEO_STATS
        AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/video_stats/CMakeLists.txt")
    add_subdirectory(video_stats)
endif()

if(NIMRTC_MODULE_VIDEO_RTCP_FEEDBACK
        AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/video_rtcp_feedback/CMakeLists.txt")
    add_subdirectory(video_rtcp_feedback)
endif()

if(NIMRTC_MODULE_VIDEO_PIPELINE
        AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/video_pipeline/CMakeLists.txt")
    if(NOT NIMRTC_MODULE_VIDEO_RTCP_FEEDBACK)
        message(FATAL_ERROR
            "NIMRTC_MODULE_VIDEO_PIPELINE requires NIMRTC_MODULE_VIDEO_RTCP_FEEDBACK "
            "(the v1.3 Q3 feedback bridge encodes NACK/PLI for the video pipeline)")
    endif()
    add_subdirectory(video_pipeline)
endif()

if(NIMRTC_MODULE_VIDEO_SINK)
    add_subdirectory(video_sink)
endif()

# CTest discovery: enable_testing() at this level so that each module's
# tests subdirectory (added via add_subdirectory(tests) in the module's
# own CMakeLists.txt) appears in the src/modules/CTestTestfile.cmake.
# The root-level subdirs("src/modules") entry then recurses into it.
if(NIMRTC_BUILD_TESTS)
    enable_testing()
    if(NOT DEFINED OPUS_UNIT_TESTS)
        set(OPUS_UNIT_TESTS 1)
    endif()
endif()

# Future modules:
# if(NIMRTC_MODULE_DTLS) add_subdirectory(dtls) endif()