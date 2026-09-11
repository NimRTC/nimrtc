# modules/audio3a/CMakeLists.txt
# ----------------------------------------------------------------------------
# Audio 3A processing module.
#
# Scope (P1):
#   - NullAudio3A stub for testing
#   - WebRtcAudio3A wrapper (requires NIMRTC_VENDORED_WEBRTC_APM=ON)
#   - Metrics: VAD, TX/RX levels
#
# P1 future:
#   - SpeexDSP alternative
#
# Dependencies:
#   - core (always)
#   - nimrtc_vendor_webrtc_apm (if NIMRTC_VENDORED_WEBRTC_APM=ON and source populated)
# ----------------------------------------------------------------------------
add_library(nimrtc_audio3a STATIC
    src/audio3a.cpp
    src/audio3a_plugin.cpp     # adapter wrapping concrete → plugins::IAudio3A
    src/webrtc_audio3a.cpp     # WebRTC APM wrapper (stub implementation)
    )

nimrtc_apply_options(nimrtc_audio3a)

# audio3a contains numerical conversion code (float/double arithmetic,
# fixed-point conversions) and log-format calls that the project's
# strict warning set rejects.  These are deliberate, internal
# conversions in DSP code where the value ranges are known — relax
# the strict flags for this module only on non-MSVC compilers.
if(NOT MSVC)
    target_compile_options(nimrtc_audio3a PRIVATE
        -Wno-double-promotion
        -Wno-sign-conversion
        -Wno-conversion)
endif()
target_link_libraries(nimrtc_audio3a
    PUBLIC
        nimrtc::core
        nimrtc::plugins       # plugin interface
    PRIVATE
    )

# Link WebRTC APM if available
# The vendor wrapper creates a stub that returns nullptr when source not populated,
# so this is safe to link unconditionally. The actual 3A processing only happens
# when webrtc_audio_processing source is present.
# NOTE: webrtc_audio_processing's CMakeLists.txt lives at the top of its tree
# (unlike libopus/libsrtp which use src/CMakeLists.txt), so we explicitly add
# it here instead of relying on src/third_party/CMakeLists.txt.
if(NIMRTC_VENDORED_WEBRTC_APM)
    if(EXISTS "${CMAKE_SOURCE_DIR}/src/third_party/webrtc_audio_processing/CMakeLists.txt")
        # Force-load the vendor CMakeLists (the global helper at
        # src/third_party/CMakeLists.txt only triggers when src/CMakeLists.txt
        # exists inside the vendor tree, which is not the layout here).
        if(NOT TARGET nimrtc_vendor_webrtc_apm)
            add_subdirectory(${CMAKE_SOURCE_DIR}/src/third_party/webrtc_audio_processing
                            ${CMAKE_BINARY_DIR}/_vendor_webrtc_apm_build EXCLUDE_FROM_ALL)
        endif()
        target_link_libraries(nimrtc_audio3a PRIVATE nimrtc_vendor_webrtc_apm)
        target_compile_definitions(nimrtc_audio3a PRIVATE NIMRTC_USE_WEBRTC_APM=1)
        # Forward WebRTC APM INTERFACE include dirs from the imported target so
        # <api/audio/audio_processing.h> resolves in this compilation unit.
        target_include_directories(nimrtc_audio3a
            PRIVATE
                $<TARGET_PROPERTY:nimrtc_vendor_webrtc_apm,INTERFACE_INCLUDE_DIRECTORIES>)
        # The vendored libwebrtc-audio-processing-2.a was built with MinGW/gcc
        # (Release, MD_DynamicRelease, _ITERATOR_DEBUG_LEVEL=0). When MSVC
        # links it into a Debug/MDd consumer it trips LNK2038 mismatch errors.
        # Match the runtime the .a was built with so the link succeeds.
        # Flags propagate to downstream consumers (tests) of nimrtc_audio3a
        # so any test executable that links this library also gets them.
        if(MSVC)
            # Don't override the runtime library here — let the consumer's
            # configuration (Debug/Release) choose.  The .a archive was built
            # with MinGW/gcc (MD_DynamicRelease, _ITERATOR_DEBUG_LEVEL=0), so
            # consumers MUST build in Release configuration.  Tests are wired
            # up to do so via NIMRTC_TEST_CONFIGURATION_TYPE.
            #
            # We still need to suppress:
            #   - LNK4098 (default-lib MSVCRT vs MSVCRTD conflict)
            #   - upstream-code warnings (narrowing, deprecation, ...)
            target_compile_options(nimrtc_audio3a
                PUBLIC /wd4068 /wd4244 /wd4267 /wd4305 /wd4800 /wd4996)
            target_compile_definitions(nimrtc_audio3a
                PUBLIC _ITERATOR_DEBUG_LEVEL=0
                       _SECURE_SCL=0
                       _HAS_ITERATOR_DEBUGGING=0)
            set_target_properties(nimrtc_audio3a PROPERTIES
                INTERFACE_LINK_OPTIONS "/NODEFAULTLIB:libcmt.lib")
        endif()
    endif()
endif()

target_include_directories(nimrtc_audio3a
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include
        # plugins/audio3a.hpp → core/registry.hpp → plugins/video_codec.hpp →
        # video_frame/frame.hpp.  video_frame's public include dir should
        # already propagate via nimrtc::plugins (which PUBLICly links
        # nimrtc::video_frame), but be explicit to keep MSVC include paths
        # intact in case transitive propagation gets pruned by generator
        # ordering (e.g. when add_subdirectory is used to load the WebRTC APM
        # vendor).
        ${CMAKE_SOURCE_DIR}/src/modules/video_frame/include)

target_compile_definitions(nimrtc_audio3a PUBLIC NIMRTC_HAS_AUDIO3A=1)

add_library(nimrtc::audio3a ALIAS nimrtc_audio3a)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
