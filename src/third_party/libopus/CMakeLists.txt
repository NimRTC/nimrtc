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

    # Upstream defaults CMAKE_MSVC_RUNTIME_LIBRARY to MultiThreaded[/Debug]
    # (static /MT /MTd) which conflicts with NimRTC's /MDd.  Override the
    # runtime on the opus target so it matches the rest of the build.
    if(MSVC)
        set_target_properties(opus PROPERTIES
            CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        # Also patch any compile flags that upstream set with the old runtime.
        get_target_property(_opus_flags opus COMPILE_FLAGS)
        if(_opus_flags)
            string(REPLACE "/MTd" "/MDd" _opus_flags "${_opus_flags}")
            string(REPLACE "/MT"  "/MD"  _opus_flags "${_opus_flags}")
            set_target_properties(opus PROPERTIES COMPILE_FLAGS "${_opus_flags}")
        endif()
        unset(_opus_flags)
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
