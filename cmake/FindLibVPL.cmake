# FindLibVPL.cmake
# ----------------------------------------------------------------------------
# Locate Intel oneVPL / libvpl (successor to Media SDK) — provides QSV
# (Quick Sync Video) encode + decode on Intel CPUs / iGPUs.
#
# Defines:
#   LIBVPL_FOUND          — TRUE if detected
#   LIBVPL_INCLUDE_DIRS   — directory containing vpl/mfxvideo.h
#   LIBVPL_LIBRARIES      — libvpl.so / vpl.dll
#   NimRTC::vpl           — IMPORTED target
#
# Notes:
#   - on Windows libvpl ships vpl.dll + vpl.lib + headers under vpl/.
#   - on Linux libvpl ships libvpl.so.
# ----------------------------------------------------------------------------

set(_probe_paths)
if(DEFINED ENV{LIBVPL_SDK_DIR})
    list(APPEND _probe_paths "$ENV{LIBVPL_SDK_DIR}/include")
endif()
if(DEFINED ENV{INTEL_VPL_SDK_DIR})
    list(APPEND _probe_paths "$ENV{INTEL_VPL_SDK_DIR}/include")
endif()
if(DEFINED ENV{MFX_HOME})
    list(APPEND _probe_paths "$ENV{MFX_HOME}/include")
endif()

list(APPEND _probe_paths
    "C:/Program Files/Intel/oneVPL/include"
    "C:/Program Files (x86)/Intel/oneVPL/include"
    "C:/Program Files/Intel/MediaSDK/include"
    "C:/Program Files (x86)/Intel/MediaSDK/include"
    "/usr/include/vpl"
    "/usr/include"
    "/usr/local/include"
    "/usr/include/x86_64-linux-gnu")

find_path(LIBVPL_INCLUDE_DIR
    NAMES vpl/mfxvideo.h mfxvideo.h
    PATHS ${_probe_paths}
    NO_DEFAULT_PATH)

find_path(LIBVPL_INCLUDE_DIR NAMES vpl/mfxvideo.h)

if(NOT LIBVPL_LIBRARY)
    set(_probe_lib_paths
        "C:/Program Files/Intel/oneVPL/lib"
        "C:/Program Files (x86)/Intel/oneVPL/lib"
        "C:/Program Files/Intel/MediaSDK/lib/x64"
        "/usr/lib/x86_64-linux-gnu"
        "/usr/lib64"
        "/usr/lib")
    find_library(LIBVPL_LIBRARY
        NAMES vpl libvpl
        PATHS ${_probe_lib_paths}
        NO_DEFAULT_PATH)
endif()

find_library(LIBVPL_LIBRARY NAMES vpl libvpl)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LibVPL
    REQUIRED_VARS LIBVPL_INCLUDE_DIR LIBVPL_LIBRARY)

if(LIBVPL_FOUND)
    set(LIBVPL_INCLUDE_DIRS ${LIBVPL_INCLUDE_DIR})
    set(LIBVPL_LIBRARIES    ${LIBVPL_LIBRARY})

    if(NOT TARGET NimRTC::vpl)
        add_library(NimRTC::vpl UNKNOWN IMPORTED)
        set_target_properties(NimRTC::vpl PROPERTIES
            IMPORTED_LOCATION             "${LIBVPL_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${LIBVPL_INCLUDE_DIR}")
    endif()

    mark_as_advanced(LIBVPL_INCLUDE_DIR LIBVPL_LIBRARY)
endif()
