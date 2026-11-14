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

# The resampler's tests (remote-desktop plan F5).  They are a separate
# binary so a resampler failure does not hide the device-layer results,
# and so the F5 acceptance criteria can be run on their own:
#   ctest -R resampler
add_executable(test_resampler
    test_resampler.cpp
    )

target_link_libraries(test_resampler
    PRIVATE
        nimrtc::audio_device
        nimrtc::core
        nimrtc::plugins
        GTest::gtest_main
    )

nimrtc_apply_options(test_resampler LENIENT)

add_test(NAME resampler_tests
         COMMAND test_resampler)

# The two-source mixer's tests (remote-desktop plan F6).  They are a
# separate binary so a mixer failure does not hide the device-layer
# results, and so the F6 acceptance criteria can be run on their own:
#   ctest -R mixer
add_executable(test_mixer
    test_mixer.cpp
    )

target_link_libraries(test_mixer
    PRIVATE
        nimrtc::audio_device
        nimrtc::core
        nimrtc::plugins
        GTest::gtest_main
    )

nimrtc_apply_options(test_mixer LENIENT)

add_test(NAME mixer_tests
         COMMAND test_mixer)

# The system-audio (loopback) capture device's tests (remote-desktop
# plan F6).  They are a separate binary because the F6 acceptance
# criterion spans both the factory and the synthetic device:
#   ctest -R system_audio
add_executable(test_system_audio
    test_system_audio.cpp
    )

target_link_libraries(test_system_audio
    PRIVATE
        nimrtc::audio_device
        nimrtc::core
        nimrtc::plugins
        GTest::gtest_main
    )

nimrtc_apply_options(test_system_audio LENIENT)

add_test(NAME system_audio_tests
         COMMAND test_system_audio)

# The playback-progress tracker's tests (remote-desktop plan F3).  They
# are a separate binary so a tracker failure does not hide the
# device-layer results, and so the F3 acceptance criteria can be run on
# their own:
#   ctest -R playback_progress
add_executable(test_playback_progress
    test_playback_progress.cpp
    )

target_link_libraries(test_playback_progress
    PRIVATE
        nimrtc::audio_device
        nimrtc::core
        nimrtc::plugins
        GTest::gtest_main
    )

nimrtc_apply_options(test_playback_progress LENIENT)

add_test(NAME playback_progress_tests
         COMMAND test_playback_progress)

# The AEC-delay estimator's tests (remote-desktop plan F4).  They are a
# separate binary so an estimator failure does not hide the device-layer
# results, and so the F4 acceptance criteria can be run on their own:
#   ctest -R aec_delay
add_executable(test_aec_delay
    test_aec_delay.cpp
    )

target_link_libraries(test_aec_delay
    PRIVATE
        nimrtc::audio_device
        nimrtc::core
        nimrtc::plugins
        GTest::gtest_main
    )

nimrtc_apply_options(test_aec_delay LENIENT)

add_test(NAME aec_delay_tests
         COMMAND test_aec_delay)
