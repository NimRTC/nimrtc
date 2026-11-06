# third_party/libopus/CMakeLists.txt
# =============================================================================
# libopus — Opus audio codec library (RFC 6716).
#
# Vendored from: https://github.com/xiph/opus (opus-1.6.1) under src/.
#
# We add_subdirectory() the upstream CMakeLists.txt from src/ and re-export
# the target as nimrtc::vendor::libopus. Upstream libopus produces target
# `opus` (alias `Opus::opus`). We mirror that to NimRTC's naming convention
# (nimrtc_vendor_libopus / nimrtc::vendor::libopus) via ALIAS so consumers
# can stay unchanged.
#
# IMPORTANT: this assumes src/ is populated. If src/ is empty (fresh clone),
# consumers must run scripts/vendor-fetch.sh libopus first; or set
# NIMRTC_BUILD_AUDIO=OFF to skip audio.
# =============================================================================

set(_src_dir "${CMAKE_CURRENT_SOURCE_DIR}/src")

if(EXISTS "${_src_dir}/CMakeLists.txt")
    # Disable building upstream's demos / tests — we only want the library.
    set(OPUS_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(OPUS_BUILD_TESTING     OFF CACHE BOOL "" FORCE)
    set(OPUS_BUILD_PROGRAMS    OFF CACHE BOOL "" FORCE)
    set(OPUS_FIXED_POINT       OFF CACHE BOOL "" FORCE)
    set(OPUS_NONTHREADSAFE_PSEUDOSTACK OFF CACHE BOOL "" FORCE)
    # silk/typedef.h turns silk_assert into MSVC's _ASSERTE on Windows MSVC
    # builds (without NO_ASSERTS).  _ASSERTE pulls in _CrtDbgReportW which
    # then requires MSVCRTD.lib — and a multi-config /MDd NimRTC consumer
    # often doesn't link the debug CRT, producing LNK2001 unresolved externals.
    # Force the no-op path universally (we don't ship a debug-opus anyway).
    set(NO_ASSERTS ON CACHE BOOL "" FORCE)

    # NO_ASSERTS is consumed by silk/typedef.h — must propagate to the
    # compile step, not just live as a CMake variable.
    add_compile_definitions(NO_ASSERTS=1)
    # These two pull in C runtime helpers (CrtDbgReportW, etc.) that may not
    # be available on minimal dev environments without ucrtbased.  Use a
    # generator expression so this fires on every multi-config build (Visual
    # Studio leaves CMAKE_BUILD_TYPE empty).
    set(OPUS_HARDENING  "$<$<CONFIG:Debug>:OFF>$<$<NOT:$<CONFIG:Debug>>:ON>"
        CACHE BOOL "" FORCE)
    set(OPUS_ASSERTIONS "$<$<CONFIG:Debug>:OFF>$<$<NOT:$<CONFIG:Debug>>:ON>"
        CACHE BOOL "" FORCE)

    add_subdirectory(${_src_dir} _nimrtc_libopus_build EXCLUDE_FROM_ALL)

    # Upstream picks the runtime through the *variable*
    # CMAKE_MSVC_RUNTIME_LIBRARY, set in its own directory scope, so it
    # shadows NimRTC's project-wide /MD (MultiThreadedDLL).  In Debug that
    # builds opus against MSVCRTD while every other NimRTC target uses
    # MSVCRT: two CRT heaps in one process, plus an LNK4098 at link time.
    # Mixed-CRT heap ownership is what used to crash the engine tests that
    # touch the Opus encoder/decoder path.
    #
    # The target property is named MSVC_RUNTIME_LIBRARY — CMAKE_... is only
    # the variable upstream reads, so setting it as a property was a no-op.
    #
    # The value must track whatever the *rest* of the tree uses, not a
    # hard-coded /MD: with NIMRTC_TEST_RUNTIME_LIB=DEFAULT the top-level
    # scope leaves CMAKE_MSVC_RUNTIME_LIBRARY unset and every NimRTC target
    # falls back to CMake's per-config default (/MDd in Debug, /MD in
    # Release).  Upstream's own default happens to be that same per-config
    # expression, so reusing it in that mode keeps opus in step instead of
    # re-introducing the mismatch in the opposite direction.
    if(MSVC)
        if(CMAKE_MSVC_RUNTIME_LIBRARY)
            set(_opus_runtime "${CMAKE_MSVC_RUNTIME_LIBRARY}")
            set(_opus_mt_debug "/MDd")
            set(_opus_mt_release "/MD")
        else()
            set(_opus_runtime "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
            set(_opus_mt_debug "/MDd")
            set(_opus_mt_release "/MD")
        endif()
        set_target_properties(opus PROPERTIES
            MSVC_RUNTIME_LIBRARY "${_opus_runtime}")
        # Also patch any compile flags that upstream set with the old runtime.
        get_target_property(_opus_flags opus COMPILE_FLAGS)
        if(_opus_flags)
            string(REPLACE "/MTd" "${_opus_mt_debug}" _opus_flags "${_opus_flags}")
            string(REPLACE "/MT"  "${_opus_mt_release}" _opus_flags "${_opus_flags}")
            set_target_properties(opus PROPERTIES COMPILE_FLAGS "${_opus_flags}")
        endif()
        unset(_opus_flags)
        unset(_opus_runtime)
        unset(_opus_mt_debug)
        unset(_opus_mt_release)
    endif()

    # Alias the upstream `opus` target → NimRTC's standard name.
    add_library(nimrtc_vendor_libopus INTERFACE)
    target_link_libraries(nimrtc_vendor_libopus INTERFACE opus)
    target_include_directories(nimrtc_vendor_libopus
        INTERFACE
            $<BUILD_INTERFACE:${_src_dir}/include>
    )
    target_compile_definitions(nimrtc_vendor_libopus
        INTERFACE
            NIMRTC_USE_LIBOPUS=1
            OPUS_BUILD=1
    )
    add_library(nimrtc::vendor::libopus ALIAS nimrtc_vendor_libopus)
else()
    # Stub target — fall back to the empty INTERFACE target. Linking this
    # would fail at link time, but CMake configure succeeds (matches the
    # historical behaviour before the source was populated).
    add_library(nimrtc_vendor_libopus INTERFACE)
    add_library(nimrtc::vendor::libopus ALIAS nimrtc_vendor_libopus)
    target_include_directories(nimrtc_vendor_libopus
        INTERFACE
            $<BUILD_INTERFACE:${_src_dir}/include>
    )
    target_compile_definitions(nimrtc_vendor_libopus
        INTERFACE
            NIMRTC_USE_LIBOPUS=1
    )
endif()
