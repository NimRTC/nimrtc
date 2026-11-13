/**
 * @file src/modules/video_frame/src/scaler.cpp
 * @brief VideoFrameScaler 鈥?libyuv-backed scaling + colour conversion.
 *
 * Implements the declarations in `nimrtc/video_frame/scaler.hpp`.
 *
 * Two build shapes are supported:
 *
 *   - `NIMRTC_USE_LIBYUV=1`  鈥?the vendored libyuv is linked in; every
 *                              call goes to the SIMD implementation.
 *   - otherwise               鈥?the module still compiles, `available()`
 *                              returns false, and every call returns a
 *                              clean `NotImplemented` error.  This is the
 *                              `NIMRTC_VENDORED_LIBYUV=OFF` path (and the
 *                              fresh-clone-without-submodule path).
 */

#include <nimrtc/video_frame/scaler.hpp>

#include <string>

#if defined(NIMRTC_USE_LIBYUV) && NIMRTC_USE_LIBYUV
    #include <libyuv/convert.h>
    #include <libyuv/convert_argb.h>
    #include <libyuv/scale.h>
#endif

namespace nimrtc::video_frame {

namespace {

#if defined(NIMRTC_USE_LIBYUV) && NIMRTC_USE_LIBYUV

/// libyuv's FilterMode and NimRTC's ScaleFilter share the same numbering
/// (kFilterNone / kFilterLinear / kFilterBilinear / kFilterBox), so the
/// translation is a cast 鈥?but it is written out as a switch so that a
/// future re-ordering of NimRTC's enum cannot silently pick the wrong
/// libyuv filter.
constexpr libyuv::FilterMode to_libyuv_filter(ScaleFilter f) noexcept {
    switch (f) {
        case ScaleFilter::kPoint:    return libyuv::kFilterNone;
        case ScaleFilter::kLinear:   return libyuv::kFilterLinear;
        case ScaleFilter::kBilinear: return libyuv::kFilterBilinear;
        case ScaleFilter::kBox:      return libyuv::kFilterBox;
    }
    return libyuv::kFilterBilinear;
}

#endif // NIMRTC_USE_LIBYUV

constexpr const char* kUnavailableMessage =
    "video_frame: libyuv is not available in this build "
    "(NIMRTC_VENDORED_LIBYUV=OFF, or the submodule was not populated). "
    "Use VideoFrameScaler::available() to probe before calling.";

} // namespace

// ---------------------------------------------------------------------------
// VideoFrameScaler
// ---------------------------------------------------------------------------

bool VideoFrameScaler::available() noexcept {
#if defined(NIMRTC_USE_LIBYUV) && NIMRTC_USE_LIBYUV
    return true;
#else
    return false;
#endif
}

VideoFrameScaler::VideoFrameScaler(ScaleFilter filter) noexcept
    : filter_(filter) {}

core::Result<void> VideoFrameScaler::scale_i420(
    const std::uint8_t* src_y, std::int32_t src_stride_y,
    const std::uint8_t* src_u, std::int32_t src_stride_u,
    const std::uint8_t* src_v, std::int32_t src_stride_v,
    std::uint32_t src_width, std::uint32_t src_height,
    std::uint8_t* dst_y, std::int32_t dst_stride_y,
    std::uint8_t* dst_u, std::int32_t dst_stride_u,
    std::uint8_t* dst_v, std::int32_t dst_stride_v,
    std::uint32_t dst_width, std::uint32_t dst_height) const {
#if defined(NIMRTC_USE_LIBYUV) && NIMRTC_USE_LIBYUV
    // ---- argument validation -------------------------------------------
    if (src_y == nullptr || src_u == nullptr || src_v == nullptr ||
        dst_y == nullptr || dst_u == nullptr || dst_v == nullptr) {
        return core::Result<void>::fail(
            core::ErrorCode::InvalidArgument,
            "video_frame::VideoFrameScaler::scale_i420: null plane pointer");
    }
    if (src_width == 0 || src_height == 0 ||
        dst_width == 0 || dst_height == 0) {
        return core::Result<void>::fail(
            core::ErrorCode::InvalidArgument,
            "video_frame::VideoFrameScaler::scale_i420: zero dimension "
            "(source " + std::to_string(src_width) + "x" +
            std::to_string(src_height) + ", destination " +
            std::to_string(dst_width) + "x" + std::to_string(dst_height) + ")");
    }
    // libyuv's I420 API wants signed strides; the module's layout uses
    // signed strides too, but a negative stride (bottom-up image) is not
    // something libyuv supports.
    if (src_stride_y <= 0 || src_stride_u <= 0 || src_stride_v <= 0 ||
        dst_stride_y <= 0 || dst_stride_u <= 0 || dst_stride_v <= 0) {
        return core::Result<void>::fail(
            core::ErrorCode::InvalidArgument,
            "video_frame::VideoFrameScaler::scale_i420: non-positive stride");
    }

    const int rc = libyuv::I420Scale(
        src_y, src_stride_y,
        src_u, src_stride_u,
        src_v, src_stride_v,
        static_cast<int>(src_width), static_cast<int>(src_height),
        dst_y, dst_stride_y,
        dst_u, dst_stride_u,
        dst_v, dst_stride_v,
        static_cast<int>(dst_width), static_cast<int>(dst_height),
        to_libyuv_filter(filter_));

    if (rc != 0) {
        return core::Result<void>::fail(
            core::ErrorCode::InternalError,
            "video_frame::VideoFrameScaler::scale_i420: libyuv I420Scale "
            "failed (rc=" + std::to_string(rc) + ")");
    }
    return core::Result<void>::make_ok();
#else
    (void)src_y; (void)src_stride_y;
    (void)src_u; (void)src_stride_u;
    (void)src_v; (void)src_stride_v;
    (void)src_width; (void)src_height;
    (void)dst_y; (void)dst_stride_y;
    (void)dst_u; (void)dst_stride_u;
    (void)dst_v; (void)dst_stride_v;
    (void)dst_width; (void)dst_height;
    return core::Result<void>::fail(core::ErrorCode::NotImplemented,
                                     kUnavailableMessage);
#endif
}

core::Result<std::shared_ptr<VideoFrameBuffer>>
VideoFrameScaler::scale_to_buffer(const VideoFrameBuffer& src,
                                  std::uint32_t dst_width,
                                  std::uint32_t dst_height,
                                  VideoFrameBufferPool* pool) const {
    // ---- validation that does not depend on libyuv ------------------------
    if (!src.valid()) {
        return core::Result<std::shared_ptr<VideoFrameBuffer>>::fail(
            core::ErrorCode::InvalidArgument,
            "video_frame::VideoFrameScaler::scale_to_buffer: source buffer "
            "is empty");
    }
    if (src.format() != PixelFormat::kI420) {
        return core::Result<std::shared_ptr<VideoFrameBuffer>>::fail(
            core::ErrorCode::InvalidArgument,
            "video_frame::VideoFrameScaler::scale_to_buffer: source format "
            "must be I420 (got " +
            std::string(pixel_format_name(src.format())) + ")");
    }
    if (dst_width == 0 || dst_height == 0) {
        return core::Result<std::shared_ptr<VideoFrameBuffer>>::fail(
            core::ErrorCode::InvalidArgument,
            "video_frame::VideoFrameScaler::scale_to_buffer: zero target "
            "dimension");
    }
    if (!available()) {
        return core::Result<std::shared_ptr<VideoFrameBuffer>>::fail(
            core::ErrorCode::NotImplemented, kUnavailableMessage);
    }

    // ---- destination -------------------------------------------------------
    const VideoFrameLayout dst_layout =
        compute_layout(PixelFormat::kI420, dst_width, dst_height);

    std::shared_ptr<VideoFrameBuffer> dst;
    if (pool != nullptr) {
        dst = pool->acquire(dst_layout);
        if (!dst) {
            return core::Result<std::shared_ptr<VideoFrameBuffer>>::fail(
                core::ErrorCode::OutOfMemory,
                "video_frame::VideoFrameScaler::scale_to_buffer: the pool "
                "could not provide a destination buffer");
        }
    } else {
        dst = std::make_shared<VideoFrameBuffer>(dst_layout);
    }

    const VideoFrameLayout& sl = src.layout();
    const auto rc = scale_i420(
        src.plane(0), sl.strides[0],
        src.plane(1), sl.strides[1],
        src.plane(2), sl.strides[2],
        sl.width, sl.height,
        dst->plane(0), dst_layout.strides[0],
        dst->plane(1), dst_layout.strides[1],
        dst->plane(2), dst_layout.strides[2],
        dst_width, dst_height);

    if (!rc.ok()) {
        return core::Result<std::shared_ptr<VideoFrameBuffer>>::fail(rc.error());
    }
    return core::Result<std::shared_ptr<VideoFrameBuffer>>::ok(std::move(dst));
}

core::Result<void> VideoFrameScaler::bgra_to_i420(
    std::uint32_t width,
    std::uint32_t height,
    const std::uint8_t* bgra_in, std::int32_t bgra_stride,
    std::uint8_t* y_out, std::int32_t y_stride,
    std::uint8_t* u_out, std::int32_t u_stride,
    std::uint8_t* v_out, std::int32_t v_stride) const {
#if defined(NIMRTC_USE_LIBYUV) && NIMRTC_USE_LIBYUV
    if (bgra_in == nullptr || y_out == nullptr ||
        u_out == nullptr || v_out == nullptr) {
        return core::Result<void>::fail(
            core::ErrorCode::InvalidArgument,
            "video_frame::VideoFrameScaler::bgra_to_i420: null plane "
            "pointer");
    }
    if (width == 0 || height == 0) {
        return core::Result<void>::fail(
            core::ErrorCode::InvalidArgument,
            "video_frame::VideoFrameScaler::bgra_to_i420: zero "
            "dimension");
    }
    if (bgra_stride <= 0 || y_stride <= 0 || u_stride <= 0 || v_stride <= 0) {
        return core::Result<void>::fail(
            core::ErrorCode::InvalidArgument,
            "video_frame::VideoFrameScaler::bgra_to_i420: non-positive "
            "stride");
    }

    // libyuv spells its packed formats by *word* order, not by memory
    // order.  Its "ARGB" is the 32-bit word 0xAARRGGBB, which on every
    // little-endian target NimRTC supports is laid out in memory as
    // B, G, R, A — exactly NimRTC's `PixelFormat::kBGRA`.  libyuv's own
    // "BGRA" is the word 0xBBGGRRAA, i.e. memory order A, R, G, B, and
    // is *not* what this module's callers hand us.
    //
    // So: NimRTC BGRA  ->  libyuv ARGBToI420 / I420ToARGB.
    const int rc = libyuv::ARGBToI420(
        bgra_in, bgra_stride,
        y_out, y_stride,
        u_out, u_stride,
        v_out, v_stride,
        static_cast<int>(width), static_cast<int>(height));

    if (rc != 0) {
        return core::Result<void>::fail(
            core::ErrorCode::InternalError,
            "video_frame::VideoFrameScaler::bgra_to_i420: libyuv "
            "ARGBToI420 failed (rc=" + std::to_string(rc) + ")");
    }
    return core::Result<void>::make_ok();
#else
    (void)width; (void)height;
    (void)bgra_in; (void)bgra_stride;
    (void)y_out; (void)y_stride;
    (void)u_out; (void)u_stride;
    (void)v_out; (void)v_stride;
    return core::Result<void>::fail(core::ErrorCode::NotImplemented,
                                     kUnavailableMessage);
#endif
}

core::Result<void> VideoFrameScaler::i420_to_bgra(
    std::uint32_t width,
    std::uint32_t height,
    const std::uint8_t* y_in, std::int32_t y_stride,
    const std::uint8_t* u_in, std::int32_t u_stride,
    const std::uint8_t* v_in, std::int32_t v_stride,
    std::uint8_t* bgra_out, std::int32_t bgra_stride) const {
#if defined(NIMRTC_USE_LIBYUV) && NIMRTC_USE_LIBYUV
    if (y_in == nullptr || u_in == nullptr || v_in == nullptr ||
        bgra_out == nullptr) {
        return core::Result<void>::fail(
            core::ErrorCode::InvalidArgument,
            "video_frame::VideoFrameScaler::i420_to_bgra: null plane "
            "pointer");
    }
    if (width == 0 || height == 0) {
        return core::Result<void>::fail(
            core::ErrorCode::InvalidArgument,
            "video_frame::VideoFrameScaler::i420_to_bgra: zero "
            "dimension");
    }
    if (y_stride <= 0 || u_stride <= 0 || v_stride <= 0 ||
        bgra_stride <= 0) {
        return core::Result<void>::fail(
            core::ErrorCode::InvalidArgument,
            "video_frame::VideoFrameScaler::i420_to_bgra: non-positive "
            "stride");
    }

    // See the note in `bgra_to_i420`: NimRTC's `kBGRA` is libyuv's
    // "ARGB" (memory order B, G, R, A), so the matching libyuv entry
    // point is `I420ToARGB`.
    const int rc = libyuv::I420ToARGB(
        y_in, y_stride,
        u_in, u_stride,
        v_in, v_stride,
        bgra_out, bgra_stride,
        static_cast<int>(width), static_cast<int>(height));

    if (rc != 0) {
        return core::Result<void>::fail(
            core::ErrorCode::InternalError,
            "video_frame::VideoFrameScaler::i420_to_bgra: libyuv "
            "I420ToARGB failed (rc=" + std::to_string(rc) + ")");
    }
    return core::Result<void>::make_ok();
#else
    (void)width; (void)height;
    (void)y_in; (void)y_stride;
    (void)u_in; (void)u_stride;
    (void)v_in; (void)v_stride;
    (void)bgra_out; (void)bgra_stride;
    return core::Result<void>::fail(core::ErrorCode::NotImplemented,
                                     kUnavailableMessage);
#endif
}

} // namespace nimrtc::video_frame
