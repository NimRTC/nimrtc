# modules/sdp/CMakeLists.txt
# ----------------------------------------------------------------------------
# SDP (RFC 4566) plus WebRTC-specific extensions (RFC 8829 / draft-ietf-mmusic-rids,
# draft-ietf-mmusic-msid).
#
# Scope (P1+):
#   - Parse and emit SDP session-level + media-level attributes
#   - Reorder attributes per RFC for maximum interop
#
# Out of scope:
#   - SDP-based offer/answer state machine (lives in the peer-connection layer, P3+)
#
# Dependencies:
#   - core (always)
# ----------------------------------------------------------------------------
add_library(nimrtc_sdp STATIC
    src/parser.cpp
    src/munger.cpp
    src/sdp_plugin.cpp           # adapter wrapping concrete → plugins::ISDP
    )

nimrtc_apply_options(nimrtc_sdp)

# The SDP module passes RFC-specified integer fields (version, ttl, id, …)
# as uint64_t to its formatting helpers.  Converting a small positive int
# to uint64_t is safe and intentional — suppress the resulting
# -Wsign-conversion noise for this module only.
if(NOT MSVC)
    target_compile_options(nimrtc_sdp PRIVATE -Wno-sign-conversion)
endif()
target_link_libraries(nimrtc_sdp
    PUBLIC
        nimrtc::core
        nimrtc::plugins       # plugin interface (for ISDP / ISDPFactory)
)

target_include_directories(nimrtc_sdp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

target_compile_definitions(nimrtc_sdp PUBLIC NIMRTC_HAS_SDP=1)

add_library(nimrtc::sdp ALIAS nimrtc_sdp)

if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()