# third_party/libopus/CMakeLists.txt
# =============================================================================
# libopus — Opus audio codec library.
#
# Populated by: git clone https://github.com/xiph/opus.git third_party/libopus/src
#
# Target produced: nimrtc_vendor_libopus
# Dependencies propagated: none (INTERFACE only).
# =============================================================================
# NOTE: this file is only added via add_subdirectory() when
# third_party/libopus/src/ exists (checked by the parent third_party/CMakeLists.txt).
# NimRTCVendored.cmake wires this target to modules that need it.
# =============================================================================

set(_src_dir "${CMAKE_CURRENT_SOURCE_DIR}/src")

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
