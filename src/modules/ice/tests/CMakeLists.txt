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

add_executable(test_ice_test test_ice.cpp)
add_executable(test_consent_freshness_test test_consent_freshness.cpp)

# The VS multi-config generator sets LinkLibraryDependencies=false on
# Application targets, which strips transitive PUBLIC deps from the vcxproj.
# Enable the legacy behaviour so nimrtc_ice.lib's PUBLIC deps
# (nimrtc::core, nimrtc::plugins) appear in the link line.
set_target_properties(test_ice_test PROPERTIES
    VS_LINK_LIBRARY_DEPENDENCIES TRUE)
set_target_properties(test_consent_freshness_test PROPERTIES
    VS_LINK_LIBRARY_DEPENDENCIES TRUE)

nimrtc_apply_options(test_ice_test) LENIENT
nimrtc_link_libjuice(test_ice_test)

nimrtc_apply_options(test_consent_freshness_test) LENIENT
nimrtc_link_libjuice(test_consent_freshness_test)

target_link_libraries(test_ice_test PRIVATE
    nimrtc::ice
    nimrtc::core
    nimrtc::plugins
    GTest::gtest_main
    GTest::gtest)

target_link_libraries(test_consent_freshness_test PRIVATE
    nimrtc::ice
    nimrtc::core
    nimrtc::plugins
    GTest::gtest_main
    GTest::gtest)

# NOTE: No top-level add_test() —see modules/audio3a/tests/CMakeLists.txt
# for the rationale (CMake 4.0 multi-config conditional bug).
if(COMMAND gtest_discover_tests)
    gtest_discover_tests(test_ice_test
        PROPERTIES TIMEOUT 30
        DISCOVERY_TIMEOUT 10)
    gtest_discover_tests(test_consent_freshness_test
        PROPERTIES TIMEOUT 30
        DISCOVERY_TIMEOUT 10)
endif()
