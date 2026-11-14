# modules/h265/CMakeLists.txt
# ----------------------------------------------------------------------------
# H.265 / HEVC module — Annex B bitstream parser + stub decoder +
# IVideoCodec plugin adapter + hardware backend table.
#
# Per ARCHITECTURE.md §1.4 + §11.3 and the remote-desktop plan's RD-2:
# NimRTC does NOT bundle a software HEVC decoder (x265 is GPL-2.0 and
# excluded by the project's licensing policy).  This module ships:
#   - BitstreamParser (Annex B iteration over two-byte-header NAL units)
#   - StubDecoder (consumes an access unit, reports its key-frame-ness)
#   - CodecPluginAdapter (wraps the Decoder behind plugins::IVideoCodec)
#   - The HEVC backend table (NVENC / NVDEC first, stub last)
#   - Auto-registration with core::PluginRegistry under id "h265"
#
# The NVIDIA backends (NVENC / NVDEC) live in the shared `nvcodec`
# module — they serve H.264, H.265 and AV1 from the same driver code, so
# they are owned by that lower-level module and consumed here through its
# generic entry points (see src/modules/nvcodec/).
# ----------------------------------------------------------------------------
add_library(nimrtc_h265 STATIC
    src/decoder.cpp
    src/codec_plugin.cpp
    src/hw_backends.cpp
    src/vaapi_h265_encoder.cpp
    src/qsv_h265_encoder.cpp
    src/amf_h265_encoder.cpp
    )

nimrtc_apply_options(nimrtc_h265)
target_link_libraries(nimrtc_h265
    PUBLIC
        nimrtc::core
        nimrtc::plugins        # for plugins::IVideoCodec / IVideoCodecFactory
        nimrtc::video_frame    # for VideoFrameBuffer / VideoFrameInfo
        nimrtc::video_payload  # for h265::NaluType / NaluHeader
        nimrtc::nvcodec        # for the shared NVENC / NVDEC backends
    )

target_include_directories(nimrtc_h265
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

target_compile_definitions(nimrtc_h265 PUBLIC NIMRTC_HAS_H265=1)

add_library(nimrtc::h265 ALIAS nimrtc_h265)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
