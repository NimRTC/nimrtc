# FindAMF.cmake
# ----------------------------------------------------------------------------
# Locate AMD AMF (Advanced Media Framework) — header-only SDK from
# https://github.com/GPUOpen-LibrariesAndSDKs/AMF
#
# Defines:
#   AMF_FOUND          — TRUE if SDK (headers) detected
#   AMF_INCLUDE_DIRS   — directory containing AMF/core/Factory.h
#   AMF_DEFINITIONS    — compile definitions (AMF_CORE_STATIC if static link)
#   AMF_VERSION        — best-effort version string
#   NimRTC::amf        — INTERFACE IMPORTED target for consumers
#
# Notes:
#   - AMF is essentially a header-only SDK on the developer side; at
#     runtime the application loads amfrt64.dll (Windows) / libamfrt64.so
#     (Linux).  Loading is done via LoadLibrary / dlopen, NOT a normal
#     link dependency, so we do not search for an .so / .dll here.
#   - Detection is header-only: presence of AMF/core/Factory.h is the
#     success criterion.
# ----------------------------------------------------------------------------

set(_probe_paths)
if(DEFINED ENV{AMF_SDK_DIR})
    list(APPEND _probe_paths "$ENV{AMF_SDK_DIR}/amf/public/include")
endif()

list(APPEND _probe_paths
    "C:/Program Files/AMD/AMF/include"
    "C:/Program Files/AMD/AMF/amf/public/include"
    "${CMAKE_SOURCE_DIR}/third_party/AMF/amf/public/include"
    "/usr/include/AMF"
    "/usr/local/include/AMF"
    "/opt/amd/amf/include")

find_path(AMF_INCLUDE_DIR
    NAMES AMF/core/Factory.h
    PATHS ${_probe_paths}
    DOC "Directory containing AMF/core/Factory.h"
    NO_DEFAULT_PATH)

# Fall back to a global search.
find_path(AMF_INCLUDE_DIR NAMES AMF/core/Factory.h)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(AMF
    REQUIRED_VARS AMF_INCLUDE_DIR
    VERSION_VAR   AMF_VERSION)

if(AMF_FOUND)
    set(AMF_INCLUDE_DIRS ${AMF_INCLUDE_DIR})
    set(AMF_DEFINITIONS  "")

    if(NOT TARGET NimRTC::amf)
        add_library(NimRTC::amf INTERFACE IMPORTED)
        set_target_properties(NimRTC::amf PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${AMF_INCLUDE_DIR}"
            INTERFACE_COMPILE_DEFINITIONS "${AMF_DEFINITIONS}")
    endif()

    mark_as_advanced(AMF_INCLUDE_DIR)
endif()
