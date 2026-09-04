# NimRTCTest.cmake
# ----------------------------------------------------------------------------
# Standardised unit-test target.
#
# Usage:
#     include(NimRTCTest)
#     nimrtc_add_test(test_jb modules/jb/tests/test_jb.cpp)
#
# NOTE: NIMRTC_FETCH_GTEST is initialised by the top-level CMakeLists.txt
# BEFORE this file is included. If you call include(NimRTCTest) from another
# entry point, declare that cache var first.
# ----------------------------------------------------------------------------

# Early exit when tests are disabled — makes nimrtc_add_test a no-op so
# module-level tests/CMakeLists.txt files are still parsed without errors.
if(NOT NIMRTC_BUILD_TESTS)
    include_guard(GLOBAL)
    function(nimrtc_add_test source)
        # no-op
    endfunction()
    return()
endif()

find_package(GTest QUIET)

if(NOT GTEST_FOUND)
    if(NIMRTC_FETCH_GTEST)
        message(STATUS "GoogleTest not found locally — fetching via FetchContent")
        include(FetchContent)
        FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://gitee.com/mirrors/googletest.git
            GIT_TAG        release-1.12.1)
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    else()
        message(FATAL_ERROR
            "GoogleTest not found. Either:\n"
            "  1. install it (apt install libgtest-dev, etc.), OR\n"
            "  2. set NIMRTC_FETCH_GTEST=ON (default), OR\n"
            "  3. set NIMRTC_BUILD_TESTS=OFF for now")
    endif()
endif()

# -----------------------------------------------------------------------------
# Standardised test target
# -----------------------------------------------------------------------------
function(nimrtc_add_test source)
    # Args after source are additional sources / dep targets
    set(multi_value_args DEPS)
    # cmake_parse_arguments signature:
    #   cmake_parse_arguments(<prefix> <options> <one_value_args>
    #                          <multi_value_args> <args>...)
    # DEPS is the *name* of a multi-value option; the actual values come
    # from the trailing ${ARGN}.
    cmake_parse_arguments(NIMRTC_TEST "" "" "DEPS" ${source} ${ARGN})

    # Test name = basename of source without .cpp
    get_filename_component(src_path ${source} ABSOLUTE)
    get_filename_component(src_dir  ${src_path} DIRECTORY)
    get_filename_component(src_name ${source} NAME_WLE)
    string(REPLACE "/tests/" "_test_" test_name ${src_dir})
    string(REPLACE "/" "_" test_name ${test_name})
    set(test_target "${src_name}_test")

    # -------------------------------------------------------------------------
    # The VS multi-config generator sets LinkLibraryDependencies=false on
    # Application targets, which strips transitive PUBLIC deps from the vcxproj.
    # We must set VS_LINK_LIBRARY_DEPENDENCIES=TRUE BEFORE target_link_libraries
    # — order matters: target_link_libraries() snapshots the link-deps table at
    # call time, and the property flip does not retroactively re-emit them
    # into the generated vcxproj's <AdditionalDependencies> list.
    # -------------------------------------------------------------------------
    add_executable(${test_target} ${src_path})
    nimrtc_apply_options(${test_target})
    set_target_properties(${test_target} PROPERTIES
        VS_LINK_LIBRARY_DEPENDENCIES TRUE)

    if(NIMRTC_TEST_DEPS)
        target_link_libraries(${test_target} PRIVATE
            ${NIMRTC_TEST_DEPS}
            GTest::gtest_main)
    else()
        target_link_libraries(${test_target} PRIVATE GTest::gtest_main)
    endif()

    # -------------------------------------------------------------------------
    # Include directories — all tests need core, plugins, and the generated
    # build/include/ directory.  Module-level tests (src/modules/<x>/tests/)
    # also need their own module include/ directory.
    # -------------------------------------------------------------------------
    set(test_includes
        "${CMAKE_SOURCE_DIR}/src/core/include"
        "${CMAKE_BINARY_DIR}/include"
        "${CMAKE_SOURCE_DIR}/src/plugins/include")

    # Detect module-level tests: src/modules/<name>/tests/*.cpp
    # In that case add src/modules/<name>/include as well.
    string(FIND "${src_dir}" "/modules/" modules_pos)
    if(modules_pos GREATER -1)
        # src_dir = .../src/modules/<name>/tests
        # parent = .../src/modules/<name>
        get_filename_component(module_root "${src_dir}" DIRECTORY)
        list(APPEND test_includes "${module_root}/include")
    endif()

    target_include_directories(${test_target} PRIVATE ${test_includes})

    add_test(NAME ${test_name} COMMAND ${test_target})
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 30)

    # gtest_discover_tests where supported
    if(COMMAND gtest_discover_tests)
        gtest_discover_tests(${test_target}
            PROPERTIES TIMEOUT 30
            DISCOVERY_TIMEOUT 10)
    endif()
endfunction()