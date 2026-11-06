# modules/audio_rx/CMakeLists.txt
# ----------------------------------------------------------------------------
# Audio receive pipeline module (v1.2.0, audio pipeline).
#
# The module owns the receive-side audio pipeline: jitter buffer, decoder,
# concealment, FEC recovery, downmix and the playout clock.  It is
# self-contained (L3): it depends on nimrtc::core and nimrtc::plugins only.
#
# The jitter buffer and the decoder are *injected* through the plugin seams
# (plugins::IJB and plugins::IAudioDecoder) and are resolved through
# core::PluginRegistry -- the module therefore has NO dependency on
# src/modules/jb or src/modules/opus.
#
# The engine reaches this module only through plugins::IAudioReceiver
# (registered under the id "nimrtc_audio_rx").
# ----------------------------------------------------------------------------
add_library(nimrtc_audio_rx STATIC
    src/receiver.cpp
    src/receiver_helpers.cpp
    src/receiver_plugin.cpp        # adapter wrapping concrete -> plugins::IAudioReceiver
    )

nimrtc_apply_options(nimrtc_audio_rx)

target_link_libraries(nimrtc_audio_rx
    PUBLIC
        nimrtc::core
        nimrtc::plugins       # plugin interface (for IJB / IAudioDecoder / IAudioReceiver)
    )

target_include_directories(nimrtc_audio_rx
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

target_compile_definitions(nimrtc_audio_rx PUBLIC NIMRTC_HAS_AUDIO_RX=1)

add_library(nimrtc::audio_rx ALIAS nimrtc_audio_rx)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
