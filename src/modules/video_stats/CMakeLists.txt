# modules/video_stats/CMakeLists.txt
# ----------------------------------------------------------------------------
# Per-stream video statistics — frame counters, bitrate EMA, freeze
# detection, QP accumulation, RTCP feedback accounting.
#
# Per ARCHITECTURE.md §9 + §11.6: this is the P1 video stat scaffolding
# that exposes a snapshot per session (RTT/loss/jitter/bitrate/BWE
# decision trail). The video-specific subset of "每路会话可独立拉取"
# (frames_received, frames_decoded, current_bitrate_bps, freeze_count,
# keyframe_interval_avg_ms, …) lives here; RTT/loss/jitter/BWE trail
# will be added by the transport/BWE modules in a later round.
#
# No codec dependency, no network I/O.  Pure C++ + std::atomic.
# ----------------------------------------------------------------------------
add_library(nimrtc_video_stats STATIC
    src/video_stats.cpp
    )

nimrtc_apply_options(nimrtc_video_stats)
target_link_libraries(nimrtc_video_stats
    PUBLIC
        nimrtc::core
    )

target_include_directories(nimrtc_video_stats
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

target_compile_definitions(nimrtc_video_stats PUBLIC NIMRTC_HAS_VIDEO_STATS=1)

add_library(nimrtc::video_stats ALIAS nimrtc_video_stats)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()