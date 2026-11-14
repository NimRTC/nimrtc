# modules/video_capture/tests/CMakeLists.txt
add_executable(test_video_capture
    test_video_capture.cpp
    )

target_link_libraries(test_video_capture
    PRIVATE
        nimrtc::video_capture
        nimrtc::core
        nimrtc::plugins
        GTest::gtest_main
    )

add_test(NAME video_capture_tests
         COMMAND test_video_capture)

# ---------------------------------------------------------------------------
# The Windows backends' tests (remote-desktop plan A3, M1-c).
#
# They are a separate binary because they need the Windows platform
# libraries (d3d11 / dxgi / windowsapp) that the portable test does not
# link, and because they are only meaningful on a Windows host.
# ---------------------------------------------------------------------------
if(WIN32)
    add_executable(test_windows_capture
        test_windows_capture.cpp
        )

    target_link_libraries(test_windows_capture
        PRIVATE
            nimrtc::video_capture
            nimrtc::core
            nimrtc::plugins
            GTest::gtest_main
        )

    add_test(NAME windows_capture_tests
             COMMAND test_windows_capture)
endif()

# ---------------------------------------------------------------------------
# The portable capture core's tests (remote-desktop plan A4 / A5 / A7 /
# A8).
#
# These run on EVERY platform: the core is portable by construction, and
# the base class is exercised with a fake platform implementation.  That
# is what makes the macOS and Linux adapters verifiable from the Windows
# CI leg -- the mechanism they delegate to is pinned down here.
# ---------------------------------------------------------------------------
add_executable(test_unix_capture_core
    test_unix_capture_core.cpp
    )

target_link_libraries(test_unix_capture_core
    PRIVATE
        nimrtc::video_capture
        nimrtc::core
        nimrtc::plugins
        GTest::gtest_main
    )

add_test(NAME unix_capture_core_tests
         COMMAND test_unix_capture_core)

# ---------------------------------------------------------------------------
# The cursor-compositing layer's tests (remote-desktop plan A6).
#
# These run on EVERY platform: the compositor is portable by
# construction, and the platform *sources* (GetCursorInfo + DrawIconEx
# on Windows, XFixesGetCursorImage on X11) are thin adapters over it.
# The tests use a fake cursor source, so they need no display server.
# ---------------------------------------------------------------------------
add_executable(test_cursor_compositor
    test_cursor_compositor.cpp
    )

target_link_libraries(test_cursor_compositor
    PRIVATE
        nimrtc::video_capture
        nimrtc::core
        nimrtc::plugins
        GTest::gtest_main
    )

add_test(NAME cursor_compositor_tests
         COMMAND test_cursor_compositor)

# ---------------------------------------------------------------------------
# The camera capture layer's tests (remote-desktop plan A9).
#
# These run on EVERY platform too: the camera *layer* is portable, and
# the synthetic camera needs no hardware and no permission prompt, so
# the camera path is verifiable in CI on every leg.
# ---------------------------------------------------------------------------
add_executable(test_camera_capture
    test_camera_capture.cpp
    )

target_link_libraries(test_camera_capture
    PRIVATE
        nimrtc::video_capture
        nimrtc::core
        nimrtc::plugins
        GTest::gtest_main
    )

add_test(NAME camera_capture_tests
         COMMAND test_camera_capture)
