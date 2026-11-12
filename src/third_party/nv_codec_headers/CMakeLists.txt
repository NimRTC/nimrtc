# src/third_party/nv_codec_headers/CMakeLists.txt
# =============================================================================
# nv-codec-headers — NVIDIA Video Codec SDK headers (MIT, vendored).
#
# This is a *header-only* vendor: it ships nvEncodeAPI.h (NVENC) plus the
# nvcuvid / CUVID headers and the FFmpeg-style dynamic-loader headers
# (dynlink_loader.h, dynlink_cuda.h, dynlink_nvcuvid.h,
# dynlink_cuviddec.h).
#
# No NVIDIA binary is linked or shipped.  At runtime the NVENC / NVDEC
# backends load nvEncodeAPI64.dll / nvcuvid.dll / nvcuda.dll from the
# installed NVIDIA driver with LoadLibrary (Windows) or dlopen (Linux),
# so the build works on a machine with no SDK installed at all.
#
# See README.md in this directory for the provenance, the licence and the
# ABI-consistency rules.
# =============================================================================

add_library(nimrtc_vendor_nv_codec_headers INTERFACE)
add_library(nimrtc::vendor::nv_codec_headers ALIAS nimrtc_vendor_nv_codec_headers)

target_include_directories(nimrtc_vendor_nv_codec_headers SYSTEM INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/ffnvcodec>
    $<INSTALL_INTERFACE:include/ffnvcodec>)
