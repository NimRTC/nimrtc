# modules/av1/CMakeLists.txt
# ----------------------------------------------------------------------------
# AV1 module — Annex B OBU parser + stub decoder + IVideoCodec plugin
# adapter + hardware backend table.
#
# Per ARCHITECTURE.md §1.4 and the remote-desktop plan's RD-2:
# NimRTC does NOT bundle a software AV1 decoder by default (dav1d is an
# optional vendor dependency behind `NIMRTC_VENDORED_DAV1D`).  This module
# ships:
#   - BitstreamParser (Annex B iteration over OBUs)
#   - StubDecoder (consumes an access unit, reports its key-frame-ness)
#   - CodecPluginAdapter (wraps the Decoder behind plugins::IVideoCodec)
#   - The AV1 backend table (NVENC / NVDEC first, stub last)
#   - Auto-registration with core::PluginRegistry under id "av1"
#
# The NVIDIA backends (NVENC / NVDEC) live in the shared `nvcodec`
# module — they serve H.264, H.265 and AV1 from the same driver code, so
# they are owned by that lower-level module and consumed here through its
# generic entry points (see src/modules/nvcodec/).
# ----------------------------------------------------------------------------
add_library(nimrtc_av1 STATIC
    src/decoder.cpp
    src/codec_plugin.cpp
    src/hw_backends.cpp
    src/amf_av1_encoder.cpp
    src/vaapi_av1_encoder.cpp
    src/svt_av1_encoder.cpp
    )

nimrtc_apply_options(nimrtc_av1)
target_link_libraries(nimrtc_av1
    PUBLIC
        nimrtc::core
        nimrtc::plugins        # for plugins::IVideoCodec / IVideoCodecFactory
        nimrtc::video_frame    # for VideoFrameBuffer / VideoFrameInfo
        nimrtc::video_payload  # for av1::ObuType / ObuHeader
        nimrtc::nvcodec        # for the shared NVENC / NVDEC backends
    )

target_include_directories(nimrtc_av1
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

target_compile_definitions(nimrtc_av1 PUBLIC NIMRTC_HAS_AV1=1)

# Real software AV1 decoder (remote-desktop plan WP-B).  dav1d is
# decode-only, so this adds only the decoder source.  The helper is a
# no-op unless the vendored dav1d source tree is populated.
include(NimRTHwPlugins)
nimrtc_link_dav1d(nimrtc_av1)

# The optional software AV1 encoder (remote-desktop plan B11).  The
# helper is a no-op unless NIMRTC_VENDORED_SVT_AV1=ON AND the vendored
# source tree is populated; with the option off the `svt_av1` row is
# simply absent from the registry (the plan's B11 acceptance).
nimrtc_link_svt_av1(nimrtc_av1)

add_library(nimrtc::av1 ALIAS nimrtc_av1)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
