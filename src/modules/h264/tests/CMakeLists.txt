# modules/h264/tests/CMakeLists.txt
# =============================================================================
# h264 module tests
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
# test_h264_codec — bitstream + stub decoder + plugin adapter tests
# -----------------------------------------------------------------------------
add_executable(test_h264_codec test_h264_codec.cpp)

set_target_properties(test_h264_codec PROPERTIES
    VS_LINK_LIBRARY_DEPENDENCIES TRUE)

nimrtc_apply_options(test_h264_codec LENIENT)

target_link_libraries(test_h264_codec PRIVATE
    nimrtc::h264
    nimrtc::video_frame
    nimrtc::video_payload
    nimrtc::core
    nimrtc::plugins
    GTest::gtest_main
    GTest::gtest)

add_test(NAME modules_h264_tests_test_h264_codec
         COMMAND test_h264_codec)
set_tests_properties(modules_h264_tests_test_h264_codec PROPERTIES TIMEOUT 30)

if(COMMAND gtest_discover_tests)
    gtest_discover_tests(test_h264_codec
        PROPERTIES TIMEOUT 30
        DISCOVERY_TIMEOUT 10)
endif()

# -----------------------------------------------------------------------------
# test_openh264_software — the REAL software H.264 codec (remote-desktop
# plan WP-B).  Only built when the vendored OpenH264 source tree is
# present and NIMRTC_PLUGINS_OPENH264=ON; otherwise the synthetic stub
# stays in charge and there is nothing to test here.
# -----------------------------------------------------------------------------
if(NIMRTC_PLUGINS_OPENH264 AND NIMRTC_VENDORED_OPENH264 AND
   EXISTS "${CMAKE_SOURCE_DIR}/src/third_party/openh264/src/codec/api/wels/codec_api.h")
    add_executable(test_openh264_software test_openh264_software.cpp)

    set_target_properties(test_openh264_software PROPERTIES
        VS_LINK_LIBRARY_DEPENDENCIES TRUE)

    nimrtc_apply_options(test_openh264_software LENIENT)

    target_link_libraries(test_openh264_software PRIVATE
        nimrtc::h264
        nimrtc::video_frame
        nimrtc::video_payload
        nimrtc::core
        nimrtc::plugins
        GTest::gtest_main
        GTest::gtest)

    add_test(NAME modules_h264_tests_test_openh264_software
             COMMAND test_openh264_software)
    set_tests_properties(modules_h264_tests_test_openh264_software PROPERTIES TIMEOUT 60)

    if(COMMAND gtest_discover_tests)
        gtest_discover_tests(test_openh264_software
            PROPERTIES TIMEOUT 60
            DISCOVERY_TIMEOUT 10)
    endif()
endif()

# -----------------------------------------------------------------------------
# test_hw_backends — HW backend registry + helper unit tests
# -----------------------------------------------------------------------------
# This test file accesses the private header src/hw_backend_base.hpp via the
# include path. The PRIVATE include directory is added below so the test exe
# can resolve `hw_backend_base.hpp` from `${CMAKE_SOURCE_DIR}/src/modules/h264/src`.
add_executable(test_hw_backends test_hw_backends.cpp)

set_target_properties(test_hw_backends PROPERTIES
    VS_LINK_LIBRARY_DEPENDENCIES TRUE)

nimrtc_apply_options(test_hw_backends LENIENT)

target_include_directories(test_hw_backends PRIVATE
    "${CMAKE_SOURCE_DIR}/src/modules/h264/src")

target_link_libraries(test_hw_backends PRIVATE
    nimrtc::h264
    nimrtc::video_frame
    nimrtc::video_payload
    nimrtc::core
    nimrtc::plugins
    GTest::gtest_main
    GTest::gtest)

# Skip this test on hosts without a GPU/SDK link. The test itself probes the
# SDK and is safe to run when none of the HW plugins are enabled — it just
# verifies the dispatch surface and the helper functions.
add_test(NAME modules_h264_tests_test_hw_backends
         COMMAND test_hw_backends)
set_tests_properties(modules_h264_tests_test_hw_backends PROPERTIES TIMEOUT 30)

if(COMMAND gtest_discover_tests)
    gtest_discover_tests(test_hw_backends
        PROPERTIES TIMEOUT 30
        DISCOVERY_TIMEOUT 10)
endif()
