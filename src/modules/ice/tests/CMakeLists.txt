# modules/ice/tests/CMakeLists.txt
# =============================================================================
# ICE transport module unit tests.
# Built via standard CMake + GoogleTest (FetchContent) directly so we have
# full control over include paths without depending on a correct DEPS
# invocation through nimrtc_add_test().
# =============================================================================

# Each module's tests subdirectory must call enable_testing() locally so its
# add_test() calls get written to a per-directory CTestTestfile.cmake.
# (The root-level enable_testing() doesn't recurse into subdirs by itself.)
enable_testing()

find_package(GTest QUIET)

if(NOT GTEST_FOUND)
    if(NIMRTC_FETCH_GTEST)
        include(FetchContent)
        FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG        release-1.12.1)
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    else()
        message(FATAL_ERROR "GoogleTest not found")
    endif()
endif()

add_executable(test_ice_test test_ice.cpp)

# The VS multi-config generator sets LinkLibraryDependencies=false on
# Application targets, which strips transitive PUBLIC deps from the vcxproj.
# Enable the legacy behaviour so nimrtc_ice.lib's PUBLIC deps
# (nimrtc::core, nimrtc::plugins) appear in the link line.
set_target_properties(test_ice_test PROPERTIES
    VS_LINK_LIBRARY_DEPENDENCIES TRUE)

nimrtc_apply_options(test_ice_test)
nimrtc_link_libjuice(test_ice_test)

target_link_libraries(test_ice_test PRIVATE
    nimrtc::ice
    nimrtc::core
    nimrtc::plugins
    GTest::gtest_main
    GTest::gtest)

add_test(NAME modules_ice_tests_test_ice COMMAND test_ice_test)
set_tests_properties(modules_ice_tests_test_ice PROPERTIES TIMEOUT 30)
