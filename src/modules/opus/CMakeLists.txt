# modules/opus/CMakeLists.txt
# ----------------------------------------------------------------------------
# Opus audio codec module (RFC 6716).
#
# P1 implementation:
#   - Real Opus encode/decode via vendored libopus (src/third_party/libopus/src)
#   - OpusEncoder / OpusDecoder concrete API (wraps opus_encoder_* / opus_decoder_*)
#   - RFC 7587 RTP packetisation helpers (TOC byte framing)
#   - plugins::ICodec adapter (R2-Batch1, registered as id "opus")
#
# The codec module links against `nimrtc_vendor_libopus` (the vendored libopus
# library, exposed via NimRTCVendored.cmake::nimrtc_link_libopus).
# ----------------------------------------------------------------------------
add_library(nimrtc_opus STATIC
    src/opus.cpp
    src/codec_plugin.cpp           # codec plugin adapter (R2-Batch1)
    )

nimrtc_apply_options(nimrtc_opus)
target_link_libraries(nimrtc_opus
    PUBLIC
        nimrtc::core
        nimrtc::plugins       # plugin interface (for ICodec / ICodecFactory)
    PRIVATE
        nimrtc_vendor_libopus   # real libopus (opus_encoder_* / opus_decoder_*)
    )

target_include_directories(nimrtc_opus
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

target_compile_definitions(nimrtc_opus PUBLIC NIMRTC_HAS_OPUS=1)

add_library(nimrtc::opus ALIAS nimrtc_opus)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    # ensure a CTestTestfile.cmake gets generated at this level so ctest
    # can recurse into the tests/ subdir from src/modules/
    enable_testing()
    add_subdirectory(tests)
endif()
