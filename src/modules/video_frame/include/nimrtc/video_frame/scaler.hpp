/**
 * @file nimrtc/video_frame/scaler.hpp
 * @brief VideoFrameScaler — SIMD frame scaling + colour conversion.
 *
 * Remote-desktop plan **C2** (Track C "Pixel"), on top of the vendored
 * **libyuv** (C1).  libyuv is the cross-platform scale / colour library
 * the plan names as the default (RD-10): one implementation covers
 * Windows / macOS / Linux / Android / iOS, with SSSE3 + AVX2 on x86-64
 * and NEON / SVE2 / SME on arm64.
 *
 * ## What this replaces
 *
 * Before this, the `video_frame` module had only hand-written *scalar*
 * reference converters (`convert_i420_to_bgra`, `convert_bgra_to_i420`,
 * …).  They are still here — they are the portable reference the tests
 * compare against — but they are far too slow for a 1080p30 capture →
 * encode path (a 4K capture scaled to the ladder's resolution is a
 * per-frame CPU cost the remote-desktop budget in §B.6 does not allow).
 *
 * ## Filter modes
 *
 * The plan fixes the choice per path:
 *
 *   - `ScaleFilter::kBox`     — the **display** path (quality).
 *   - `ScaleFilter::kBilinear`— the **encode** path (speed / quality
 *                                balance).
 *
 * `kPoint` and `kLinear` are the two remaining libyuv modes; they are
 * offered because a host may want them, but they are *not* what the
 * plan's two paths use.
 *
 * ## Availability
 *
 * libyuv is a **default-ON** dependency of this module, but the module
 * still compiles when the dependency is switched off
 * (`NIMRTC_VENDORED_LIBYUV=OFF`, or a clone without the submodule).  In
 * that case:
 *
 *   - `VideoFrameScaler::available()` returns false,
 *   - every scaling / conversion call returns a clean error
 *     (`core::ErrorCode::NotImplemented`) instead of crashing.
 *
 * A host therefore probes once and picks a path; it never has to
 * special-case a build configuration at compile time.
 *
 * ## Thread safety
 *
 * A `VideoFrameScaler` holds no mutable state beyond its filter mode and
 * is therefore safe to use concurrently as long as no thread changes the
 * filter mid-flight.  (libyuv's scalers are themselves stateless.)
 *
 * ## API stability
 *
 * `NIMRTC_API_STABLE`: the scaler is a durable piece of the module's
 * public surface, in the same category as `VideoFrameBuffer` /
 * `VideoFrameBufferPool` — a host builds its capture → encode pipeline
 * around it, and the plan (C2) fixes the semantics (filter modes,
 * arbitrary target sizes, up- and down-scaling).  The *behaviour* is
 * therefore committed; only additive changes are permitted within the
 * 1.x line.
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.
#include <nimrtc/core/api_version.hpp>

#include <cstdint>
#include <memory>
#include <string_view>

#include <nimrtc/core/error.hpp>

#include <nimrtc/video_frame/frame.hpp>

namespace nimrtc::video_frame {

// ---------------------------------------------------------------------------
// ScaleFilter
// ---------------------------------------------------------------------------

/**
 * @brief The scaling filter used by `VideoFrameScaler`.
 *
 * The values map 1:1 onto libyuv's `libyuv::FilterMode`, so the mapping
 * is a cast rather than a translation table.
 */
enum class NIMRTC_API_STABLE ScaleFilter : std::uint8_t {
    /// Nearest-neighbour (libyuv `kFilterNone`).  Fastest, blockiest.
    ///
    /// Not used by either of the plan's two paths: it aliases visibly on
    /// text, which is the one thing a remote-desktop user cannot tolerate.
    kPoint = 0,

    /// Horizontal interpolation only (libyuv `kFilterLinear`).
    ///
    /// Cheaper than bilinear, but it does not interpolate vertically, so
    /// it is noticeably worse on the vertical downscaling the ladder
    /// does.  Offered for completeness; not a plan path.
    kLinear = 1,

    /// Bilinear interpolation (libyuv `kFilterBilinear`).
    ///
    /// **The encode path's filter** (remote-desktop plan C2): the
    /// speed / quality balance the plan names.  It is noticeably cheaper
    /// than box filtering and, on the *downscaling* the ladder does, its
    /// quality loss is small enough that the encoder's own bit budget
    /// hides it.
    kBilinear = 2,

    /// Box (area-average) filtering (libyuv `kFilterBox`).
    ///
    /// **The display path's filter** (remote-desktop plan C2): the
    /// highest-quality mode, because the display path scales *up* to the
    /// viewer's window and any aliasing there is visible as jittery
    /// text.
    kBox = 3,
};

/** Human-readable filter name (for logging / telemetry). */
NIMRTC_API_STABLE inline std::string_view scale_filter_name(ScaleFilter f) noexcept {
    switch (f) {
        case ScaleFilter::kPoint:    return "point";
        case ScaleFilter::kLinear:   return "linear";
        case ScaleFilter::kBilinear: return "bilinear";
        case ScaleFilter::kBox:      return "box";
        default:                     return "unknown";
    }
}

// ---------------------------------------------------------------------------
// VideoFrameScaler
// ---------------------------------------------------------------------------

/**
 * @brief Scales I420 pictures and converts between BGRA and I420.
 *
 * The class is a thin, stateless wrapper around libyuv.  It never owns
 * pixels: the caller supplies the source and the destination, so the
 * scaler composes with the module's existing buffer pool without
 * copying.
 *
 * Typical use:
 *
 * ```cpp
 * if (!VideoFrameScaler::available()) {
 *     // libyuv was not built in — fall back to the scalar converters.
 *     return;
 * }
 *
 * VideoFrameScaler scaler{ScaleFilter::kBilinear};   // encode path
 * auto scaled = scaler.scale_to_buffer(*src, 1280, 720, &pool);
 * if (!scaled) { log(scaled.error().message()); }
 * ```
 */
class NIMRTC_API_STABLE VideoFrameScaler {
public:
    /**
     * @brief Capability query: is the SIMD backend linked in?
     *
     * Returns false when the module was built without libyuv (the
     * `NIMRTC_VENDORED_LIBYUV=OFF` path, or a clone without the
     * submodule).  Every scaling / conversion call then returns a clean
     * error rather than crashing.
     */
    [[nodiscard]] static bool available() noexcept;

    /**
     * @brief Construct a scaler with the given filter mode.
     *
     * @param filter The filter mode used by every subsequent call.
     *               Defaults to `kBilinear` — the encode path's filter,
     *               which is the more common consumer of this class.
     */
    explicit VideoFrameScaler(ScaleFilter filter = ScaleFilter::kBilinear) noexcept;

    /// The filter mode currently in use.
    [[nodiscard]] ScaleFilter filter() const noexcept { return filter_; }

    /// Change the filter mode.  Affects subsequent calls only.
    void set_filter(ScaleFilter filter) noexcept { filter_ = filter; }

    // -----------------------------------------------------------------------
    // Scaling
    // -----------------------------------------------------------------------

    /**
     * @brief Scale an I420 picture into caller-provided planes.
     *
     * All three planes (Y, U, V) are scaled — the chroma planes are
     * scaled with the same filter, so a solid-colour source survives
     * the round trip without banding.
     *
     * @param src_y / src_u / src_v   Source planes (I420).
     * @param src_stride_y / _u / _v  Source row strides, in bytes.
     * @param src_width / src_height  Source picture size, in pixels.
     * @param dst_y / dst_u / dst_v   Destination planes (I420).
     * @param dst_stride_y / _u / _v  Destination row strides, in bytes.
     * @param dst_width / dst_height  Destination picture size, in pixels.
     *                               May be larger (up-scaling) or smaller
     *                               (down-scaling) than the source.
     *
     * @return `core::Result<void>`; on failure the error is
     *         `InvalidArgument` (null plane, zero dimension, mismatched
     *         dimensions) or `NotImplemented` (libyuv not linked in).
     */
    [[nodiscard]] core::Result<void> scale_i420(
        const std::uint8_t* src_y, std::int32_t src_stride_y,
        const std::uint8_t* src_u, std::int32_t src_stride_u,
        const std::uint8_t* src_v, std::int32_t src_stride_v,
        std::uint32_t src_width, std::uint32_t src_height,
        std::uint8_t* dst_y, std::int32_t dst_stride_y,
        std::uint8_t* dst_u, std::int32_t dst_stride_u,
        std::uint8_t* dst_v, std::int32_t dst_stride_v,
        std::uint32_t dst_width, std::uint32_t dst_height) const;

    /**
     * @brief Scale an I420 frame buffer into a freshly acquired buffer.
     *
     * The convenience form used by the capture → encode path: the
     * destination is a `VideoFrameBuffer` with the target dimensions,
     * acquired from @p pool when one is given (so the steady state
     * allocates nothing), or freshly allocated otherwise.
     *
     * @param src          The source buffer.  Must be an I420 buffer.
     * @param dst_width    Target width, in pixels.
     * @param dst_height   Target height, in pixels.
     * @param pool         Optional buffer pool to acquire the
     *                     destination from.  May be null.
     *
     * @return The scaled buffer, or an error.
     */
    [[nodiscard]] core::Result<std::shared_ptr<VideoFrameBuffer>>
    scale_to_buffer(const VideoFrameBuffer& src,
                    std::uint32_t dst_width,
                    std::uint32_t dst_height,
                    VideoFrameBufferPool* pool = nullptr) const;

    // -----------------------------------------------------------------------
    // Colour conversion
    // -----------------------------------------------------------------------

    /**
     * @brief Convert a packed BGRA picture to I420 (BT.601 limited range).
     *
     * The SIMD counterpart of the module's free `convert_bgra_to_i420`
     * reference helper.  Same colour convention (BT.601 limited range),
     * so the two are interchangeable.
     *
     * (The method is named `bgra_to_i420` rather than
     * `convert_bgra_to_i420` because the module already exports a free
     * function with the latter name in the same namespace — a member and
     * a free function cannot share a name inside one namespace.)
     *
     * @param width / height  Picture size, in pixels.
     * @param bgra_in         Source BGRA (4 bytes per pixel).
     * @param bgra_stride     Source row stride, in bytes.
     * @param y_out / u_out / v_out  Destination planes.
     * @param y_stride / u_stride / v_stride  Destination row strides.
     */
    [[nodiscard]] core::Result<void> bgra_to_i420(
        std::uint32_t width,
        std::uint32_t height,
        const std::uint8_t* bgra_in, std::int32_t bgra_stride,
        std::uint8_t* y_out, std::int32_t y_stride,
        std::uint8_t* u_out, std::int32_t u_stride,
        std::uint8_t* v_out, std::int32_t v_stride) const;

    /**
     * @brief Convert an I420 picture to packed BGRA.
     *
     * The SIMD counterpart of the module's free `convert_i420_to_bgra`
     * reference helper; the alpha channel is filled with 0xFF.
     */
    [[nodiscard]] core::Result<void> i420_to_bgra(
        std::uint32_t width,
        std::uint32_t height,
        const std::uint8_t* y_in, std::int32_t y_stride,
        const std::uint8_t* u_in, std::int32_t u_stride,
        const std::uint8_t* v_in, std::int32_t v_stride,
        std::uint8_t* bgra_out, std::int32_t bgra_stride) const;

private:
    ScaleFilter filter_;
};

} // namespace nimrtc::video_frame
