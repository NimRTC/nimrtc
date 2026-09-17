# modules/video_sink/tests/CMakeLists.txt
# ----------------------------------------------------------------------------
# Headless / pinned sink tests (no network, no codec).
# ----------------------------------------------------------------------------
add_executable(test_video_sink test_video_sink.cpp)
target_link_libraries(test_video_sink PRIVATE
    nimrtc::video_sink
    GTest::gtest_main
    )
target_include_directories(test_video_sink PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${CMAKE_SOURCE_DIR}/src/core/include"
    "${CMAKE_SOURCE_DIR}/src/plugins/include")
if(WIN32)
    target_compile_definitions(test_video_sink PRIVATE _WIN32_WINNT=0x0601)
endif()
nimrtc_apply_options(test_video_sink) LENIENT
# See tests/CMakeLists.txt add_e2e_test() for the rationale on the COMMAND
# expression.  Just use $<TARGET_FILE> directly — the previous
# "Debug/" gen-expr prefix doubled the path under MSVC and made this
# test resolve to NOT_AVAILABLE.
add_test(NAME test_video_sink
    COMMAND $<TARGET_FILE:test_video_sink>)
set_tests_properties(test_video_sink PROPERTIES TIMEOUT 30)
if(COMMAND gtest_discover_tests)
    gtest_discover_tests(test_video_sink
        PROPERTIES TIMEOUT 30
        DISCOVERY_TIMEOUT 10)
endif()
