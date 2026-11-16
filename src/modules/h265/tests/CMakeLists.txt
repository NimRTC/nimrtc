# modules/h265/tests/CMakeLists.txt
# =============================================================================
# h265 module tests
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
            add_subdirectory(${CMAKE_SOURCE_DIR}/src/third_party/googletest
                            ${CMAKE_BINARY_DIR}/_googletest_module_subbuild)
        endif()
    else()
        message(FATAL_ERROR "GoogleTest not found")
    endif()
endif()

# -----------------------------------------------------------------------------
# test_hw_backends — the H.265 hardware backend table (remote-desktop plan
# item B5).  The test verifies the *seam*: every row exists on every
# platform, carries a defined priority, and reports `available() == false`
# **with a reason** on a host that cannot use it.
# -----------------------------------------------------------------------------
add_executable(test_h265_hw_backends test_hw_backends.cpp)

set_target_properties(test_h265_hw_backends PROPERTIES
    VS_LINK_LIBRARY_DEPENDENCIES TRUE)

nimrtc_apply_options(test_h265_hw_backends LENIENT)

target_link_libraries(test_h265_hw_backends PRIVATE
    nimrtc::h265
    nimrtc::video_frame
    nimrtc::video_payload
    nimrtc::core
    nimrtc::plugins
    GTest::gtest_main
    GTest::gtest)

add_test(NAME modules_h265_tests_test_hw_backends
         COMMAND test_h265_hw_backends)
set_tests_properties(modules_h265_tests_test_hw_backends PROPERTIES TIMEOUT 30)

if(COMMAND gtest_discover_tests)
    gtest_discover_tests(test_h265_hw_backends
        PROPERTIES TIMEOUT 30
        DISCOVERY_TIMEOUT 10)
endif()
