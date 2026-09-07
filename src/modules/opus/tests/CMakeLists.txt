# modules/opus/tests/CMakeLists.txt
# =============================================================================
# Opus encoder/decoder unit tests (real libopus-backed).
#
# Two test binaries:
#   - test_opus           : encoder/decoder roundtrip + stats + DTX + PLC
#   - test_opus_packetise : RFC 7587 §4 RTP packetisation (TOC, Code 0/1/2/3)
# =============================================================================

find_package(GTest QUIET)

if(NOT GTEST_FOUND)
    if(NIMRTC_FETCH_GTEST)
        # GTest::gtest_main is normally provided by cmake/NimRTCTest.cmake via
        # add_subdirectory(src/third_party/googletest). Only fall back here if
        # we're being built standalone (e.g. module-only build).
        if(NOT TARGET GTest::gtest_main)
            set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
            set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
            set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
            add_subdirectory(${CMAKE_SOURCE_DIR}/src/third_party/googletest ${CMAKE_BINARY_DIR}/_googletest_module_subbuild)
        endif()
    else()
        message(FATAL_ERROR "GoogleTest not found")
    endif()
endif()

add_executable(test_opus test_opus.cpp)

set_target_properties(test_opus PROPERTIES
    VS_LINK_LIBRARY_DEPENDENCIES TRUE)

nimrtc_apply_options(test_opus)

target_link_libraries(test_opus PRIVATE
    nimrtc::opus
    nimrtc::core
    GTest::gtest_main
    GTest::gtest)

# RFC 7587 RTP packetisation tests — register via nimrtc_add_test for the
# standardised wrapper (auto test discovery, module include dir, etc.).
nimrtc_add_test(${CMAKE_CURRENT_SOURCE_DIR}/test_opus_packetise.cpp
    DEPS nimrtc::opus nimrtc::core)

# NOTE: No top-level add_test() — see modules/audio3a/tests/CMakeLists.txt
# for the rationale (CMake 4.0 multi-config conditional bug).  The legacy
# test_opus binary below is also picked up via gtest_discover_tests.
if(COMMAND gtest_discover_tests)
    gtest_discover_tests(test_opus
        PROPERTIES TIMEOUT 30
        DISCOVERY_TIMEOUT 10)
endif()
