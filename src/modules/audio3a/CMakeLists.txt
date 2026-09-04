# modules/audio3a/CMakeLists.txt
# ----------------------------------------------------------------------------
# Audio 3A processing module.
#
# Scope (P1):
#   - NullAudio3A stub for testing
#   - WebRtcAudio3A wrapper (requires NIMRTC_VENDOR_WEBRTC_APM)
#   - Metrics: VAD, TX/RX levels
#
# P1 future:
#   - Full WebRTC APM integration
#   - SpeexDSP alternative
#
# Dependencies:
#   - core (always)
# ----------------------------------------------------------------------------
add_library(nimrtc_audio3a STATIC
    src/audio3a.cpp
    src/audio3a_plugin.cpp     # adapter wrapping concrete → plugins::IAudio3A
    )

nimrtc_apply_options(nimrtc_audio3a)
target_link_libraries(nimrtc_audio3a
    PUBLIC
        nimrtc::core
        nimrtc::plugins       # plugin interface
    PRIVATE
    )

target_include_directories(nimrtc_audio3a
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

target_compile_definitions(nimrtc_audio3a PUBLIC NIMRTC_HAS_AUDIO3A=1)

add_library(nimrtc::audio3a ALIAS nimrtc_audio3a)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
