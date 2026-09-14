# modules/video_rtcp_feedback/CMakeLists.txt
# ----------------------------------------------------------------------------
# Video RTCP feedback messages: PLI / FIR / SLI / NACK (RFC 4585 / 5104).
#
# Per ARCHITECTURE.md §6 (Quality feedback) + §11.6 (self-written RTCP):
#   - PLI: Picture Loss Indication, FMT=1 / PT=PSFB
#   - FIR: Full Intra Request,    FMT=4 / PT=PSFB
#   - SLI: Slice Loss Indication, FMT=2 / PT=PSFB (parse-only round-trip)
#   - NACK: Generic NACK,         FMT=1 / PT=RTPFB
#
# Also exposes a `FirCoalescer` (RFC 5104 §4.3.1.1) that rate-limits FIR
# requests to ~4 Hz per media SSRC.
#
# Dependencies:
#   - nimrtc::core   (ByteSpan / MutableByteSpan / Result / log)
#   - nimrtc::rtp    (RtcpType / RtcpFbKind enums — used by callers, not by
#                     this module's implementation)
#
# No vendor libraries; pure C++20.
# ----------------------------------------------------------------------------
add_library(nimrtc_video_rtcp_feedback STATIC
    src/video_rtcp_feedback.cpp
    )

nimrtc_apply_options(nimrtc_video_rtcp_feedback)
target_link_libraries(nimrtc_video_rtcp_feedback
    PUBLIC
        nimrtc::core
        nimrtc::rtp
    )

target_include_directories(nimrtc_video_rtcp_feedback
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

target_compile_definitions(nimrtc_video_rtcp_feedback
    PUBLIC NIMRTC_HAS_VIDEO_RTCP_FEEDBACK=1)

add_library(nimrtc::video_rtcp_feedback ALIAS nimrtc_video_rtcp_feedback)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
