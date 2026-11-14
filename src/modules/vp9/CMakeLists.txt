# modules/vp9/CMakeLists.txt
# ----------------------------------------------------------------------------
# VP9 codec module — the optional software VP9 encoder (remote-desktop plan
# B11) plus the stub decoder and the backend table.
#
# ## Why a separate module
#
# The AV1 module is codec-specific: it owns the AV1 OBU parser and the
# Annex-B framing the AV1 RTP payload format uses.  VP9 has its own
# framing (RFC 9559: one access unit per frame, no start codes), so it
# gets its own small module rather than being bolted onto the AV1 one.
# The module mirrors the AV1 module's shape exactly:
#
#   - a `Decoder` interface + a stub implementation,
#   - a `CodecPluginAdapter` that wraps the decoder behind
#     plugins::IVideoCodec,
#   - a backend table with the available() / priority /
#     zero_copy_supported contract,
#   - auto-registration with core::PluginRegistry under id "vp9".
#
# ## The optional libvpx encoder
#
# `NIMRTC_VENDORED_LIBVPX` defaults **OFF** (remote-desktop plan B11 /
# R-11: a vendored dependency that cannot build everywhere is option-gated
# and defaults OFF where it does not build — libvpx's `rtcd.pl` code
# generators need a perl interpreter, which a stock Windows CI host does
# not have).
#
# With the option OFF the `libvpx_vp9` row is simply *absent* from the
# registry, which is what the plan's B11 acceptance asks for ("with the
# option OFF the build still configures and the plug-in is absent").
# ----------------------------------------------------------------------------
add_library(nimrtc_vp9 STATIC
    src/decoder.cpp
    src/codec_plugin.cpp
    src/hw_backends.cpp
    src/libvpx_vp9_codec.cpp
    )

nimrtc_apply_options(nimrtc_vp9)
target_link_libraries(nimrtc_vp9
    PUBLIC
        nimrtc::core
        nimrtc::plugins        # for plugins::IVideoCodec / IVideoCodecFactory
        nimrtc::video_frame    # for VideoFrameBuffer / VideoFrameInfo
    )

target_include_directories(nimrtc_vp9
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

target_compile_definitions(nimrtc_vp9 PUBLIC NIMRTC_HAS_VP9=1)

# The optional software VP9 encoder (remote-desktop plan B11).  The
# helper is a no-op unless NIMRTC_VENDORED_LIBVPX=ON AND the vendored
# source tree is populated AND the build host carries a perl interpreter;
# with the option off the `libvpx_vp9` row is simply absent from the
# registry.
include(NimRTHwPlugins)
nimrtc_link_libvpx(nimrtc_vp9)

add_library(nimrtc::vp9 ALIAS nimrtc_vp9)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
