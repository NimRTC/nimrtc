# modules/rtp/CMakeLists.txt
# ----------------------------------------------------------------------------
# RTP packet parser / builder.
#
# Scope (P1+):
#   - RTP RFC 3550 packet parse and build
#   - RTCP SR / RR / NACK / REMB (subset)
#
# Out of scope (deferred):
#   - SRTP encryption (modules/srtp, P1)
#   - DTLS / ICE / SDP (their own modules)
#
# Dependencies:
#   - core (always)
#   - libopus (P1 — for audio RTP testing only)
# ----------------------------------------------------------------------------
add_library(nimrtc_rtp STATIC
    src/parser.cpp
    src/builder.cpp
    src/rtp_plugin.cpp       # adapter wrapping concrete → plugins::IRTP
    )

nimrtc_apply_options(nimrtc_rtp)
target_link_libraries(nimrtc_rtp
    PUBLIC
        nimrtc::core
        nimrtc::plugins       # plugin interface (for IRTP / IRTPFactory)
        # PUBLIC so consumers can use rtp::PacketView in their headers.
        # Cross-module visibility is explicit; we don't hide headers we
        # genuinely want others to consume.
    )

# Public include path — exports both interface headers and the export header.
target_include_directories(nimrtc_rtp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

target_compile_definitions(nimrtc_rtp PUBLIC NIMRTC_HAS_RTP=1)

add_library(nimrtc::rtp ALIAS nimrtc_rtp)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()