# modules/video_sdp_attributes/tests/CMakeLists.txt
add_executable(test_video_sdp_attributes
    test_video_sdp_attributes.cpp
    )

target_link_libraries(test_video_sdp_attributes
    PRIVATE
        nimrtc::video_sdp_attributes
        nimrtc::core
        GTest::gtest_main
    )

add_test(NAME video_sdp_attributes_tests
         COMMAND test_video_sdp_attributes)
