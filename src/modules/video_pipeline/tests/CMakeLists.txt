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

nimrtc_apply_options(test_video_pipeline) LENIENT

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