# third_party/openh264/CMakeLists.txt
# =============================================================================
# OpenH264 — H.264 encoder / decoder (Cisco Systems, BSD-2-Clause).
#
# Vendored from: https://github.com/cisco/openh264 (v2.6.0) under src/.
#
# WHY A HAND-WRITTEN WRAPPER
# --------------------------
# Upstream ships a Meson build (and a GNU Makefile), not a CMakeLists.txt.
# We cannot use Meson inside the NimRTC build (it would need a second build
# system, a second dependency, and an out-of-tree artifact the CI would have
# to provision).  So this file compiles the upstream sources directly with
# CMake and exposes the same target name the other vendors use:
#
#     nimrtc_vendor_openh264   (alias nimrtc::vendor::openh264)
#
# WHY THREE LIBRARIES, NOT ONE
# ----------------------------
# Upstream builds FOUR separate static libraries — common, processing,
# encoder and decoder — and only links them together at the very end
# (`link_whole: [libcommon, libprocessing, libencoder, libdecoder]`).
# That split is NOT cosmetic: the encoder and the decoder ship *different
# headers under the same names*:
#
#     codec/encoder/core/inc/wels_common_basis.h   (namespace WelsEnc)
#     codec/decoder/core/inc/wels_common_basis.h   (namespace WelsDec)
#
# ...and the same collision repeats for `wels_const.h`, `parameter_sets.h`,
# `picture.h` and `slice.h`.  The two `wels_common_basis.h` files even use
# the SAME include guard (`WELS_COMMON_BASIS_H__`), so if the encoder and
# the decoder were compiled as one target with both include directories on
# the include path, whichever header was seen first would silently win and
# the other half of the codec would fail to compile (or, worse, compile
# against the wrong definitions).
#
# So this wrapper mirrors upstream's split:
#
#     nimrtc_vendor_openh264_common      common/   + processing/
#     nimrtc_vendor_openh264_encoder     encoder/core + encoder/plus
#     nimrtc_vendor_openh264_decoder     decoder/core + decoder/plus
#
# and `nimrtc_vendor_openh264` is an INTERFACE target that pulls all three
# in.  Only the encoder and the decoder get their own include directory,
# which is what keeps the name collisions from ever meeting.
#
# ASM
# ---
# Upstream's x86 fast paths are NASM assembly, and its ARM paths are GAS
# `.S` files.  Neither NASM nor a GNU assembler is guaranteed to exist on a
# NimRTC build host (the Windows CI image has no NASM), so the wrapper builds
# the **portable C/C++** path only: `X86_ASM` / `HAVE_NEON` stay undefined,
# which makes upstream compile its plain-C fallbacks.  The result is a
# correct, if slower, H.264 codec — which is exactly what the software
# baseline (remote-desktop plan B3) needs: a *real* codec, not a stub.
#
# The upstream `codec/common/x86/*.asm` and `codec/**/arm*/**.S` files are
# therefore deliberately not compiled here.
#
# LICENCE
# -------
# BSD-2-Clause (see src/third_party/openh264/src/LICENSE).  The AVC patent
# attribution duty that Cisco's *prebuilt binary* carries does not apply to
# a source build, but the NOTICE file still records the attribution.
# =============================================================================

set(_src_dir "${CMAKE_CURRENT_SOURCE_DIR}/src")

# The vendored switch is the "use the vendored source build" contract.
# There is NO system fallback (L5 forbids linking a system codec), so
# switching it off means the codec is simply not available and the
# registry falls through to the software stub.
if(NOT NIMRTC_VENDORED_OPENH264)
    add_library(nimrtc_vendor_openh264 INTERFACE)
    add_library(nimrtc::vendor::openh264 ALIAS nimrtc_vendor_openh264)
    return()
endif()

if(NOT EXISTS "${_src_dir}/codec/api/wels/codec_api.h")
    # The submodule is not populated.  Declare an empty stub target so the
    # configure step succeeds; a consumer that links it fails at link time
    # with a clear "unresolved external" rather than a CMake error.
    add_library(nimrtc_vendor_openh264 INTERFACE)
    add_library(nimrtc::vendor::openh264 ALIAS nimrtc_vendor_openh264)
    target_compile_definitions(nimrtc_vendor_openh264 INTERFACE NIMRTC_USE_OPENH264=1)
    return()
endif()

set(_inc_api     "${_src_dir}/codec/api/wels")
set(_inc_common  "${_src_dir}/codec/common/inc")
set(_inc_dec     "${_src_dir}/codec/decoder/core/inc")
set(_inc_decplus "${_src_dir}/codec/decoder/plus/inc")
set(_inc_enc     "${_src_dir}/codec/encoder/core/inc")
set(_inc_encplus "${_src_dir}/codec/encoder/plus/inc")
set(_inc_proc_if "${_src_dir}/codec/processing/interface")
set(_inc_proc_common "${_src_dir}/codec/processing/src/common")
set(_inc_proc_aq "${_src_dir}/codec/processing/src/adaptivequantization")
set(_inc_proc_ds "${_src_dir}/codec/processing/src/downsample")
set(_inc_proc_sc "${_src_dir}/codec/processing/src/scrolldetection")
set(_inc_proc_vaa "${_src_dir}/codec/processing/src/vaacalc")

# -----------------------------------------------------------------------------
# version_gen.h
#
# Upstream's `version.h` includes "version_gen.h", which upstream generates
# from a template at build time.  The template is checked in, so we generate
# the header ourselves — no shell script, no `sh` dependency on Windows.
# -----------------------------------------------------------------------------
set(_version_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(_version_gen "${_version_gen_dir}/version_gen.h")
file(WRITE "${_version_gen}"
  "#ifndef VERSION_GEN_H\n"
  "#define VERSION_GEN_H\n"
  "#define VERSION_NUMBER \"2.6.0\"\n"
  "#endif // VERSION_GEN_H\n")

# -----------------------------------------------------------------------------
# Helper: apply the shared settings to one upstream library target.
# -----------------------------------------------------------------------------
function(_nimrtc_openh264_configure_target tgt)
    # SYSTEM includes: upstream headers must not be subject to NimRTC's own
    # -Werror warning set (-Wsign-conversion, -Wconversion, ...).  Without
    # SYSTEM, GCC/Clang apply those flags to every upstream header include and
    # turn upstream warnings into NimRTC build failures.
    target_include_directories(${tgt} SYSTEM PUBLIC
        "${_inc_api}"
        "${_inc_common}"
    )
    target_include_directories(${tgt} SYSTEM PRIVATE "${_version_gen_dir}")

    # Upstream's `version.h` only includes "version_gen.h" when
    # GENERATED_VERSION_HEADER is defined; otherwise it falls back to a dummy
    # string.  We define it and provide the generated header above.
    target_compile_definitions(${tgt} PRIVATE GENERATED_VERSION_HEADER=1)

    # The portable build: no X86_ASM, no HAVE_NEON, no HAVE_AVX2.  This is
    # what makes upstream compile its plain-C fallbacks (see the header
    # comment).
    target_compile_definitions(${tgt} PRIVATE NIMRTC_OPENH264_PORTABLE_BUILD=1)

    # OpenH264 is C++03-era code with a lot of implicit conversions; it must
    # NOT be compiled with NimRTC's warning set at all.
    if(MSVC)
        target_compile_options(${tgt} PRIVATE /w /wd4996)
    else()
        target_compile_options(${tgt} PRIVATE -w)
    endif()

    # The MSVC runtime must match the rest of the tree: upstream is built as
    # part of the same process as every NimRTC target, so a /MD vs /MDd split
    # produces two CRT heaps and an LNK4098 at link time.
    if(MSVC AND CMAKE_MSVC_RUNTIME_LIBRARY)
        set_target_properties(${tgt} PROPERTIES
            MSVC_RUNTIME_LIBRARY "${CMAKE_MSVC_RUNTIME_LIBRARY}")
    endif()
endfunction()

# -----------------------------------------------------------------------------
# 1. common + processing
#
# Neither of them includes the encoder-only or decoder-only headers, so they
# can share one target.  They are combined because they have no header
# collisions with each other and upstream keeps them independent only to
# support its Meson `link_whole`.
# -----------------------------------------------------------------------------
add_library(nimrtc_vendor_openh264_common STATIC
    # ---- common ----
    "${_src_dir}/codec/common/src/common_tables.cpp"
    "${_src_dir}/codec/common/src/copy_mb.cpp"
    "${_src_dir}/codec/common/src/cpu.cpp"
    "${_src_dir}/codec/common/src/crt_util_safe_x.cpp"
    "${_src_dir}/codec/common/src/deblocking_common.cpp"
    "${_src_dir}/codec/common/src/expand_pic.cpp"
    "${_src_dir}/codec/common/src/intra_pred_common.cpp"
    "${_src_dir}/codec/common/src/mc.cpp"
    "${_src_dir}/codec/common/src/memory_align.cpp"
    "${_src_dir}/codec/common/src/sad_common.cpp"
    "${_src_dir}/codec/common/src/utils.cpp"
    "${_src_dir}/codec/common/src/welsCodecTrace.cpp"
    "${_src_dir}/codec/common/src/WelsTaskThread.cpp"
    "${_src_dir}/codec/common/src/WelsThread.cpp"
    "${_src_dir}/codec/common/src/WelsThreadLib.cpp"
    "${_src_dir}/codec/common/src/WelsThreadPool.cpp"

    # ---- processing ----
    "${_src_dir}/codec/processing/src/adaptivequantization/AdaptiveQuantization.cpp"
    "${_src_dir}/codec/processing/src/backgrounddetection/BackgroundDetection.cpp"
    "${_src_dir}/codec/processing/src/common/memory.cpp"
    "${_src_dir}/codec/processing/src/common/WelsFrameWork.cpp"
    "${_src_dir}/codec/processing/src/common/WelsFrameWorkEx.cpp"
    "${_src_dir}/codec/processing/src/complexityanalysis/ComplexityAnalysis.cpp"
    "${_src_dir}/codec/processing/src/denoise/denoise.cpp"
    "${_src_dir}/codec/processing/src/denoise/denoise_filter.cpp"
    "${_src_dir}/codec/processing/src/downsample/downsample.cpp"
    "${_src_dir}/codec/processing/src/downsample/downsamplefuncs.cpp"
    "${_src_dir}/codec/processing/src/imagerotate/imagerotate.cpp"
    "${_src_dir}/codec/processing/src/imagerotate/imagerotatefuncs.cpp"
    "${_src_dir}/codec/processing/src/scenechangedetection/SceneChangeDetection.cpp"
    "${_src_dir}/codec/processing/src/scrolldetection/ScrollDetection.cpp"
    "${_src_dir}/codec/processing/src/scrolldetection/ScrollDetectionFuncs.cpp"
    "${_src_dir}/codec/processing/src/vaacalc/vaacalcfuncs.cpp"
    "${_src_dir}/codec/processing/src/vaacalc/vaacalculation.cpp"
)
target_include_directories(nimrtc_vendor_openh264_common SYSTEM PRIVATE
    "${_inc_proc_if}"
    "${_inc_proc_common}"
    "${_inc_proc_aq}"
    "${_inc_proc_ds}"
    "${_inc_proc_sc}"
    "${_inc_proc_vaa}"
)
_nimrtc_openh264_configure_target(nimrtc_vendor_openh264_common)

# -----------------------------------------------------------------------------
# 2. encoder — gets the encoder include directory and NOTHING else.
# -----------------------------------------------------------------------------
add_library(nimrtc_vendor_openh264_encoder STATIC
    "${_src_dir}/codec/encoder/core/src/au_set.cpp"
    "${_src_dir}/codec/encoder/core/src/deblocking.cpp"
    "${_src_dir}/codec/encoder/core/src/decode_mb_aux.cpp"
    "${_src_dir}/codec/encoder/core/src/encoder.cpp"
    "${_src_dir}/codec/encoder/core/src/encoder_data_tables.cpp"
    "${_src_dir}/codec/encoder/core/src/encoder_ext.cpp"
    "${_src_dir}/codec/encoder/core/src/encode_mb_aux.cpp"
    "${_src_dir}/codec/encoder/core/src/get_intra_predictor.cpp"
    "${_src_dir}/codec/encoder/core/src/md.cpp"
    "${_src_dir}/codec/encoder/core/src/mv_pred.cpp"
    "${_src_dir}/codec/encoder/core/src/nal_encap.cpp"
    "${_src_dir}/codec/encoder/core/src/paraset_strategy.cpp"
    "${_src_dir}/codec/encoder/core/src/picture_handle.cpp"
    "${_src_dir}/codec/encoder/core/src/ratectl.cpp"
    "${_src_dir}/codec/encoder/core/src/ref_list_mgr_svc.cpp"
    "${_src_dir}/codec/encoder/core/src/sample.cpp"
    "${_src_dir}/codec/encoder/core/src/set_mb_syn_cabac.cpp"
    "${_src_dir}/codec/encoder/core/src/set_mb_syn_cavlc.cpp"
    "${_src_dir}/codec/encoder/core/src/slice_multi_threading.cpp"
    "${_src_dir}/codec/encoder/core/src/svc_base_layer_md.cpp"
    "${_src_dir}/codec/encoder/core/src/svc_enc_slice_segment.cpp"
    "${_src_dir}/codec/encoder/core/src/svc_encode_mb.cpp"
    "${_src_dir}/codec/encoder/core/src/svc_encode_slice.cpp"
    "${_src_dir}/codec/encoder/core/src/svc_mode_decision.cpp"
    "${_src_dir}/codec/encoder/core/src/svc_motion_estimate.cpp"
    "${_src_dir}/codec/encoder/core/src/svc_set_mb_syn_cabac.cpp"
    "${_src_dir}/codec/encoder/core/src/svc_set_mb_syn_cavlc.cpp"
    "${_src_dir}/codec/encoder/core/src/wels_preprocess.cpp"
    "${_src_dir}/codec/encoder/core/src/wels_task_base.cpp"
    "${_src_dir}/codec/encoder/core/src/wels_task_encoder.cpp"
    "${_src_dir}/codec/encoder/core/src/wels_task_management.cpp"
    "${_src_dir}/codec/encoder/plus/src/welsEncoderExt.cpp"
)
target_include_directories(nimrtc_vendor_openh264_encoder SYSTEM PRIVATE
    "${_inc_enc}"
    "${_inc_encplus}"
    "${_inc_proc_if}"
    "${_inc_proc_common}"
    "${_inc_proc_aq}"
    "${_inc_proc_ds}"
    "${_inc_proc_sc}"
    "${_inc_proc_vaa}"
)
_nimrtc_openh264_configure_target(nimrtc_vendor_openh264_encoder)

# -----------------------------------------------------------------------------
# 3. decoder — gets the decoder include directory and NOTHING else.
# -----------------------------------------------------------------------------
add_library(nimrtc_vendor_openh264_decoder STATIC
    "${_src_dir}/codec/decoder/core/src/au_parser.cpp"
    "${_src_dir}/codec/decoder/core/src/bit_stream.cpp"
    "${_src_dir}/codec/decoder/core/src/cabac_decoder.cpp"
    "${_src_dir}/codec/decoder/core/src/deblocking.cpp"
    "${_src_dir}/codec/decoder/core/src/decoder.cpp"
    "${_src_dir}/codec/decoder/core/src/decoder_core.cpp"
    "${_src_dir}/codec/decoder/core/src/decoder_data_tables.cpp"
    "${_src_dir}/codec/decoder/core/src/decode_mb_aux.cpp"
    "${_src_dir}/codec/decoder/core/src/decode_slice.cpp"
    "${_src_dir}/codec/decoder/core/src/error_concealment.cpp"
    "${_src_dir}/codec/decoder/core/src/fmo.cpp"
    "${_src_dir}/codec/decoder/core/src/get_intra_predictor.cpp"
    "${_src_dir}/codec/decoder/core/src/manage_dec_ref.cpp"
    "${_src_dir}/codec/decoder/core/src/memmgr_nal_unit.cpp"
    "${_src_dir}/codec/decoder/core/src/mv_pred.cpp"
    "${_src_dir}/codec/decoder/core/src/parse_mb_syn_cabac.cpp"
    "${_src_dir}/codec/decoder/core/src/parse_mb_syn_cavlc.cpp"
    "${_src_dir}/codec/decoder/core/src/pic_queue.cpp"
    "${_src_dir}/codec/decoder/core/src/rec_mb.cpp"
    "${_src_dir}/codec/decoder/core/src/wels_decoder_thread.cpp"
    "${_src_dir}/codec/decoder/plus/src/welsDecoderExt.cpp"
)
target_include_directories(nimrtc_vendor_openh264_decoder SYSTEM PRIVATE
    "${_inc_dec}"
    "${_inc_decplus}"
)
_nimrtc_openh264_configure_target(nimrtc_vendor_openh264_decoder)

# -----------------------------------------------------------------------------
# The umbrella target.
#
# `nimrtc_vendor_openh264` is what the rest of NimRTC links against.  It is
# an INTERFACE target so the three upstream archives stay separate objects
# (which is what keeps the header collisions apart) while still presenting
# one name to consumers.
# -----------------------------------------------------------------------------
add_library(nimrtc_vendor_openh264 INTERFACE)
add_library(nimrtc::vendor::openh264 ALIAS nimrtc_vendor_openh264)
target_link_libraries(nimrtc_vendor_openh264 INTERFACE
    nimrtc_vendor_openh264_common
    nimrtc_vendor_openh264_encoder
    nimrtc_vendor_openh264_decoder
)

find_package(Threads REQUIRED)
target_link_libraries(nimrtc_vendor_openh264_common PUBLIC Threads::Threads)
target_link_libraries(nimrtc_vendor_openh264_encoder PUBLIC Threads::Threads)
target_link_libraries(nimrtc_vendor_openh264_decoder PUBLIC Threads::Threads)

# The public API surface of the wrapper.
target_compile_definitions(nimrtc_vendor_openh264 INTERFACE NIMRTC_USE_OPENH264=1)
