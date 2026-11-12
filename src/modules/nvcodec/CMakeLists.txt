# modules/nvcodec/CMakeLists.txt
# ----------------------------------------------------------------------------
# NVIDIA codec backend module — NVENC (encode) + NVDEC (decode),
# parameterised by codec family (H.264 / H.265 / AV1).
#
# ## Why this module exists
#
# The NVIDIA driver integration is *not* H.264-specific: the same NVENC
# session code serves H.264, HEVC and AV1, and the same NVDEC parser /
# decoder code serves all three.  What differs per codec is a small set
# of constants (the encodeGUID, the profileGUID, the CUVID codec type),
# all collected in `nv_codec_kind.hpp`.
#
# Rather than triplicate ~900 lines of driver code across three codec
# modules — which would guarantee the copies drift apart — the driver
# integration lives here, in one lower-level module that the H.264,
# H.265 and AV1 codec modules link against.  This is the documented
# cross-module dependency pattern, the same shape as `modules/jb`
# using `modules/jb_strategy`.
#
# The module also owns `HwCompositeCodec`, the adapter that pairs a
# one-directional encoder with a one-directional decoder (the engine
# holds a single IVideoCodec per session, but the HW backends are
# naturally one-directional).
#
# ## Public surface
#
#   nimrtc/nvcodec/nv_codec_kind.hpp  — the codec descriptor table
#   nimrtc/nvcodec/nv_backends.hpp    — nvenc_codec_available() /
#                                        make_nvenc_codec() /
#                                        nvdec_codec_available() /
#                                        make_nvdec_codec()
#   nimrtc/nvcodec/hw_composite_codec.hpp — the encoder+decoder adapter
#
# The generic entry points are *always* present — they return false /
# nullptr when the NVIDIA SDK is not compiled in — so the codec modules
# need no `#if` guards of their own.
# ----------------------------------------------------------------------------
add_library(nimrtc_nvcodec STATIC
    src/nv_codec_kind.cpp
    src/hw_composite_codec.cpp
    src/nvenc_encoder.cpp
    src/nvdec_decoder.cpp
    )

nimrtc_apply_options(nimrtc_nvcodec)
target_link_libraries(nimrtc_nvcodec
    PUBLIC
        nimrtc::core
        nimrtc::plugins        # for plugins::IVideoCodec
        nimrtc::video_frame    # for VideoFrame / EncodedVideoFrame
    )

target_include_directories(nimrtc_nvcodec
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

# The NVIDIA driver integration.  Each nimrtc_link_<name>(target) call is a
# no-op unless NimRTHwPlugins detected the corresponding SDK AND the user
# enabled the matching NIMRTC_PLUGINS_<NAME>=ON cache option.  When the
# SDK is absent the generic entry points still exist — they simply report
# "not available".
include(NimRTHwPlugins)
nimrtc_link_nvenc(nimrtc_nvcodec)
nimrtc_link_nvdec(nimrtc_nvcodec)

add_library(nimrtc::nvcodec ALIAS nimrtc_nvcodec)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
