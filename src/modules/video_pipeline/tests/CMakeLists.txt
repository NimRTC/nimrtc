# modules/video_pipeline/tests/CMakeLists.txt
# =============================================================================
# video_pipeline module tests
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

add_executable(test_video_pipeline test_video_pipeline.cpp)

set_target_properties(test_video_pipeline PROPERTIES
    VS_LINK_LIBRARY_DEPENDENCIES TRUE)

nimrtc_apply_options(test_video_pipeline LENIENT)

target_link_libraries(test_video_pipeline PRIVATE
    nimrtc::video_pipeline
    nimrtc::video_frame
    nimrtc::video_payload
    nimrtc::video_jb
    nimrtc::rtp
    nimrtc::core
    GTest::gtest_main
    GTest::gtest)

add_test(NAME modules_video_pipeline_tests_test_video_pipeline
         COMMAND test_video_pipeline)
set_tests_properties(modules_video_pipeline_tests_test_video_pipeline
                     PROPERTIES TIMEOUT 30)

if(COMMAND gtest_discover_tests)
    gtest_discover_tests(test_video_pipeline
        PROPERTIES TIMEOUT 30
        DISCOVERY_TIMEOUT 10)
endif()

# -----------------------------------------------------------------------------
# Remote-desktop plan B9 (dirty-rect incremental encode) + B14 (the key-frame
# storm limiter).
# -----------------------------------------------------------------------------
add_executable(test_video_pipeline_incremental test_video_pipeline_incremental.cpp)

set_target_properties(test_video_pipeline_incremental PROPERTIES
    VS_LINK_LIBRARY_DEPENDENCIES TRUE)

nimrtc_apply_options(test_video_pipeline_incremental LENIENT)

target_link_libraries(test_video_pipeline_incremental PRIVATE
    nimrtc::video_pipeline
    nimrtc::video_frame
    nimrtc::video_payload
    nimrtc::video_rtcp_feedback
    nimrtc::rtp
    nimrtc::plugins
    nimrtc::core
    GTest::gtest_main
    GTest::gtest)

add_test(NAME modules_video_pipeline_tests_test_video_pipeline_incremental
         COMMAND test_video_pipeline_incremental)
set_tests_properties(modules_video_pipeline_tests_test_video_pipeline_incremental
                     PROPERTIES TIMEOUT 30)

if(COMMAND gtest_discover_tests)
    gtest_discover_tests(test_video_pipeline_incremental
        PROPERTIES TIMEOUT 30
        DISCOVERY_TIMEOUT 10)
endif()

# -----------------------------------------------------------------------------
# Remote-desktop plan C4 (the region of interest).
# -----------------------------------------------------------------------------
add_executable(test_video_pipeline_roi test_video_pipeline_roi.cpp)

set_target_properties(test_video_pipeline_roi PROPERTIES
    VS_LINK_LIBRARY_DEPENDENCIES TRUE)

nimrtc_apply_options(test_video_pipeline_roi LENIENT)

target_link_libraries(test_video_pipeline_roi PRIVATE
    nimrtc::video_pipeline
    nimrtc::video_frame
    nimrtc::video_payload
    nimrtc::plugins
    nimrtc::core
    GTest::gtest_main
    GTest::gtest)

add_test(NAME modules_video_pipeline_tests_test_video_pipeline_roi
         COMMAND test_video_pipeline_roi)
set_tests_properties(modules_video_pipeline_tests_test_video_pipeline_roi
                     PROPERTIES TIMEOUT 30)

if(COMMAND gtest_discover_tests)
    gtest_discover_tests(test_video_pipeline_roi
        PROPERTIES TIMEOUT 30
        DISCOVERY_TIMEOUT 10)
endif()