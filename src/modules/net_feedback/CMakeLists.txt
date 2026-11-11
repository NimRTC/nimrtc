# modules/net_feedback/CMakeLists.txt
# ----------------------------------------------------------------------------
# Receiver-side network-feedback collector module (v1.2.0 M1, F2).
#
# Scope:
#   - Aggregate inbound RR / REMB / transport-cc into one snapshot
#   - Implement the plugins::INetFeedback seam
#
# Dependencies:
#   - core (always)
#   - plugins (the seam)
#   - modules/rtp (the wire formats: rtp::Parser, rtp::parse_remb).
#     This is the documented cross-module dependency pattern the modules
#     directory allows (same shape as modules/jb using modules/rtp).
# ----------------------------------------------------------------------------
add_library(nimrtc_net_feedback STATIC
    src/collector.cpp
    src/plugin.cpp
    )

nimrtc_apply_options(nimrtc_net_feedback)
target_link_libraries(nimrtc_net_feedback
    PUBLIC
        nimrtc::core
        nimrtc::plugins       # plugin interface (INetFeedback / factory)
    PRIVATE
        nimrtc::rtp           # RTCP wire formats (RR / REMB / transport-cc)
    )

target_include_directories(nimrtc_net_feedback
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

target_compile_definitions(nimrtc_net_feedback PUBLIC NIMRTC_HAS_NET_FEEDBACK=1)

if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    # ensure a CTestTestfile.cmake gets generated at this level so ctest
    # can recurse into the tests/ subdir from src/modules/ (CMake 4.0
    # VS generator quirk — same pattern as modules/rtp).
    enable_testing()
    add_subdirectory(tests)
endif()

add_library(nimrtc::net_feedback ALIAS nimrtc_net_feedback)
