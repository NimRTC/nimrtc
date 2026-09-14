# modules/video_source/tests/CMakeLists.txt
add_executable(test_video_source
    test_video_source.cpp
    )

target_link_libraries(test_video_source
    PRIVATE
        nimrtc::video_source
        nimrtc::core
        nimrtc::video_frame
        GTest::gtest_main
    )

add_test(NAME video_source_tests
         COMMAND test_video_source)
