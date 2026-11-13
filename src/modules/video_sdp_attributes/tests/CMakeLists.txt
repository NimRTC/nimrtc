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

# LENIENT: the harness is test code, so warnings are reported but not
# fatal.  It also pulls in /utf-8 on MSVC, which the sources need --
# the comments carry typographic dashes and the default CP936 code page
# would otherwise mangle them (C4819).
nimrtc_apply_options(test_video_sdp_attributes LENIENT)

add_test(NAME video_sdp_attributes_tests
         COMMAND test_video_sdp_attributes)
