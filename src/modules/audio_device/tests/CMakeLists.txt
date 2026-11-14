# modules/audio_device/tests/CMakeLists.txt
add_executable(test_audio_device
    test_audio_device.cpp
    )

target_link_libraries(test_audio_device
    PRIVATE
        nimrtc::audio_device
        nimrtc::core
        nimrtc::plugins
        GTest::gtest_main
    )

nimrtc_apply_options(test_audio_device LENIENT)

add_test(NAME audio_device_tests
         COMMAND test_audio_device)
