# modules/jb/CMakeLists.txt
# ----------------------------------------------------------------------------
# Jitter buffer.
#
# Scope (P2+):
#   - RFC 3550-style adaptive jitter buffer
#   - Per-frame ref_frame association (NimRTC §8.4 differentiation)
#
# Dependencies:
#   - core (always)
#   - modules/rtp  (consumes rtp::PacketView in its public header)
# ----------------------------------------------------------------------------
add_library(nimrtc_jb STATIC
    src/jitter_buffer.cpp
    src/jb_plugin.cpp           # adapter wrapping concrete → plugins::IJB
    )

nimrtc_apply_options(nimrtc_jb)
target_link_libraries(nimrtc_jb
    PUBLIC
        nimrtc::core
        nimrtc::plugins       # plugin interface (for IJB / IJBFactory)
        # PUBLIC: consumers can pass rtp::PacketView through their own
        # interfaces without re-declaring the type.
        nimrtc::rtp
    )

target_include_directories(nimrtc_jb
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

target_compile_definitions(nimrtc_jb PUBLIC NIMRTC_HAS_JB=1)

add_library(nimrtc::jb ALIAS nimrtc_jb)

if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()