/**
 * @file src/modules/h264/src/hw_backend_base.hpp
 * @brief Common helpers for HW-accelerated H.264 backends (NVENC/AMF/QSV/
 *        DXVA/VAAPI/NVDEC/openh264).
 *
 * All HW backends share the same output contract:
 *   - encode(): produce an Annex B bitstream (SPS + PPS + slice + IDR)
 *   - decode(): consume an Annex B bitstream, produce an I420 frame
 *
 * The helpers below are used by every backend implementation to:
 *   - Locate / extract SPS + PPS from an encoded buffer (for SDP fmtp hints)
 *   - Wrap a slice NALU into a complete Annex B access unit
 *   - Convert between YUV420 / NV12 GPU surfaces and the common I420
 *     staging buffer the engine uses for the CPU fallback path
 *   - Compute QP / IDR flags from the codec-side output
 *
 * The header is intentionally *implementation-only* (under src/, not
 * include/) — the public plugin surface is IVideoCodec in
 * <nimrtc/plugins/video_codec.hpp>; this is plumbing shared between the
 * individual HW source files.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/h264/bitstream.hpp>
#include <nimrtc/plugins/video_codec.hpp>

namespace nimrtc::h264::hw_backend {

// ---------------------------------------------------------------------------
// Bitstream helpers — re-export from bitstream.hpp so backends don't need
// to include that header directly. We keep these as free functions so
// they're easy to unit-test in isolation.
// ---------------------------------------------------------------------------

/** Locate the first SPS NALU in @p bs. Returns nullptr if absent. */
inline const nimrtc::h264::ParsedNalu*
find_sps_in(core::ByteSpan bs) noexcept {
    // The bitstream parser returns an internal vector; we scan by hand so
    // we can avoid an allocation. For a typical 128-byte Annex B packet
    // the linear scan is ~3-4 NALUs.
    while(!bs.empty()) {
        const std::uint8_t* p = bs.data();
        std::size_t i = 0;
        // Skip start code.
        if(i + 3 < bs.size() && p[i]==0 && p[i+1]==0 && p[i+2]==0 && p[i+3]==1) {
            i += 4;
        } else if(i + 2 < bs.size() && p[i]==0 && p[i+1]==0 && p[i+2]==1) {
            i += 3;
        } else {
            return nullptr;
        }
        // Find the next start code (or end of buffer).
        std::size_t j = i;
        while(j + 2 < bs.size()) {
            if(p[j]==0 && p[j+1]==0 && (p[j+2]==1 || (j+3<bs.size() && p[j+2]==0 && p[j+3]==1)))
                break;
            ++j;
        }
        std::size_t nalu_len = j - i;
        if(nalu_len > 0) {
            auto t = static_cast<nimrtc::video_payload::h264::NaluType>(p[i] & 0x1F);
            if(t == nimrtc::video_payload::h264::NaluType::kSPS) {
                static thread_local nimrtc::h264::ParsedNalu out;
                out.type = t;
                out.data = core::ByteSpan{p + i, nalu_len};
                out.start_code_len = (p[2]==1) ? 3u : 4u;
                return &out;
            }
        }
        bs = core::ByteSpan{p + j, bs.size() - j};
    }
    return nullptr;
}

/** Locate the first PPS NALU in @p bs. Returns nullptr if absent. */
inline const nimrtc::h264::ParsedNalu*
find_pps_in(core::ByteSpan bs) noexcept {
    while(!bs.empty()) {
        const std::uint8_t* p = bs.data();
        std::size_t i = 0;
        if(i + 3 < bs.size() && p[i]==0 && p[i+1]==0 && p[i+2]==0 && p[i+3]==1) {
            i += 4;
        } else if(i + 2 < bs.size() && p[i]==0 && p[i+1]==0 && p[i+2]==1) {
            i += 3;
        } else {
            return nullptr;
        }
        std::size_t j = i;
        while(j + 2 < bs.size()) {
            if(p[j]==0 && p[j+1]==0 && (p[j+2]==1 || (j+3<bs.size() && p[j+2]==0 && p[j+3]==1)))
                break;
            ++j;
        }
        std::size_t j_end = j;
        // Search next NALU for PPS
        while(j_end + 2 < bs.size()) {
            if(p[j_end]==0 && p[j_end+1]==0 &&
               (p[j_end+2]==1 || (j_end+3<bs.size() && p[j_end+2]==0 && p[j_end+3]==1)))
                break;
            ++j_end;
        }
        std::size_t nalu_len = j_end - i;
        if(nalu_len > 0) {
            auto t = static_cast<nimrtc::video_payload::h264::NaluType>(p[i] & 0x1F);
            if(t == nimrtc::video_payload::h264::NaluType::kPPS) {
                static thread_local nimrtc::h264::ParsedNalu out;
                out.type = t;
                out.data = core::ByteSpan{p + i, nalu_len};
                out.start_code_len = (p[2]==1) ? 3u : 4u;
                return &out;
            }
        }
        if(j >= bs.size()) break;
        bs = core::ByteSpan{p + j, bs.size() - j};
    }
    return nullptr;
}

/** True if @p bs contains an IDR slice NALU (NAL type 5). */
inline bool contains_idr_slice(core::ByteSpan bs) noexcept {
    while(!bs.empty()) {
        const std::uint8_t* p = bs.data();
        std::size_t i = 0;
        if(i + 3 < bs.size() && p[i]==0 && p[i+1]==0 && p[i+2]==0 && p[i+3]==1) i += 4;
        else if(i + 2 < bs.size() && p[i]==0 && p[i+1]==0 && p[i+2]==1) i += 3;
        else return false;
        auto t = static_cast<nimrtc::video_payload::h264::NaluType>(p[i] & 0x1F);
        if(t == nimrtc::video_payload::h264::NaluType::kSliceIDR) return true;
        // Advance to next start code.
        std::size_t j = i + 1;
        while(j + 2 < bs.size()) {
            if(p[j]==0 && p[j+1]==0 && (p[j+2]==1 || (j+3<bs.size() && p[j+2]==0 && p[j+3]==1)))
                break;
            ++j;
        }
        if(j >= bs.size()) return false;
        bs = core::ByteSpan{p + j, bs.size() - j};
    }
    return false;
}

// ---------------------------------------------------------------------------
// Annex B packer —wraps an "raw" NALU array produced by an HW encoder
// (which often returns SPS / PPS / slice as separate buffers with their
// length-prefixed forms) into a single Annex B access unit suitable for
// direct emission on the wire.
// ---------------------------------------------------------------------------

/** Pack (sps, pps, slice) into a single Annex B buffer. The slice may be
 *  empty (e.g. when only headers are emitted by an IDR access unit). */
inline std::size_t pack_annex_b(core::ByteSpan sps,
                                core::ByteSpan pps,
                                core::ByteSpan slice,
                                std::uint8_t* dst,
                                std::size_t   dst_capacity) noexcept {
    constexpr std::uint8_t sc4[4] = {0x00, 0x00, 0x00, 0x01};
    const std::size_t need = (sps.empty() ? 0 : 4 + sps.size()) +
                             (pps.empty() ? 0 : 4 + pps.size()) +
                             (slice.empty() ? 0 : 4 + slice.size());
    if(need > dst_capacity) return 0;
    std::size_t off = 0;
    if(!sps.empty()) {
        std::memcpy(dst + off, sc4, 4); off += 4;
        std::memcpy(dst + off, sps.data(), sps.size()); off += sps.size();
    }
    if(!pps.empty()) {
        std::memcpy(dst + off, sc4, 4); off += 4;
        std::memcpy(dst + off, pps.data(), pps.size()); off += pps.size();
    }
    if(!slice.empty()) {
        std::memcpy(dst + off, sc4, 4); off += 4;
        std::memcpy(dst + off, slice.data(), slice.size()); off += slice.size();
    }
    return off;
}

// ---------------------------------------------------------------------------
// I420 staging —when an HW encoder outputs to GPU memory, we still need
// to produce CPU pixels so the engine's CPU fallback path can render the
// frame. This helper is a *no-op* when the encoder wrote directly into
// the engine-supplied CPU buffer; it only fires when the encoder returns
// a GPU handle that needs a blit. Each backend decides whether to call it.
//
// NV12 → I420 conversion layout (per pixel pair):
//   Y plane  : WxH bytes (luma)
//   UV plane : (W/2)x(H/2) interleaved NV12 (UVUV...)
//   I420     : Y plane + U plane + V plane
//
// We expand NV12's interleaved UV into separate U/V planes; this is the
// same conversion used in ffmpeg's nv12_to_yuv420p. The simple loop is
// fine for our staging purposes (no SIMD needed — the engine only does
// this on slow path).
// ---------------------------------------------------------------------------

/** Convert an NV12 GPU readback into I420 (3 separate planes).
 *  @p nv12 / @p w / @p h describe the input. Caller allocates the three
 *  I420 planes (sizes are w*h, (w/2)*(h/2), (w/2)*(h/2)). */
inline void nv12_to_i420(const std::uint8_t* nv12,
                         std::uint32_t w, std::uint32_t h,
                         std::uint8_t* y_out,
                         std::uint8_t* u_out,
                         std::uint8_t* v_out) noexcept {
    const std::size_t y_size = static_cast<std::size_t>(w) * h;
    std::memcpy(y_out, nv12, y_size);
    const std::uint8_t* uv = nv12 + y_size;
    const std::size_t uv_w = w / 2;
    const std::size_t uv_h = h / 2;
    for(std::size_t row = 0; row < uv_h; ++row) {
        for(std::size_t col = 0; col < uv_w; ++col) {
            u_out[row * uv_w + col] = uv[(row * w) + (col * 2)];
            v_out[row * uv_w + col] = uv[(row * w) + (col * 2) + 1];
        }
    }
}

// ---------------------------------------------------------------------------
// Common parameters —passed from IVideoCodec::config() into the backend's
// session-open code. Kept as a POD so the SDK wrappers can stash it
// without virtuals.
// ---------------------------------------------------------------------------

struct SessionParams {
    std::uint32_t width            = 0;
    std::uint32_t height           = 0;
    std::uint32_t fps              = 30;
    std::uint32_t bitrate_bps      = 0;
    std::uint32_t keyframe_interval = 60;
    bool          force_idr_next   = false;
};

// ---------------------------------------------------------------------------
// Status mapping helper —turns SDK-specific error codes into NimRTC
// Status values. The actual SDK code isn't included in this header (it's
// platform-specific), so backends call this with a generic "code".
// ---------------------------------------------------------------------------

inline plugins::Status map_error(int code) noexcept {
    if(code == 0) return plugins::kOk;
    if(code < 0)  return plugins::kErrInternal;
    return plugins::kErrInvalidParam;
}

} // namespace nimrtc::h264::hw_backend
