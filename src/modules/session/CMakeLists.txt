# modules/session/CMakeLists.txt
# ----------------------------------------------------------------------------
# Session orchestration module — Track / Stream / BUNDLE / rtcp-mux.
#
# Scope (P1 — NimRTC-V2 §5 scope list: Session BUNDLE/rtcp-mux):
#   - Track class: audio / video / data track with direction and SSRC state.
#   - Stream class: group of Tracks (WebRTC MediaStream mapping).
#   - Session class: manages Streams, BUNDLE configuration, rtcp-mux flag.
#
# Out of scope:
#   - Actual SDP generation (delegated to nimrtc_sdp / engine.cpp).
#   - ICE transport wiring (delegated to ICE module).
#
# Dependencies:
#   - core          (always)
#   - plugins       (header-only interface layer)
#   - nimrtc_sdp    (SDP m-line / mid management)
#   - nimrtc_rtp    (SSRC types, RTP packet structure)
# ----------------------------------------------------------------------------
add_library(nimrtc_session STATIC
    src/track.cpp
    src/session.cpp
    )

nimrtc_apply_options(nimrtc_session)
target_link_libraries(nimrtc_session
    PUBLIC
        nimrtc::core
        nimrtc::plugins
        nimrtc::sdp
        nimrtc::rtp
    )

target_include_directories(nimrtc_session
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

target_compile_definitions(nimrtc_session PUBLIC NIMRTC_HAS_SESSION=1)

add_library(nimrtc::session ALIAS nimrtc_session)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
