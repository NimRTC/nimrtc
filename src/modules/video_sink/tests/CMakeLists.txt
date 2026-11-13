# modules/video_sink/tests/CMakeLists.txt
# ----------------------------------------------------------------------------
# Headless / pinned sink tests (no network, no codec) + the Windows
# D3D11 / DXGI renderer tests (remote-desktop plan E2 / E8).
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
nimrtc_apply_options(test_video_sink LENIENT)
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

# ---------------------------------------------------------------------------
# The Windows D3D11 / DXGI renderer (remote-desktop plan E2 / E8).
#
# The test runs the renderer's presentation clock and its latest-wins
# back-pressure policy.  It needs a Direct3D 11 device; on a machine
# without one the device-level cases *skip* (they do not fail — "this
# machine has no GPU" is not a renderer bug).
# ---------------------------------------------------------------------------
add_executable(test_d3d11_renderer test_d3d11_renderer.cpp)
target_link_libraries(test_d3d11_renderer PRIVATE
    nimrtc::video_sink
    GTest::gtest_main
    )
target_include_directories(test_d3d11_renderer PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${CMAKE_SOURCE_DIR}/src/core/include"
    "${CMAKE_SOURCE_DIR}/src/plugins/include")
if(WIN32)
    target_compile_definitions(test_d3d11_renderer PRIVATE _WIN32_WINNT=0x0A00)
endif()
nimrtc_apply_options(test_d3d11_renderer LENIENT)
add_test(NAME test_d3d11_renderer
    COMMAND $<TARGET_FILE:test_d3d11_renderer>)
set_tests_properties(test_d3d11_renderer PROPERTIES TIMEOUT 60)
if(COMMAND gtest_discover_tests)
    gtest_discover_tests(test_d3d11_renderer
        PROPERTIES TIMEOUT 60
        DISCOVERY_TIMEOUT 10)
endif()
