/**
 * @file src/engine/src/engine_video_staging.hpp
 * @brief PRIVATE engine collaborator: the source-frame **staging** step.
 *
 * This header is PRIVATE: it is not installed and no public header
 * includes it (Layout Invariant 4).
 *
 * ## Why it is a header of its own
 *
 * The staging step — "turn a caller-owned `plugins::VideoSourceFrame`
 * into a refcounted `video_frame::VideoFrameBuffer` the codec can read"
 * — used to live in `engine_media.cpp`'s anonymous namespace.  That was
 * fine while there was exactly one send path.  The multi-stream plane
 * (remote-desktop plan H1 / H2) added a **second** send path: every
 * extra video stream stages into *its own* buffer before its own
 * encoder sees the frame.
 *
 * Two copies of the colour-space rules would be two opinions about
 * what a BGRA frame means, and they would drift.  So the rule lives
 * here, once, and both send paths include it.
 *
 * ## The rule
 *
 * The staged buffer always uses the *codec-facing* pixel format (I420
 * by default) rather than the source's own format, so a source that
 * emits NV12 is converted once here instead of forcing every codec to
 * handle both layouts.  BGRA is converted with the shared BT.601
 * transform every desktop-capture path needs; NV12 is de-interleaved;
 * I420 / YV12 are copied plane by plane.
 *
 * The conversion is *counted* (plan C3): the caller receives the
 * `ConversionKind` so the engine can report how much colour work it
 * did — and can show that the zero-copy path does none.
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <memory>

#include <nimrtc/plugins/video_codec.hpp>
#include <nimrtc/video_frame/frame.hpp>

namespace nimrtc::engine::detail {

/**
 * @brief What the staging step had to do to the source frame.
 *
 * The plan's C3 row asks for the *conversion count* to be reported, and
 * for it to be zero on the zero-copy path.  This enum is what makes
 * that countable: a frame that was already in the canonical format
 * needs a plain copy, a packed frame needs a colour-space conversion,
 * and a frame that arrived as a GPU surface needs neither.
 */
enum class ConversionKind : std::uint8_t {
    kNone = 0,     ///< nothing was staged (the frame had no pixels)
    kCopy = 1,     ///< a plain plane copy — no colour conversion
    kConvert = 2,  ///< a colour-space conversion (BGRA → I420, NV12 → I420)
};

/** Copy one plane row-block from a caller-owned source into a staged buffer.
 *
 *  @param dst        Destination plane base (already offset to the plane).
 *  @param dst_stride Destination row stride in bytes.
 *  @param src        Source plane base.
 *  @param src_stride Source row stride in bytes.
 *  @param rows       Number of rows to copy.
 *  @param row_bytes  Number of bytes to copy per row.
 */
inline void copy_plane_rows(std::uint8_t* dst, std::int32_t dst_stride,
                            const std::uint8_t* src, std::int32_t src_stride,
                            std::uint32_t rows, std::size_t row_bytes) noexcept {
    if (!dst || !src || rows == 0 || row_bytes == 0) return;

    // The strides are signed because a layout can describe a bottom-up
    // plane (row 0 at the highest address).  Keep the arithmetic signed
    // so that case works, instead of narrowing them to size_t and
    // wrapping a negative stride into a huge offset.
    const std::ptrdiff_t d_step = dst_stride;
    const std::ptrdiff_t s_step = src_stride;
    std::uint8_t* d = dst;
    const std::uint8_t* s = src;
    for (std::uint32_t row = 0; row < rows; ++row) {
        std::memcpy(d, s, row_bytes);
        d += d_step;
        s += s_step;
    }
}

/** Stage a caller-owned `plugins::VideoSourceFrame` into an owned
 *  `video_frame::VideoFrameBuffer` that the codec can read.
 *
 *  Why this exists: the plugin-level `VideoSourceFrame` is a *view* — its
 *  `plane_*` pointers belong to the source and are only valid for the
 *  duration of the callback.  The codec seam, in contrast, takes a
 *  `plugins::VideoFrame` whose `buffer` is a refcounted
 *  `VideoFrameBuffer` that must stay alive across the `encode()` call.
 *  Software codecs can ignore the pixels entirely, but a hardware encoder
 *  (NVENC) reads them: with a null `buffer` it rejects the frame with
 *  `kErrInvalidParam`.  So the engine owns the copy.
 *
 *  Note on `plane_offset_*`: the plugin producers (e.g. the memory video
 *  source) already resolve the offsets into the `plane_y/u/v` pointers, so
 *  the pointers are treated as absolute plane bases here and the offset
 *  fields are ignored — adding them again would read past the plane.
 *
 *  @param frame  Caller-owned source frame (planes valid for this call).
 *  @param fmt    Target pixel format for the staged buffer.
 *  @param pool   Pool the staged buffer's storage comes from (Phase 3 / C1).
 *  @param out_conversion_kind  (optional) Reports what the staging did,
 *         so the engine can count the conversions (plan C3).  When null
 *         the classification is skipped.
 *  @return The staged buffer, or nullptr when the frame cannot be staged
 *          (no planes, zero dimensions, or an unsupported source format).
 */
inline std::shared_ptr<video_frame::VideoFrameBuffer> stage_source_frame(
        const plugins::VideoSourceFrame& frame,
        plugins::VideoPixelFormat fmt,
        video_frame::VideoFrameBufferPool& pool,
        ConversionKind* out_conversion_kind = nullptr) noexcept {
    if (frame.width == 0 || frame.height == 0) return nullptr;
    if (!frame.plane_y) return nullptr;

    // Only I420-family sources can be staged without a colour-space
    // conversion.  Packed BGRA needs one, and the engine owns it now:
    // `video_frame::convert_bgra_to_i420()` is the shared BT.601
    // transform every desktop-capture path needs (the synthetic capture
    // device produces BGRA, and so do the Windows / macOS compositors).
    //
    // RGBA is *not* accepted: it is a different byte order, and the
    // shared transform reads BGR.  A source that has RGBA must convert
    // it itself rather than have the engine guess.
    const bool source_is_nv12 = frame.format == plugins::VideoPixelFormat::kNV12;
    const bool source_is_bgra = frame.format == plugins::VideoPixelFormat::kBGRA;
    const bool source_is_planar_420 =
        frame.format == plugins::VideoPixelFormat::kI420
     || frame.format == plugins::VideoPixelFormat::kYV12
     || source_is_nv12
     || frame.format == plugins::VideoPixelFormat::kUnknown;
    if (!source_is_planar_420 && !source_is_bgra) return nullptr;

    // The target must be a planar 4:2:0 layout — that is what every
    // hardware encoder consumes today.
    if (fmt != plugins::VideoPixelFormat::kI420
     && fmt != plugins::VideoPixelFormat::kYV12) {
        return nullptr;
    }

    const auto layout = video_frame::compute_layout(fmt, frame.width, frame.height);
    if (layout.buffer_size == 0) return nullptr;

    // Phase 3 / C1: take the storage from the pool instead of allocating
    // a fresh frame-sized block per frame.  At 1080p30 that was a ~3 MB
    // allocation *and* a ~3 MB memset every 33 ms, on top of the copy the
    // staging itself does.
    auto staged = pool.acquire(layout);
    if (!staged) return nullptr;

    const std::uint32_t half_w = (frame.width + 1u) / 2u;
    const std::uint32_t half_h = (frame.height + 1u) / 2u;

    // The source strides default to a tight packing when the producer did
    // not set them (e.g. a hand-built test frame).
    const std::int32_t src_stride_y = frame.stride_y > 0
        ? frame.stride_y : static_cast<std::int32_t>(frame.width);
    // NV12's chroma plane is interleaved, so one row of it spans the full
    // width; a planar 4:2:0 chroma row spans half the width.
    const std::int32_t src_stride_c = frame.stride_u > 0
        ? frame.stride_u
        : static_cast<std::int32_t>(source_is_nv12 ? frame.width : half_w);

    if (source_is_bgra) {
        // Packed BGRA source (the desktop-compositor format).  One
        // BT.601 conversion into the staged I420 planes — no
        // intermediate copy, no second colour-space pass.
        //
        // The source stride defaults to the tight packing (4 bytes per
        // pixel), which is what every compositor hands over.
        const std::int32_t bgra_stride = frame.stride_y > 0
            ? frame.stride_y
            : static_cast<std::int32_t>(frame.width * 4u);
        video_frame::convert_bgra_to_i420(
            frame.width, frame.height,
            frame.plane_y, bgra_stride,
            staged->plane(0), layout.strides[0],
            staged->plane(1), layout.strides[1],
            staged->plane(2), layout.strides[2]);
        if (out_conversion_kind) *out_conversion_kind = ConversionKind::kConvert;
        return staged;
    }

    if (source_is_nv12) {
        // NV12 source: one interleaved UV plane.  De-interleave it into
        // the staged I420 U and V planes (this also copies the luma plane).
        if (!frame.plane_u) return nullptr;
        video_frame::convert_nv12_to_i420(
            frame.width, frame.height,
            frame.plane_y, src_stride_y,
            frame.plane_u, src_stride_c,
            staged->plane(0), layout.strides[0],
            staged->plane(1), layout.strides[1],
            staged->plane(2), layout.strides[2]);
        if (out_conversion_kind) *out_conversion_kind = ConversionKind::kConvert;
        return staged;
    }

    // I420 / YV12 source: three separate planes.
    if (!frame.plane_u || !frame.plane_v) return nullptr;

    // ---- Luma ---------------------------------------------------------------
    copy_plane_rows(staged->plane(0), layout.strides[0],
                    frame.plane_y, src_stride_y,
                    frame.height, frame.width);

    // ---- Chroma -------------------------------------------------------------
    copy_plane_rows(staged->plane(1), layout.strides[1],
                    frame.plane_u, src_stride_c,
                    half_h, half_w);
    copy_plane_rows(staged->plane(2), layout.strides[2],
                    frame.plane_v, src_stride_c,
                    half_h, half_w);
    if (out_conversion_kind) *out_conversion_kind = ConversionKind::kCopy;
    return staged;
}

}  // namespace nimrtc::engine::detail
