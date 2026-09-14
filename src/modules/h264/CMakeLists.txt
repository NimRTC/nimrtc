# modules/h264/CMakeLists.txt
# ----------------------------------------------------------------------------
# H.264 module — Annex B bitstream + stub decoder + IVideoCodec plugin adapter.
#
# Per ARCHITECTURE.md §1.4 + §11.3: NimRTC does NOT bundle an H.264 decoder
# (patent fees). This module ships a stub decoder + bitstream parser. Host
# applications register their own decoder (libopenh264 / ffmpeg / HW) via
# the plugins::IVideoCodecFactory interface.
#
# P1 minimal:
#   - BitstreamParser (Annex B iteration, find SPS/PPS/IDR)
#   - SpsSummary (best-effort profile extraction for SDP hints)
#   - StubDecoder (consumes NAL units, produces empty VideoFrame)
#   - CodecPluginAdapter (wraps Decoder behind plugins::IVideoCodec)
#   - Auto-registration with core::PluginRegistry under id "h264"
#
# P3 HW backend sources (conditionally added by NimRTHwPlugins):
#   - nvenc_encoder.cpp : NVIDIA NVENC + NVDEC      (NIMRTC_PLUGINS_NVENC_ON)
#   - amf_encoder.cpp   : AMD AMF                   (NIMRTC_PLUGINS_AMF_ON, Win)
#   - qsv_encoder.cpp   : Intel QSV / libvpl        (NIMRTC_PLUGINS_QSV_ON)
#   - dxva_decoder.cpp  : Microsoft DXVA / MF       (NIMRTC_PLUGINS_DXVA_ON, Win)
#   - vaapi_encoder.cpp : Linux VA-API              (NIMRTC_PLUGINS_VAAPI_ON, Linux)
#   - openh264_encoder.cpp : OpenH264               (NIMRTC_PLUGINS_OPENH264_ON)
# ----------------------------------------------------------------------------
add_library(nimrtc_h264 STATIC
    src/bitstream.cpp
    src/decoder.cpp
    src/codec_plugin.cpp
    src/hw_backends.cpp
    )

nimrtc_apply_options(nimrtc_h264)
target_link_libraries(nimrtc_h264
    PUBLIC
        nimrtc::core
        nimrtc::plugins        # for plugins::IVideoCodec / IVideoCodecFactory
        nimrtc::video_frame    # for VideoFrameBuffer / VideoFrameInfo
        nimrtc::video_payload  # for h264::NaluType
    )

target_include_directories(nimrtc_h264
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

target_compile_definitions(nimrtc_h264 PUBLIC NIMRTC_HAS_H264=1)

# HW backend SDK integration.  Each nimrtc_link_<name>(target) call is a
# no-op unless NimRTHwPlugins detected the corresponding SDK AND the user
# enabled the matching NIMRTC_PLUGINS_<NAME>=ON cache option.
include(NimRTHwPlugins)
nimrtc_link_nvenc(nimrtc_h264)
nimrtc_link_amf(nimrtc_h264)
nimrtc_link_qsv(nimrtc_h264)
nimrtc_link_dxva(nimrtc_h264)
nimrtc_link_vaapi(nimrtc_h264)
nimrtc_link_nvdec(nimrtc_h264)
nimrtc_link_openh264(nimrtc_h264)

add_library(nimrtc::h264 ALIAS nimrtc_h264)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
