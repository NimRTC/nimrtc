# modules/audio3a/tests/CMakeLists.txt
# =============================================================================
# Audio3A plugin adapter smoke tests.
#
# Verifies:
#   - NullPluginFactory registers under id "webrtc"
#   - factory->create() returns a PluginAdapter wrapping NullAudio3A
#   - adapter implements plugins::IAudio3A lifecycle (open / close)
#   - process_capture / process_render round-trip (null stub = pass-through)
#   - stats() returns sane values
#   - callbacks are invoked after process_capture
#
# Uses GoogleTest (FetchContent if not installed).
# =============================================================================

enable_testing()

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

add_executable(test_audio3a_plugin test_audio3a_plugin.cpp)

set_target_properties(test_audio3a_plugin PROPERTIES
    VS_LINK_LIBRARY_DEPENDENCIES TRUE)

nimrtc_apply_options(test_audio3a_plugin LENIENT)

target_link_libraries(test_audio3a_plugin PRIVATE
    nimrtc::audio3a
    nimrtc::core
    nimrtc::plugins
    GTest::gtest_main
    GTest::gtest)

# NOTE: No top-level add_test() for the wrapper executable.  CMake 4.0's
# multi-config generator wraps plain add_test() in a CTEST_CONFIGURATION_TYPE
# conditional that marks the wrapper as NOT_AVAILABLE when ctest is invoked
# without `-C <config>`.  The gtest_discover_tests() call below creates
# per-case add_test() entries at build time that bypass the conditional,
# so every test case in this binary is still discovered and runnable.
# -----------------------------------------------------------------------------
# gtest_discover_tests emits per-case add_test() entries at build time that
# bypass the CTEST_CONFIGURATION_TYPE conditional wrapping that CMake 4.0
# generates for plain add_test() calls in CTestTestfile.cmake (otherwise
# tests show as NOT_AVAILABLE under multi-config generators when ctest is
# invoked without `-C <config>`).
if(COMMAND gtest_discover_tests)
    gtest_discover_tests(test_audio3a_plugin
        PROPERTIES TIMEOUT 30
        DISCOVERY_TIMEOUT 10)
endif()
