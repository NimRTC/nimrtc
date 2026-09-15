# webrtc_audio_processing/CMakeLists.txt
# =============================================================================
# WebRTC Audio Processing (APM) — vendored, pre-built via Meson.
#
# Upstream: https://gitlab.freedesktop.org/pulseaudio/webrtc-audio-processing
#           (also: https://chromium.googlesource.com/webrtc/modules/audio_processing)
# License:  BSD-3-Clause (same as WebRTC)
#
# Provides:
#   - AEC: Acoustic Echo Cancellation (desktop AEC3, mobile AECm)
#   - ANS: Ambient Noise Suppression (low/medium/high)
#   - AGC: Automatic Gain Control (fixed/digital/analog)
#   - VAD: Voice Activity Detection
#   - Transient Suppression (keyboard, mouse clicks)
#
# IMPORTANT: This module is pre-built separately with Meson (the upstream
# CMakeLists.txt is incomplete and Google uses GN). The canonical build
# script lives at tools/build_webrtc_apm.cmd (Windows) or
# tools/build_webrtc_apm.sh (Linux/macOS) and is invoked via:
#
#     python tools/fetch_webrtc_apm.py    # clone + meson build (one-shot)
#
# On Windows the legacy path build\build_webrtc_apm.cmd is also recognised
# by the Python helper but is gitignored and will be removed.  The
# audio3a module's CMakeLists only locates the resulting pre-built static
# libraries below; it does not invoke the build script itself.
#
# =============================================================================

cmake_minimum_required(VERSION 3.22)

# -----------------------------------------------------------------------------
# Guard: ensure the pre-built library tree exists
#
# The prebuilt is produced by Meson (GCC/Clang-style). Its archives use the
# `.a` suffix on every platform, regardless of MSVC's `CMAKE_STATIC_LIBRARY_SUFFIX`
# (which is `.lib`).  So look for `.a` first, then fall back to `.lib` for
# MSVC-native builds, then emit a clear error if neither exists.
# -----------------------------------------------------------------------------
set(_vendor_root "${CMAKE_CURRENT_SOURCE_DIR}/build_meson")
set(_apm_dir     "${_vendor_root}/webrtc/modules/audio_processing")

# Pick the first existing archive for each ABI variant.
set(_apm_lib "")
foreach(_cand
        "${_apm_dir}/libwebrtc-audio-processing-2.a"
        "${_apm_dir}/libwebrtc-audio-processing-2${CMAKE_STATIC_LIBRARY_SUFFIX}")
    if(NOT _apm_lib AND EXISTS "${_cand}")
        set(_apm_lib "${_cand}")
    endif()
endforeach()

set(_apm_privatearch_lib "")
foreach(_cand
        "${_apm_dir}/libwebrtc_audio_processing_privatearch.a"
        "${_apm_dir}/libwebrtc_audio_processing_privatearch${CMAKE_STATIC_LIBRARY_SUFFIX}")
    if(NOT _apm_privatearch_lib AND EXISTS "${_cand}")
        set(_apm_privatearch_lib "${_cand}")
    endif()
endforeach()

set(_abs_root    "${_vendor_root}/subprojects/abseil-cpp-20240722.0")
set(_include_root "${CMAKE_CURRENT_SOURCE_DIR}/src")

if(NOT _apm_lib)
    # The prebuilt static library (.a / .lib) is not present.  This is the
    # normal case on CI runners and in source-only clones.  Behaviour is
    # controlled by NIMRTC_VENDORED_WEBRTC_APM (default ON):
    #
    #   * NIMRTC_VENDORED_WEBRTC_APM=ON  -> the user explicitly opted in to
    #     the vendored full implementation, so the missing build artifact
    #     IS an error.  Bail out with FATAL_ERROR plus actionable guidance.
    #
    #   * NIMRTC_VENDORED_WEBRTC_APM=OFF -> the user opted out (CI default,
    #     consumers that don't need 3A).  Define a no-op INTERFACE target so
    #     downstream `target_link_libraries(... nimrtc_vendor_webrtc_apm)`
    #     keeps working, and emit a STATUS message so the user sees why
    #     audio3a will run as the built-in stub.
    if(NIMRTC_VENDORED_WEBRTC_APM)
        message(FATAL_ERROR
            "WebRTC APM pre-built library not found.  Looked for any of:\n"
            "  ${_apm_dir}/libwebrtc-audio-processing-2.a\n"
            "  ${_apm_dir}/libwebrtc-audio-processing-2${CMAKE_STATIC_LIBRARY_SUFFIX}\n"
            "\n"
            "Run the build script first:\n"
            "  Windows: build\\build_webrtc_apm.cmd\n"
            "  Linux:   ./build/build_webrtc_apm.sh\n"
            "\n"
            "The script:\n"
            "  1. Clones https://gitlab.freedesktop.org/pulseaudio/webrtc-audio-processing\n"
            "  2. Configures with meson (with abseil-cpp subproject)\n"
            "  3. Builds the static library + abseil-cpp")
    else()
        message(STATUS
            "WebRTC APM pre-built library not found, and "
            "NIMRTC_VENDORED_WEBRTC_APM=OFF.  Defining a stub "
            "nimrtc_vendor_webrtc_apm INTERFACE target so audio3a can "
            "still link; AEC/ANS/AGC/VAD will fall back to the built-in "
            "stub implementation.  Run `python tools/fetch_webrtc_apm.py` "
            "and reconfigure with -DNIMRTC_VENDORED_WEBRTC_APM=ON to "
            "enable real 3A processing.")
        if(NOT TARGET nimrtc_vendor_webrtc_apm)
            add_library(nimrtc_vendor_webrtc_apm INTERFACE)
        endif()
        if(NOT TARGET nimrtc::vendor::webrtc_apm)
            add_library(nimrtc::vendor::webrtc_apm ALIAS nimrtc_vendor_webrtc_apm)
        endif()
        # Stop here — the rest of this file defines the real imported
        # target and would crash if we let it run without an .a path.
        return()
    endif()
endif()

# -----------------------------------------------------------------------------
# Project
# -----------------------------------------------------------------------------
project(nimrtc_webrtc_apm_wrapper LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# -----------------------------------------------------------------------------
# Discover all abseil-cpp static libraries (built as part of the meson build)
# -----------------------------------------------------------------------------
file(GLOB _absl_libs
    "${_abs_root}/libabsl_*.a"
    "${_abs_root}/libabsl_*/*.a"
)

if(NOT _absl_libs)
    message(WARNING
        "No abseil-cpp static libraries found under ${_abs_root}. "
        "The build may fail to link consumers of nimrtc_vendor_webrtc_apm.")
endif()

# -----------------------------------------------------------------------------
# Create the wrapper interface target that exposes the upstream static libs
# and required include directories / compile definitions.
# -----------------------------------------------------------------------------
add_library(nimrtc_vendor_webrtc_apm STATIC IMPORTED GLOBAL)
add_library(nimrtc::vendor::webrtc_apm ALIAS nimrtc_vendor_webrtc_apm)

# Locate the apm impl library (and its AVX2 privatearch sibling if present)
set(_imported_libraries "${_apm_lib}")
if(_apm_privatearch_lib)
    list(APPEND _imported_libraries "${_apm_privatearch_lib}")
endif()

# Append all abseil static libraries so transitive linking works
list(APPEND _imported_libraries ${_absl_libs})

set_target_properties(nimrtc_vendor_webrtc_apm PROPERTIES
    IMPORTED_LOCATION             "${_apm_lib}"
    INTERFACE_INCLUDE_DIRECTORIES "${_include_root}/webrtc"
    INTERFACE_COMPILE_DEFINITIONS "WEBRTC_POSIX=1;WEBRTC_WIN=1;WEBRTC_AUDIO_PROCESSING_ONLY_BUILD=1"
    INTERFACE_LINK_LIBRARIES      "${_imported_libraries}"
)

# On Windows the WinSock library is required by the upstream code
if(WIN32)
    target_link_libraries(nimrtc_vendor_webrtc_apm INTERFACE ws2_32)
    target_link_libraries(nimrtc_vendor_webrtc_apm INTERFACE winmm)
    set_target_properties(nimrtc_vendor_webrtc_apm PROPERTIES
        INTERFACE_COMPILE_DEFINITIONS "WEBRTC_WIN=1")

    # The vendored libwebrtc-audio-processing-2.a was built with MinGW/gcc
    # (Release, MD_DynamicRelease, _ITERATOR_DEBUG_LEVEL=0). When MSVC
    # links it into a Debug/MDd consumer it trips LNK2038 mismatch errors.
    # Match the runtime the .a was built with so the link succeeds.
    if(MSVC)
        target_compile_options(nimrtc_vendor_webrtc_apm INTERFACE
            /MD                          # multi-threaded DLL runtime (not /MDd)
            /wd4068                      # unknown pragma warnings from upstream
            /wd4244 /wd4267 /wd4305      # narrowing conversions in upstream code
            /wd4800 /wd4996)             # forcing-value-to-bool / deprecation
        target_compile_definitions(nimrtc_vendor_webrtc_apm INTERFACE
            _ITERATOR_DEBUG_LEVEL=0
            _SECURE_SCL=0
            _HAS_ITERATOR_DEBUGGING=0)
        # The static .a may carry its own MSVCRT.lib reference; remove the
        # default CRT lib so MSVC picks the same CRT used by /MD above.
        set_target_properties(nimrtc_vendor_webrtc_apm PROPERTIES
            INTERFACE_LINK_OPTIONS "/NODEFAULTLIB:libcmt.lib")
    endif()
endif()

# SIMD hints — the apm static lib is already built with the right defines
# for the host architecture; we just forward AVX2 hints to consumers.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|AMD64|x86|i.86)")
    set_target_properties(nimrtc_vendor_webrtc_apm PROPERTIES
        INTERFACE_COMPILE_DEFINITIONS "WEBRTC_USE_SSE2=1;WEBRTC_ENABLE_AVX2=1")
endif()
