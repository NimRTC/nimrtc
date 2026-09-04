# modules/ice/CMakeLists.txt
# ----------------------------------------------------------------------------
# ICE (Interactive Connectivity Establishment) module.
#
# Scope (P1):
#   - IceTransport : ITransport backed by libjuice (RFC 8445)
#   - Candidate gathering (host, srflx, relay)
#   - STUN/TURN protocol support
#   - Connectivity checks and nomination
#   - Component multiplexing (RTP/RTCP)
#   - Lite mode support
#
# Dependencies:
#   - core (always)
#   - plugins (ITransport / IPlugin interfaces)
#   - nimrtc_vendor_libjuice (vendored, MPL-2.0)
# ----------------------------------------------------------------------------
add_library(nimrtc_ice STATIC
    src/ice.cpp
    )

nimrtc_apply_options(nimrtc_ice)
target_link_libraries(nimrtc_ice
    PUBLIC
        nimrtc::core
        nimrtc::plugins       # plugin interface (ITransport / IPlugin)
    PRIVATE
        nimrtc_vendor_libjuice
    )

target_include_directories(nimrtc_ice
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

target_compile_definitions(nimrtc_ice PUBLIC NIMRTC_HAS_ICE=1)

add_library(nimrtc::ice ALIAS nimrtc_ice)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    # ensure a CTestTestfile.cmake gets generated at this level so ctest
    # can recurse into the tests/ subdir from src/modules/
    enable_testing()
    add_subdirectory(tests)
endif()
