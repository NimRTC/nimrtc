# modules/audio3a/tests/CMakeLists.txt
# =============================================================================
# Audio3A plugin adapter smoke tests.
#
# Verifies:
#   - NullPluginFactory registers under id "webrtc"
#   - factory->create() returns a PluginAdapter wrapping NullAudio3A
#   - adapter implements plugins::IAudio3A lifecycle (open / close)
#   - process_capture / process_render round-trip (null stub = pass-through)
#   - stats() returns sane values
#   - callbacks are invoked after process_capture
#
# Uses GoogleTest (FetchContent if not installed).
# =============================================================================

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

add_executable(test_audio3a_plugin test_audio3a_plugin.cpp)

set_target_properties(test_audio3a_plugin PROPERTIES
    VS_LINK_LIBRARY_DEPENDENCIES TRUE)

nimrtc_apply_options(test_audio3a_plugin)

target_link_libraries(test_audio3a_plugin PRIVATE
    nimrtc::audio3a
    nimrtc::core
    nimrtc::plugins
    GTest::gtest_main
    GTest::gtest)

add_test(NAME modules_audio3a_tests_plugin COMMAND test_audio3a_plugin)
set_tests_properties(modules_audio3a_tests_plugin PROPERTIES TIMEOUT 30)
