# modules/opus/CMakeLists.txt
# ----------------------------------------------------------------------------
# Opus audio codec module (RFC 6716).
#
# P1 status: stub (no real encoding).  Real libopus integration lands
# when src/third_party/libopus/src is populated.
#
# Scope (P1):
#   - OpusEncoder / OpusDecoder stub (PCM passthrough)
#   - RFC 7587 RTP packetisation helpers
# ----------------------------------------------------------------------------
add_library(nimrtc_opus STATIC
    src/opus.cpp
    )

nimrtc_apply_options(nimrtc_opus)
target_link_libraries(nimrtc_opus
    PUBLIC
        nimrtc::core
    )

target_include_directories(nimrtc_opus
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    )

add_library(nimrtc::opus ALIAS nimrtc_opus)
