# modules/video_sink/tests/CMakeLists.txt
# ----------------------------------------------------------------------------
# Headless / pinned sink tests (no network, no codec) + the presentation
# renderer tests (remote-desktop plan E2 / E3 / E4 / E7 / E8).
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
# expression.  Just use $<TARGET_FILE> directly -- the previous
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
# without one the device-level cases *skip* (they do not fail -- "this
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

# ---------------------------------------------------------------------------
# The portable presenter core (remote-desktop plan E2 / E3 / E4 / E8).
#
# The core is the *platform-neutral* half of every renderer: the
# latest-wins mailbox, the presentation clock and the drop accounting.
# It has no GPU / window-system dependency, so it is tested on every
# platform -- including the Windows CI leg -- with a fake `present()`
# callback.  That is what covers the macOS and Linux adapters' timing
# behaviour on a host that cannot run them.
# ---------------------------------------------------------------------------
add_executable(test_presenter_core test_presenter_core.cpp)
target_link_libraries(test_presenter_core PRIVATE
    nimrtc::video_sink
    GTest::gtest_main
    )
target_include_directories(test_presenter_core PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${CMAKE_SOURCE_DIR}/src/core/include"
    "${CMAKE_SOURCE_DIR}/src/plugins/include")
nimrtc_apply_options(test_presenter_core LENIENT)
add_test(NAME test_presenter_core
    COMMAND $<TARGET_FILE:test_presenter_core>)
set_tests_properties(test_presenter_core PROPERTIES TIMEOUT 60)
if(COMMAND gtest_discover_tests)
    gtest_discover_tests(test_presenter_core
        PROPERTIES TIMEOUT 60
        DISCOVERY_TIMEOUT 10)
endif()

# ---------------------------------------------------------------------------
# The portable scale-to-fit helper (remote-desktop plan E7).
#
# A pure function -- source size, target size, mode in; destination
# rectangle out.  No display, no GPU, no platform SDK.
# ---------------------------------------------------------------------------
add_executable(test_scale_to_fit test_scale_to_fit.cpp)
target_link_libraries(test_scale_to_fit PRIVATE
    nimrtc::video_sink
    GTest::gtest_main
    )
target_include_directories(test_scale_to_fit PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${CMAKE_SOURCE_DIR}/src/core/include"
    "${CMAKE_SOURCE_DIR}/src/plugins/include")
nimrtc_apply_options(test_scale_to_fit LENIENT)
add_test(NAME test_scale_to_fit
    COMMAND $<TARGET_FILE:test_scale_to_fit>)
set_tests_properties(test_scale_to_fit PROPERTIES TIMEOUT 30)
if(COMMAND gtest_discover_tests)
    gtest_discover_tests(test_scale_to_fit
        PROPERTIES TIMEOUT 30
        DISCOVERY_TIMEOUT 10)
endif()

# ---------------------------------------------------------------------------
# The A/V sync decision (remote-desktop plan E5) and the displayed-frame
# tracker (plan E9).
#
# Both are pure bookkeeping -- no GPU, no display, no platform SDK -- so
# they are exercised on every CI leg, including the ones with no screen.
# ---------------------------------------------------------------------------
add_executable(test_video_sync test_video_sync.cpp)
target_link_libraries(test_video_sync PRIVATE
    nimrtc::video_sink
    GTest::gtest_main
    )
target_include_directories(test_video_sync PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${CMAKE_SOURCE_DIR}/src/core/include"
    "${CMAKE_SOURCE_DIR}/src/plugins/include")
nimrtc_apply_options(test_video_sync LENIENT)
add_test(NAME test_video_sync
    COMMAND $<TARGET_FILE:test_video_sync>)
set_tests_properties(test_video_sync PROPERTIES TIMEOUT 30)
if(COMMAND gtest_discover_tests)
    gtest_discover_tests(test_video_sync
        PROPERTIES TIMEOUT 30
        DISCOVERY_TIMEOUT 10)
endif()
