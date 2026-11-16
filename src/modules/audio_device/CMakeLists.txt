# modules/audio_device/CMakeLists.txt
# ----------------------------------------------------------------------------
# The audio-device module (remote-desktop plan Track F, F2).
#
# This module owns the *device* half of the audio path:
#
#   - the shared bounded ring buffer between the device thread and the
#     host (the same "bounded queue with an explicit drop policy" idea
#     the video-capture module's CaptureQueue implements),
#   - the PortAudio-backed capture and playback devices (the ONE
#     cross-platform backend RD-10 asks for, instead of three
#     hand-written platform backends),
#   - the synthetic device that makes the whole layer testable on a CI
#     machine with no sound card,
#   - and the device-loss (hot-unplug) path, which is a *clean error*,
#     never a crash.
#
# The *seam* half (F1) lives in src/plugins/:
#
#   - plugins/audio_device.hpp  (the shared device vocabulary)
#   - plugins/audio_source.hpp  (IAudioSource + IAudioSourceFactory)
#   - plugins/audio_sink.hpp    (IAudioSink + IAudioSinkFactory)
#
# Per Layout Invariant 3 (AGENTS.md): the module is self-contained --
# own CMakeLists.txt, include/, src/, tests/ -- and depends only on
# nimrtc::core, nimrtc::plugins (the seams it implements) and the
# vendored PortAudio.
#
# The engine wiring (plan F3 — reporting the played-out samples to the
# 3A's `set_render_delivered()`) is a SEPARATE item and lives in
# src/engine/.  This module only provides the substrate: the sink knows
# exactly how many samples it consumed.
# ----------------------------------------------------------------------------
set(NIMRTC_AUDIO_DEVICE_SOURCES
    src/audio_device.cpp
    # The playback-progress tracker (remote-desktop plan F3): the sink
    # reports how many samples it actually played out, so the 3A's AEC
    # reference stays aligned.
    src/playback_progress.cpp
    # The AEC-delay estimator (remote-desktop plan F4): the
    # loudspeaker -> microphone loop delay is measured (or configured).
    src/aec_delay.cpp
    # The two-source mixer (remote-desktop plan F6): the microphone and
    # the system audio, mixed or kept separate by configuration.
    src/mixer.cpp
    src/portaudio_backend.cpp
    src/resampler.cpp
    src/ring_buffer.cpp
    src/synthetic_backend.cpp
    # The system-audio (loopback) capture device (remote-desktop plan
    # F6): the factory, the device identity and the CI-testable
    # synthetic device, plus the platform half (WASAPI / Core Audio /
    # PulseAudio).
    src/system_audio.cpp
    src/system_audio_platform.cpp
    )

add_library(nimrtc_audio_device STATIC
    ${NIMRTC_AUDIO_DEVICE_SOURCES}
    )

nimrtc_apply_options(nimrtc_audio_device)
target_link_libraries(nimrtc_audio_device
    PUBLIC
        nimrtc::core
        nimrtc::plugins
    )

target_include_directories(nimrtc_audio_device
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

target_compile_definitions(nimrtc_audio_device
    PUBLIC NIMRTC_HAS_AUDIO_DEVICE=1)

# PortAudio (MIT, vendored).  The helper sets NIMRTC_USE_PORTAUDIO=1 or
# =0 depending on whether the vendored tree is available, so the module
# compiles its "no PortAudio backend" path when the submodule is not
# populated.
nimrtc_link_vendor_portaudio(nimrtc_audio_device)

# libsamplerate (BSD-2-Clause, vendored) — the sample-rate converter
# behind the module's `Resampler` (remote-desktop plan F5).  The helper
# sets NIMRTC_HAS_LIBSAMPLERATE=1 or =0 depending on whether the
# vendored tree is available, so the module compiles its pure-C++
# linear-interpolation fallback when the submodule is not populated or
# the vendored switch is off.  No system samplerate is ever linked (L5).
nimrtc_link_vendor_libsamplerate(nimrtc_audio_device)

# The system-audio (loopback) backend on Windows uses WASAPI directly
# (IMMDeviceEnumerator / IAudioClient with AUDCLNT_STREAMFLAGS_LOOPBACK).
# The import libraries are the Windows SDK's, not a third-party
# dependency, so L5 is unaffected.
if(WIN32)
    target_link_libraries(nimrtc_audio_device PRIVATE ole32)
endif()

add_library(nimrtc::audio_device ALIAS nimrtc_audio_device)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    # ensure a CTestTestfile.cmake gets generated at this level so ctest
    # can recurse into the tests/ subdir from src/modules/ (CMake 4.0 VS
    # generator quirk — same pattern as modules/rtp).
    enable_testing()
    add_subdirectory(tests)
endif()
