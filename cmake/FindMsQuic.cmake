# FindMsQuic.cmake
# ----------------------------------------------------------------------------
# Locate the msquic C library.
#
# This module is used by NimRTC's CMake build system to locate msquic
# headers and library for the WebTransport / QUIC integration
# (ADR-014, v0.14.0-beta.1).
#
# Defines:
#   MSQUIC_FOUND          — TRUE if msquic is detected
#   MSQUIC_INCLUDE_DIRS   — directory containing msquic.h (C API header)
#   MSQUIC_LIBRARIES      — full path to the msquic static library
#   MSQUIC_DEFINITIONS    — compile definitions
#   NimRTC::msquic       — IMPORTED target for consumers to link against
#
# Cache variables (for user override):
#   MSQUIC_ROOT           — pre-set msquic root directory
#   MSQUIC_INCLUDE_DIR    — pre-set include directory
#   MSQUIC_LIBRARY        — pre-set library path
#
# Detection logic:
#   1. Honour user-supplied MSQUIC_ROOT / MSQUIC_INCLUDE_DIR / MSQUIC_LIBRARY
#      (cache vars take precedence over all detection paths).
#   2. Probe vendored path: src/third_party/msquic/src/inc/msquic.h
#      (the git submodule layout; primary path for NimRTC's own builds).
#   3. Probe system paths: /usr/local, /usr, /opt on Unix;
#      common install prefixes on Windows.
#   4. find_path / find_library as the last resort.
#
# The vendored path (step 2) is the primary path because msquic is a git
# submodule in NimRTC's own build. The system-path fallback (step 3) enables
# users who install msquic from a system package manager to use the module
# without extra CMake flags.
# ----------------------------------------------------------------------------

# Honour user-supplied MSQUIC_ROOT first.
if(NOT MSQUIC_ROOT)
    set(MSQUIC_ROOT "" CACHE PATH "Root directory of msquic installation")
endif()

# Honour user-supplied include/library overrides.
if(NOT MSQUIC_INCLUDE_DIR)
    set(MSQUIC_INCLUDE_DIR "" CACHE PATH "Directory containing msquic.h")
endif()
if(NOT MSQUIC_LIBRARY)
    set(MSQUIC_LIBRARY "" CACHE FILEPATH "Path to msquic library file")
endif()

# ---------------------------------------------------------------------------
# Step 1: user-supplied MSQUIC_INCLUDE_DIR / MSQUIC_LIBRARY
# ---------------------------------------------------------------------------
if(MSQUIC_INCLUDE_DIR AND EXISTS "${MSQUIC_INCLUDE_DIR}/msquic.h")
    set(_msquic_inc_found TRUE)
elseif(MSQUIC_ROOT AND EXISTS "${MSQUIC_ROOT}/inc/msquic.h")
    set(MSQUIC_INCLUDE_DIR "${MSQUIC_ROOT}/inc" CACHE PATH "" FORCE)
    set(_msquic_inc_found TRUE)
endif()

if(MSQUIC_LIBRARY AND EXISTS "${MSQUIC_LIBRARY}")
    set(_msquic_lib_found TRUE)
elseif(MSQUIC_ROOT)
    # Probe common library names per platform.
    set(_msquic_lib_names "")
    if(WIN32)
        set(_msquic_lib_names msquic libmsquic)
    else()
        set(_msquic_lib_names msquic libmsquic.a)
    endif()
    foreach(_lib IN LISTS _msquic_lib_names)
        if(EXISTS "${MSQUIC_ROOT}/lib/${_lib}.lib")
            set(MSQUIC_LIBRARY "${MSQUIC_ROOT}/lib/${_lib}.lib" CACHE FILEPATH "" FORCE)
            set(_msquic_lib_found TRUE)
            break()
        elseif(EXISTS "${MSQUIC_ROOT}/lib64/${_lib}.so")
            set(MSQUIC_LIBRARY "${MSQUIC_ROOT}/lib64/${_lib}.so" CACHE FILEPATH "" FORCE)
            set(_msquic_lib_found TRUE)
            break()
        elseif(EXISTS "${MSQUIC_ROOT}/lib/${_lib}.a")
            set(MSQUIC_LIBRARY "${MSQUIC_ROOT}/lib/${_lib}.a" CACHE FILEPATH "" FORCE)
            set(_msquic_lib_found TRUE)
            break()
        elseif(EXISTS "${MSQUIC_ROOT}/lib/${_lib}")
            set(MSQUIC_LIBRARY "${MSQUIC_ROOT}/lib/${_lib}" CACHE FILEPATH "" FORCE)
            set(_msquic_lib_found TRUE)
            break()
        endif()
    endforeach()
endif()

# ---------------------------------------------------------------------------
# Step 2: vendored path (primary path for NimRTC's own builds)
# ---------------------------------------------------------------------------
# The git submodule layout: src/third_party/msquic/src/inc/msquic.h
# Note: CMAKE_SOURCE_DIR is the NimRTC root, not this cmake/ directory.
if(NOT _msquic_inc_found)
    set(_vendored_inc "${CMAKE_SOURCE_DIR}/src/third_party/msquic/src/inc")
    if(EXISTS "${_vendored_inc}/msquic.h")
        set(MSQUIC_INCLUDE_DIR "${_vendored_inc}" CACHE PATH "" FORCE)
        set(_msquic_inc_found TRUE)
    endif()
endif()

if(NOT _msquic_lib_found)
    # In the vendored path, msquic is a static library built by the upstream
    # CMakeLists.txt. The library target is `msquic` (a STATIC lib produced
    # by add_subdirectory in src/third_party/msquic/CMakeLists.txt).
    # When using the vendored path, consumers should link via the CMake
    # INTERFACE target `nimrtc::vendor::msquic` directly (exposed by
    # src/third_party/msquic/CMakeLists.txt), NOT via the FindMsQuic.cmake
    # library path. This find module primarily serves the system-install path.
    # We still set MSQUIC_FOUND=TRUE if headers are found (library is optional
    # for header-only API inspection, and the vendored build uses the CMake
    # target instead of the find module library path).
    if(_msquic_inc_found)
        set(_msquic_lib_found TRUE)
    endif()
endif()

# ---------------------------------------------------------------------------
# Step 3: system paths
# ---------------------------------------------------------------------------
if(NOT _msquic_inc_found)
    find_path(MSQUIC_INCLUDE_DIR
        NAMES msquic.h
        PATHS
            "/usr/local/include"
            "/usr/include"
            "/opt/local/include"
            "/opt/msquic/include"
            "C:/Program Files/msquic/include"
        PATH_SUFFIXES include)
endif()

# ---------------------------------------------------------------------------
# Standard find_path / find_library fallback
# ---------------------------------------------------------------------------
if(NOT _msquic_inc_found)
    find_path(MSQUIC_INCLUDE_DIR NAMES msquic.h)
endif()
if(NOT _msquic_lib_found)
    find_library(MSQUIC_LIBRARY
        NAMES msquic libmsquic)
endif()

# ---------------------------------------------------------------------------
# Result
# ---------------------------------------------------------------------------
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(MsQuic
    REQUIRED_VARS MSQUIC_INCLUDE_DIR
    HANDLE_COMPONENTS)

if(MSQUIC_FOUND)
    set(MSQUIC_INCLUDE_DIRS "${MSQUIC_INCLUDE_DIR}")
    set(MSQUIC_LIBRARIES    "${MSQUIC_LIBRARY}")
    set(MSQUIC_DEFINITIONS  "")

    # Create the NimRTC::msquic IMPORTED target for consumers.
    # If a library file was found, use it. If only headers were found (vendored
    # CMake target path), create a header-only INTERFACE target.
    if(MSQUIC_LIBRARY AND EXISTS "${MSQUIC_LIBRARY}")
        if(NOT TARGET NimRTC::msquic)
            if(MSQUIC_LIBRARY MATCHES "\\.lib$|\\.dll$|\\.so$|\\.a$")
                add_library(NimRTC::msquic UNKNOWN IMPORTED)
                set_target_properties(NimRTC::msquic PROPERTIES
                    IMPORTED_LOCATION             "${MSQUIC_LIBRARY}"
                    INTERFACE_INCLUDE_DIRECTORIES "${MSQUIC_INCLUDE_DIR}")
            else()
                add_library(NimRTC::msquic INTERFACE IMPORTED)
                set_target_properties(NimRTC::msquic PROPERTIES
                    INTERFACE_INCLUDE_DIRECTORIES "${MSQUIC_INCLUDE_DIR}")
            endif()
        endif()
    else()
        # Headers-only: create an INTERFACE target (consumers use it for
        # include dirs only; they link the actual msquic library via
        # the nimrtc_vendor_msquic CMake target from src/third_party/).
        if(NOT TARGET NimRTC::msquic)
            add_library(NimRTC::msquic INTERFACE)
            set_target_properties(NimRTC::msquic PROPERTIES
                INTERFACE_INCLUDE_DIRECTORIES "${MSQUIC_INCLUDE_DIR}")
        endif()
    endif()

    mark_as_advanced(MSQUIC_ROOT MSQUIC_INCLUDE_DIR MSQUIC_LIBRARY)
endif()
