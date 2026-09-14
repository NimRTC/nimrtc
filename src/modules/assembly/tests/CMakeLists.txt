# modules/assembly/tests/CMakeLists.txt
# =============================================================================
# Assembly module unit tests.
#
# Verifies:
#   - ProfileRegistry singleton and default profile registration
#   - ProfileRegistry::find() returns correct profiles
#   - ProfileRegistry::all_names() includes all built-in profiles
#   - ProfileRegistry::register_profile() overwrite semantics
#   - Builder.load_profile() + override chain
#   - JSON round-trip (profile_to_json_string →profile_from_json_file)
#   - Per-profile invariants (empty plugin names, feature flags)
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

add_executable(test_assembly test_assembly.cpp)

# Suppress MSVC C4996 (deprecated getenv) —std::getenv is portable and
# intentionally used here to locate the build dir.
target_compile_definitions(test_assembly PRIVATE _CRT_SECURE_NO_WARNINGS)

set_target_properties(test_assembly PROPERTIES
    VS_LINK_LIBRARY_DEPENDENCIES TRUE)

nimrtc_apply_options(test_assembly)

target_link_libraries(test_assembly PRIVATE
    nimrtc::assembly
    nimrtc::core
    nimrtc::plugins
    GTest::gtest_main
    GTest::gtest)

# Copy JSON profiles into the build directory for the tests.
# Ensure target directory exists —configure_file's COPYONLY mode doesn't
# create missing parent directories, which silently skips the copy on
# fresh builds.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/profiles")
# Source layout: src/modules/assembly/tests/CMakeLists.txt →profiles/ is at
# the repo root, i.e. ../../../../profiles/ from this file.
file(GLOB profile_jsons "${CMAKE_CURRENT_SOURCE_DIR}/../../../../profiles/*.json")
foreach(json_file IN LISTS profile_jsons)
    get_filename_component(json_name "${json_file}" NAME)
    configure_file(
        "${json_file}"
        "${CMAKE_BINARY_DIR}/profiles/${json_name}"
        COPYONLY)
endforeach()

add_test(NAME modules_assembly_tests COMMAND test_assembly)
# Run tests from the build directory so that profile_path()'s fallback
# ("."-relative path) resolves to ${CMAKE_BINARY_DIR}/profiles/.
set_tests_properties(modules_assembly_tests PROPERTIES
    TIMEOUT 30
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
