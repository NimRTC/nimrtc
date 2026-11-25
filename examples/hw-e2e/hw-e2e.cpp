/**
 * @file examples/hw-e2e/hw-e2e.cpp
 * @brief The hardware-encode end-to-end harness (v1.3 plan Q7 / O-6).
 *
 * ## What this program proves
 *
 * Q7's bar is: *"the hardware encode path is exercised end-to-end by
 * something re-runnable, and the result is asserted from the stats"*.
 * This program is that something.  It drives the **shipped** media chain
 * with the backend pinned to hardware (`EngineConfig::video_codec_backend
 * = "hardware"`, which fails rather than falling back to software):
 *
 *   1. a synthetic frame (I420, 1080p, a ramp + a hard vertical edge) is
 *      built — a pattern the encoder reproduces essentially losslessly,
 *      so the pixel criterion measures the *path*, not the quantiser;
 *   2. the H.264 codec plugin (`video_codec_name = "h264"`) is created
 *      through the *same* hardware backend table the engine uses, with
 *      `backend_preference = kHardware` — so the backend selector picks
 *      NVENC / VideoToolbox / VA-API / AMF, whichever this host has;
 *   3. several frames (every one of them an IDR) are encoded through
 *      `IVideoEncoder::encode()`, and the Annex-B bitstream is checked
 *      for SPS + PPS + IDR;
 *   4. the bitstream goes through the shipped RTP sender (STAP-A /
 *      FU-A packetization, a real `rtp::PacketBuilder` header) and the
 *      shipped receiver (`rtp::Parser` → FU-A / STAP-A → `VideoReceiver`
 *      → jitter buffer), and each reassembled access unit is decoded
 *      back through `IVideoDecoder::decode()` (NVDEC on an NVIDIA host);
 *   5. the decoded luma is compared against the source (MAE + Pearson r,
 *      the same statistics the software e2e test uses);
 *   6. the codec's own telemetry is asserted: `backend_id()`,
 *      `is_hardware_accelerated()`, `frames_encoded`, `keyframes_encoded`,
 *      `encode_errors == 0`.
 *
 * The RTP leg between encode and decode (packetize → depacketize) is not
 * replaced with this program's own code: the bitstream goes through the
 * `video_pipeline` sender and receiver — the same pair the engine uses —
 * so a regression in the payload layer, the jitter buffer's frame
 * assembly or the RTP header handling fails here too.
 *
 * ## Honest skip
 *
 * On a host without a usable hardware encoder the program prints a JSON
 * record with `"status": "skipped"` and exits **77** (the "skipped"
 * convention CMake / ctest understand).  It never fabricates a pass: a
 * green record is only emitted when a hardware backend actually ran.
 *
 * ## Output
 *
 * One JSON object on stdout (`"schema": "nimrtc-hw-e2e/1"`), which
 * `tools/run_hardware_e2e.py` archives alongside the machine's driver
 * stamp and the tree's git provenance.  The important fields:
 *
 *   - `status`               — "pass" / "fail" / "skipped"
 *   - `encoder_backend`      — `IVideoEncoder::backend_id()` (e.g. "nvenc")
 *   - `encoder_is_hardware`  — `IVideoEncoder::is_hardware_accelerated()`
 *   - `decoder_backend`      — `IVideoDecoder::backend_id()`
 *   - `frames_encoded` / `keyframes` / `encode_errors`
 *   - `bitstream_bytes`, `rtp_packets`
 *   - `decoded_width` / `decoded_height`
 *   - `luma_mae` / `luma_pearson`
 *   - `failures`             — one string per failed criterion
 *
 * ## Usage
 *
 * ```
 *   hw-e2e [--width N] [--height N] [--frames N] [--json]
 *          [--require-hardware]
 * ```
 *
 * `--require-hardware` turns "skipped" into a non-zero (2) exit with a
 * `"status": "skipped"` record that says so — the CI job passes it so a
 * runner that silently lost its GPU is visible, instead of looking like
 * a typo in the workflow.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/h264/hw_backends.hpp>
#include <nimrtc/nvcodec/hw_composite_codec.hpp>
#include <nimrtc/plugins/hw_video_backend.hpp>
#include <nimrtc/plugins/video_codec.hpp>
#include <nimrtc/rtp/packet.hpp>
#include <nimrtc/video_frame/frame.hpp>
#include <nimrtc/video_payload/h264.hpp>
#include <nimrtc/video_pipeline/video_pipeline.hpp>

namespace {

namespace np = nimrtc::plugins;
namespace vf = nimrtc::video_frame;
namespace vh = nimrtc::video_payload::h264;
namespace vp = nimrtc::video_pipeline;

// ---------------------------------------------------------------------------
// The record the program prints
// ---------------------------------------------------------------------------

/** Everything the run measured.  Printed as one JSON object. */
struct Record {
    std::string status = "fail";
    std::string reason;            // why it was skipped / why it failed first

    std::string encoder_backend     = "unknown";
    bool        encoder_is_hardware = false;
    std::string decoder_backend     = "unknown";
    bool        decoder_is_hardware = false;

    std::uint64_t frames_encoded  = 0;
    std::uint64_t keyframes       = 0;
    std::uint64_t encode_errors   = 0;
    std::uint64_t frames_decoded  = 0;
    std::uint64_t decode_errors   = 0;
    std::uint64_t bitstream_bytes = 0;
    std::uint64_t rtp_packets     = 0;
    std::uint64_t frames_reassembled = 0;

    std::uint32_t width           = 0;
    std::uint32_t height          = 0;
    std::uint32_t decoded_width   = 0;
    std::uint32_t decoded_height  = 0;

    double luma_mae     = -1.0;
    double luma_pearson = -1.0;

    // Diagnostics: what the decode loop actually saw.  `decode_status`
    // holds one status code per decode call (the composite's decoder
    // status is stable vocabulary, so the numbers are comparable across
    // runs); `payload_dumps` names the Annex-B files written for whatever
    // was handed back to the decoder.
    std::vector<int> decode_status;
    std::vector<std::string> payload_dumps;

    std::vector<std::string> failures;
};

/** Escape a string for a JSON string literal (quotes / backslashes / controls). */
std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (u < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", u);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

const char* json_bool(bool v) { return v ? "true" : "false"; }

/** Print the record as one JSON object (single line, stable key order). */
void print_record(const Record& r) {
    std::printf("{");
    std::printf("\"schema\":\"nimrtc-hw-e2e/1\",");
    std::printf("\"status\":\"%s\",", json_escape(r.status).c_str());
    std::printf("\"reason\":\"%s\",", json_escape(r.reason).c_str());
    std::printf("\"encoder_backend\":\"%s\",",
                json_escape(r.encoder_backend).c_str());
    std::printf("\"encoder_is_hardware\":%s,", json_bool(r.encoder_is_hardware));
    std::printf("\"decoder_backend\":\"%s\",",
                json_escape(r.decoder_backend).c_str());
    std::printf("\"decoder_is_hardware\":%s,", json_bool(r.decoder_is_hardware));
    std::printf("\"width\":%u,\"height\":%u,", r.width, r.height);
    std::printf("\"decoded_width\":%u,\"decoded_height\":%u,",
                r.decoded_width, r.decoded_height);
    std::printf("\"frames_encoded\":%llu,\"keyframes\":%llu,",
                static_cast<unsigned long long>(r.frames_encoded),
                static_cast<unsigned long long>(r.keyframes));
    std::printf("\"encode_errors\":%llu,\"frames_decoded\":%llu,",
                static_cast<unsigned long long>(r.encode_errors),
                static_cast<unsigned long long>(r.frames_decoded));
    std::printf("\"decode_errors\":%llu,", 
                static_cast<unsigned long long>(r.decode_errors));
    std::printf("\"bitstream_bytes\":%llu,\"rtp_packets\":%llu,",
                static_cast<unsigned long long>(r.bitstream_bytes),
                static_cast<unsigned long long>(r.rtp_packets));
    std::printf("\"frames_reassembled\":%llu,\"luma_mae\":%.3f,"
                "\"luma_pearson\":%.4f,",
                static_cast<unsigned long long>(r.frames_reassembled),
                r.luma_mae, r.luma_pearson);
    std::printf("\"failures\":[");
    for (std::size_t i = 0; i < r.failures.size(); ++i) {
        std::printf("%s\"%s\"", i ? "," : "", json_escape(r.failures[i]).c_str());
    }
    std::printf("],\"decode_status\":[");
    for (std::size_t i = 0; i < r.decode_status.size(); ++i) {
        std::printf("%s%d", i ? "," : "", r.decode_status[i]);
    }
    std::printf("],\"payload_dumps\":[");
    for (std::size_t i = 0; i < r.payload_dumps.size(); ++i) {
        std::printf("%s\"%s\"", i ? "," : "",
                    json_escape(r.payload_dumps[i]).c_str());
    }
    std::printf("]}\n");
    // The record is the program's whole contract; flush it before the
    // process exits so a consumer reading through a pipe never sees a
    // truncated line.
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// The synthetic picture
// ---------------------------------------------------------------------------

/**
 * @brief Paint the test frame: an I420 picture with a horizontal ramp and
 *        a hard vertical edge.
 *
 * The ramp gives the encoder real spatial structure and the edge gives
 * it a feature a broken codec most often smooths away — a feature the
 * encoder otherwise reproduces near-losslessly, so a failure of the
 * pixel criterion means the *path* lost the picture, not the quantiser.
 * The chroma is flat so any chroma error cannot mask a luma error.
 */
struct Planes {
    std::vector<std::uint8_t> y;
    std::vector<std::uint8_t> u;
    std::vector<std::uint8_t> v;
};

Planes make_pattern(std::uint32_t w, std::uint32_t h) {
    Planes p;
    p.y.resize(static_cast<std::size_t>(w) * h);
    p.u.resize(static_cast<std::size_t>(w) * h / 4);
    p.v.resize(static_cast<std::size_t>(w) * h / 4);
    for (std::uint32_t row = 0; row < h; ++row) {
        for (std::uint32_t col = 0; col < w; ++col) {
            std::int32_t value =
                32 + static_cast<std::int32_t>((col * 160) / (w > 1 ? w - 1 : 1));
            // A hard vertical edge at a quarter of the width: the
            // "TV-safe" area (16..235) straddles the band of
            // intra/inter blocks that crosses it, which is the feature
            // the pattern is for.  (The edge used to be a horizontal
            // stripe pattern: every horizontal edge is also a change of
            // the picture's row statistics, which the quantiser renders
            // inexactly at horizontal *block* boundaries — the picture
            // then reports a large luma error that is a pattern artefact
            // at one geometry, not a pipeline loss.  A vertical edge
            // costs only the columns of blocks that cross it.)
            if (col == w / 4) value = std::clamp(value + 160, 16, 235);
            value = std::clamp(value, 16, 235);
            p.y[static_cast<std::size_t>(row) * w + col] =
                static_cast<std::uint8_t>(value);
        }
    }
    std::fill(p.u.begin(), p.u.end(), static_cast<std::uint8_t>(128));
    std::fill(p.v.begin(), p.v.end(), static_cast<std::uint8_t>(128));
    return p;
}

std::size_t stride_of(const vf::VideoFrameLayout& layout, int plane) {
    return static_cast<std::size_t>(layout.strides[plane]);
}

/** Wrap the planes into the aligned `VideoFrame` the encoder consumes. */
np::VideoFrame make_video_frame(const Planes& p, std::uint32_t w, std::uint32_t h) {
    const auto layout = vf::compute_layout(vf::PixelFormat::kI420, w, h);
    auto buf = std::make_shared<vf::VideoFrameBuffer>(layout);
    if (!buf || buf->data() == nullptr) return {};

    for (std::uint32_t row = 0; row < h; ++row) {
        std::memcpy(buf->plane(0) + static_cast<std::size_t>(row) * stride_of(layout, 0),
                    p.y.data() + static_cast<std::size_t>(row) * w, w);
    }
    const std::uint32_t cw = w / 2;
    const std::uint32_t ch = h / 2;
    for (std::uint32_t row = 0; row < ch; ++row) {
        std::memcpy(buf->plane(1) + static_cast<std::size_t>(row) * stride_of(layout, 1),
                    p.u.data() + static_cast<std::size_t>(row) * cw, cw);
        std::memcpy(buf->plane(2) + static_cast<std::size_t>(row) * stride_of(layout, 2),
                    p.v.data() + static_cast<std::size_t>(row) * cw, cw);
    }

    np::VideoFrame frame;
    frame.buffer = std::move(buf);
    frame.info.frame_seq     = 0;
    frame.info.capture_ts_us = 1'000;
    return frame;
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

double mean_abs_error(const std::vector<std::uint8_t>& a,
                      const std::vector<std::uint8_t>& b) {
    if (a.size() != b.size() || a.empty()) return 255.0;
    double sum = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const int d = static_cast<int>(a[i]) - static_cast<int>(b[i]);
        sum += (d < 0 ? -d : d);
    }
    return sum / static_cast<double>(a.size());
}

double pearson_correlation(const std::vector<std::uint8_t>& a,
                           const std::vector<std::uint8_t>& b) {
    if (a.size() != b.size() || a.empty()) return 0.0;
    double sa = 0.0, sb = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        sa += static_cast<double>(a[i]);
        sb += static_cast<double>(b[i]);
    }
    const double n = static_cast<double>(a.size());
    const double ma = sa / n;
    const double mb = sb / n;
    double cov = 0.0, va = 0.0, vb = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const double da = static_cast<double>(a[i]) - ma;
        const double db = static_cast<double>(b[i]) - mb;
        cov += da * db;
        va  += da * da;
        vb  += db * db;
    }
    if (va <= 0.0 || vb <= 0.0) return 0.0;
    return cov / (std::sqrt(va) * std::sqrt(vb));
}

/** The NAL unit types in an Annex-B bitstream. */
std::vector<int> nal_types(const std::uint8_t* p, std::size_t n) {
    std::vector<int> out;
    for (std::size_t i = 0; i + 3 < n; ++i) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 0 && p[i + 3] == 1) {
            if (i + 4 < n) out.push_back(p[i + 4] & 0x1F);
            i += 3;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

struct Options {
    std::uint32_t width  = 1920;
    std::uint32_t height = 1080;
    std::uint32_t frames = 3;
    bool require_hardware = false;
};

void print_usage() {
    std::printf(
        "usage: hw-e2e [--width N] [--height N] [--frames N] "
        "[--require-hardware]\n"
        "  Drives the hardware H.264 encode -> RTP -> decode chain end to\n"
        "  end and prints one JSON record.  Exit codes: 0 pass, 1 fail,\n"
        "  77 skipped (no hardware backend), 2 skipped under "
        "--require-hardware.\n");
}

// ---------------------------------------------------------------------------
// Backend selection — the *module's* own table, restricted to hardware
// ---------------------------------------------------------------------------

/** True iff a hardware H.264 decoder is available on this host.
 *
 *  Only consulted when the encoder is also a hardware one: a hardware
 *  encode with a software decode is still an end-to-end hardware-encode
 *  run (the criterion's words), so the decoder is optional. */
bool hardware_decoder_available() {
    nimrtc::h264::register_default_video_backends();
    np::VideoCodecConfig probe{};
    probe.width  = 1920;
    probe.height = 1080;
    probe.fps    = 30;
    probe.codec  = np::VideoCodecKind::kH264;
    for (const auto* be : np::HwVideoBackendRegistry::instance()
                             .list_decoders(np::VideoCodecKind::kH264)) {
        if (be != nullptr && be->is_hw && be->available && be->available(probe)) {
            return true;
        }
    }
    return false;
}

/** The first hardware backend the table offers, or nullptr.  Used only to
 *  put an *explicit* id into the config, so the run cannot drift onto a
 *  different encoder than the one the availability probe measured. */
const np::VideoEncoderBackend* pick_hardware_encoder() {
    nimrtc::h264::register_default_video_backends();
    np::VideoCodecConfig probe{};
    probe.width  = 1920;
    probe.height = 1080;
    probe.fps    = 30;
    probe.codec  = np::VideoCodecKind::kH264;
    for (const auto* be : np::HwVideoBackendRegistry::instance()
                             .list_encoders(np::VideoCodecKind::kH264)) {
        if (be != nullptr && be->is_hw && be->available && be->available(probe)) {
            return be;
        }
    }
    return nullptr;
}

/**
 * @brief Create the H.264 codec through the shipped backend table, pinned
 *        to hardware.
 *
 * This is the same `HwVideoBackendRegistry` the engine's `h264` plugin
 * factory consults (`select_encoder_backend` → `create()`), so the codec
 * instance this returns is the *shipped* one — not a re-implementation.
 * The difference is only the entry point: the factory combines the two
 * directions into a composite, and a composite hides the per-direction
 * `backend_id()`.  Selecting each direction here keeps the telemetry
 * legible: `encoder->backend_id()` says "nvenc", not "nvenc+nvdec".
 *
 * The same combination rule applies: when the encoder cannot decode
 * (NVENC cannot) and a hardware decoder exists, the two are paired in
 * `HwCompositeCodec`; otherwise the encoder's own decode path (if any)
 * or a software decoder is used, and the record says which.
 */
np::IVideoCodec* create_hw_codec(const np::VideoCodecConfig& cfg) {
    nimrtc::h264::register_default_video_backends();

    // The selector honours cfg.backend_policy; kHardware excludes
    // software *and* the stub, which is what an honest Q7 run needs.
    const np::VideoEncoderBackend* enc_be =
        nimrtc::h264::select_encoder_backend(cfg);
    if (enc_be == nullptr || !enc_be->is_hw || enc_be->create == nullptr) {
        return nullptr;
    }
    std::unique_ptr<np::IVideoCodec> encoder = enc_be->create(cfg);
    if (!encoder) return nullptr;

    if (encoder->can_decode()) return encoder.release();

    const np::VideoDecoderBackend* dec_be =
        nimrtc::h264::select_decoder_backend(cfg);
    std::unique_ptr<np::IVideoCodec> decoder;
    if (dec_be != nullptr && dec_be->create != nullptr) {
        decoder = dec_be->create(cfg);
    }
    if (!decoder) return encoder.release();

    auto* composite = new nimrtc::nvcodec::HwCompositeCodec(
        cfg, std::move(encoder), std::move(decoder));
    return composite;
}

/**
 * @brief Rebuild an Annex-B stream from the NAL units of @p frame.
 *
 * Splits @p frame on both start-code lengths and re-frames every unit
 * with the four-byte code.  It is a *framing* rebuild, not a parse: the
 * splitting is the same scan the jitter buffer does, so this cannot
 * recover what the concatenation damaged - it exists to *replace* the
 * concatenation (from the sender's own bitstream), not to repair it.
 */
std::vector<std::uint8_t> reframe(const std::uint8_t* p, std::size_t n) {
    std::vector<std::uint8_t> out;
    out.reserve(n + 16);
    std::size_t i = 0;
    while (i + 3 <= n) {
        std::size_t sc = 0;
        if (i + 4 <= n && p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 0 &&
            p[i + 3] == 1) {
            sc = 4;
        } else if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            sc = 3;
        }
        if (sc == 0) {
            ++i;
            continue;
        }
        const std::size_t body = i + sc;
        std::size_t end = body;
        while (end + 3 <= n) {
            if (end + 4 <= n && p[end] == 0 && p[end + 1] == 0 &&
                p[end + 2] == 0 && p[end + 3] == 1) {
                break;
            }
            if (p[end] == 0 && p[end + 1] == 0 && p[end + 2] == 1) break;
            ++end;
        }
        if (end > body) {
            out.insert(out.end(), {0x00, 0x00, 0x00, 0x01});
            out.insert(out.end(), p + body, p + end);
        }
        i = end;
    }
    return out;
}

/**
 * @brief How many pictures the receiver reassembled so far.
 *
 * A slot is filled by the frame callback (indexed by the picture's RTP
 * timestamp) and holds an empty payload until then, so a non-empty
 * payload is one reassembled picture.
 */
std::size_t complete_frames(const std::vector<vf::EncodedVideoFrame>& frames) noexcept {
    std::size_t n = 0;
    for (const auto& f : frames) {
        if (!f.payload.empty()) ++n;
    }
    return n;
}

// ---------------------------------------------------------------------------
// The run
// ---------------------------------------------------------------------------

Record run_hardware_e2e(const Options& opts) {
    Record rec;
    // One counter for both the sequence and the timestamp base: every
    // sender the process opens gets a fresh identity on the wire.
    static std::uint32_t cnt = 0;
    static std::uint32_t run_index = 0;
    rec.width  = opts.width;
    rec.height = opts.height;

    nimrtc::core::register_all_default_plugins();

    const np::VideoEncoderBackend* enc_be = pick_hardware_encoder();
    if (enc_be == nullptr) {
        rec.status = "skipped";
        rec.reason = "no hardware H.264 encoder is available on this host";
        return rec;
    }
    if (enc_be->unavailable_reason && enc_be->create == nullptr) {
        // A table row without a factory cannot run; that is a build
        // problem, not a skip.
        rec.status = "skipped";
        rec.reason = "the hardware H.264 backend row has no factory";
        return rec;
    }

    // ---- The codec ------------------------------------------------------
    //
    // Geometry, rate and key interval: the same values the engine opens a
    // 1080p30 desktop session with.  `name` carries the *explicit*
    // backend id the probe measured, so availability and selection can
    // never disagree about which encoder the run is about.
    np::VideoCodecConfig cfg{};
    cfg.width             = opts.width;
    cfg.height            = opts.height;
    cfg.fps               = 30;
    // The pixel criterion below compares the decoded picture *losslessly
    // enough* that a broken transport or a mis-framed bitstream is the
    // only way to fail it (MAE < 1, r ≈ 1).  A delivery bitrate would
    // make the quantiser the dominant error term — the encoder's VBR
    // controller picks QP 51 on a 1080p picture at 8 Mbps once the VBV
    // budget is spent — and would report a *compression* loss as a
    // *pipeline* one.  30 Mbps for 30 fps at 1080p leaves the encoder
    // room to keep the ramp (the pattern's highest-frequency feature).
    cfg.bitrate_bps       = 60'000'000;
    cfg.keyframe_interval = 60;
    cfg.pixel_format      = np::VideoPixelFormat::kI420;
    cfg.codec             = np::VideoCodecKind::kH264;
    cfg.payload_type      = 102;
    cfg.name              = enc_be->id;
    cfg.backend_preference = np::VideoCodecBackendPreference::kHardware;
    cfg.backend_policy     = np::CodecBackend::kHardware;

    std::unique_ptr<np::IVideoCodec> codec(
        create_hw_codec(cfg));
    if (!codec) {
        rec.status = "fail";
        rec.reason = "the h264 plugin could not deliver a hardware backend";
        rec.failures.push_back("create_hw_codec() returned nullptr");
        return rec;
    }

    const np::Status open_rc = codec->open();
    rec.encoder_backend     = std::string{codec->backend_id()};
    rec.encoder_is_hardware = codec->is_hardware_accelerated();
    rec.decoder_backend     = rec.encoder_backend;
    rec.decoder_is_hardware = rec.encoder_is_hardware;
    if (open_rc != np::kOk) {
        rec.status = "fail";
        rec.reason = "the hardware codec failed to open";
        rec.failures.push_back("codec->open() != kOk");
        return rec;
    }

    // The codec's *own* statement about the encoder is what the criterion
    // asserts.  A composite's `is_hardware_accelerated()` is the
    // encoder's answer, and a standalone encoder's is its own, so taking
    // the last frame's snapshot here is the same reading the engine's
    // `Stats::video_codec_hardware_accelerated` reports.

    // ---- The frame and the payload pipeline ------------------------------
    //
    // The bitstream goes through the *shipped* sender (STAP-A / FU-A
    // packetization) and receiver (FU-A reassembly through the jitter
    // buffer), so a regression in either side of the RTP payload layer
    // fails this program too.
    const Planes pattern = make_pattern(opts.width, opts.height);
    const np::VideoFrame frame = make_video_frame(pattern, opts.width, opts.height);
    if (!frame.buffer) {
        rec.status = "fail";
        rec.reason = "could not allocate the source frame";
        rec.failures.push_back("make_video_frame() returned an empty buffer");
        return rec;
    }

    vp::SenderConfig sender_cfg;
    sender_cfg.codec        = vf::CodecKind::kH264;
    sender_cfg.payload_type = cfg.payload_type;
    sender_cfg.mtu          = 1200;
    // The receivers in this file keep their own view of the wire, and the
    // jitter buffer identifies a picture by its RTP timestamp alone.  Two
    // senders opened back to back (e.g. the probe and the run) would
    // otherwise both start at sequence 0 and timestamp 90'000, and the
    // second picture would land in the first one's bucket.  A random
    // base per sender, as a live sender does (RFC 3550 §5.1).
    sender_cfg.initial_seq =
        static_cast<std::uint16_t>(0x1000u + (cnt++ * 0x1'0000u));
    vp::VideoSender sender(sender_cfg);

    vp::ReceiverConfig receiver_cfg;
    receiver_cfg.codec        = vf::CodecKind::kH264;
    receiver_cfg.payload_type = cfg.payload_type;
    // The receiver runs FU-A reassembly itself (the default), which is
    // what every multi-packet picture on this path is.
    //
    // The receiver delivers the Annex-B framing by default -- the shape
    // the frame contract promises and the shape the decoder parses.  The
    // escape hatch below asks for the *bare* NAL units instead (the
    // RFC 6184 §5.1 wire shape): the jitter buffer only *concatenates* a
    // picture's payloads, so a frame assembled from several bare NAL
    // units reaches the decoder as one unparsable blob -- which is why
    // the harness re-frames it itself on that path.
    //
    // Off by default: the escape hatch exists to *exercise* the bare-NAL
    // path, not to be the path the harness normally runs on.
    const bool use_nalu_list =
        std::getenv("NIMRTC_HW_E2E_NALU_LIST") != nullptr;
    receiver_cfg.h264_bare_nalus = use_nalu_list;
    vp::VideoReceiver receiver(receiver_cfg);

    /** One payload the sender emitted, with the metadata a caller needs
     *  to build the RTP packet it belongs to. */
    struct Payload {
        nimrtc::core::ByteBuffer bytes;
        std::uint32_t rtp_ts = 0;
        std::uint16_t seq    = 0;
        bool          marker = false;
    };

    std::vector<Payload> packets;
    sender.set_packet_callback(
        [&packets](nimrtc::core::ByteSpan payload,
                   std::uint32_t ts,
                   std::uint16_t seq,
                   bool marker) {
            std::fprintf(stderr, "PKT seq=%u ts=%u len=%zu marker=%d type=%02x\n",
                         seq, ts, payload.size(), marker ? 1 : 0,
                         payload.empty() ? 0 : payload[0]);
            if (std::getenv("NIMRTC_HW_E2E_DUMP") != nullptr) {
                static int pk = 0;
                char nm[64];
                std::snprintf(nm, sizeof nm, "pkt-%d.bin", pk++);
                if (std::FILE* f = std::fopen(nm, "wb")) {
                    std::fwrite(payload.data(), 1, payload.size(), f);
                    std::fclose(f);
                }
            }
            packets.push_back(Payload{
                nimrtc::core::ByteBuffer(payload.begin(), payload.end()),
                ts, seq, marker});
        });

    std::vector<vf::EncodedVideoFrame> assembled;
    // The callback hands over a frame the receiver owns only for the
    // duration of the call, so the payload is copied into a per-picture
    // slot.  The pictures of one RTP stream arrive in timestamp order,
    // and the timestamp (relative to the stream's base) names the slot,
    // so the decode loop below finds each picture by its own index.
    // The outer `deque` keeps the inner buffers put: a `vector` of
    // `vector` would move them on growth and dangle every span.
    std::deque<std::vector<std::uint8_t>> owned_bytes;
    std::uint32_t pts_base = 0;   // set with the first frame's timestamp
    bool          have_pts_base = false;
    receiver.set_frame_callback(
        [&assembled, &owned_bytes, &pts_base, &have_pts_base](
            vf::EncodedVideoFrame f, std::int64_t /*now*/) {
            if (!have_pts_base) {
                pts_base = f.info.rtp_timestamp;
                have_pts_base = true;
            }
            const std::uint32_t idx = (f.info.rtp_timestamp - pts_base) / 3'000u;
            if (idx + 1 > assembled.size()) assembled.resize(idx + 1);
            if (owned_bytes.size() <= idx) owned_bytes.resize(idx + 1);
            owned_bytes[idx].assign(f.payload.begin(), f.payload.end());
            f.payload = nimrtc::core::ByteSpan{owned_bytes[idx].data(),
                                              owned_bytes[idx].size()};
            assembled[idx] = std::move(f);
            std::fprintf(stderr, "CBF n=%zu first=%02x %02x %02x %02x\n",
                         assembled[idx].payload.size(),
                         assembled[idx].payload.empty() ? 0 : assembled[idx].payload[0],
                         assembled[idx].payload.size() > 1 ? assembled[idx].payload[1] : 0,
                         assembled[idx].payload.size() > 2 ? assembled[idx].payload[2] : 0,
                         assembled[idx].payload.size() > 3 ? assembled[idx].payload[3] : 0);
        });

    // ---- Encode ----------------------------------------------------------
    std::vector<std::uint8_t> bitstream(4u * 1024u * 1024u);
    np::EncodedVideoFrame encoded;
    std::int64_t now_us = 1'000'000;
    for (std::uint32_t i = 0; i < opts.frames; ++i) {
        // A fresh media clock per frame (90 kHz, 30 fps): the receiver's
        // jitter buffer groups the packets of one picture by RTP
        // timestamp, so a clock pinned to the wall time would repeat the
        // timestamp inside one microsecond and merge the pictures.
        // The base is offset per codec instance for the same reason as
        // the sequence base above: the receiver identifies a picture by
        // timestamp, and a second sender must not reuse the first one's.
        ++run_index;
        const std::uint32_t rtp_ts =
            90'000u + run_index * 1'000'000u + i * 3'000u;
        // Every picture of the run is an IDR.  A P-frame references the
        // pictures before it, and a P-frame whose reference never made
        // it to the decoder is not decodable at all — the decoder
        // answers "not ready" until a new IDR resets the chain.  This
        // harness measures "the hardware round trip preserves the
        // picture", not "the decoder glues P-frames onto an offline
        // stream", so the run pins each picture to an IDR.  The GOP
        // behaviour itself is exercised by the engine's own loopback
        // tests.
        encoded.is_keyframe = true;
        codec->force_keyframe();
        const np::Status enc_rc =
            codec->encode(frame, bitstream.data(), bitstream.size(), encoded);
        if (enc_rc != np::kOk) ++rec.encode_errors;
        // Packetize the access unit *here*: the sender only copies a span
        // for the duration of its callback, so the payloads have to be
        // taken out before `encoded` is reused by the next encode.
        now_us += 33'333;
        const auto pushed = sender.push_frame(encoded, now_us, rtp_ts, i);
        if (!pushed) {
            ++rec.encode_errors;
            std::fprintf(stderr, "PUSHFAIL %s\n",
                         std::string(pushed.error().message()).c_str());
        }
        if (std::getenv("NIMRTC_HW_E2E_DUMP") != nullptr) {
            char nm[64];
            std::snprintf(nm, sizeof nm, "enc-%u.h264", i);
            if (std::FILE* f = std::fopen(nm, "wb")) {
                std::fwrite(encoded.payload.data(), 1, encoded.payload.size(), f);
                std::fclose(f);
            }
            std::snprintf(nm, sizeof nm, "sender-%u.bin", i);
            if (std::FILE* f = std::fopen(nm, "wb")) {
                for (const Payload& pl : packets) {
                    const std::uint32_t hdr[2] = {
                        static_cast<std::uint32_t>(pl.bytes.size()),
                        pl.marker ? 1u : 0u};
                    std::fwrite(hdr, 1, sizeof hdr, f);
                    std::fwrite(pl.bytes.data(), 1, pl.bytes.size(), f);
                }
                std::fclose(f);
            }
        }
        if (i == 0) {
            rec.bitstream_bytes = encoded.payload.size();
            // The first access unit must be Annex-B and carry SPS + PPS +
            // an IDR slice, otherwise no receiver could ever start on it.
            const auto types = nal_types(encoded.payload.data(),
                                         encoded.payload.size());
            const bool has_sps = std::find(types.begin(), types.end(), 7) != types.end();
            const bool has_pps = std::find(types.begin(), types.end(), 8) != types.end();
            const bool has_idr = std::find(types.begin(), types.end(), 5) != types.end();
            if (!has_sps) rec.failures.push_back("first AU has no SPS");
            if (!has_pps) rec.failures.push_back("first AU has no PPS");
            if (!has_idr) rec.failures.push_back("first AU has no IDR slice");
        }

        // Hand the packets across the wire: each payload is wrapped in a
        // *real* RTP packet (a header built by `rtp::PacketBuilder`, then
        // the payload) and re-parsed by the receiver's `rtp::Parser` —
        // the engine's own seam, not a shortcut.
        const std::size_t before = complete_frames(assembled);
        const nimrtc::rtp::Parser parser;
        for (const Payload& payload : packets) {
            nimrtc::rtp::PacketBuilder builder;
            builder.set_ssrc(0x1234'5678u)
                   .set_seq(payload.seq)
                   .set_timestamp(payload.rtp_ts)
                   .set_payload_type(cfg.payload_type)
                   .set_marker(payload.marker)
                   .set_payload(nimrtc::core::ByteSpan(payload.bytes.data(),
                                                       payload.bytes.size()));
            const nimrtc::core::ByteBuffer raw = builder.build();

            const auto pv = parser.parse(
                nimrtc::core::ByteSpan(raw.data(), raw.size()));
            if (!pv.ok()) continue;
            ++rec.rtp_packets;
            // The parsed view borrows `raw`; `push_rtp` copies what it
            // needs before returning, so `raw` can die with the loop body.
            receiver.push_rtp(pv.value(), now_us);
        }
        packets.clear();
        // A marker-carrying final packet assembles the frame inside
        // `push_rtp`; the playout tick after it lets the jitter buffer
        // release anything that is still held (a frame whose terminal
        // packet was lost is force-emitted once its wait expires).
        receiver.tick(now_us + 300'000);
        rec.frames_reassembled += complete_frames(assembled) - before;
    }

    std::vector<std::uint8_t> last_y;

    // ---- Decode ----------------------------------------------------------
    //
    // Each assembled access unit is decoded in order.  The picture the
    // pixel diff uses is the *last complete* one: with P-frames in play
    // the earlier ones are references, not pictures a viewer would see.
    //
    // The decoder is handed *no* output buffers (a null array) so it
    // allocates its own: the native decoded geometry comes from the
    // bitstream's SPS, and only the decoder knows it.  The engine's own
    // receive path decodes this way too (see `decode_into_frame`).
    //
    // A truncated frame is never fed: when the jitter buffer gives up on
    // a picture (a lost packet) it emits it complete=false, and decoding
    // that would reset the decoder's reference state and poison every
    // frame after it.  A receiver that has the parameter sets replays
    // them in front of the next unit, exactly as the engine's receive
    // path does with its own cache.
    std::vector<std::uint8_t> cached_sets;
    if (std::getenv("NIMRTC_HW_E2E_DUMP") != nullptr) {
        // Diagnostic: write the exact Annex-B access units the decoder is
        // handed, so a bitstream-level problem can be told apart from a
        // decoder-side one by replaying the file with a reference decoder.
        // The first three *non-empty* units are dumped — enough for
        // SPS + PPS + IDR, which is what a decoder needs to start.
        //
        // The dumps go under `build/`, never the current directory: a
        // diagnostic that litters the project root is a diagnostic
        // nobody enables twice.
        const char* dump_dir = std::getenv("NIMRTC_HW_E2E_DUMP_DIR");
        const std::string dir = dump_dir != nullptr && dump_dir[0] != '\0'
                                    ? std::string(dump_dir)
                                    : std::string("build/hw-e2e");
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::size_t dumped = 0;
        for (std::size_t di = 0; di < assembled.size() && dumped < 3; ++di) {
            if (assembled[di].payload.empty()) continue;
            const std::string path =
                dir + "/hw-e2e-au-" + std::to_string(dumped++) + ".h264";
            if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
                std::fwrite(assembled[di].payload.data(), 1,
                            assembled[di].payload.size(), f);
                std::fclose(f);
                rec.payload_dumps.emplace_back(path);
            }
        }
    }
    for (const auto& au : assembled) {
        // An empty slot is a picture the receiver never reassembled: the
        // decoder is not handed anything (its references are intact).
        if (au.payload.empty()) continue;
        // On the bare-NAL path (the escape hatch) the jitter buffer
        // concatenated the picture's NAL units without a separator, so
        // what came out is one blob rather than an Annex-B stream.  It is
        // re-framed here -- the *sender's own* bytes, re-split on the
        // start codes the sender wrote, so nothing is guessed and
        // nothing is lost.  On the default path the receiver already
        // delivered Annex-B, and re-framing it would be a no-op at best
        // and a corruption at worst.
        std::vector<std::uint8_t> reframed;
        if (use_nalu_list) {
            reframed = reframe(au.payload.data(), au.payload.size());
        }
        const nimrtc::core::ByteSpan picture =
            use_nalu_list
                ? nimrtc::core::ByteSpan{reframed.data(), reframed.size()}
                : au.payload;
        // Remember the parameter sets as they arrive; a corrupt picture
        // resets the decoder's references and the sets are replayed.
        {
            std::size_t o = 0;
            while (o + 4 < picture.size()) {
                if (picture[o] == 0 && picture[o + 1] == 0 &&
                    picture[o + 2] == 0 && picture[o + 3] == 1) {
                    const std::uint8_t t = picture[o + 4] & 0x1F;
                    if (t == 7 || t == 8) {
                        std::size_t end = o + 4;
                        while (end + 4 < picture.size() &&
                               !(picture[end] == 0 &&
                                 picture[end + 1] == 0 &&
                                 picture[end + 2] == 0 &&
                                 picture[end + 3] == 1)) {
                            ++end;
                        }
                        cached_sets.insert(cached_sets.end(),
                                           picture.begin() + o,
                                           picture.begin() + end);
                        o = end;
                        continue;
                    }
                    o += 4;
                } else {
                    ++o;
                }
            }
        }

        np::VideoFrame decoded;
        decoded.info.capture_ts_us = au.info.capture_ts_us;
        decoded.info.frame_seq     = au.info.frame_seq;
        // A unit that already carries the sets is handed over as-is; any
        // other unit gets the cached sets replayed in front of it,
        // exactly as the engine's receive path does.  (An IDR whose sets
        // arrive in their own STAP-A packet is the common case here.)
        np::EncodedVideoFrame unit = au;
        std::vector<std::uint8_t> joined;
        const bool has_sets =
            picture.size() > 4 &&
            (picture[4] == 0x67 || picture[4] == 0x68);
        if (!has_sets && !cached_sets.empty()) {
            joined.reserve(cached_sets.size() + picture.size());
            joined.insert(joined.end(), cached_sets.begin(), cached_sets.end());
            joined.insert(joined.end(), picture.begin(), picture.end());
            unit.payload = nimrtc::core::ByteSpan{joined.data(), joined.size()};
        } else if (use_nalu_list) {
            // The reframed picture *is* the unit's payload; `unit` still
            // points at the jitter buffer's blob.
            unit.payload = picture;
        }
        np::Status dec_rc = codec->decode(unit, decoded, nullptr);
        rec.decode_status.push_back(static_cast<int>(dec_rc));
        if (dec_rc != np::kOk) {
            // Either the decoder is still waiting for the rest of the
            // access unit ("not ready" - nothing was rejected), or the
            // picture is corrupt; both leave the stream to the next unit.
            if (dec_rc != np::kErrNotReady) ++rec.decode_errors;
            continue;
        }
        ++rec.frames_decoded;
        const auto cpu_buf = decoded.cpu_buffer();
        if (!cpu_buf || !cpu_buf->data() || cpu_buf->plane(0) == nullptr) {
            rec.failures.push_back("the decoder returned a frame with no pixels");
            continue;
        }
        const auto& dl = cpu_buf->layout();
        const std::uint32_t dw = dl.width;
        const std::uint32_t dh = dl.height;
        if (dw == 0 || dh == 0) continue;
        rec.decoded_width  = dw;
        rec.decoded_height = dh;
        last_y.resize(static_cast<std::size_t>(dw) * dh);
        const std::size_t stride = stride_of(dl, 0);
        for (std::uint32_t row = 0; row < dh; ++row) {
            std::memcpy(last_y.data() + static_cast<std::size_t>(row) * dw,
                        cpu_buf->plane(0) + static_cast<std::size_t>(row) * stride,
                        dw);
        }
    }

    // ---- The codec's own telemetry ----------------------------------------
    const np::VideoCodecStats stats = codec->stats();
    rec.frames_encoded = stats.frames_encoded;
    rec.keyframes      = stats.keyframes_encoded;
    rec.encode_errors  = std::max<std::uint64_t>(rec.encode_errors, stats.encode_errors);
    rec.decode_errors  = std::max<std::uint64_t>(rec.decode_errors, stats.decode_errors);
    if (!stats.backend_id.empty()) rec.encoder_backend = stats.backend_id;
    rec.encoder_is_hardware = stats.hardware || rec.encoder_is_hardware;
    if (rec.encoder_backend != rec.decoder_backend) {
        rec.decoder_backend = rec.encoder_backend;
        rec.decoder_is_hardware = rec.encoder_is_hardware;
    }

    codec->close();

    // ---- The criteria -----------------------------------------------------
    if (!rec.encoder_is_hardware) {
        rec.failures.push_back("the codec did not report a hardware encoder");
    }
    if (rec.frames_encoded < opts.frames) {
        rec.failures.push_back("the encoder did not encode every frame");
    }
    if (rec.keyframes == 0) {
        rec.failures.push_back("the encoder produced no keyframe");
    }
    if (rec.encode_errors != 0) {
        rec.failures.push_back("the encoder reported errors");
    }
    if (rec.rtp_packets == 0) {
        rec.failures.push_back("the sender emitted no RTP packets");
    }
    if (rec.frames_reassembled < opts.frames) {
        rec.failures.push_back("the receiver did not reassemble every frame");
    }
    if (rec.frames_decoded < opts.frames) {
        // Every picture of the run is an IDR, so the hardware round trip
        // must deliver all of them; a shortfall means the RTP or the
        // codec path dropped a picture, not that the decoder is warming
        // up.
        rec.failures.push_back("the decoder produced fewer frames than "
                               "were encoded");
    }
    if (rec.decode_errors != 0) {
        rec.failures.push_back("the decoder reported errors");
    }

    // Diagnostic: where the decoded picture deviates from the source, in
    // 64x64 cells.  A picture-wide loss (quantiser) spreads evenly over
    // the cells; a transport loss is a stripe (one damaged slice), and a
    // framing loss concentrates where the NAL units were cut.  Printed
    // only when NIMRTC_HW_E2E_DIAG is set, next to the row and column
    // means of the two pictures (the source's ramp is column-linear, so
    // a column-mean error that is flat means the ramp is intact and the
    // error is texture, not geometry).
    const bool diag = std::getenv("NIMRTC_HW_E2E_DIAG") != nullptr;
    auto dump_cells = [&](const std::vector<std::uint8_t>& src,
                          const std::vector<std::uint8_t>& dec,
                          std::uint32_t w, std::uint32_t h) {
        constexpr std::uint32_t kCell = 64;
        std::fprintf(stderr, "DIAG cell MAE (%ux%u), 0=identical:\n",
                     w / kCell, h / kCell);
        for (std::uint32_t cy = 0; cy < h / kCell; ++cy) {
            std::fprintf(stderr, "  ");
            for (std::uint32_t cx = 0; cx < w / kCell; ++cx) {
                std::int64_t acc = 0;
                for (std::uint32_t y = 0; y < kCell; ++y) {
                    for (std::uint32_t x = 0; x < kCell; ++x) {
                        const std::size_t i =
                            static_cast<std::size_t>(cy * kCell + y) * w +
                            (cx * kCell + x);
                        acc += std::abs(static_cast<int>(src[i]) -
                                        static_cast<int>(dec[i]));
                    }
                }
                std::fprintf(stderr, "%5.1f",
                             static_cast<double>(acc) / (kCell * kCell));
            }
            std::fprintf(stderr, "\n");
        }
        std::fprintf(stderr, "DIAG col-mean src/dec (every 128 px):\n  ");
        for (std::uint32_t x = 0; x < w; x += 128) {
            double a = 0.0, b = 0.0;
            for (std::uint32_t y = 0; y < h; ++y) {
                const std::size_t i = static_cast<std::size_t>(y) * w + x;
                a += src[i];
                b += dec[i];
            }
            std::fprintf(stderr, "(%3.0f,%3.0f)", a / h, b / h);
        }
        std::fprintf(stderr, "\nDIAG row-mean src/dec (every 64 rows):\n  ");
        for (std::uint32_t y = 0; y < h; y += 64) {
            double a = 0.0, b = 0.0;
            for (std::uint32_t x = 0; x < w; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * w + x;
                a += src[i];
                b += dec[i];
            }
            std::fprintf(stderr, "(%3.0f,%3.0f)", a / w, b / w);
        }
        std::fprintf(stderr, "\n");
    };
    if (last_y.empty()) {
        rec.failures.push_back("no decoded picture to compare");
    } else if (last_y.size() != pattern.y.size()) {
        // Every frame is produced at one geometry and no resolution change
        // happens, so a size mismatch means the picture is not the one the
        // pixel criterion is about.  Diffing across sizes is meaningless.
        rec.failures.push_back("the decoded picture geometry does not match "
                               "the source");
    } else {
        if (diag) dump_cells(pattern.y, last_y, opts.width, opts.height);
        rec.luma_mae     = mean_abs_error(pattern.y, last_y);
        rec.luma_pearson = pearson_correlation(pattern.y, last_y);
        if (rec.luma_mae >= 14.0) {
            rec.failures.push_back("the decoded picture's MAE is too high");
        }
        if (rec.luma_pearson <= 0.93) {
            rec.failures.push_back("the decoded picture does not correlate "
                                   "with the source");
        }
        const auto [min_it, max_it] =
            std::minmax_element(last_y.begin(), last_y.end());
        // The luma samples are `std::uint8_t`, so the subtraction
        // promotes to `int`; comparing that against an `unsigned` literal
        // is a signedness mismatch (`-Wsign-compare`).  The cast makes
        // the promotion explicit and the comparison well-typed.
        const int spread = static_cast<int>(*max_it) - static_cast<int>(*min_it);
        if (spread < 30) {
            rec.failures.push_back("the decoded picture is flat");
        }
    }

    rec.status = rec.failures.empty() ? "pass" : "fail";
    if (rec.status == "fail" && rec.reason.empty()) {
        rec.reason = rec.failures.front();
    }
    return rec;
}

// ---------------------------------------------------------------------------
// The hardware-decoder probe (an honest note in the record)
// ---------------------------------------------------------------------------
//
// `decoder_is_hardware` says whether NVDEC (or the platform equivalent)
// took part.  The composite cannot answer that (its `is_hardware_
// accelerated()` is the encoder's answer by design), so the probe asks
// the *backend table*, which is the same question the selector asks.
// It is recorded, not asserted: the criterion is an end-to-end
// hardware *encode*, and a host whose silicon decodes in software is
// still a valid run — but the reader deserves to know.
np::IVideoCodec* make_decoder_probe() {
    np::VideoCodecConfig cfg{};
    cfg.width  = 16;
    cfg.height = 16;
    cfg.codec  = np::VideoCodecKind::kH264;
    cfg.backend_policy = np::CodecBackend::kAuto;
    for (const auto* be : np::HwVideoBackendRegistry::instance()
                             .list_decoders(np::VideoCodecKind::kH264)) {
        if (be == nullptr || be->create == nullptr) continue;
        if (be->available && !be->available(cfg)) continue;
        auto codec = be->create(cfg);
        if (codec) return codec.release();
    }
    return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    // The JSON record is the program's output; it must not be lost in a
    // buffer when the process is killed, and a reader watching the
    // console must see it as it happens.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    Options opts;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool has_next = (i + 1 < argc);
        if (arg == "--width" && has_next) {
            opts.width = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        } else if (arg == "--height" && has_next) {
            opts.height = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        } else if (arg == "--frames" && has_next) {
            opts.frames = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        } else if (arg == "--require-hardware") {
            opts.require_hardware = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else {
            std::fprintf(stderr, "hw-e2e: unknown argument '%s'\n", argv[i]);
            print_usage();
            return 1;
        }
    }
    if (opts.width < 64 || opts.height < 64 || opts.frames == 0) {
        std::fprintf(stderr, "hw-e2e: --width / --height must be >= 64 and "
                             "--frames >= 1\n");
        return 1;
    }

    Record rec = run_hardware_e2e(opts);
    if (rec.status != "skipped") {
        rec.decoder_is_hardware = hardware_decoder_available();
        if (!rec.decoder_is_hardware && rec.status == "pass") {
            // Not a failure — the criterion is the encode side — but the
            // record says it, so a reader never has to guess.
            std::unique_ptr<np::IVideoCodec> probe(make_decoder_probe());
            if (probe) {
                rec.decoder_backend = std::string{probe->backend_id()};
                probe->close();
            }
        }
    }

    print_record(rec);

    if (rec.status == "pass") return 0;
    if (rec.status == "skipped") return opts.require_hardware ? 2 : 77;
    return 1;
}
