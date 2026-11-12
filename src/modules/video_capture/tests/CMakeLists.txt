# modules/video_capture/tests/CMakeLists.txt
add_executable(test_video_capture
    test_video_capture.cpp
    test_capture_source_adapter.cpp
    test_cursor_compositor.cpp
    test_dxgi_desktop_dup.cpp
    test_win_cursor_source.cpp
    test_wgc_capture.cpp
    test_linux_capture.cpp
    test_macos_capture.cpp
    )

target_link_libraries(test_video_capture
    PRIVATE
        nimrtc::video_capture
        nimrtc::core
        nimrtc::plugins
        GTest::gtest_main
    )

# LENIENT: the harness is test code, so warnings are reported but not
# fatal.  It also pulls in /utf-8 on MSVC, which the sources need --
# the comments carry typographic dashes and the default CP936 code page
# would otherwise mangle them (C4819).
nimrtc_apply_options(test_video_capture LENIENT)

add_test(NAME video_capture_tests
         COMMAND test_video_capture)
