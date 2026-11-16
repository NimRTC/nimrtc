# modules/video_bitrate_control/tests/CMakeLists.txt
add_executable(test_video_bitrate_control
    test_video_bitrate_control.cpp
    )

target_link_libraries(test_video_bitrate_control
    PRIVATE
        nimrtc::video_bitrate_control
        nimrtc::core
        nimrtc::plugins
        GTest::gtest_main
    )

add_test(NAME video_bitrate_control_tests
         COMMAND test_video_bitrate_control)
