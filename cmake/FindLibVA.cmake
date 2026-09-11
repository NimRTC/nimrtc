# FindLibVA.cmake
# ----------------------------------------------------------------------------
# Locate libva — Linux-only VA-API (Video Acceleration API) headers + libs.
# Provides VA-API encode + decode entry points used by the vaapi plugin.
#
# Defines:
#   LIBVA_FOUND          — TRUE if libva detected
#   LIBVA_INCLUDE_DIRS   — directory containing va/va.h
#   LIBVA_LIBRARIES      — libva.so + libva-drm.so + libva-x11.so (when found)
#   LIBVA_DEFINITIONS    — compile definitions
#   NimRTC::va           — IMPORTED target
#
# Detection:
#   1. find_package(libva) — honours pkg-config / system-config.
#   2. Manual fallback: find_path + find_library against standard Linux dirs.
# ----------------------------------------------------------------------------

if(WIN32)
    # libva is a Linux-only API; on Windows return a clean "not found".
    set(LIBVA_FOUND FALSE)
    return()
endif()

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PC_LIBVA QUIET libva)
    pkg_check_modules(PC_LIBVA_DRM QUIET libva-drm)
    pkg_check_modules(PC_LIBVA_X11 QUIET libva-x11)
endif()

if(NOT LIBVA_INCLUDE_DIR)
    set(_probe_paths ${PC_LIBVA_INCLUDE_DIRS}
        /usr/include
        /usr/local/include
        /usr/include/x86_64-linux-gnu
        /usr/include/aarch64-linux-gnu)
    find_path(LIBVA_INCLUDE_DIR
        NAMES va/va.h
        PATHS ${_probe_paths}
        NO_DEFAULT_PATH)
endif()

if(NOT LIBVA_LIBRARY)
    set(_probe_lib_paths ${PC_LIBVA_LIBRARY_DIRS}
        /usr/lib/x86_64-linux-gnu
        /usr/lib64
        /usr/lib/aarch64-linux-gnu
        /usr/local/lib64
        /usr/local/lib)
    find_library(LIBVA_LIBRARY
        NAMES va
        PATHS ${_probe_lib_paths}
        NO_DEFAULT_PATH)
endif()

find_library(LIBVA_DRM_LIBRARY  NAMES va-drm PATHs ${_probe_lib_paths})
find_library(LIBVA_X11_LIBRARY  NAMES va-x11 PATHS ${_probe_lib_paths})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LibVA
    REQUIRED_VARS LIBVA_INCLUDE_DIR LIBVA_LIBRARY)

if(LIBVA_FOUND)
    set(LIBVA_INCLUDE_DIRS ${LIBVA_INCLUDE_DIR})
    set(LIBVA_LIBRARIES
        ${LIBVA_LIBRARY}
        ${LIBVA_DRM_LIBRARY}
        ${LIBVA_X11_LIBRARY})
    # Filter empty entries (find_library returns LIBVA_DRM_LIBRARY-NOTFOUND
    # when the package isn't present).
    list(REMOVE_ITEM LIBVA_LIBRARIES LIBVA_DRM_LIBRARY-NOTFOUND)
    list(REMOVE_ITEM LIBVA_LIBRARIES LIBVA_X11_LIBRARY-NOTFOUND)
    list(REMOVE_ITEM LIBVA_LIBRARIES LIBVA_LIBRARY-NOTFOUND)

    if(NOT TARGET NimRTC::va)
        add_library(NimRTC::va UNKNOWN IMPORTED)
        set_target_properties(NimRTC::va PROPERTIES
            IMPORTED_LOCATION             "${LIBVA_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${LIBVA_INCLUDE_DIR}")
    endif()

    mark_as_advanced(LIBVA_INCLUDE_DIR LIBVA_LIBRARY
                     LIBVA_DRM_LIBRARY LIBVA_X11_LIBRARY)
endif()
