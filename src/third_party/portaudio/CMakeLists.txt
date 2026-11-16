# third_party/portaudio/CMakeLists.txt
# =============================================================================
# PortAudio — the cross-platform audio I/O library (MIT licence).
#
# Vendored from: https://github.com/PortAudio/portaudio (master) under src/.
#
# WHY A HAND-WRITTEN WRAPPER
# --------------------------
# Upstream *does* ship a CMakeLists.txt (added in 2024), but it is a
# stand-alone project build: it calls `project()`, it defines its own
# options (PA_USE_*, PA_BUILD_SHARED_LIBS, ...), it registers ctest
# entries, and it installs its own package config.  Entering it with
# `add_subdirectory()` would:
#
#   - override the NimRTC build's `CMAKE_C_FLAGS` / `CMAKE_CXX_FLAGS`
#     in the DSound path (upstream rewrites /MD -> /MT for its DLL),
#   - pollute the global option namespace with `PA_*` cache entries,
#   - and pull upstream's `install()` rules into the NimRTC bundle.
#
# So this wrapper compiles the upstream sources *directly* with CMake and
# exposes the same target name every other NimRTC vendor uses:
#
#     nimrtc_vendor_portaudio   (alias nimrtc::vendor::portaudio)
#
# WHAT IS COMPILED
# ----------------
# The upstream tree is split into three layers:
#
#   src/common/*.c   the portable core (converters, the process loop,
#                    the ring buffer, pa_front.c — the public API)
#   src/os/<plat>/*  the platform glue (host-API table, CoInitialize,
#                    the Unix pthread helpers)
#   src/hostapi/*    one directory per backend (WASAPI, MME, DSound,
#                    ASIO, Core Audio, ALSA, PulseAudio, JACK, ...)
#
# The host-API table (`paHostApiInitializers[]`) is compiled with the
# `PA_USE_<X>` switches, so exactly the backends whose sources are in
# the source list get registered.  A backend whose switch is 0 is not
# compiled at all — there is no "disabled but present" state, which is
# what keeps the binary small and the link line honest.
#
# THE PLATFORM MATRIX (policy K8b)
# --------------------------------
#   Windows (MSVC)  — WASAPI (shared), MME (waveIn/waveOut),
#                     DirectSound.  ASIO is deliberately OFF: the
#                     Steinberg ASIO SDK may not be redistributed, and
#                     upstream's own CMake downloads it at configure
#                     time — neither is acceptable inside NimRTC's
#                     vendored build.  WDM-KS is OFF for the same
#                     reason (it adds a KS driver dependency for a
#                     backend WASAPI already covers).
#   macOS (Clang)   — Core Audio.
#   Linux (GCC)     — ALSA and PulseAudio, each enabled only when the
#                     corresponding development package is present on
#                     the build host (find_package); JACK is OFF
#                     (libjack is not a NimRTC dependency).
#
# No system audio library is *required*: when no backend is available
# the library still compiles and PortAudio simply reports "no host
# API" at runtime — the NimRTC module then reports the device layer
# as unavailable instead of failing the build.
#
# LICENCE
# -------
# MIT (see src/third_party/portaudio/src/LICENSE.txt).
# =============================================================================

set(_pa_dir "${CMAKE_CURRENT_SOURCE_DIR}/src")

# The vendored switch is the "use the vendored source build" contract.
# There is NO system fallback (L5 forbids linking a system audio
# library), so switching it off means the audio-device module simply
# has no PortAudio backend — the synthetic backend stays in charge.
if(NOT NIMRTC_VENDORED_PORTAUDIO)
    add_library(nimrtc_vendor_portaudio INTERFACE)
    add_library(nimrtc::vendor::portaudio ALIAS nimrtc_vendor_portaudio)
    target_compile_definitions(nimrtc_vendor_portaudio INTERFACE NIMRTC_USE_PORTAUDIO=0)
    return()
endif()

# The submodule is not populated: declare an empty stub so the configure
# step succeeds.  A consumer that links it fails at link time with a
# clear "unresolved external" rather than a CMake error.
if(NOT EXISTS "${_pa_dir}/src/common/pa_front.c")
    add_library(nimrtc_vendor_portaudio INTERFACE)
    add_library(nimrtc::vendor::portaudio ALIAS nimrtc_vendor_portaudio)
    target_compile_definitions(nimrtc_vendor_portaudio INTERFACE NIMRTC_USE_PORTAUDIO=0)
    message(STATUS "  PortAudio          : vendored submodule not populated — "
                   "the audio-device module has no PortAudio backend")
    return()
endif()

set(_pa_sources
    # ---- portable core ----
    "${_pa_dir}/src/common/pa_allocation.c"
    "${_pa_dir}/src/common/pa_converters.c"
    "${_pa_dir}/src/common/pa_cpuload.c"
    "${_pa_dir}/src/common/pa_debugprint.c"
    "${_pa_dir}/src/common/pa_dither.c"
    "${_pa_dir}/src/common/pa_front.c"
    "${_pa_dir}/src/common/pa_process.c"
    "${_pa_dir}/src/common/pa_ringbuffer.c"
    "${_pa_dir}/src/common/pa_stream.c"
    "${_pa_dir}/src/common/pa_trace.c"
)

set(_pa_includes
    "${_pa_dir}/include"
    "${_pa_dir}/src/common"
)

set(_pa_defs
    # Upstream's own `PA_USE_*` switches.  A backend that is not listed
    # here is not compiled at all.
    PA_USE_ASIO=0
    PA_USE_WDMKS=0
    PA_USE_SKELETON=0
)

# -----------------------------------------------------------------------------
# Per-platform source selection.
# -----------------------------------------------------------------------------
if(WIN32)
    list(APPEND _pa_sources
        # ---- Win32 glue ----
        "${_pa_dir}/src/os/win/pa_win_coinitialize.c"
        "${_pa_dir}/src/os/win/pa_win_hostapis.c"
        "${_pa_dir}/src/os/win/pa_win_util.c"
        "${_pa_dir}/src/os/win/pa_win_version.c"
        "${_pa_dir}/src/os/win/pa_win_waveformat.c"
        # ---- host APIs ----
        "${_pa_dir}/src/hostapi/wasapi/pa_win_wasapi.c"
        "${_pa_dir}/src/hostapi/wmme/pa_win_wmme.c"
        "${_pa_dir}/src/hostapi/dsound/pa_win_ds.c"
        "${_pa_dir}/src/hostapi/dsound/pa_win_ds_dynlink.c"
    )
    list(APPEND _pa_includes
        "${_pa_dir}/src/os/win"
        "${_pa_dir}/src/hostapi/wasapi"
        "${_pa_dir}/src/hostapi/dsound"
    )
    list(APPEND _pa_defs
        PA_USE_WASAPI=1
        PA_USE_WMME=1
        PA_USE_DS=1
    )

    if(MSVC)
        # The plain x86 converters are an MSVC-only fast path (they use
        # MSVC intrinsics).  GCC/Clang build the portable path.
        list(APPEND _pa_sources "${_pa_dir}/src/os/win/pa_x86_plain_converters.c")
        # Upstream asks for DirectSound's full-duplex entry point; it
        # exists in the Windows SDK but not in the MinGW headers.
        list(APPEND _pa_defs PAWIN_USE_DIRECTSOUNDFULLDUPLEXCREATE=1)
    endif()
elseif(APPLE)
    list(APPEND _pa_sources
        "${_pa_dir}/src/os/unix/pa_pthread_util.c"
        "${_pa_dir}/src/os/unix/pa_unix_hostapis.c"
        "${_pa_dir}/src/os/unix/pa_unix_util.c"
        "${_pa_dir}/src/hostapi/coreaudio/pa_mac_core.c"
        "${_pa_dir}/src/hostapi/coreaudio/pa_mac_core_blocking.c"
        "${_pa_dir}/src/hostapi/coreaudio/pa_mac_core_utilities.c"
    )
    list(APPEND _pa_includes
        "${_pa_dir}/src/os/unix"
        "${_pa_dir}/src/hostapi/coreaudio"
    )
    list(APPEND _pa_defs
        PA_USE_COREAUDIO=1
    )
else()
    list(APPEND _pa_sources
        "${_pa_dir}/src/os/unix/pa_pthread_util.c"
        "${_pa_dir}/src/os/unix/pa_unix_hostapis.c"
        "${_pa_dir}/src/os/unix/pa_unix_util.c"
    )
    list(APPEND _pa_includes "${_pa_dir}/src/os/unix")

    # ALSA / PulseAudio are enabled only when the build host carries the
    # development package.  Neither is a hard NimRTC dependency: without
    # them PortAudio still builds and simply reports "no host API".
    find_package(ALSA QUIET)
    if(ALSA_FOUND AND TARGET ALSA::ALSA)
        list(APPEND _pa_sources "${_pa_dir}/src/hostapi/alsa/pa_linux_alsa.c")
        list(APPEND _pa_defs PA_USE_ALSA=1)
    endif()

    find_package(PulseAudio QUIET)
    if(PulseAudio_FOUND AND TARGET PulseAudio::PulseAudio)
        list(APPEND _pa_sources
            "${_pa_dir}/src/hostapi/pulseaudio/pa_linux_pulseaudio.c"
            "${_pa_dir}/src/hostapi/pulseaudio/pa_linux_pulseaudio_block.c"
            "${_pa_dir}/src/hostapi/pulseaudio/pa_linux_pulseaudio_cb.c"
        )
        list(APPEND _pa_defs PA_USE_PULSEAUDIO=1)
    endif()
endif()

add_library(nimrtc_vendor_portaudio STATIC
    ${_pa_sources}
)
add_library(nimrtc::vendor::portaudio ALIAS nimrtc_vendor_portaudio)

# SYSTEM includes: upstream headers must not be subject to NimRTC's own
# -Werror warning set.  The upstream tree is C89-era code with plenty of
# implicit conversions and old-style casts.
target_include_directories(nimrtc_vendor_portaudio SYSTEM
    PUBLIC
        "${_pa_dir}/include"
    PRIVATE
        ${_pa_includes}
)

target_compile_features(nimrtc_vendor_portaudio PRIVATE c_std_11)
target_compile_definitions(nimrtc_vendor_portaudio
    PUBLIC
        ${_pa_defs}
    PRIVATE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        # NOTE: WIN32_LEAN_AND_MEAN is deliberately NOT defined here.
        # It hides <mmsystem.h> from <windows.h>, and the DirectSound
        # backend's <dsound.h> needs the WAVEFORMATEX family from
        # there -- without it the SDK header does not parse.
)

# Upstream's own sources are not warning-clean under NimRTC's flags, so
# they are compiled with the warnings switched off (the vendor targets
# are excluded from nimrtc_apply_options() anyway — see
# cmake/NimRTCOptions.cmake).
if(MSVC)
    target_compile_options(nimrtc_vendor_portaudio PRIVATE /w)
else()
    target_compile_options(nimrtc_vendor_portaudio PRIVATE -w)
endif()

# The MSVC runtime must match the rest of the tree: upstream is built as
# part of the same process as every NimRTC target, so a /MD vs /MDd split
# produces two CRT heaps and an LNK4098 at link time.
if(MSVC AND CMAKE_MSVC_RUNTIME_LIBRARY)
    set_target_properties(nimrtc_vendor_portaudio PROPERTIES
        MSVC_RUNTIME_LIBRARY "${CMAKE_MSVC_RUNTIME_LIBRARY}")
endif()

find_package(Threads REQUIRED)
target_link_libraries(nimrtc_vendor_portaudio PUBLIC Threads::Threads)

if(WIN32)
    # ole32 / uuid  — the COM plumbing WASAPI and MME need.
    # winmm         — the waveIn / waveOut API behind the MME backend.
    # dsound        — DirectSound's full-duplex entry point.
    # avrt          — WASAPI's multimedia thread ordering (loaded at
    #                runtime by upstream, linked here for the import
    #                library on the MinGW leg).
    target_link_libraries(nimrtc_vendor_portaudio PUBLIC
        ole32
        uuid
        winmm
        dsound
    )
elseif(APPLE)
    target_link_libraries(nimrtc_vendor_portaudio PUBLIC
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework AudioUnit"
        "-framework CoreFoundation"
        "-framework CoreServices"
    )
else()
    target_link_libraries(nimrtc_vendor_portaudio PUBLIC m)
    if(TARGET ALSA::ALSA)
        target_link_libraries(nimrtc_vendor_portaudio PUBLIC ALSA::ALSA)
    endif()
    if(TARGET PulseAudio::PulseAudio)
        target_link_libraries(nimrtc_vendor_portaudio PUBLIC PulseAudio::PulseAudio)
    endif()
endif()

# The public API surface of the wrapper.
target_compile_definitions(nimrtc_vendor_portaudio INTERFACE NIMRTC_USE_PORTAUDIO=1)
