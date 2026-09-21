/**
 * @file src/modules/opus/src/opus.cpp
 * @brief Opus codec wrapper — real libopus encode/decode (RFC 6716).
 *
 * Wraps libopus's OpusEncoder / OpusDecoder behind our nimrtc::opus::{Encoder,
 * Decoder} facade. libopus is vendored at src/third_party/libopus/src and
 * exposed via NimRTCVendored.cmake (target nimrtc_vendor_libopus).
 *
 * ## Frame size
 *
 * libopus requires frames of 2.5, 5, 10, 20, 40, or 60 ms.  At 48 kHz these
 * correspond to 120 / 240 / 480 / 960 / 1920 / 2880 samples per channel.
 * NimRTC accepts all six sizes (see is_valid_frame_size() below); the
 * default config targets 20 ms frames (960 samples @ 48 kHz) — the most
 * common choice for WebRTC voice.  Callers that submit other sizes will
 * get an encode failure (we return 0).
 *
 * ## Application mode (P2#10)
 *
 * The encoder's libopus application mode is configurable via
 * EncoderConfig::application.  NimRTC exposes two values without pulling
 * in <opus.h> at the public-header level:
 *
 *   - OPUS_APPLICATION_VOIP  (1) — default.  Optimised for voice /
 *     interactive speech; best for the WebRTC peer path.
 *   - OPUS_APPLICATION_AUDIO (2) — for music / non-interactive audio;
 *     better for ASR pipelines and agent-gateway music streaming.
 *
 * Any other value falls back to VOIP for forward-compatibility.
 *
 * ## PLC
 *
 * libopus doesn't have an explicit PLC call; PLC is invoked by passing a
 * NULL packet to opus_decode{,_float}, which produces concealment audio
 * based on the decoder's internal history.
 *
 * @note P1 — replaces the PCM passthrough stub.  Real audio path now goes
 *       through libopus, enabling Tier 0 Chrome ↔ NimRTC interop (§4.2).
 */

#include <nimrtc/opus/opus.hpp>

#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <stdexcept>

#include <opus.h>   // libopus vendored at src/third_party/libopus/src

#include <nimrtc/core/log.hpp>

namespace nimrtc::opus {

namespace {

// libopus's frame_size must be one of the allowed durations × Fs.  NimRTC
// targets 20 ms frames — a WebRTC-compatible default.
constexpr std::size_t kSamplesPer20Ms48K = 960;   // 48 kHz × 20 ms
constexpr std::size_t kSamplesPer10Ms48K = 480;   // 48 kHz × 10 ms
constexpr std::size_t kSamplesPerFrame   = kSamplesPer20Ms48K;  // P1 default

// libopus allows 120, 240, 480, 960, 1920, 2880 (i.e. 2.5, 5, 10, 20, 40, 60 ms).

// Resolve EncoderConfig.application → libopus OPUS_APPLICATION_* value.
// libopus defines these as 2048 (VOIP) and 2049 (AUDIO) since libopus 1.1;
// older releases used 1 / 2.  We accept either form so that existing callers
// (which sometimes still use the legacy 1 / 2 mapping) keep working.
inline int resolve_opus_application(int user_choice) noexcept {
    if (user_choice == 2049 || user_choice == 2) return 2049;
    if (user_choice == 2048 || user_choice == 1) return 2048;
    return 2048;  // safe default for any unknown value
}

inline bool is_valid_frame_size(std::size_t samples_per_channel) noexcept {
    // libopus allows 120, 240, 480, 960, 1920, 2880 (i.e. 2.5, 5, 10, 20, 40, 60 ms).
    switch (samples_per_channel) {
        case 120: case 240: case 480: case 960: case 1920: case 2880:
            return true;
        default:
            return false;
    }
}

constexpr auto kLog = core::log::Level::Debug;

// Format-string attribute required by Clang/GCC to suppress
// -Wformat-nonliteral on this variadic helper (the format string
// comes from the caller; the compiler cannot prove it is a literal).
// Forward-declare with the attribute, then define separately so GCC
// (which rejects attributes on inline function *definitions*) is
// happy with the same syntax as Clang.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
inline void opus_debug(const char* fmt, ...) noexcept;

inline void opus_debug(const char* fmt, ...) noexcept {
    if (core::log::Logger::instance().level() <= kLog) {
        char buf[256];
        std::va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        core::log::Logger::instance().debug(buf);
    }
}

} // anonymous namespace

// -----------------------------------------------------------------------------
// Encoder::Impl
// -----------------------------------------------------------------------------

struct Encoder::Impl {
    EncoderConfig         config;
    Encoder::Stats        stats{};
    OpusEncoder*          enc   = nullptr;
    int                   error = OPUS_OK;
    bool                  dtx_active = false;

    explicit Impl(EncoderConfig c)
        : config(std::move(c))
        , enc(opus_encoder_create(
              static_cast<opus_int32>(config.sample_rate_hz),
              static_cast<int>(config.channels),
              resolve_opus_application(config.application),
              &error))
    {
        if (error != OPUS_OK || enc == nullptr) {
            throw std::runtime_error(std::string{"opus_encoder_create failed: "}
                                     + opus_strerror(error));
        }
        // Apply config knobs.
        opus_encoder_ctl(enc, OPUS_SET_BITRATE(
            static_cast<int>(config.bitrate_bps)));
        opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(config.complexity));
        // libopus has no explicit VAD knob — VAD behaviour is governed by
        // DTX (when DTX=1 the encoder runs an internal VAD and produces
        // silence frames).  When DTX=0 we still want VBR for higher
        // quality at the chosen bitrate.
        opus_encoder_ctl(enc, OPUS_SET_VBR(config.dtx_enabled ? 0 : 1));
        opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(config.fec_enabled ? 1 : 0));
        opus_encoder_ctl(enc, OPUS_SET_DTX(config.dtx_enabled ? 1 : 0));
        // Note: OPUS_SET_VAD does not exist; VAD is implicit via DTX.

        opus_debug("opus::Encoder created (rate=%u ch=%u bitrate=%d)",
                   config.sample_rate_hz, config.channels, config.bitrate_bps);
    }

    ~Impl() {
        if (enc) opus_encoder_destroy(enc);
    }

    void reconfigure(const EncoderConfig& c) {
        config = c;
        if (!enc) return;
        opus_encoder_ctl(enc, OPUS_SET_BITRATE(static_cast<int>(c.bitrate_bps)));
        opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(c.complexity));
        opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(c.fec_enabled ? 1 : 0));
        opus_encoder_ctl(enc, OPUS_SET_DTX(c.dtx_enabled ? 1 : 0));
    }
};

Encoder::Encoder(EncoderConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

Encoder::~Encoder() = default;
Encoder::Encoder(Encoder&&) noexcept = default;
Encoder& Encoder::operator=(Encoder&&) noexcept = default;

std::size_t Encoder::encode(const float* pcm,
                             std::size_t num_samples,
                             std::uint8_t* output,
                             std::size_t output_capacity) noexcept {
    if (!pcm || !output) return 0;
    if (num_samples == 0 || output_capacity == 0) return 0;
    if (!impl_ || !impl_->enc) return 0;

    // libopus requires a fixed frame size.  We accept any of the six
    // permitted Opus frame sizes (2.5, 5, 10, 20, 40, 60 ms — i.e.
    // 120, 240, 480, 960, 1920, 2880 samples per channel at 48 kHz) and
    // reject anything else (callers should chunk their PCM into frames
    // before encoding).  See is_valid_frame_size() above for the full list.
    if (!is_valid_frame_size(num_samples)) return 0;

    const opus_int32 frame_size = static_cast<opus_int32>(num_samples
        / (impl_->config.channels == 0 ? 1 : impl_->config.channels));
    const opus_int32 n = opus_encode_float(impl_->enc, pcm, frame_size,
                                            output,
                                            static_cast<opus_int32>(output_capacity));
    if (n < 0) {
        opus_debug("opus::Encoder::encode failed: %s", opus_strerror(n));
        return 0;
    }
    impl_->stats.frames_encoded++;
    impl_->stats.bytes_encoded += static_cast<std::uint64_t>(n);
    return static_cast<std::size_t>(n);
}

std::size_t Encoder::encode_frame(const float* pcm,
                                   std::size_t num_samples,
                                   std::uint8_t* output,
                                   std::size_t output_capacity,
                                   bool force_dtx) noexcept {
    if (force_dtx) {
        impl_->stats.dtx_frames++;
        return 0;
    }
    if (!is_valid_frame_size(num_samples)) return 0;
    return encode(pcm, num_samples, output, output_capacity);
}

std::size_t Encoder::encode_plc(std::uint8_t* output,
                                 std::size_t output_capacity) noexcept {
    // libopus has no PLC encoder — the codec *decodes* concealment on packet
    // loss, never *encodes* it.  The encoder-side PLC is a no-op: we just
    // surface the metric.
    impl_->stats.plc_frames++;
    (void)output;
    (void)output_capacity;
    return 0;
}

void Encoder::reset() noexcept {
    if (impl_ && impl_->enc) {
        // libopus doesn't expose a public reset; re-create by re-applying CTL.
        opus_encoder_ctl(impl_->enc, OPUS_RESET_STATE);
        impl_->stats = Encoder::Stats{};
        impl_->dtx_active = false;
    }
}

bool Encoder::is_dtx() const noexcept {
    int v = 0;
    if (impl_ && impl_->enc) {
        opus_encoder_ctl(impl_->enc, OPUS_GET_DTX(&v));
    }
    return v != 0;
}

Encoder::Stats Encoder::stats() const noexcept {
    return impl_ ? impl_->stats : Encoder::Stats{};
}

// -----------------------------------------------------------------------------
// Decoder::Impl
// -----------------------------------------------------------------------------

struct Decoder::Impl {
    DecoderConfig         config;
    Decoder::Stats        stats{};
    OpusDecoder*          dec   = nullptr;
    int                   error = OPUS_OK;
    opus_int32            last_frame_samples = kSamplesPerFrame;

    explicit Impl(DecoderConfig c)
        : config(std::move(c))
        , dec(opus_decoder_create(
              static_cast<opus_int32>(config.sample_rate_hz),
              static_cast<int>(config.channels),
              &error))
    {
        if (error != OPUS_OK || dec == nullptr) {
            throw std::runtime_error(std::string{"opus_decoder_create failed: "}
                                     + opus_strerror(error));
        }
        opus_decoder_ctl(dec, OPUS_SET_COMPLEXITY(config.complexity));
        opus_debug("opus::Decoder created (rate=%u ch=%u)",
                   config.sample_rate_hz, config.channels);
    }

    ~Impl() {
        if (dec) opus_decoder_destroy(dec);
    }

    void reconfigure(const DecoderConfig& c) {
        config = c;
        if (dec) opus_decoder_ctl(dec, OPUS_SET_COMPLEXITY(c.complexity));
    }
};

Decoder::Decoder(DecoderConfig cfg)
    : impl_(std::make_unique<Impl>(std::move(cfg))) {}

Decoder::~Decoder() = default;
Decoder::Decoder(Decoder&&) noexcept = default;
Decoder& Decoder::operator=(Decoder&&) noexcept = default;

std::size_t Decoder::decode(const std::uint8_t* encoded,
                             std::size_t encoded_len,
                             float* output,
                             std::size_t output_capacity) noexcept {
    if (!output || output_capacity < kSamplesPerFrame) return 0;
    if (!impl_ || !impl_->dec) return 0;

    const opus_int32 max_samples = static_cast<opus_int32>(output_capacity);
    opus_int32 n_bytes = static_cast<opus_int32>(encoded_len);
    const unsigned char* packet = (encoded && n_bytes > 0) ? encoded : nullptr;

    const opus_int32 n = opus_decode_float(impl_->dec, packet, n_bytes,
                                            output, max_samples, /*decode_fec*/ 0);
    if (n < 0) {
        opus_debug("opus::Decoder::decode failed: %s", opus_strerror(n));
        impl_->stats.errors++;
        return 0;
    }
    impl_->stats.frames_decoded++;
    impl_->stats.bytes_decoded += encoded_len;
    impl_->last_frame_samples = n;
    return static_cast<std::size_t>(n);
}

std::size_t Decoder::decode_plc(float* output,
                                 std::size_t output_capacity) noexcept {
    if (!output || output_capacity < kSamplesPerFrame) return 0;
    if (!impl_ || !impl_->dec) return 0;
    // libopus PLC = decode a NULL packet.  The decoder interpolates audio
    // based on its internal history.
    const opus_int32 max_samples = static_cast<opus_int32>(output_capacity);
    const opus_int32 n = opus_decode_float(impl_->dec, nullptr, 0,
                                            output, max_samples, /*decode_fec*/ 0);
    if (n < 0) {
        impl_->stats.errors++;
        return 0;
    }
    impl_->stats.plc_frames++;
    impl_->last_frame_samples = n;
    return static_cast<std::size_t>(n);
}

void Decoder::reset() noexcept {
    if (impl_ && impl_->dec) {
        opus_decoder_ctl(impl_->dec, OPUS_RESET_STATE);
        impl_->stats = Decoder::Stats{};
    }
}

Decoder::Stats Decoder::stats() const noexcept {
    return impl_ ? impl_->stats : Decoder::Stats{};
}

// -----------------------------------------------------------------------------
// RFC 7587 RTP packetisation helpers
// -----------------------------------------------------------------------------
//
// The TOC byte's 5-bit config field follows RFC 6716 §3.1 Table 2:
//
//   0..3   SILK-only NB        10/20/40/60 ms
//   4..7   SILK-only MB        10/20/40/60 ms
//   8..11  SILK-only WB        10/20/40/60 ms
//   12..13 Hybrid    SWB       10/20 ms
//   14..15 Hybrid    FB        10/20 ms
//   16..19 CELT-only NB        2.5/5/10/20 ms
//   20..23 CELT-only WB        2.5/5/10/20 ms
//   24..27 CELT-only SWB       2.5/5/10/20 ms
//   28..31 CELT-only FB        2.5/5/10/20 ms
//
// Within each range config N is the smallest duration and config N+3 (or
// N+1 for Hybrid) is the largest.  Frame-size ↔ config offset within a
// range is deterministic given the duration.
//
// For RFC 7587 the only knob a packetiser needs is the per-frame duration
// in ms. We therefore resolve config purely by frame_size_ms using the
// lowest config in the broadest bandwidth range (CELT-only FB covers all
// supported durations: 2.5, 5, 10, 20 ms; the SILK-only ranges cover
// 10/20/40/60 ms). A real bandwidth hint could be layered on top later.

namespace {

// 120 ms ceiling: at 2.5 ms per frame we can pack at most 48 frames.
// 120 / 2.5 = 48 (RFC 7587 §4.2, RFC 6716 §3.2.5 [R5]).
inline constexpr std::size_t kMaxFramesPerPacket = 48;

inline constexpr std::uint8_t kCode0_SingleFrame       = 0;
inline constexpr std::uint8_t kCode1_TwoEqualFrames    = 1;
inline constexpr std::uint8_t kCode2_TwoUnequalFrames  = 2;
inline constexpr std::uint8_t kCode3_NFrames           = 3;

// Encode an Opus frame length in 1 or 2 bytes per RFC 6716 §3.2.1.
// Returns number of bytes written (1 or 2), or 0 if the length cannot
// be encoded (> 1275 — RFC 6716 [R2]).
//
//   value 0           → DTX / lost
//   value 1..251      → 1 byte, value as-is
//   value 252..1275   → 2 bytes (b0, b1) where b0 ∈ [252..255] and
//                       total = b0 + 4 * b1
//
// For the 2-byte form, we pick b0 in [252..255] (low) and b1 in [0..255]
// such that len = b0 + 4 * b1.  Solving for b0 = 252..255 gives:
//   b1 = (len - 252) / 4   (floor)
//   b0 = (len - 252) & 3   (low 2 bits)
//   first_byte = 252 + b0
inline std::size_t encode_frame_length(std::uint16_t len,
                                       std::uint8_t* out) noexcept {
    if (len > 1275) return 0;  // RFC 6716 [R2]
    if (len <= 251) {
        out[0] = static_cast<std::uint8_t>(len);
        return 1;
    }
    // 252..1275 → 2-byte form.
    const std::uint16_t offset = static_cast<std::uint16_t>(len - 252);
    const std::uint16_t b1     = static_cast<std::uint16_t>(offset >> 2);  // /=4
    const std::uint8_t  b0     = static_cast<std::uint8_t>(offset & 0x03);
    // b1 is at most (1275-252)/4 = 255. b0 is in [0..3].
    out[0] = static_cast<std::uint8_t>(252 + b0);
    out[1] = static_cast<std::uint8_t>(b1);
    return 2;
}

// Decode an Opus frame length from a 1- or 2-byte sequence. `pos` advances
// past the consumed bytes. Returns 0xFFFF on malformed input.
inline std::uint16_t decode_frame_length(const std::uint8_t* in,
                                          std::size_t len,
                                          std::size_t* pos) noexcept {
    if (*pos >= len) return 0xFFFF;
    const std::uint8_t b0 = in[*pos];
    if (b0 <= 251) {
        (*pos)++;
        return b0;
    }
    // b0 in 252..255 → 2-byte length; total = b0 + b1*4 per RFC 6716.
    if (*pos + 1 >= len) return 0xFFFF;
    const std::uint8_t b1 = in[*pos + 1];
    const std::uint32_t total = static_cast<std::uint32_t>(b0)
                              + static_cast<std::uint32_t>(b1) * 4u;
    if (total > 1275) return 0xFFFF;  // RFC 6716 [R2]
    *pos += 2;
    return static_cast<std::uint16_t>(total);
}

} // anonymous namespace

std::optional<std::uint8_t> toc_config_for_frame_size_ms(
    std::uint16_t frame_size_ms) noexcept {
    // RFC 6716 §3.1 Table 2 — see module-level comment for the layout.
    // We always emit the canonical config for the chosen duration:
    //   2.5 / 5 / 10 / 20 ms → FB CELT (configs 28..31)
    //   40 / 60 ms            → WB SILK last two slots (10, 11)
    // Round-trip via depacketise() preserves frame_size_ms exactly.
    using R = std::optional<std::uint8_t>;
    switch (frame_size_ms) {
        case 3:   return R{std::in_place, static_cast<std::uint8_t>(28)};   // 2.5 ms (RFC 7587 §6.1: rounded up)
        case 5:   return R{std::in_place, static_cast<std::uint8_t>(29)};
        case 10:  return R{std::in_place, static_cast<std::uint8_t>(30)};
        case 20:  return R{std::in_place, static_cast<std::uint8_t>(31)};
        case 40:  return R{std::in_place, static_cast<std::uint8_t>(10)};   // SILK WB, 40 ms
        case 60:  return R{std::in_place, static_cast<std::uint8_t>(11)};   // SILK WB, 60 ms
        default:  return std::nullopt;
    }
}

namespace {

// Inverse mapping used by depacketise() — given a TOC config field,
// return the corresponding frame size in ms (the canonical duration
// for the chosen config slot).
inline std::uint16_t frame_size_ms_for_config(std::uint8_t config) noexcept {
    config &= 0x1F;
    // CELT ranges (16..31) — sizes: 2.5, 5, 10, 20 ms.
    // Each range covers all four durations; we map by index within range.
    if (config >= 28) { // FB CELT
        switch (config) {
            case 28: return 3;    // 2.5 ms
            case 29: return 5;
            case 30: return 10;
            case 31: return 20;
        }
    }
    if (config >= 24) { // SWB CELT
        switch (config - 24) {
            case 0: return 3;
            case 1: return 5;
            case 2: return 10;
            case 3: return 20;
        }
    }
    if (config >= 20) { // WB CELT
        switch (config - 20) {
            case 0: return 3;
            case 1: return 5;
            case 2: return 10;
            case 3: return 20;
        }
    }
    if (config >= 16) { // NB CELT
        switch (config - 16) {
            case 0: return 3;
            case 1: return 5;
            case 2: return 10;
            case 3: return 20;
        }
    }
    // Hybrid (12..15) — SWB/FB, 10 or 20 ms only.
    if (config >= 12) return (config == 12 || config == 13) ? 10 : 20;
    // SILK-only (0..11) — 10, 20, 40, 60 ms in each range; index in
    // range maps to 10, 20, 40, 60 ms respectively.
    static constexpr std::uint16_t silk_ms[4] = { 10, 20, 40, 60 };
    return silk_ms[config & 0x03];
}

} // anonymous namespace

std::size_t packetise(const CodecFrame* frames,
                      std::size_t num_frames,
                      std::uint32_t /*sample_rate_hz*/,
                      bool cbr,
                      std::uint8_t* output,
                      std::size_t output_capacity) noexcept {
    if (!output || output_capacity < 1) return 0;
    if (!frames) return 0;
    if (num_frames == 0 || num_frames > kMaxFramesPerPacket) return 0;

    // Sanity-check inputs.
    for (std::size_t i = 0; i < num_frames; ++i) {
        if (!frames[i].data && frames[i].size != 0) return 0;
        if (frames[i].size > kMaxOpusFrameBytes) return 0;
    }

    // Resolve per-frame TOC config + s field. All frames must agree on
    // config (RFC 7587 §4.2 — Opus frames in a single packet share mode,
    // bandwidth, frame size, and channel count).
    std::uint8_t s_field = frames[0].is_stereo & 0x03;
    std::uint8_t resolved_config = 0;
    for (std::size_t i = 0; i < num_frames; ++i) {
        const auto cfg = toc_config_for_frame_size_ms(frames[i].frame_size_ms);
        if (!cfg) return 0;
        const std::uint8_t c = static_cast<std::uint8_t>(*cfg & 0x1F);
        if (i == 0) resolved_config = c;
        // Reject mixed configs / s fields across the packet (RFC 7587 §4.2).
        if (i > 0 && c != resolved_config) return 0;
        if ((frames[i].is_stereo & 0x03) != s_field) return 0;
    }

    // ------------------------------------------------------------------
    // Code 0: one frame in the packet (RFC 6716 §3.2.2, Figure 2).
    //   Layout: [TOC][N-1 bytes of frame data]
    // ------------------------------------------------------------------
    if (num_frames == 1) {
        const std::size_t need = 1 + frames[0].size;
        if (output_capacity < need) return 0;
        output[0] = make_toc_byte(resolved_config, s_field, kCode0_SingleFrame);
        if (frames[0].size > 0) {
            std::memcpy(output + 1, frames[0].data, frames[0].size);
        }
        return need;
    }

    // ------------------------------------------------------------------
    // Code 1: two frames, equal compressed size (RFC 6716 §3.2.3, Figure 3).
    //   Layout: [TOC][frame1 (N-1)/2 bytes][frame2 (N-1)/2 bytes]
    //   [R3]: N-1 must be even.
    // ------------------------------------------------------------------
    if (num_frames == 2 && frames[0].size == frames[1].size) {
        const std::size_t per = frames[0].size;
        const std::size_t need = 1 + 2 * per;
        // [R3]: N-1 (= 2*per) is always even — auto-satisfied.
        if (output_capacity < need) return 0;
        output[0] = make_toc_byte(resolved_config, s_field, kCode1_TwoEqualFrames);
        if (per > 0) {
            std::memcpy(output + 1,          frames[0].data, per);
            std::memcpy(output + 1 + per,    frames[1].data, per);
        }
        return need;
    }

    // ------------------------------------------------------------------
    // Code 2: two frames with different sizes (RFC 6716 §3.2.4, Figure 4).
    //   Layout: [TOC][N1 length 1-2 bytes][frame1 N1 bytes][frame2]
    // ------------------------------------------------------------------
    if (num_frames == 2) {
        std::uint8_t lenbuf[2];
        const std::size_t len_bytes = encode_frame_length(
            static_cast<std::uint16_t>(frames[0].size), lenbuf);
        if (len_bytes == 0) return 0;
        const std::size_t need = 1 + len_bytes + frames[0].size + frames[1].size;
        if (output_capacity < need) return 0;
        output[0] = make_toc_byte(resolved_config, s_field, kCode2_TwoUnequalFrames);
        std::memcpy(output + 1, lenbuf, len_bytes);
        std::size_t off = 1 + len_bytes;
        if (frames[0].size > 0) {
            std::memcpy(output + off, frames[0].data, frames[0].size);
            off += frames[0].size;
        }
        if (frames[1].size > 0) {
            std::memcpy(output + off, frames[1].data, frames[1].size);
            off += frames[1].size;
        }
        return off;
    }

    // ------------------------------------------------------------------
    // Code 3: N >= 3 frames, signalled count (RFC 6716 §3.2.5, Figures 5-7).
    //   Layout:
    //     [TOC][frame_count byte (v|p|M)][optional padding length bytes]
    //     [M-1 length entries (VBR only)][frame 1]...[frame M][padding]
    //
    //   M (in bits 7..2) must be non-zero, and the total audio duration
    //   MUST NOT exceed 120 ms (RFC 6716 [R5]).
    // ------------------------------------------------------------------
    const std::size_t m = num_frames;
    // Compute total duration in ms (sum of per-frame frame_size_ms).
    // We reject packets that exceed 120 ms — RFC 6716 [R5].
    std::uint32_t total_ms = 0;
    for (std::size_t i = 0; i < m; ++i) {
        total_ms += frames[i].frame_size_ms;
        if (total_ms > 120) return 0;
    }
    if (m > kMaxFramesPerPacket) return 0;  // sanity re-check

    // Build the frame-count byte: bit 0 = v (VBR=1), bit 1 = p (padding), bits 7..2 = M.
    // Opus padding is not used here (always 0).
    std::uint8_t v_bit = cbr ? 0 : 1;
    std::uint8_t p_bit = 0;
    std::uint8_t m_field = static_cast<std::uint8_t>(m & 0x3F);
    const std::uint8_t fc_byte = static_cast<std::uint8_t>(
        (v_bit << 0) | (p_bit << 1) | (m_field << 2));

    // For CBR (RFC 6716 §3.2.5), all M frames MUST share the same compressed
    // size — otherwise depacketise() cannot recover the per-frame boundaries
    // (the R/M divisibility rule). Reject mixed sizes early.
    if (cbr) {
        const std::size_t ref = frames[0].size;
        for (std::size_t i = 1; i < m; ++i) {
            if (frames[i].size != ref) return 0;
        }
    }

    // Length table size: M-1 entries (VBR) or 0 (CBR). Each entry is 1 or 2 bytes.
    std::size_t len_table_bytes = 0;
    if (!cbr) {
        std::uint8_t scratch[2];
        for (std::size_t i = 0; i + 1 < m; ++i) {
            const std::size_t n = encode_frame_length(
                static_cast<std::uint16_t>(frames[i].size), scratch);
            if (n == 0) return 0;
            len_table_bytes += n;
        }
    }

    const std::size_t need = 1 + 1 + len_table_bytes;
    std::size_t total_bytes = need;
    for (std::size_t i = 0; i < m; ++i) total_bytes += frames[i].size;
    if (output_capacity < total_bytes) return 0;

    output[0] = make_toc_byte(resolved_config, s_field, kCode3_NFrames);
    output[1] = fc_byte;
    std::size_t off = 2;

    if (!cbr) {
        std::uint8_t scratch[2];
        for (std::size_t i = 0; i + 1 < m; ++i) {
            const std::size_t n = encode_frame_length(
                static_cast<std::uint16_t>(frames[i].size), scratch);
            // encode_frame_length already validated above, n > 0 here.
            std::memcpy(output + off, scratch, n);
            off += n;
        }
    }

    for (std::size_t i = 0; i < m; ++i) {
        if (frames[i].size > 0) {
            std::memcpy(output + off, frames[i].data, frames[i].size);
            off += frames[i].size;
        }
    }
    return off;
}

std::size_t depacketise(const std::uint8_t* payload,
                        std::size_t payload_len,
                        PacketView* views,
                        std::size_t views_capacity,
                        std::size_t* num_views) noexcept {
    if (num_views) *num_views = 0;
    if (!payload || payload_len < 1) return 0;
    if (!views || views_capacity == 0) return 0;

    const std::uint8_t toc = payload[0];
    // RFC 6716 §3.1 Figure 1:
    //   bits 7..3 = config (5 bits)
    //   bit  2   = s      (1 bit)
    //   bits 1..0 = c      (2 bits — the "code" 0..3)
    const std::uint8_t config = static_cast<std::uint8_t>((toc >> 3) & 0x1F);
    const std::uint8_t s_field = static_cast<std::uint8_t>((toc >> 2) & 0x01);
    const std::uint8_t code = static_cast<std::uint8_t>(toc & 0x03);
    const std::uint16_t frame_size_ms = frame_size_ms_for_config(config);

    // ------------------------------------------------------------------
    // Code 0: single frame (RFC 6716 §3.2.2, Figure 2).
    //   Layout: [TOC][N-1 bytes]. No length encoding — the whole rest is
    //   the frame.
    // ------------------------------------------------------------------
    if (code == kCode0_SingleFrame) {
        if (views_capacity < 1) return 0;
        views[0].data          = payload + 1;
        views[0].size          = payload_len - 1;
        views[0].config        = config;
        views[0].s             = s_field;
        views[0].code          = code;
        views[0].frame_size_ms = frame_size_ms;
        *num_views = 1;
        return 1;
    }

    // ------------------------------------------------------------------
    // Code 1: two frames, equal compressed size (RFC 6716 §3.2.3).
    //   Layout: [TOC][frame1 (N-1)/2 bytes][frame2 (N-1)/2 bytes].
    //   [R3]: N-1 must be even.
    // ------------------------------------------------------------------
    if (code == kCode1_TwoEqualFrames) {
        const std::size_t after_toc = payload_len - 1;
        if ((after_toc & 0x1u) != 0) return 0;  // [R3]
        const std::size_t per = after_toc / 2;
        if (views_capacity < 2) return 0;
        views[0].data          = payload + 1;
        views[0].size          = per;
        views[0].config        = config;
        views[0].s             = s_field;
        views[0].code          = code;
        views[0].frame_size_ms = frame_size_ms;
        views[1].data          = payload + 1 + per;
        views[1].size          = per;
        views[1].config        = config;
        views[1].s             = s_field;
        views[1].code          = code;
        views[1].frame_size_ms = frame_size_ms;
        *num_views = 2;
        return 2;
    }

    // ------------------------------------------------------------------
    // Code 2: two frames with different compressed sizes (RFC 6716 §3.2.4).
    //   Layout: [TOC][N1 length 1-2 bytes][frame1 N1 bytes][frame2].
    //   [R4]: N1 must fit in the remaining payload.
    // ------------------------------------------------------------------
    if (code == kCode2_TwoUnequalFrames) {
        if (payload_len < 2) return 0;          // need at least 1 len byte
        if (views_capacity < 2) return 0;
        std::size_t pos = 1;
        const std::uint16_t n1 = decode_frame_length(payload, payload_len, &pos);
        if (n1 == 0xFFFF) return 0;
        if (n1 > payload_len - pos) return 0;   // [R4]
        views[0].data          = payload + pos;
        views[0].size          = n1;
        views[0].config        = config;
        views[0].s             = s_field;
        views[0].code          = code;
        views[0].frame_size_ms = frame_size_ms;
        const std::size_t off2 = pos + n1;
        if (off2 > payload_len) return 0;
        views[1].data          = payload + off2;
        views[1].size          = payload_len - off2;
        views[1].config        = config;
        views[1].s             = s_field;
        views[1].code          = code;
        views[1].frame_size_ms = frame_size_ms;
        *num_views = 2;
        return 2;
    }

    // ------------------------------------------------------------------
    // Code 3: M >= 1 frames, signalled count (RFC 6716 §3.2.5).
    //   Layout: [TOC][fc byte (v|p|M)][optional padding length bytes]
    //           [M-1 length entries (VBR only)][frame 1]...[frame M][pad]
    //   CBR uses fixed per-frame length R/M (no length table).
    //   VBR uses M-1 length entries; last frame consumes remainder.
    // ------------------------------------------------------------------
    if (code == kCode3_NFrames) {
        if (payload_len < 2) return 0;  // RFC 6716 [R6,R7] — at least 2 bytes
        const std::uint8_t fc = payload[1];
        const std::uint8_t v_bit = static_cast<std::uint8_t>(fc & 0x01);
        const std::uint8_t p_bit = static_cast<std::uint8_t>((fc >> 1) & 0x01);
        const std::uint8_t m_field = static_cast<std::uint8_t>((fc >> 2) & 0x3F);
        if (m_field == 0) return 0;        // RFC 6716 [R5]
        const std::size_t m = m_field;

        std::size_t pos = 2;

        // Optional padding-length bytes. Skip them; we don't strip padding.
        // p_bit == 1 ⇒ at least one byte follows indicating padding.
        // p_bit == 0 ⇒ no padding-length byte.
        // For 1-byte form: value ∈ [0..254] means that many bytes of padding
        // (in addition to the byte itself). Value 255 ⇒ a second byte follows.
        // We skip the entire padding block.
        std::size_t padding_header_bytes = 0;
        std::size_t padding_size = 0;
        if (p_bit) {
            if (pos >= payload_len) return 0;
            const std::uint8_t b0 = payload[pos];
            if (b0 == 255) {
                if (pos + 1 >= payload_len) return 0;
                padding_size = 254u + static_cast<std::size_t>(payload[pos + 1]);
                padding_header_bytes = 2;
            } else {
                padding_size = static_cast<std::size_t>(b0);
                padding_header_bytes = 1;
            }
        }
        pos += padding_header_bytes;

        // RFC 6716 [R6,R7]: padding must fit in the remaining payload.
        if (pos + padding_size > payload_len) return 0;

        const std::size_t data_end = payload_len - padding_size;

        if (v_bit) {
            // VBR: M-1 length entries, last frame consumes remainder.
            // Per RFC 6716 §3.2.5 Figure 7, the lengths table precedes all
            // frame data; once the table is consumed we slice frame data
            // sequentially.
            if (views_capacity < m) return 0;
            std::size_t lengths[kMaxFramesPerPacket];
            for (std::size_t i = 0; i + 1 < m; ++i) {
                const std::uint16_t n = decode_frame_length(payload, data_end, &pos);
                if (n == 0xFFFF) return 0;
                lengths[i] = n;
            }
            const std::size_t data_start = pos;
            std::size_t total = 0;
            for (std::size_t i = 0; i + 1 < m; ++i) total += lengths[i];
            if (total > data_end - data_start) return 0;  // RFC 6716 [R7]
            const std::size_t last = data_end - data_start - total;
            std::size_t cursor = data_start;
            for (std::size_t i = 0; i + 1 < m; ++i) {
                views[i].data          = payload + cursor;
                views[i].size          = lengths[i];
                views[i].config        = config;
                views[i].s             = s_field;
                views[i].code          = code;
                views[i].frame_size_ms = frame_size_ms;
                cursor += lengths[i];
            }
            views[m - 1].data          = payload + cursor;
            views[m - 1].size          = last;
            views[m - 1].config        = config;
            views[m - 1].s             = s_field;
            views[m - 1].code          = code;
            views[m - 1].frame_size_ms = frame_size_ms;
        } else {
            // CBR: each frame has equal size R/M, where R = data_end - pos.
            const std::size_t r = data_end - pos;
            if (r % m != 0) return 0;  // RFC 6716 [R6]
            const std::size_t per = r / m;
            if (views_capacity < m) return 0;
            for (std::size_t i = 0; i < m; ++i) {
                views[i].data          = payload + pos + i * per;
                views[i].size          = per;
                views[i].config        = config;
                views[i].s             = s_field;
                views[i].code          = code;
                views[i].frame_size_ms = frame_size_ms;
            }
        }

        *num_views = m;
        return m;
    }

    return 0;  // unreachable
}

} // namespace nimrtc::opus
