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
    src/rtcp.cpp             # v1.2.0 M1: RTCP compound demux (F1) + REMB (F7)
    src/rtcp_xr.cpp          # v1.2.0 X1/X2: RFC 3611 XR blocks
    src/rtcp_tmmbr.cpp       # v1.2.0 X3/X4: RFC 5104 TMMBR / TMMBN
    src/timeline.cpp           # ADR-017: NTP↔RTP-TS + abs-send-time helpers
    src/rtp_plugin.cpp       # adapter wrapping concrete → plugins::IRTP
    src/xr_report_plugin.cpp # v1.2.0 X1: adapter → plugins::IXrReport
    )

# STRICT_CONVERSIONS: this module is where a narrowing conversion is a
# *protocol* bug, not a style question -- an RTP header field is a
# 16-bit quantity on the wire, and the parser / builder are the only
# places that know that.  The module is small and has no historical
# backlog, so keeping the conversion warnings fatal here costs
# nothing and protects the wire format.
nimrtc_apply_options(nimrtc_rtp STRICT_CONVERSIONS)
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
    # ensure a CTestTestfile.cmake gets generated at this level so ctest
    # can recurse into the tests/ subdir from src/modules/ (CMake 4.0
    # VS generator quirk — without enable_testing() here the file is
    # silently skipped).
    enable_testing()
    add_subdirectory(tests)
endif()