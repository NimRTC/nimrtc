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
