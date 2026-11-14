# modules/video_pipeline/CMakeLists.txt
# ----------------------------------------------------------------------------
# Video pipeline glue layer (VideoReceiver + VideoSender reference impls).
#
# Sits on top of:
#   - core          : ByteSpan / Result / Logger
#   - video_frame   : EncodedVideoFrame, CodecKind
#   - video_payload : H.264/VP8/VP9 depacketizers + packetizers
#   - video_jb      : IVideoJitterBuffer (frame-level reorder/timeout)
#   - rtp           : PacketView (parsed RTP header)
#   - video_rtcp_feedback : NACK/PLI/FIR wire format (v1.1 Q3 feedback bridge)
#
# No vendor libraries; pure C++.
# ----------------------------------------------------------------------------
add_library(nimrtc_video_pipeline STATIC
    src/video_pipeline.cpp
    src/video_pipeline_plugin.cpp     # adapter wrapping concrete → plugins::IVideo{Rx,Tx}
    src/keyframe_limiter.cpp          # the key-frame storm limiter (plan B14)
    src/incremental_encode.cpp        # the dirty-rect incremental encode (plan B9)
    src/roi.cpp                      # the region-of-interest (plan C4)
    )

nimrtc_apply_options(nimrtc_video_pipeline)
target_link_libraries(nimrtc_video_pipeline
    PUBLIC
        nimrtc::core
        nimrtc::plugins
        nimrtc::video_frame
        nimrtc::video_payload
        nimrtc::video_jb
        nimrtc::rtp
        nimrtc::video_rtcp_feedback
    )

target_include_directories(nimrtc_video_pipeline
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

target_compile_definitions(nimrtc_video_pipeline PUBLIC NIMRTC_HAS_VIDEO_PIPELINE=1)

add_library(nimrtc::video_pipeline ALIAS nimrtc_video_pipeline)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()