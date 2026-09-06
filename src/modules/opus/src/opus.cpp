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
 * NimRTC targets 20 ms frames by default (960 samples @ 48 kHz) — the most
 * common choice for WebRTC voice.  Callers that submit other sizes will get
 * an encode failure (we return 0).
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

// Map sample rate → libopus application mode (voice is best for WebRTC).
constexpr int kOpusApplication = OPUS_APPLICATION_VOIP;

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
              kOpusApplication,
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

    // libopus requires a fixed frame size; if the caller submitted something
    // else, we accept 480 or 960 at 48 kHz as the common case and reject
    // anything else (callers should chunk their PCM into frames).
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
// RTP packetisation (RFC 7587)
// -----------------------------------------------------------------------------

std::size_t packetise(const std::uint8_t* /*frames*/,
                     std::size_t /*num_frames*/,
                     std::uint8_t* output,
                     std::size_t output_capacity,
                     std::uint8_t channels,
                     bool /*cbr*/) noexcept {
    if (!output || output_capacity < 1) return 0;
    // RFC 7587 TOC byte for single-frame Opus:
    //   For mono: 0x80 (config=1, mono, no FEC)
    //   For stereo: 0x78 (config=0, stereo, no FEC)
    output[0] = static_cast<std::uint8_t>(
        (channels == 1) ? 0x80 : 0x78);
    return 1;  // stub: only the TOC byte (frame body is written separately by RTP layer)
}

} // namespace nimrtc::opus
