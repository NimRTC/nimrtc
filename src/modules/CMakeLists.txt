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
# ----------------------------------------------------------------------------

# -----------------------------------------------------------------------------
# Module switch — disable a module to skip its subdirectory entirely.
# -----------------------------------------------------------------------------
option(NIMRTC_MODULE_RTP  "Build RTP module"  ON)
option(NIMRTC_MODULE_SDP  "Build SDP module"  ON)
option(NIMRTC_MODULE_JB   "Build jitter buffer module"  ON)
option(NIMRTC_MODULE_BWE  "Build bandwidth estimator module"  ON)
option(NIMRTC_MODULE_AUDIO3A  "Build audio 3A module"  ON)
option(NIMRTC_MODULE_ICE  "Build ICE module"  ON)
option(NIMRTC_MODULE_SRTP  "Build SRTP module"  ON)
option(NIMRTC_MODULE_DTLS  "Build DTLS module"  ON)
option(NIMRTC_MODULE_OPUS  "Build Opus codec module"  ON)
option(NIMRTC_MODULE_SCHED "Build unified sending scheduler module" ON)
option(NIMRTC_MODULE_DATACHANNEL  "Build DataChannel module"  ON)
option(NIMRTC_MODULE_SESSION "Build session module"  ON)
option(NIMRTC_MODULE_ASSEMBLY "Build assembly module (Profile/Builder)"  ON)

# Video modules (P1 minimal — standalone, not wired into the engine yet).
# See docs/zh/NimRTC-V2-技术文档.md §2.3 + §6 + §11.5/11.6.
option(NIMRTC_MODULE_VIDEO_FRAME   "Build video frame module (pixel formats, layouts, conversions)" ON)
option(NIMRTC_MODULE_VIDEO_PAYLOAD "Build video RTP payload module (H.264/VP8/VP9 packetizers)"  ON)
option(NIMRTC_MODULE_VIDEO_JB      "Build video-aware jitter buffer module (FU-A reassembly)"     ON)
option(NIMRTC_MODULE_H264          "Build H.264 module (bitstream + stub decoder + plugin adapter)" ON)
# R2 modules (added in this round):
option(NIMRTC_MODULE_VIDEO_SDP_ATTRIBUTES
       "Build video SDP attribute module (H.264/VP8/VP9 fmtp parsing)" ON)
option(NIMRTC_MODULE_VIDEO_SOURCE
       "Build video source module (memory frame source for testing/demo)" ON)
option(NIMRTC_MODULE_VIDEO_STATS
       "Build video stats module (per-stream statistics)" ON)
option(NIMRTC_MODULE_VIDEO_RTCP_FEEDBACK
       "Build video RTCP feedback module (PLI/FIR/SLI generation & parsing)" ON)
option(NIMRTC_MODULE_VIDEO_PIPELINE
       "Build video pipeline module (VideoReceiver/VideoSender glue)" ON)
option(NIMRTC_MODULE_VIDEO_SINK
       "Build video sink module (headless / pinned render sinks)" ON)

# -----------------------------------------------------------------------------
# Module targets — order is irrelevant; each is self-contained.
# -----------------------------------------------------------------------------
if(NIMRTC_MODULE_RTP)
    add_subdirectory(rtp)
endif()

if(NIMRTC_MODULE_SDP)
    add_subdirectory(sdp)
endif()

if(NIMRTC_MODULE_JB)
    add_subdirectory(jb)
endif()

if(NIMRTC_MODULE_BWE)
    add_subdirectory(bwe)
endif()

if(NIMRTC_MODULE_SCHED)
    add_subdirectory(sched)
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

if(NIMRTC_MODULE_SESSION)
    add_subdirectory(session)
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
    add_subdirectory(video_jb)
endif()

if(NIMRTC_MODULE_H264)
    add_subdirectory(h264)
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