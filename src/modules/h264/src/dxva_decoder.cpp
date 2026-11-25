/**
 * @file src/modules/h264/src/dxva_decoder.cpp
 * @brief The Microsoft DXVA H.264 decoder — a hardware decode backend.
 *
 * ## What this is
 *
 * The decoder is the Media Foundation H.264 MFT
 * (`CLSID_MSH264DecoderMFT`), which is the Windows system H.264
 * decoder and uses DXVA — the DirectX video acceleration — behind it
 * when the GPU can do it.
 *
 * It is a **decode-only** backend, so it implements the codec seam with
 * `can_encode() == false`: the send path is not served, and the codec
 * module pairs it with an encoder through the `HwCompositeCodec`
 * adapter (see `src/modules/nvcodec/src/hw_composite_codec.cpp`).
 *
 * ## Why it matters: the fallback chain
 *
 * The backend table ranks the decoders (see `hw_backends.cpp`):
 *
 *   430  nvdec_h264   NVIDIA
 *   320  dxva_h264    Microsoft  <- this one
 *    50  openh264_h264  software
 *     0  stub_h264     synthetic
 *
 * So on a machine whose NVIDIA driver cannot decode (an old driver, a
 * non-default adapter, a GPU with no NVDEC unit), DXVA is the *hardware*
 * decode path that keeps `--backend hardware` working.  Without it the
 * only remaining decoder is the software one, and a host that asked for
 * hardware would have to fall back to the CPU.
 *
 * ## The port from the old seam
 *
 * This file used to target the pre-split codec interface — the one with
 * `VideoFrameType`, `EncodedImage`, `VideoEncoderConfig` and
 * `VideoCodecOptions`, and with `decode(const uint8_t*, int, ...)`
 * writing into the frame's own plane pointers.  Those types are gone:
 * the seam is now split into `IVideoEncoder` and `IVideoDecoder`, and a
 * frame is a refcounted `VideoFrameBuffer` rather than a struct with
 * borrowed plane pointers.
 *
 * The old file therefore **never compiled** — the option
 * `NIMRTC_PLUGINS_DXVA` is OFF by default and CI never turned it on,
 * so the rot was invisible.  This rewrite targets the current seam.
 *
 * ## The three contracts the port has to honour
 *
 * 1. **The frame is a buffer, not a struct of pointers.**  The decoder
 *    either writes into the caller's `output_buffers` or allocates its
 *    own `VideoFrameBuffer` and publishes it through `raw_out.buffer`.
 *    The dimensions and the pixel format ride on the buffer's layout,
 *    so `VideoFrame::width()` / `height()` / `format()` read them from
 *    there.
 *
 * 2. **The bitstream format has to be stated.**  `EncodedVideoFrame`
 *    carries `nalu_format`, and the MFT wants Annex B — start codes,
 *    not length prefixes.  A length-prefixed payload is converted here,
 *    because the MFT does not accept it and silently failing would
 *    look like "the decoder is broken".
 *
 * 3. **The geometry is the bitstream's, not the config's.**  The MFT
 *    learns the real picture size from the SPS.  A decoder's configured
 *    width / height is an *upper bound*, so a frame that comes out
 *    larger than the bound is refused rather than written past the end
 *    of the caller's buffers.
 *
 * ## What the MFT actually needs (measured, not guessed)
 *
 * The recipe below was established empirically against
 * `CLSID_MSH264DecoderMFT` on Windows; each point is a difference
 * between "works" and "silently produces no picture at all":
 *
 *  - **Low-latency mode is mandatory.**  Without
 *    `CODECAPI_AVLowLatencyMode = 1` the MFT buffers a whole GOP
 *    (15–30 frames) before emitting anything, and a live stream that
 *    never reaches that depth produces *no* output — every
 *    `ProcessOutput` answers `MF_E_TRANSFORM_NEED_MORE_INPUT`.
 *
 *  - **The payload must be Annex B.**  The MFT's `MFVideoFormat_H264`
 *    (AVC) subtype nominally wants length-prefixed NAL units, but it
 *    only decodes when the bytes actually carry start codes.  Feeding
 *    it genuinely length-prefixed data yields zero pictures.  The
 *    `MFVideoFormat_H264_ES` subtype is accepted too, but it lags: it
 *    emitted 3 pictures for 5 inputs where the AVC subtype emitted 5.
 *
 *  - **The parameter sets may travel with the keyframe.**  The MFT
 *    remembers the SPS / PPS it saw in the IDR, so the frames that
 *    follow decode without them.  The decoder therefore only has to
 *    convert the framing — it does not have to cache the parameter
 *    sets itself.
 *
 *  - **The output subtype is NV12.**  A DXVA decode lands on a DXGI
 *    surface in NV12; the copy into the caller's I420 planes happens
 *    here.
 *
 *  - **The readback goes through the subresource index.**  The decoder's
 *    texture has `ArraySize = 10` (its internal surface pool), so
 *    `CopyResource` into a staging texture fails and a one-slice
 *    staging texture cannot be created.  The slice the picture lives in
 *    comes from `IMFDXGIBuffer::GetSubresourceIndex`, and the copy is a
 *    `CopySubresourceRegion` of that one slice.
 *
 *  - **The staging texture's mapping holds *both* planes.**  A staging
 *    texture of a multi-planar format has exactly **one** mappable
 *    subresource: mapping subresource 1 (the chroma) fails with
 *    `E_INVALIDARG`.  The single mapping covers the whole picture: the
 *    luma's rows first, then the chroma's rows, each with the texture's
 *    own row pitch.  Copying only the luma's bytes leaves the chroma
 *    zeroed, and the picture comes out with a *grey* chroma — the luma
 *    is byte-exact while the chroma is not.
 *
 *  - **The MFT allocates the output sample.**  `MFT_OUTPUT_DATA_BUFFER::
 *    pSample` is filled in by the MFT and the *caller* owns it
 *    afterwards.  A sample that is never released keeps a slice of the
 *    decoder's surface pool alive; the pool runs dry after about ten
 *    frames and the next `ProcessOutput` blocks forever waiting for a
 *    surface that will never come.
 */

#include <nimrtc/h264/hw_backends.hpp>

#if defined(NIMRTC_PLUGINS_DXVA_ON) && defined(_WIN32)

#include <windows.h>

#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <icodecapi.h>
#include <codecapi.h>
#include <wrl/client.h>  // ComPtr

#include <cstdint>
#include <cstring>
#include <deque>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <nimrtc/core/log.hpp>
#include <nimrtc/h264/codec_plugin.hpp>
#include <nimrtc/plugins/video_codec.hpp>
#include <nimrtc/video_frame/frame.hpp>

// The decoder implements the *deprecated* combined codec seam, which is
// what the backend registry hands out (`VideoDecoderBackend::create`
// returns a `std::unique_ptr<IVideoCodec>`).  The suppression is scoped
// to this file's use of the name.
NIMRTC_SUPPRESS_DEPRECATION_BEGIN

using Microsoft::WRL::ComPtr;

namespace nimrtc::h264 {
namespace {

/** Render an HRESULT for the log. */
std::string hr_text(HRESULT hr) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::setw(8) << std::setfill('0')
        << static_cast<unsigned long>(hr);
    return oss.str();
}

} // namespace
} // namespace nimrtc::h264

namespace nimrtc::h264::dxva_backend {
namespace {

/** True iff the payload starts with an Annex B start code. */
bool looks_like_annex_b(const std::uint8_t* p, std::size_t size) noexcept {
    if (!p || size < 4) return false;
    if (p[0] == 0 && p[1] == 0 && p[2] == 1) return true;
    return p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1;
}

/**
 * @brief Convert a length-prefixed NAL unit stream into Annex B.
 *
 * The MFT wants Annex B (start codes).  The engine's RTP path delivers
 * length-prefixed NAL units, so the conversion happens here — the
 * alternative would be a decoder that rejects every frame the engine
 * sends it.
 *
 * A payload that is *already* Annex B is passed through unchanged: the
 * length-prefix parse below would read the start code as a (huge)
 * length and bail out.
 */
std::vector<std::uint8_t> to_annex_b(const std::uint8_t* payload,
                                      std::size_t size) {
    std::vector<std::uint8_t> out;
    out.reserve(size + 64);

    std::size_t offset = 0;
    bool converted = false;

    while (offset + 4 <= size) {
        const std::uint32_t nalu_len =
            (static_cast<std::uint32_t>(payload[offset]) << 24) |
            (static_cast<std::uint32_t>(payload[offset + 1]) << 16) |
            (static_cast<std::uint32_t>(payload[offset + 2]) << 8) |
             static_cast<std::uint32_t>(payload[offset + 3]);

        // A sane NAL length.  A start code parses as a large number, so
        // this is also the test that says "this is not length-prefixed".
        if (nalu_len == 0 || nalu_len > size - offset - 4) break;

        offset += 4;
        out.push_back(0);
        out.push_back(0);
        out.push_back(0);
        out.push_back(1);
        out.insert(out.end(), payload + offset, payload + offset + nalu_len);
        offset += nalu_len;
        converted = true;
    }

    if (!converted) {
        // Not length-prefixed — assume it is already Annex B (or
        // something we cannot improve on) and pass it through.
        out.assign(payload, payload + size);
    }
    return out;
}

// ---------------------------------------------------------------------------
// The SPS's conformance window
// ---------------------------------------------------------------------------
//
// H.264 codes in whole 16x16 macroblocks, so a picture whose visible size
// is not a multiple of 16 is *coded* larger than it is shown: a 322x242
// stream is coded as 336x256, and the SPS's frame-cropping window names
// the rectangle that is actually shown (the top-left 322x242).
//
// The MFT does not expose that window, so it is read out of the SPS
// directly.  This is not an optimisation: without it the decoder would
// hand the caller a picture that is 14 columns and 14 rows wider than
// the geometry the peer announced.

/** The visible geometry named by an SPS. */
struct SpsCrop {
    bool valid = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    /** The offset of the visible rectangle inside the coded picture. */
    std::uint32_t left = 0;
    std::uint32_t top = 0;
};

namespace {

/** A bit reader over an RBSP (emulation-prevention bytes already removed). */
class RbspBits {
public:
    RbspBits(const std::uint8_t* data, std::size_t size) noexcept
        : data_(data), size_(size) {}

    bool read_bit(std::uint32_t& out) noexcept {
        if (bit_ >= size_ * 8) return false;
        const std::size_t byte_idx = bit_ / 8;
        const std::uint32_t bit_in_byte = 7u - static_cast<std::uint32_t>(bit_ % 8);
        out = (data_[byte_idx] >> bit_in_byte) & 0x01u;
        ++bit_;
        return true;
    }

    bool read_bits(std::uint32_t n, std::uint32_t& out) noexcept {
        std::uint32_t v = 0;
        for (std::uint32_t i = 0; i < n; ++i) {
            std::uint32_t b = 0;
            if (!read_bit(b)) return false;
            v = (v << 1) | b;
        }
        out = v;
        return true;
    }

    /** An unsigned Exp-Golomb code (ue(v)). */
    bool read_ue(std::uint32_t& out) noexcept {
        std::uint32_t zeros = 0;
        std::uint32_t b = 0;
        while (read_bit(b)) {
            if (b) {
                std::uint32_t rest = 0;
                if (!read_bits(zeros, rest)) return false;
                out = (1u << zeros) - 1u + rest;
                return true;
            }
            if (++zeros > 32) return false;
        }
        return false;
    }

    /** A signed Exp-Golomb code (se(v)). */
    bool read_se(std::int32_t& out) noexcept {
        std::uint32_t v = 0;
        if (!read_ue(v)) return false;
        const std::int32_t k = static_cast<std::int32_t>((v + 1) / 2);
        out = (v & 1u) ? k : -k;
        return true;
    }

private:
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t bit_ = 0;
};

} // namespace

/**
 * @brief Parse the visible geometry out of an SPS NAL unit.
 *
 * @param nalu       The SPS NAL unit, *including* its 1-byte NAL header.
 * @param nalu_size  Its size in bytes.
 */
SpsCrop parse_sps_crop(const std::uint8_t* nalu, std::size_t nalu_size) noexcept {
    SpsCrop out;
    if (!nalu || nalu_size < 4) return out;

    // Strip the emulation-prevention bytes: the RBSP is what the syntax
    // elements are actually coded in.
    std::vector<std::uint8_t> rbsp;
    rbsp.reserve(nalu_size);
    for (std::size_t i = 1; i < nalu_size; ++i) {  // skip the NAL header
        if (i + 2 < nalu_size && nalu[i] == 0 && nalu[i + 1] == 0 && nalu[i + 2] == 0x03) {
            rbsp.push_back(0);
            rbsp.push_back(0);
            i += 2;
        } else {
            rbsp.push_back(nalu[i]);
        }
    }

    RbspBits bits(rbsp.data(), rbsp.size());

    // The first three bytes of the SPS are *fixed-length*: profile_idc,
    // then the constraint_set_flags byte (with its two reserved bits),
    // then level_idc.  Only then does the Exp-Golomb syntax start.
    //
    // Reading only two bytes here shifts the whole SPS by eight bits —
    // every ue(v) after it lands on the wrong bit, and the crop window
    // that comes out of it is nonsense (a 322x242 stream's SPS yields
    // "32x2688" that way).
    std::uint32_t profile_idc = 0, constraint_flags = 0, level_idc = 0;
    std::uint32_t sps_id = 0;
    if (!bits.read_bits(8, profile_idc)) return out;
    if (!bits.read_bits(8, constraint_flags)) return out;
    if (!bits.read_bits(8, level_idc)) return out;
    (void)constraint_flags;
    (void)level_idc;
    if (!bits.read_ue(sps_id)) return out;

    // The chroma format decides the crop unit, so it has to be read
    // before the frame-cropping offsets.
    std::uint32_t chroma_format_idc = 1;
    std::uint32_t separate_colour_plane = 0;
    const bool high_profile =
        profile_idc == 100 || profile_idc == 110 || profile_idc == 122 ||
        profile_idc == 244 || profile_idc == 44  || profile_idc == 83  ||
        profile_idc == 86  || profile_idc == 118 || profile_idc == 128 ||
        profile_idc == 138 || profile_idc == 139 || profile_idc == 134 ||
        profile_idc == 135;

    if (high_profile) {
        if (!bits.read_ue(chroma_format_idc)) return out;
        if (chroma_format_idc == 3) {
            if (!bits.read_bit(separate_colour_plane)) return out;
        }
        std::uint32_t tmp = 0;
        if (!bits.read_ue(tmp)) return out;   // bit_depth_luma_minus8
        if (!bits.read_ue(tmp)) return out;   // bit_depth_chroma_minus8
        if (!bits.read_bit(tmp)) return out;  // qpprime_y_zero_transform_bypass
        std::uint32_t scaling_matrix_present = 0;
        if (!bits.read_bit(scaling_matrix_present)) return out;
        if (scaling_matrix_present) {
            const std::uint32_t list_count =
                (chroma_format_idc != 3) ? 8u : 12u;
            for (std::uint32_t i = 0; i < list_count; ++i) {
                std::uint32_t present = 0;
                if (!bits.read_bit(present)) return out;
                if (!present) continue;
                const std::uint32_t size = (i < 6) ? 16u : 64u;
                std::int32_t last = 8, next = 8;
                for (std::uint32_t j = 0; j < size; ++j) {
                    if (next != 0) {
                        std::int32_t delta = 0;
                        if (!bits.read_se(delta)) return out;
                        next = (last + delta + 256) % 256;
                    }
                    last = (next == 0) ? last : next;
                }
            }
        }
    }

    std::uint32_t tmp = 0;
    if (!bits.read_ue(tmp)) return out;   // log2_max_frame_num_minus4
    std::uint32_t poc_type = 0;
    if (!bits.read_ue(poc_type)) return out;
    if (poc_type == 0) {
        if (!bits.read_ue(tmp)) return out;  // log2_max_pic_order_cnt_lsb_minus4
    } else if (poc_type == 1) {
        std::uint32_t flag = 0;
        if (!bits.read_bit(flag)) return out;
        std::int32_t se = 0;
        if (!bits.read_se(se)) return out;
        if (!bits.read_se(se)) return out;
        std::uint32_t cycle = 0;
        if (!bits.read_ue(cycle)) return out;
        for (std::uint32_t i = 0; i < cycle; ++i) {
            if (!bits.read_se(se)) return out;
        }
    }
    if (!bits.read_ue(tmp)) return out;   // max_num_ref_frames
    if (!bits.read_bit(tmp)) return out;  // gaps_in_frame_num_value_allowed

    std::uint32_t width_in_mbs_minus1 = 0, height_in_map_units_minus1 = 0;
    if (!bits.read_ue(width_in_mbs_minus1)) return out;
    if (!bits.read_ue(height_in_map_units_minus1)) return out;

    std::uint32_t frame_mbs_only_flag = 1;
    if (!bits.read_bit(frame_mbs_only_flag)) return out;
    if (!frame_mbs_only_flag) {
        if (!bits.read_bit(tmp)) return out;  // mb_adaptive_frame_field_flag
    }
    if (!bits.read_bit(tmp)) return out;      // direct_8x8_inference_flag

    std::uint32_t cropping_present = 0;
    if (!bits.read_bit(cropping_present)) return out;

    const std::uint32_t pic_width_in_mbs = width_in_mbs_minus1 + 1;
    const std::uint32_t pic_height_in_map_units = height_in_map_units_minus1 + 1;

    std::uint32_t crop_left = 0, crop_right = 0, crop_top = 0, crop_bottom = 0;
    if (cropping_present) {
        if (!bits.read_ue(crop_left)) return out;
        if (!bits.read_ue(crop_right)) return out;
        if (!bits.read_ue(crop_top)) return out;
        if (!bits.read_ue(crop_bottom)) return out;
    }

    // The crop units (ITU-T H.264 §7.4.2.1.1).  For the common 4:2:0
    // case they are 2x2 (and 2x1 for a field-coded picture).
    std::uint32_t sub_width_c = 2, sub_height_c = 2;
    if (chroma_format_idc == 0) {
        sub_width_c = 1;
        sub_height_c = 1;
    } else if (chroma_format_idc == 1) {
        sub_width_c = 2;
        sub_height_c = 2;
    } else if (chroma_format_idc == 2) {
        sub_width_c = 2;
        sub_height_c = 1;
    } else {  // 4:4:4
        sub_width_c = 1;
        sub_height_c = 1;
    }
    if (separate_colour_plane) {
        sub_width_c = 1;
        sub_height_c = 1;
    }

    const std::uint32_t crop_unit_x = sub_width_c;
    const std::uint32_t crop_unit_y = sub_height_c * (2u - frame_mbs_only_flag);

    const std::uint32_t frame_height_in_mbs =
        (2u - frame_mbs_only_flag) * pic_height_in_map_units;

    const std::uint32_t coded_width = pic_width_in_mbs * 16u;
    const std::uint32_t coded_height = frame_height_in_mbs * 16u;

    const std::uint32_t visible_width =
        coded_width - (crop_left + crop_right) * crop_unit_x;
    const std::uint32_t visible_height =
        coded_height - (crop_top + crop_bottom) * crop_unit_y;

    if (visible_width == 0 || visible_height == 0 ||
        visible_width > coded_width || visible_height > coded_height) {
        return out;
    }

    out.valid = true;
    out.width = visible_width;
    out.height = visible_height;
    // The visible rectangle is not always at the top-left: the crop
    // window's offsets are what place it inside the coded picture.
    out.left = crop_left * crop_unit_x;
    out.top  = crop_top  * crop_unit_y;
    return out;
}

/**
 * @brief Find the SPS in an Annex B bitstream.
 *
 * @return The SPS NAL unit (including its 1-byte header), or `nullptr`.
 */
const std::uint8_t* find_sps(const std::uint8_t* data, std::size_t size,
                             std::size_t* nalu_size) noexcept {
    if (!data || size < 4) return nullptr;

    std::size_t i = 0;
    while (i + 3 <= size) {
        // A start code is `00 00 01`, optionally preceded by a third
        // zero byte (the 4-byte form).
        std::size_t nal_start = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            nal_start = i + 3;
        } else if (i + 4 <= size && data[i] == 0 && data[i + 1] == 0 &&
                   data[i + 2] == 0 && data[i + 3] == 1) {
            nal_start = i + 4;
        } else {
            ++i;
            continue;
        }

        // The next start code ends this NAL unit.
        std::size_t end = nal_start;
        while (end + 3 <= size) {
            if ((data[end] == 0 && data[end + 1] == 0 && data[end + 2] == 1) ||
                (end + 4 <= size && data[end] == 0 && data[end + 1] == 0 &&
                 data[end + 2] == 0 && data[end + 3] == 1)) {
                break;
            }
            ++end;
        }
        if (end > nal_start && end - nal_start >= 2) {
            const std::uint8_t nal_type = data[nal_start] & 0x1Fu;
            if (nal_type == 7) {   // SPS
                if (nalu_size) *nalu_size = end - nal_start;
                return data + nal_start;
            }
        }
        // Continue at the next start code; the end of this NAL unit is
        // where the next one begins.
        i = end;
    }
    return nullptr;
}

/**
 * @brief Convert a DXVA NV12 surface into planar I420.
 *
 * The MFT's output is NV12 (interleaved chroma) — that is what a DXVA
 * decoder produces — so the conversion happens here.
 *
 * The picture is macroblock-padded: H.264 codes in whole 16x16 luma
 * macroblocks, so a 322x242 stream is *coded* as 336x256 and the MFT
 * hands the padded picture over.  The visible picture is the top-left
 * rectangle of it, whose size the SPS's conformance window names — so
 * the copy takes only that rectangle.
 *
 * @param src           The NV12 surface.
 * @param src_pitch     The surface's row pitch, in bytes.
 * @param coded_height  The surface's full height (the padded one).  The
 *                      chroma plane starts right after the luma rows.
 * @param width         The visible width.
 * @param height        The visible height.
 */
void nv12_to_i420(const std::uint8_t* src,
                  std::int32_t src_pitch,
                  std::uint32_t coded_height,
                  std::uint32_t width, std::uint32_t height,
                  std::uint32_t left, std::uint32_t top,
                  std::uint8_t* y_dst, std::int32_t y_stride,
                  std::uint8_t* u_dst, std::int32_t u_stride,
                  std::uint8_t* v_dst, std::int32_t v_stride) noexcept {
    if (!src || !y_dst) return;

    const std::uint32_t cw = (width + 1) / 2;
    const std::uint32_t ch = (height + 1) / 2;

    // The Y plane: the visible rows, the visible columns.
    for (std::uint32_t row = 0; row < height; ++row) {
        std::memcpy(y_dst + static_cast<std::size_t>(row) * y_stride,
                    src + static_cast<std::size_t>(row + top) * src_pitch + left,
                    width);
    }

    // The interleaved UV plane, de-interleaved into U and V.  It starts
    // after the *coded* luma rows, not after the visible ones, and the
    // visible rectangle's chroma offset is half the luma one.
    const std::uint8_t* uv = src + static_cast<std::size_t>(src_pitch) * coded_height;
    if (!u_dst || !v_dst) return;
    for (std::uint32_t row = 0; row < ch; ++row) {
        const std::uint8_t* s = uv + static_cast<std::size_t>(row + top / 2) * src_pitch
                                    + (left / 2) * 2;
        std::uint8_t* u = u_dst + static_cast<std::size_t>(row) * u_stride;
        std::uint8_t* v = v_dst + static_cast<std::size_t>(row) * v_stride;
        for (std::uint32_t col = 0; col < cw; ++col) {
            u[col] = s[col * 2];
            v[col] = s[col * 2 + 1];
        }
    }
}

/**
 * @brief Copy a DXVA NV12 surface into a destination that is itself NV12.
 *
 * The same macroblock crop as `nv12_to_i420`: the destination is packed
 * (the caller-supplied plane has no stride), so the luma rows and the
 * interleaved chroma rows are copied one at a time.
 */
void copy_nv12(const std::uint8_t* src, std::int32_t src_pitch,
               std::uint32_t coded_height,
               std::uint32_t width, std::uint32_t height,
               std::uint32_t left, std::uint32_t top,
               std::uint8_t* dst, std::int32_t dst_pitch) noexcept {
    if (!src || !dst) return;

    for (std::uint32_t row = 0; row < height; ++row) {
        std::memcpy(dst + static_cast<std::size_t>(row) * dst_pitch,
                    src + static_cast<std::size_t>(row + top) * src_pitch + left,
                    width);
    }

    const std::uint32_t cw = (width + 1) / 2;
    const std::uint32_t ch = (height + 1) / 2;

    // The destination's chroma plane follows its *visible* luma rows.
    const std::uint8_t* src_uv = src + static_cast<std::size_t>(src_pitch) * coded_height;
    std::uint8_t* dst_uv = dst + static_cast<std::size_t>(dst_pitch) * height;
    for (std::uint32_t row = 0; row < ch; ++row) {
        std::memcpy(dst_uv + static_cast<std::size_t>(row) * dst_pitch,
                    src_uv + static_cast<std::size_t>(row + top / 2) * src_pitch
                             + (left / 2) * 2,
                    cw * 2);
    }
}

/** The media subtype GUID, rendered for the log. */
const char* subtype_name(const GUID& guid) noexcept {
    if (guid == MFVideoFormat_H264)    return "H264";
    if (guid == MFVideoFormat_H264_ES) return "H264_ES";
    if (guid == MFVideoFormat_NV12)    return "NV12";
    if (guid == MFVideoFormat_I420)    return "I420";
    if (guid == MFVideoFormat_IYUV)    return "IYUV";
    if (guid == MFVideoFormat_YV12)    return "YV12";
    if (guid == MFVideoFormat_YUY2)    return "YUY2";
    if (guid == MFVideoFormat_RGB32)   return "RGB32";
    if (guid == MFVideoFormat_RGB24)   return "RGB24";
    return "Unknown";
}

// ---------------------------------------------------------------------------
// The COM / Media Foundation lifetime
// ---------------------------------------------------------------------------

/**
 * @brief Owns this thread's COM apartment and the MF global state.
 *
 * Media Foundation's `MFStartup` is process-global and reference
 * counted, while `CoInitializeEx` is per-thread.  The decoder runs on
 * the engine's media thread, so the apartment is initialised when the
 * decoder opens and torn down when it closes.
 *
 * `CoInitializeEx` returns `S_FALSE` when the apartment was *already*
 * initialised on this thread — that is a success, not a failure, and
 * the matching `CoUninitialize` must still be called (COM balances the
 * count).  `RPC_E_CHANGED_MODE` means the thread already lives in a
 * different apartment model, which is fatal for the MFT — the decoder
 * refuses to open rather than half-working.
 */
class MfSession {
public:
    MfSession() = default;
    MfSession(const MfSession&)            = delete;
    MfSession& operator=(const MfSession&) = delete;

    ~MfSession() { shutdown(); }

    bool start() noexcept {
        if (started_) return true;

        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr)) {
            NIMRTC_LOG_ERROR("dxva: CoInitializeEx failed (hr="
                             << hr_text(hr)
                             << ") — the thread cannot host an MFT");
            return false;
        }
        com_ = true;

        const HRESULT mhr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (FAILED(mhr)) {
            NIMRTC_LOG_ERROR("dxva: MFStartup failed (hr=" << hr_text(mhr) << ")");
            shutdown();
            return false;
        }
        started_ = true;
        return true;
    }

    void shutdown() noexcept {
        if (started_) {
            MFShutdown();
            started_ = false;
        }
        if (com_) {
            CoUninitialize();
            com_ = false;
        }
    }

private:
    bool started_ = false;
    bool com_     = false;
};

} // namespace

// ---------------------------------------------------------------------------
// DxvaDecoder
// ---------------------------------------------------------------------------

class DxvaDecoder final : public plugins::IVideoCodec {
public:
    explicit DxvaDecoder(plugins::VideoCodecConfig cfg) noexcept
        : cfg_(std::move(cfg)) {}

    ~DxvaDecoder() override { close(); }

    DxvaDecoder(const DxvaDecoder&)            = delete;
    DxvaDecoder& operator=(const DxvaDecoder&) = delete;

    // ---- plugins::IPlugin --------------------------------------------------

    const char* name() const noexcept override {
        return "nimrtc::h264::dxva_backend::DxvaDecoder (Media Foundation / DXVA)";
    }

    plugins::Status open() noexcept override {
        std::lock_guard<std::mutex> lock(mtx_);
        if (opened_) return plugins::kOk;

        if (!mf_.start()) return plugins::kErrHardwareError;

        const HRESULT hr = create_transform();
        if (FAILED(hr)) {
            NIMRTC_LOG_ERROR("dxva: the H.264 decoder MFT could not be created (hr="
                             << hr_text(hr) << ")");
            mf_.shutdown();
            return plugins::kErrHardwareError;
        }

        opened_ = true;
        return plugins::kOk;
    }

    void close() noexcept override {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!opened_) return;

        if (transform_) {
            transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
            transform_.Reset();
        }
        input_type_.Reset();
        output_type_.Reset();
        device_manager_.Reset();
        d3d_device_.Reset();
        device_context_.Reset();

        mf_.shutdown();
        opened_ = false;
    }

    // ---- plugins::IVideoCodec ----------------------------------------------

    // Decode only.  The send path is served by another backend, and the
    // codec module pairs the two through the composite adapter.
    bool can_encode() const noexcept override { return false; }
    bool can_decode() const noexcept override { return true; }

    plugins::VideoCodecKind kind() const noexcept override {
        return plugins::VideoCodecKind::kH264;
    }
    std::string_view codec_name() const noexcept override { return "h264"; }

    plugins::Status encode(const plugins::VideoFrame&, std::uint8_t*,
                           std::size_t,
                           plugins::EncodedVideoFrame&) noexcept override {
        return plugins::kErrUnsupported;
    }

    plugins::Status force_keyframe() noexcept override {
        // A decoder never emits keyframes.
        return plugins::kErrUnsupported;
    }

    plugins::Status decode(const plugins::EncodedVideoFrame& encoded,
                           plugins::VideoFrame& raw_out,
                           std::uint8_t* const* output_buffers) noexcept override {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!opened_ || !transform_) return plugins::kErrNotReady;
        if (encoded.payload.empty() || encoded.payload.data() == nullptr) {
            stats_.decode_errors++;
            return plugins::kErrInvalidParam;
        }

        // ---- the bitstream the MFT wants ------------------------------------
        //
        // The MFT wants Annex B.  The engine delivers length-prefixed
        // NAL units (the RTP payload format), so the conversion happens
        // here.  A payload that already carries start codes is passed
        // through untouched.
        const std::uint8_t* src = encoded.payload.data();
        std::size_t src_size = encoded.payload.size();
        std::vector<std::uint8_t> annex_b;
        if (encoded.nalu_format == plugins::NaluFormat::kLengthPrefixed ||
            !looks_like_annex_b(src, src_size)) {
            annex_b = to_annex_b(src, src_size);
            src = annex_b.data();
            src_size = annex_b.size();
        }

        // ---- feed the MFT ---------------------------------------------------
        //
        // A caller that gets `kErrNotReady` retries with the *same*
        // payload.  Handing it to the MFT again would decode the access
        // unit twice: the picture would appear twice and the stream
        // would drift away from the reference.  The payload is therefore
        // recognised and the duplicate submission is suppressed — the
        // caller's retry only asks "is there a picture yet?", and the
        // answer comes from the drain below.
        const PayloadKey key{src, src_size,
                             encoded.info.frame_seq,
                             encoded.info.rtp_timestamp};
        const bool duplicate = have_pending_ && (pending_ == key);
        if (!duplicate) {
            if (!submit(src, static_cast<DWORD>(src_size))) {
                stats_.decode_errors++;
                return plugins::kErrInternal;
            }
            pending_      = key;
            have_pending_ = true;
            in_flight_.push_back(SubmittedFrame{encoded.info.frame_seq,
                                                 encoded.info.capture_ts_us,
                                                 encoded.info.rtp_timestamp});
        }
        // ---- the visible geometry --------------------------------------------
        //
        // The MFT hands the *coded* picture over, which H.264 pads out to
        // whole macroblocks.  The visible rectangle is named by the SPS's
        // frame-cropping window, which the MFT does not expose — so it is
        // read out of the SPS here.  The result is remembered, because the
        // SPS only rides along with the key frames.
        if (last_sps_.valid == false || encoded.is_keyframe) {
            std::size_t sps_size = 0;
            const std::uint8_t* sps = find_sps(src, src_size, &sps_size);
            if (sps) {
                const SpsCrop crop = parse_sps_crop(sps, sps_size);
                if (crop.valid) {
                    last_sps_ = crop;
                    NIMRTC_LOG_DEBUG("dxva: the SPS names a " << crop.width << "x"
                                     << crop.height << " visible picture (crop "
                                     << crop.left << "," << crop.top << ")");
                }
            }
        }

        // ---- take the output -------------------------------------------------
        DecodedPicture pic{};
        if (!drain(pic)) {
            // The MFT buffered the data without completing a picture.  Not
            // an error — there is simply nothing to show yet.
            stats_.frames_skipped++;
            return plugins::kErrNotReady;
        }

        // The output buffer is locked by `drain()`.  Every path below
        // has to release it, so the copy runs under a guard that always
        // unlocks — an early return must not leave the buffer locked.
        struct PictureGuard {
            DxvaDecoder* self;
            ~PictureGuard() { self->release_picture(); }
        } guard{this};

        // The picture has come out of the MFT.  It belongs to the access
        // unit whose identity `drain()` attached to it — which is
        // generally *not* the access unit the caller just submitted,
        // because the MFT's pipeline is more than one access unit deep.
        //
        // The pending marker is released when the picture belongs to the
        // pending access unit: that access unit has been consumed, so a
        // later call carrying it again must submit it again rather than
        // be silently swallowed.
        if (have_pending_ && pic.identity.frame_seq == pending_.frame_seq &&
            pic.identity.rtp_timestamp == pending_.rtp_timestamp) {
            have_pending_ = false;
        }

        if (pic.coded_width == 0 || pic.coded_height == 0) {
            stats_.decode_errors++;
            return plugins::kErrCorrupt;
        }

        // ---- the visible geometry --------------------------------------------
        //
        // The SPS names the visible rectangle; without one the configured
        // geometry is the best guess.  The crop offsets say *where* the
        // visible rectangle sits inside the coded picture.
        if (last_sps_.valid) {
            pic.visible_width  = last_sps_.width;
            pic.visible_height = last_sps_.height;
            pic.left = last_sps_.left;
            pic.top  = last_sps_.top;
        } else {
            // No SPS to read: the configured geometry is the only
            // description of the visible picture there is.  It is used
            // only when it fits inside the coded picture — the coded
            // picture is the hard upper bound.
            const std::uint32_t cfg_w = cfg_.width  ? cfg_.width  : pic.coded_width;
            const std::uint32_t cfg_h = cfg_.height ? cfg_.height : pic.coded_height;
            pic.visible_width  = (cfg_w <= pic.coded_width)  ? cfg_w : pic.coded_width;
            pic.visible_height = (cfg_h <= pic.coded_height) ? cfg_h : pic.coded_height;
            pic.left = 0;
            pic.top  = 0;
        }

        // The visible picture must fit into the configured buffers: the
        // *coded* picture is allowed to be larger, because H.264 pads it
        // out to whole macroblocks and the crop happens on the way out.
        if (cfg_.width && cfg_.height &&
            (pic.visible_width > cfg_.width || pic.visible_height > cfg_.height)) {
            NIMRTC_LOG_ERROR("dxva: the decoded frame " << pic.visible_width << "x"
                             << pic.visible_height << " does not fit the configured "
                             << cfg_.width << "x" << cfg_.height
                             << " output buffers");
            stats_.decode_errors++;
            return plugins::kErrInvalidParam;
        }

        // ---- the destination -------------------------------------------------
        //
        // The same three-way contract as the other decoders: the caller
        // supplied the planes, or the decoder allocates its own buffer and
        // publishes it through `raw_out.buffer`.
        const bool nv12_out = (cfg_.pixel_format == plugins::VideoPixelFormat::kNV12);
        const auto layout = video_frame::compute_layout(
            nv12_out ? video_frame::PixelFormat::kNV12
                     : video_frame::PixelFormat::kI420,
            pic.visible_width, pic.visible_height);

        const bool caller_supplied = (output_buffers && output_buffers[0] != nullptr);

        std::shared_ptr<video_frame::VideoFrameBuffer> cpu_buf;
        if (caller_supplied) {
            if (auto existing = raw_out.cpu_buffer();
                existing && existing->data() == output_buffers[0]) {
                cpu_buf = std::move(existing);
            }
        }
        if (!cpu_buf) {
            cpu_buf = std::make_shared<video_frame::VideoFrameBuffer>(layout);
            if (!cpu_buf || !cpu_buf->data()) {
                stats_.decode_errors++;
                return plugins::kErrInternal;
            }
        }

        if (nv12_out) {
            // The caller-supplied NV12 plane is tightly packed (the
            // interface carries no stride), so the destination stride is
            // the width itself.
            copy_nv12(pic.data, pic.pitch, pic.coded_height,
                      pic.visible_width, pic.visible_height,
                      pic.left, pic.top,
                      caller_supplied ? output_buffers[0] : cpu_buf->plane(0),
                      caller_supplied ? static_cast<std::int32_t>(pic.visible_width)
                                      : layout.strides[0]);
        } else {
            std::uint8_t* y_dst = caller_supplied ? output_buffers[0] : cpu_buf->plane(0);
            std::uint8_t* u_dst = caller_supplied ? output_buffers[1] : cpu_buf->plane(1);
            std::uint8_t* v_dst = caller_supplied ? output_buffers[2] : cpu_buf->plane(2);
            const std::int32_t y_stride = caller_supplied
                ? static_cast<std::int32_t>(pic.visible_width) : layout.strides[0];
            const std::int32_t u_stride = caller_supplied
                ? static_cast<std::int32_t>(pic.visible_width / 2) : layout.strides[1];
            const std::int32_t v_stride = caller_supplied
                ? static_cast<std::int32_t>(pic.visible_width / 2) : layout.strides[2];

            nv12_to_i420(pic.data, pic.pitch, pic.coded_height,
                         pic.visible_width, pic.visible_height,
                         pic.left, pic.top,
                         y_dst, y_stride, u_dst, u_stride, v_dst, v_stride);
        }

        raw_out.buffer = std::move(cpu_buf);
        // The identity is the one of the access unit the picture came
        // from — not the one the caller happened to pass in.  The MFT's
        // pipeline is a couple of access units deep, so the two differ.
        raw_out.info.capture_ts_us = pic.identity.capture_ts_us;
        raw_out.info.frame_seq     = pic.identity.frame_seq;
        raw_out.info.rtp_timestamp = pic.identity.rtp_timestamp;

        stats_.frames_decoded++;
        stats_.bytes_decoded += encoded.payload.size();
        return plugins::kOk;
    }

    plugins::VideoCodecConfig config() const noexcept override { return cfg_; }

    plugins::Status update_config(plugins::VideoCodecConfig cfg) noexcept override {
        // The geometry is the bitstream's; only the descriptive fields
        // are taken.  Changing the geometry would need a new MFT
        // session, which the caller gets by closing and reopening.
        cfg_ = std::move(cfg);
        return plugins::kOk;
    }

    plugins::VideoCodecStats stats() const noexcept override {
        std::lock_guard<std::mutex> lock(mtx_);
        plugins::VideoCodecStats s = stats_;
        s.backend_id = "dxva";
        s.hardware   = opened_;
        return s;
    }

    std::uint8_t payload_type() const noexcept override {
        return cfg_.payload_type;
    }

    std::string_view backend_id() const noexcept override { return "dxva"; }

    bool is_hardware_accelerated() const noexcept override {
        // Only *actually* hardware when the D3D manager was accepted:
        // without it the MFT falls back to its own software path, and
        // claiming "hardware" then would be a lie.
        return opened_ && hardware_path_;
    }

private:
    // ---- The MFT plumbing --------------------------------------------------

    /**
     * @brief One access unit that is in flight inside the MFT.
     *
     * The MFT buffers internally — it needs a couple of access units
     * before it emits the first picture — so the picture that comes out
     * of it belongs to an access unit that was submitted *earlier* than
     * the one the caller is currently asking about.
     *
     * The information about that access unit is therefore remembered
     * here, in submission order, and handed to the picture when the
     * picture is finally extracted.  Without it the timestamps would be
     * off by the depth of the MFT's pipeline.
     */
    struct SubmittedFrame {
        /** The identity of the access unit. */
        std::uint32_t frame_seq     = 0;
        std::int64_t  capture_ts_us = 0;
        std::uint32_t rtp_timestamp = 0;
    };

    /** One decoded picture, ready to be copied out.
     *
     * H.264 codes in whole 16x16 macroblocks, so a picture whose
     * visible size is not a multiple of 16 is *coded* larger than it
     * is shown: a 322x242 stream is coded as 336x256.  The decoder
     * surface therefore carries the coded size, and the visible size
     * is the rectangle that the conformance window (or the
     * configured geometry) names.  The copy takes the visible
     * rectangle out of the coded picture.
     */
    struct DecodedPicture {
        const std::uint8_t* data = nullptr;
        std::int32_t pitch = 0;
        /** The size of the decoded surface (macroblock-padded). */
        std::uint32_t coded_width  = 0;
        std::uint32_t coded_height = 0;
        /** The size of the picture that is actually shown. */
        std::uint32_t visible_width  = 0;
        std::uint32_t visible_height = 0;
        /** Where the visible rectangle sits inside the coded picture. */
        std::uint32_t left = 0;
        std::uint32_t top  = 0;
        /** The identity of the access unit this picture came from. */
        SubmittedFrame identity{};
    };

    HRESULT create_transform() noexcept {
        HRESULT hr = CoCreateInstance(CLSID_MSH264DecoderMFT, nullptr,
                                      CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&transform_));
        if (FAILED(hr)) return hr;

        // ---- the low-latency mode ------------------------------------------
        //
        // This is the single most important setting in the whole file.
        // Without it the MFT buffers a full GOP (15–30 frames) before it
        // emits anything, and a live stream that never reaches that
        // depth produces *no* output at all — every `ProcessOutput`
        // answers `MF_E_TRANSFORM_NEED_MORE_INPUT` and the decoder looks
        // broken rather than "buffered".
        if (FAILED(set_low_latency())) {
            NIMRTC_LOG_WARN("dxva: the low-latency mode could not be enabled — "
                            "the decoder will buffer a GOP before it emits "
                            "anything");
        }

        // ---- the input type: H.264 ------------------------------------------
        ComPtr<IMFMediaType> in_type;
        hr = MFCreateMediaType(&in_type);
        if (FAILED(hr)) return hr;

        hr = in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (FAILED(hr)) return hr;
        //
        // The AVC subtype is used rather than the elementary-stream one:
        // both are accepted, but the AVC subtype decodes one access unit
        // per picture (1:1), while the ES subtype lags behind on a live
        // stream.  The payload itself is Annex B either way — see the
        // file header.
        hr = in_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        if (FAILED(hr)) return hr;

        if (cfg_.width && cfg_.height) {
            hr = MFSetAttributeSize(in_type.Get(), MF_MT_FRAME_SIZE,
                                    cfg_.width, cfg_.height);
            if (FAILED(hr)) return hr;
        }
        if (cfg_.fps) {
            hr = MFSetAttributeRatio(in_type.Get(), MF_MT_FRAME_RATE,
                                     cfg_.fps, 1);
            if (FAILED(hr)) return hr;
        }
        hr = MFSetAttributeRatio(in_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (FAILED(hr)) return hr;
        hr = in_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (FAILED(hr)) return hr;

        hr = transform_->SetInputType(0, in_type.Get(), 0);
        if (FAILED(hr)) {
            NIMRTC_LOG_ERROR("dxva: the MFT rejected the H.264 input type (hr="
                             << hr_text(hr) << ")");
            return hr;
        }
        input_type_ = std::move(in_type);

        // ---- the output type: NV12 ------------------------------------------
        //
        // NV12 is what a DXVA decoder produces, and it is what the
        // conversion below expects.
        ComPtr<IMFMediaType> out_type;
        hr = MFCreateMediaType(&out_type);
        if (FAILED(hr)) return hr;
        hr = out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (FAILED(hr)) return hr;
        hr = out_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        if (FAILED(hr)) return hr;
        if (cfg_.width && cfg_.height) {
            hr = MFSetAttributeSize(out_type.Get(), MF_MT_FRAME_SIZE,
                                    cfg_.width, cfg_.height);
            if (FAILED(hr)) return hr;
        }
        if (cfg_.fps) {
            hr = MFSetAttributeRatio(out_type.Get(), MF_MT_FRAME_RATE,
                                     cfg_.fps, 1);
            if (FAILED(hr)) return hr;
        }
        hr = out_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (FAILED(hr)) return hr;

        hr = transform_->SetOutputType(0, out_type.Get(), 0);
        if (FAILED(hr)) {
            NIMRTC_LOG_ERROR("dxva: the MFT rejected the NV12 output type (hr="
                             << hr_text(hr) << ")");
            return hr;
        }
        output_type_ = std::move(out_type);

        // ---- the D3D manager: this is what makes it *hardware* --------------
        //
        // Without a D3D manager the MFT decodes on the CPU.  The manager
        // is what routes the decode through DXVA, so its acceptance is
        // the difference between "a hardware decoder" and "a software
        // decoder wearing a hardware decoder's name".
        attach_d3d_manager();

        hr = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        if (FAILED(hr)) return hr;
        hr = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        if (FAILED(hr)) return hr;

        return S_OK;
    }

    /**
     * @brief Turn on the MFT's low-latency mode.
     *
     * The setting lives on `ICodecAPI`, which the transform exposes as
     * a second interface.  `CODECAPI_AVLowLatencyMode` is a `UINT32`
     * codec property; a `VARIANT` of type `VT_UI4` carries it.
     */
    HRESULT set_low_latency() noexcept {
        ComPtr<ICodecAPI> codec_api;
        HRESULT hr = transform_.As(&codec_api);
        if (FAILED(hr)) return hr;

        VARIANT value;
        VariantInit(&value);
        value.vt    = VT_UI4;
        value.uiVal = 1;
        hr = codec_api->SetValue(&CODECAPI_AVLowLatencyMode, &value);
        VariantClear(&value);
        return hr;
    }

    /**
     * @brief Attach a D3D11 device as the MFT's D3D manager.
     *
     * The MFT does not take the `ID3D11Device` itself: it takes an
     * `IMFDXGIDeviceManager` — the DXGI↔MF bridge object that owns the
     * reset token and hands out the device to the transform.  Handing
     * the MFT the raw device is rejected with `E_NOTIMPL`, which is
     * exactly the "the decode will run on the CPU" path this method
     * is meant to avoid.
     *
     * The MFT answers `MFT_MESSAGE_SET_D3D_MANAGER` with whether it
     * accepted the manager.  A MFT that does not support DXVA rejects
     * it, and the decode then runs on the CPU — which is fine, but the
     * backend has to *say* so rather than claim a hardware path it does
     * not have.
     */
    void attach_d3d_manager() noexcept {
        static const D3D_FEATURE_LEVEL kLevels[] = {
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
        };
        D3D_FEATURE_LEVEL achieved = D3D_FEATURE_LEVEL_11_0;

        const HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            kLevels, ARRAYSIZE(kLevels), D3D11_SDK_VERSION,
            &d3d_device_, &achieved, &device_context_);
        if (FAILED(hr)) {
            NIMRTC_LOG_WARN("dxva: the D3D11 device could not be created (hr="
                            << hr_text(hr)
                            << ") — the MFT will decode on the CPU");
            return;
        }

        // The device manager wraps the device and owns the reset token
        // the MFT hands back when it loses the device.
        UINT reset_token = 0;
        HRESULT mhr = MFCreateDXGIDeviceManager(&reset_token,
                                                &device_manager_);
        if (FAILED(mhr)) {
            NIMRTC_LOG_WARN("dxva: the DXGI device manager could not be created "
                            "(hr=" << hr_text(mhr)
                            << ") — the MFT will decode on the CPU");
            d3d_device_.Reset();
            return;
        }

        mhr = device_manager_->ResetDevice(d3d_device_.Get(), reset_token);
        if (FAILED(mhr)) {
            NIMRTC_LOG_WARN("dxva: the DXGI device manager rejected the D3D11 "
                            "device (hr=" << hr_text(mhr)
                            << ") — the MFT will decode on the CPU");
            device_manager_.Reset();
            d3d_device_.Reset();
            return;
        }

        mhr = transform_->ProcessMessage(
            MFT_MESSAGE_SET_D3D_MANAGER,
            reinterpret_cast<ULONG_PTR>(device_manager_.Get()));
        if (FAILED(mhr)) {
            NIMRTC_LOG_WARN("dxva: the MFT did not accept the D3D manager (hr="
                            << hr_text(mhr)
                            << ") — the decode will run on the CPU");
            device_manager_.Reset();
            d3d_device_.Reset();
            return;
        }

        hardware_path_ = true;
        NIMRTC_LOG_INFO("dxva: the D3D manager was accepted — the decode runs "
                        "through DXVA");
    }

    /** Hand one access unit to the MFT. */
    bool submit(const std::uint8_t* data, DWORD size) noexcept {
        ComPtr<IMFMediaBuffer> buffer;
        HRESULT hr = MFCreateMemoryBuffer(size, &buffer);
        if (FAILED(hr)) return false;

        std::uint8_t* dst = nullptr;
        DWORD max_len = 0, cur_len = 0;
        hr = buffer->Lock(&dst, &max_len, &cur_len);
        if (FAILED(hr)) return false;
        std::memcpy(dst, data, size);
        hr = buffer->Unlock();
        if (FAILED(hr)) return false;
        hr = buffer->SetCurrentLength(size);
        if (FAILED(hr)) return false;

        ComPtr<IMFSample> sample;
        hr = MFCreateSample(&sample);
        if (FAILED(hr)) return false;
        hr = sample->AddBuffer(buffer.Get());
        if (FAILED(hr)) return false;

        // The sample time is a synthetic 30 fps timeline: the MFT only
        // uses it for its own bookkeeping, and the real timestamps travel
        // in the RTP header.
        const LONGLONG sample_time =
            static_cast<LONGLONG>(stats_.frames_decoded) * 333333;
        sample->SetSampleTime(sample_time);   // non-fatal if it fails

        hr = transform_->ProcessInput(0, sample.Get(), 0);
        if (hr == MF_E_NOTACCEPTING) {
            // The MFT is full: drain what it has and try once more.
            DecodedPicture drained{};
            drain(drained);
            hr = transform_->ProcessInput(0, sample.Get(), 0);
        }
        if (FAILED(hr)) {
            NIMRTC_LOG_WARN("dxva: ProcessInput failed (hr=" << hr_text(hr) << ")");
            return false;
        }
        return true;
    }

    /**
     * @brief Take the pending output, if any.
     *
     * The MFT may announce a stream change before it emits the first
     * picture (the real geometry comes out of the SPS).  That is not a
     * lost frame: the renegotiation happens and the output is requested
     * again, so the loop below keeps trying instead of dropping the
     * picture on the floor.
     *
     * @return false when there is no picture yet — the MFT is still
     *         buffering, which is normal for a stream whose reference
     *         frame has not arrived.
     */
    bool drain(DecodedPicture& out) noexcept {
        if (!transform_) return false;

        for (int attempt = 0; attempt < 4; ++attempt) {
            MFT_OUTPUT_DATA_BUFFER out_buf{};
            DWORD status = 0;

            // The MFT allocates the output buffer itself: a caller-
            // supplied buffer would have to be sized for the *padded*
            // picture, whose stride the caller cannot know in advance.
            const HRESULT hr = transform_->ProcessOutput(0, 1, &out_buf, &status);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return false;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                // The MFT may have allocated a sample before it noticed
                // the format change; that sample still belongs to us.
                release_output_sample(out_buf.pSample);
                // The output format changed: renegotiate the output type.
                renegotiate_output();
                continue;
            }
            if (FAILED(hr) || !out_buf.pSample) {
                release_output_sample(out_buf.pSample);
                NIMRTC_LOG_DEBUG("dxva: ProcessOutput produced no picture (hr="
                                 << hr_text(hr) << " sample="
                                 << (out_buf.pSample ? "yes" : "no") << ")");
                return false;
            }

            // The MFT allocated the sample, so the caller owns it now.
            // Hold on to it until the pixels have been copied out —
            // `release_picture()` hands it back.  `Attach` takes the
            // reference over as it is; a plain assignment would add one
            // of its own and leak the surface slice.
            output_sample_.Attach(out_buf.pSample);

            // With a D3D manager attached the MFT produces its output on
            // a **DXGI surface**, not in system memory.  The media buffer
            // is then an `IMFDXGIBuffer`, and the surface has to be read
            // back explicitly.  When the D3D manager was *not* accepted
            // the MFT falls back to its own software path, which
            // delivers a contiguous system buffer instead.
            const bool ok = read_back_surface(out_buf.pSample, out)
                                ? true
                                : read_system_buffer(out_buf.pSample, out);
            if (!ok) {
                // Hand the surface slice back right away: the caller
                // gets no picture, so nothing is going to copy out of
                // it later.
                output_sample_.Reset();
                return false;
            }

            // The picture belongs to the *oldest* access unit that is
            // still in flight: the MFT emits the pictures in the order
            // it received the access units, and the caller's request is
            // about the newest one.  Taking the head of the queue is
            // what keeps the timestamps attached to the right frame.
            if (!in_flight_.empty()) {
                out.identity = in_flight_.front();
                in_flight_.pop_front();
            }
            return true;
        }
        return false;
    }

    /**
     * @brief Release an output sample the MFT allocated.
     *
     * The MFT hands the ownership of the sample to the caller; a sample
     * that is not released keeps a slice of the decoder's surface pool
     * alive, and the pool running dry hangs the decoder.  See
     * `output_sample_`.
     */
    static void release_output_sample(IMFSample* sample) noexcept {
        if (sample) sample->Release();
    }

    /** Release the output buffer locked by `drain()`. */
    void release_picture() noexcept {
        if (locked_buffer_) {
            locked_buffer_->Unlock();
            locked_buffer_.Reset();
            locked_data_ = nullptr;
        }
        // The MFT-allocated sample goes back now that the pixels have
        // been copied out of it.  This is what keeps the decoder's
        // surface pool from running dry.
        if (output_sample_) {
            output_sample_.Reset();
        }
    }

    /**
     * @brief Read a system-memory output into the staged picture.
     *
     * This is the path the MFT takes when the D3D manager was rejected:
     * the decode ran on the CPU and the pixels are already in a
     * contiguous buffer.  The buffer stays locked until
     * `release_picture()` is called, because the copy into the caller's
     * planes happens after `drain()` returned.
     */
    bool read_system_buffer(IMFSample* sample, DecodedPicture& out) noexcept {
        ComPtr<IMFMediaBuffer> media_buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&media_buffer))) {
            return false;
        }

        // The picture's real geometry comes from the media type the MFT
        // is currently producing, not from the configuration: the MFT
        // learns the real size from the bitstream's SPS.
        UINT32 width = 0, height = 0;
        std::int32_t stride = 0;
        ComPtr<IMFMediaType> cur_type;
        if (SUCCEEDED(transform_->GetOutputCurrentType(0, &cur_type))) {
            MFGetAttributeSize(cur_type.Get(), MF_MT_FRAME_SIZE, &width, &height);
            // The stride is the *padded* row pitch, which is not the
            // width.  Without it the copy would shear the picture.
            const UINT32 s = MFGetAttributeUINT32(cur_type.Get(),
                                                  MF_MT_DEFAULT_STRIDE, 0);
            if (s) stride = static_cast<std::int32_t>(s);
        }
        if (stride <= 0) stride = static_cast<std::int32_t>(width);

        std::uint8_t* data = nullptr;
        DWORD length = 0;
        if (FAILED(media_buffer->Lock(&data, nullptr, &length))) return false;

        out.data   = data;
        out.pitch  = stride;
        // The media type reports the *coded* size; the visible size is
        // the configured one (the conformance window is not exposed by
        // the MFT).
        out.coded_width  = width  ? width  : cfg_.width;
        out.coded_height = height ? height : cfg_.height;
        out.visible_width  = cfg_.width  ? cfg_.width  : out.coded_width;
        out.visible_height = cfg_.height ? cfg_.height : out.coded_height;

        // The buffer stays locked until the caller has copied the pixels
        // out (see `release_picture()`).
        locked_buffer_ = media_buffer;
        locked_data_   = data;
        return true;
    }

    /**
     * @brief Read a DXGI-surface output back into system memory.
     *
     * With a D3D manager attached, the MFT decodes onto a DXGI surface
     * rather than into a system-memory buffer.  The surface has to be
     * copied into a staging texture and mapped before the pixels can be
     * read — that is the cost of a hardware decode into a CPU-side
     * pipeline, and it is why the backend reports
     * `zero_copy_supported == false`.
     *
     * Three details are not obvious and were all measured:
     *
     *  - The decoder's texture has `ArraySize = 10` (its internal
     *    surface pool), so the staging texture must be created with
     *    `ArraySize = 1` — creating it with the source's array size
     *    fails with `E_INVALIDARG`.
     *
     *  - `CopyResource` cannot be used either: it copies *all* the
     *    slices, and the sizes do not match.  The slice the picture
     *    actually lives in comes from `IMFDXGIBuffer::GetSubresourceIndex`,
     *    and the copy is a `CopySubresourceRegion` of that one slice.
     *
     *  - A staging texture of a multi-planar format has exactly **one**
     *    mappable subresource — mapping subresource 1 (the chroma)
     *    fails with `E_INVALIDARG`.  The one mapping covers *both*
     *    planes: the luma's rows first, then the chroma's rows, each
     *    with the texture's own row pitch.  So the whole mapped region
     *    has to be copied, not just the luma's part.
     *
     * The staged NV12 picture is kept in `staging_` and stays valid
     * until the next decode replaces it.
     */
    bool read_back_surface(IMFSample* sample, DecodedPicture& out) noexcept {
        ComPtr<IMFMediaBuffer> media_buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&media_buffer))) {
            return false;
        }

        ComPtr<IMFDXGIBuffer> dxgi_buffer;
        HRESULT hr = media_buffer.As(&dxgi_buffer);
        if (FAILED(hr)) {
            NIMRTC_LOG_WARN("dxva: the output buffer is neither a system "
                            "buffer nor a DXGI buffer (hr=" << hr_text(hr) << ")");
            return false;
        }

        ComPtr<ID3D11Texture2D> surface;
        hr = dxgi_buffer->GetResource(__uuidof(ID3D11Texture2D),
                                      reinterpret_cast<void**>(surface.GetAddressOf()));
        if (FAILED(hr) || !surface) {
            NIMRTC_LOG_WARN("dxva: the DXGI buffer did not hand out a texture "
                            "(hr=" << hr_text(hr) << ")");
            return false;
        }

        D3D11_TEXTURE2D_DESC desc{};
        surface->GetDesc(&desc);

        // The staging texture mirrors the surface's layout so the copy
        // is a straight blit — except for the array size, which must
        // describe the single slice that is copied.
        desc.Usage          = D3D11_USAGE_STAGING;
        desc.BindFlags      = 0;
        desc.MiscFlags      = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.ArraySize      = 1;

        ComPtr<ID3D11Texture2D> staging;
        hr = d3d_device_->CreateTexture2D(&desc, nullptr, &staging);
        if (FAILED(hr)) {
            NIMRTC_LOG_WARN("dxva: the staging texture could not be created "
                            "(hr=" << hr_text(hr) << ")");
            return false;
        }

        ID3D11DeviceContext* ctx = device_context_.Get();
        if (!ctx) return false;

        // Which slice of the decoder's surface pool the picture is in.
        UINT slice = 0;
        dxgi_buffer->GetSubresourceIndex(&slice);

        NIMRTC_LOG_DEBUG("dxva: reading back a " << desc.Width << "x" << desc.Height
                         << " surface (format=" << desc.Format
                         << " array=" << desc.ArraySize
                         << " slice=" << slice << ")");

        ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0,
                                   surface.Get(), slice, nullptr);

        // The copy is queued on the context; the map has to see it.  A
        // flush is cheap here — the copy is a single slice — and without
        // it the map can fail with E_OUTOFMEMORY.
        ctx->Flush();

        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr = ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            NIMRTC_LOG_WARN("dxva: the staging texture could not be mapped "
                            "(hr=" << hr_text(hr) << ")");
            return false;
        }

        // The surface is NV12: the Y plane covers the whole picture and
        // the interleaved UV plane follows it, each with its own pitch.
        const std::uint32_t width  = desc.Width;
        const std::uint32_t height = desc.Height;
        const std::uint32_t y_pitch  = mapped.RowPitch;
        // The chroma plane's pitch is not exposed by the mapping, but it
        // is the *same* as the luma's: the texture's rows are allocated
        // for the wider plane and the chroma rows sit right after the
        // luma's, on the same stride.  (Measured: a 336x256 NV12
        // surface with a 384-byte pitch -- the chroma rows are 384
        // bytes apart as well.)
        const std::uint32_t uv_pitch = y_pitch;
        const std::size_t   y_size   = static_cast<std::size_t>(y_pitch) * height;
        const std::size_t   uv_size  = static_cast<std::size_t>(uv_pitch) * (height / 2);

        staging_.resize(y_size + uv_size);
        // The *whole* mapped region is copied: the luma's rows and then
        // the chroma's rows.
        //
        // A staging texture of a multi-planar format has **one** mappable
        // subresource -- mapping subresource 1 (the chroma) fails with
        // `E_INVALIDARG` -- and the mapping covers both planes: the
        // luma's rows first, then the chroma's rows, each with the
        // texture's own row pitch.  Copying only the luma's bytes leaves
        // the chroma zeroed, and the picture comes out with a *grey*
        // chroma: the luma is byte-exact while the chroma is not.
        std::memcpy(staging_.data(), mapped.pData, y_size + uv_size);

        ctx->Unmap(staging.Get(), 0);

        out.data   = staging_.data();
        out.pitch  = static_cast<std::int32_t>(y_pitch);
        out.coded_width  = width;
        out.coded_height = height;
        // The MFT does not expose the conformance window, so the
        // visible size comes from the configuration: the caller knows
        // the geometry it announced to the peer.
        out.visible_width  = cfg_.width  ? cfg_.width  : width;
        out.visible_height = cfg_.height ? cfg_.height : height;
        return true;
    }

    /**
     * @brief Re-negotiate the output type after a stream change.
     *
     * The MFT announces a stream change when the picture format settles
     * (the real geometry comes out of the SPS).  The renegotiation has
     * to land on **NV12**: that is the format the readback and the
     * I420 conversion below expect, and picking whatever type happens
     * to be first in the available list yields a format the staging
     * texture cannot be mapped from.
     */
    void renegotiate_output() noexcept {
        ComPtr<IMFMediaType> fallback;
        ComPtr<IMFMediaType> chosen;
        UINT32 fallback_w = 0, fallback_h = 0;

        for (UINT32 i = 0;; ++i) {
            ComPtr<IMFMediaType> candidate;
            if (FAILED(transform_->GetOutputAvailableType(0, i, &candidate)) ||
                !candidate) {
                break;
            }
            GUID subtype{};
            if (FAILED(candidate->GetGUID(MF_MT_SUBTYPE, &subtype))) continue;
            if (subtype == MFVideoFormat_NV12) {
                chosen = candidate;
                break;
            }
            if (!fallback) {
                fallback = candidate;
                MFGetAttributeSize(fallback.Get(), MF_MT_FRAME_SIZE,
                                   &fallback_w, &fallback_h);
            }
        }

        // NV12 is the format the readback path understands.  If it is
        // not on offer, the fallback is used and the readback will fail
        // loudly rather than silently mis-interpret the pixels.
        ComPtr<IMFMediaType> new_type = chosen ? chosen : fallback;
        if (!new_type) return;

        UINT32 w = 0, h = 0;
        MFGetAttributeSize(new_type.Get(), MF_MT_FRAME_SIZE, &w, &h);

        const HRESULT hr = transform_->SetOutputType(0, new_type.Get(), 0);
        if (FAILED(hr)) {
            NIMRTC_LOG_WARN("dxva: the renegotiated output type was rejected (hr="
                            << hr_text(hr) << ")");
            return;
        }
        NIMRTC_LOG_INFO("dxva: the output type was renegotiated to "
                        << w << "x" << h
                        << (chosen ? " (NV12)" : " (no NV12 available)"));
    }

    // ---- The state ----------------------------------------------------------

    /** The identity of a payload, used to recognise a retry. */
    struct PayloadKey {
        const std::uint8_t* src      = nullptr;
        std::size_t         src_size = 0;
        std::uint32_t       frame_seq = 0;
        std::uint32_t       rtp_timestamp = 0;

        bool operator==(const PayloadKey& other) const noexcept {
            return src == other.src && src_size == other.src_size &&
                   frame_seq == other.frame_seq &&
                   rtp_timestamp == other.rtp_timestamp;
        }
    };

    mutable std::mutex           mtx_;
    plugins::VideoCodecConfig    cfg_{};
    plugins::VideoCodecStats     stats_{};

    MfSession                    mf_{};
    ComPtr<IMFTransform>         transform_;
    ComPtr<IMFMediaType>         input_type_;
    ComPtr<IMFMediaType>         output_type_;
    ComPtr<ID3D11Device>         d3d_device_;
    ComPtr<ID3D11DeviceContext>  device_context_;
    ComPtr<IMFDXGIDeviceManager> device_manager_;

    ComPtr<IMFMediaBuffer>       locked_buffer_;
    std::uint8_t*                locked_data_ = nullptr;
    std::vector<std::uint8_t>    staging_;

    /**
     * The output sample the MFT allocated for the current picture.
     *
     * When the caller leaves `MFT_OUTPUT_DATA_BUFFER::pSample` null,
     * the MFT allocates the sample itself — and then *the caller owns
     * it*.  Letting it go is not a leak of a few bytes: with a D3D
     * manager attached the sample owns a slice of the decoder's DXGI
     * surface pool, and the pool is finite.  Once it runs dry the MFT
     * blocks inside `ProcessOutput` waiting for a surface that will
     * never be released, and the decode hangs forever.
     *
     * The sample is therefore released in `release_picture()`, together
     * with the buffer lock.
     */
    ComPtr<IMFSample>            output_sample_;

    /** The visible geometry the last SPS named. */
    SpsCrop                      last_sps_;

    /**
     * The access units that were handed to the MFT and have not come
     * back out of it yet, in submission order.
     *
     * The head is paired with the next picture the MFT produces.
     */
    std::deque<SubmittedFrame>   in_flight_;

    /**
     * The payload of the access unit that is currently in flight.
     *
     * A caller that gets `kErrNotReady` retries with the *same* payload.
     * Handing that payload to the MFT a second time would decode it
     * twice — the picture would appear twice and the stream would drift
     * away from the reference — so the payload is recognised and the
     * second submission is suppressed.
     */
    bool                         have_pending_ = false;
    PayloadKey                   pending_{};

    bool                         opened_        = false;
    bool                         hardware_path_ = false;
};

} // namespace nimrtc::h264::dxva_backend

namespace nimrtc::h264 {

bool dxva_h264_available() noexcept {
#if defined(NIMRTC_PLUGINS_DXVA_ON) && defined(_WIN32)
    // The probe asks the *system* whether the H.264 decoder MFT exists.
    // It does not create a session: an availability probe that opens a
    // full decoder session would cost far more than it is worth, and the
    // session's own failure is reported by the create path.
    //
    // The COM apartment is initialised for the probe and torn down
    // again: `CoInitializeEx` is per-thread and the probe may run on a
    // different thread than the decoder will.
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        NIMRTC_LOG_INFO("dxva: COM initialization failed (hr="
                        << hr_text(hr)
                        << ") — the DXVA decoder is not available");
        return false;
    }

    bool available = false;
    {
        ComPtr<IMFTransform> probe;
        const HRESULT chr = CoCreateInstance(CLSID_MSH264DecoderMFT, nullptr,
                                             CLSCTX_INPROC_SERVER,
                                             IID_PPV_ARGS(&probe));
        available = SUCCEEDED(chr);
        if (!available) {
            NIMRTC_LOG_INFO("dxva: the H.264 decoder MFT is not available (hr="
                            << hr_text(chr) << ")");
        }
        // The probe is released *before* the apartment is torn down: the
        // MFT lives in the apartment, and releasing a COM pointer after
        // `CoUninitialize` is an access violation.
    }
    CoUninitialize();
    return available;
#else
    return false;
#endif
}

std::unique_ptr<plugins::IVideoCodec>
make_dxva_h264_codec(plugins::VideoCodecConfig config) {
#if defined(NIMRTC_PLUGINS_DXVA_ON) && defined(_WIN32)
    auto codec = std::make_unique<dxva_backend::DxvaDecoder>(std::move(config));
    if (codec->open() != plugins::kOk) {
        NIMRTC_LOG_ERROR("dxva: the decoder could not be opened");
        return nullptr;
    }
    return codec;
#else
    (void)config;
    return nullptr;
#endif
}

} // namespace nimrtc::h264

NIMRTC_SUPPRESS_DEPRECATION_END

#else // NIMRTC_PLUGINS_DXVA_ON && _WIN32

namespace nimrtc::h264 {

bool dxva_h264_available() noexcept {
    return false;
}

std::unique_ptr<plugins::IVideoCodec>
make_dxva_h264_codec(plugins::VideoCodecConfig) {
    return nullptr;
}

} // namespace nimrtc::h264

#endif // NIMRTC_PLUGINS_DXVA_ON && _WIN32
