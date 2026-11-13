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
