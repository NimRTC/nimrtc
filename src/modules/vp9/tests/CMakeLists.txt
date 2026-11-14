# modules/vp9/tests/CMakeLists.txt
# =============================================================================
# vp9 module tests
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
# test_hw_backends — the VP9 backend table (remote-desktop plan item B11).
# The test verifies the *seam*: the stub row always exists, the optional
# libvpx row is present only when the vendored build is compiled in, and
# every unavailable row reports `available() == false` **with a reason**.
# -----------------------------------------------------------------------------
add_executable(test_vp9_hw_backends test_hw_backends.cpp)

set_target_properties(test_vp9_hw_backends PROPERTIES
    VS_LINK_LIBRARY_DEPENDENCIES TRUE)

nimrtc_apply_options(test_vp9_hw_backends LENIENT)

target_link_libraries(test_vp9_hw_backends PRIVATE
    nimrtc::vp9
    nimrtc::video_frame
    nimrtc::core
    nimrtc::plugins
    GTest::gtest_main
    GTest::gtest)

add_test(NAME modules_vp9_tests_test_hw_backends
         COMMAND test_vp9_hw_backends)
set_tests_properties(modules_vp9_tests_test_hw_backends PROPERTIES TIMEOUT 30)

if(COMMAND gtest_discover_tests)
    gtest_discover_tests(test_vp9_hw_backends
        PROPERTIES TIMEOUT 30
        DISCOVERY_TIMEOUT 10)
endif()
