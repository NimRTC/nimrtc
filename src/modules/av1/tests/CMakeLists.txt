# modules/av1/tests/CMakeLists.txt
# =============================================================================
# av1 module tests
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
# test_hw_backends — the AV1 hardware backend table (remote-desktop plan
# item B6).  The test verifies the *seam*: every row exists on every
# platform, carries a defined priority, and reports `available() == false`
# **with a reason** on a host that cannot use it.
# -----------------------------------------------------------------------------
add_executable(test_av1_hw_backends test_hw_backends.cpp)

set_target_properties(test_av1_hw_backends PROPERTIES
    VS_LINK_LIBRARY_DEPENDENCIES TRUE)

nimrtc_apply_options(test_av1_hw_backends LENIENT)

target_link_libraries(test_av1_hw_backends PRIVATE
    nimrtc::av1
    nimrtc::video_frame
    nimrtc::video_payload
    nimrtc::core
    nimrtc::plugins
    GTest::gtest_main
    GTest::gtest)

add_test(NAME modules_av1_tests_test_hw_backends
         COMMAND test_av1_hw_backends)
set_tests_properties(modules_av1_tests_test_hw_backends PROPERTIES TIMEOUT 30)

if(COMMAND gtest_discover_tests)
    gtest_discover_tests(test_av1_hw_backends
        PROPERTIES TIMEOUT 30
        DISCOVERY_TIMEOUT 10)
endif()

# -----------------------------------------------------------------------------
# test_dav1d_software — the REAL software AV1 decoder (remote-desktop plan
# WP-B).  Only built when the vendored dav1d source tree is present and
# NIMRTC_PLUGINS_DAV1D=ON; otherwise the synthetic stub stays in charge
# and there is nothing to test here.
# -----------------------------------------------------------------------------
if(NIMRTC_PLUGINS_DAV1D AND NIMRTC_VENDORED_DAV1D AND
   EXISTS "${CMAKE_SOURCE_DIR}/src/third_party/dav1d/src/include/dav1d/dav1d.h")
    add_executable(test_dav1d_software test_dav1d_software.cpp)

    set_target_properties(test_dav1d_software PROPERTIES
        VS_LINK_LIBRARY_DEPENDENCIES TRUE)

    nimrtc_apply_options(test_dav1d_software LENIENT)

    target_link_libraries(test_dav1d_software PRIVATE
        nimrtc::av1
        nimrtc::video_frame
        nimrtc::video_payload
        nimrtc::core
        nimrtc::plugins
        GTest::gtest_main
        GTest::gtest)

    add_test(NAME modules_av1_tests_test_dav1d_software
             COMMAND test_dav1d_software)
    set_tests_properties(modules_av1_tests_test_dav1d_software PROPERTIES TIMEOUT 60)

    if(COMMAND gtest_discover_tests)
        gtest_discover_tests(test_dav1d_software
            PROPERTIES TIMEOUT 60
            DISCOVERY_TIMEOUT 10)
    endif()
endif()
