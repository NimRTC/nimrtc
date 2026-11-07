/**
 * @file src/modules/opus/src/audio_decoder_plugin.cpp
 * @brief The Opus implementation of plugins::IAudioDecoder (audio pipeline WP-2).
 *
 * Turns one inbound RTP payload into PCM frames:
 *
 *   1. `opus::depacketise()` splits the RFC 7587 payload into its Opus
 *      frames (RFC 6716 §3.2 codes 0..3);
 *   2. every frame is decoded with the module's concrete `opus::Decoder`;
 *   3. a gap — either signalled by the caller through `decode_plc()` or
 *      detected from a jump in the RTP timestamp — makes the next packet
 *      be decoded in FEC mode first, which reconstructs the **lost** frame
 *      from the redundancy the peer embedded in that packet.  The
 *      reconstructed frame is reported with `fec_recovered == true`.
 *
 * The plugin is registered under the id "opus" in the registry's audio
 * decoder category, next to the codec plugin that uses the same id in the
 * codec category.
 */

#include "audio_decoder_plugin.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/plugin_id.hpp>
#include <nimrtc/core/registry.hpp>

namespace nimrtc::opus {

namespace {

/** Maximum number of Opus frames one RFC 7587 payload may carry
 *  (RFC 6716 §3.2.5 [R5]: 120 ms at the shortest 2.5 ms frame). */
constexpr std::size_t kMaxFramesPerPacket = 48;

/** Longest Opus frame libopus can produce: 60 ms. */
constexpr std::uint32_t kMaxFrameMs = 60;

} // anonymous namespace

// ---------------------------------------------------------------------------
// AudioDecoderPlugin::Impl
// ---------------------------------------------------------------------------

struct AudioDecoderPlugin::Impl {
    plugins::AudioDecodeFormat fmt{};
    DecoderConfig               dec_cfg{};
    Decoder                     decoder;

    plugins::IAudioDecoder::Stats stats{};
    /** PCM of the frames produced by the current call.  Each frame owns its
     *  own buffer, so the pointers stay valid until the next call (the
     *  seam's contract). */
    std::vector<std::vector<float>> frame_pcm;

    /** Scratch buffer for one decoded frame. */
    std::vector<float> scratch;

    /** Scratch buffer for the Opus packet handed to libopus.
     *
     *  libopus decodes *complete* Opus packets — a TOC byte followed by
     *  the frame's body (RFC 6716 §3.1).  The depacketiser strips the
     *  TOC, so the plugin puts it back before it calls the decoder; the
     *  buffer holds the rebuilt packet. */
    std::vector<std::uint8_t> packet_scratch;

    /** RTP timestamp the next frame is expected to carry. */
    std::uint32_t next_ts = 0;
    /** Whether next_ts is meaningful (a packet was decoded before). */
    bool have_ts = false;
    /** A concealment was produced → the next packet may carry the FEC. */
    bool pending_fec = false;
    /** The plugin is open (the decoder exists). */
    bool open = true;

    explicit Impl(plugins::AudioDecodeFormat f)
        : fmt(f)
        , dec_cfg(make_decoder_config(f))
        , decoder(dec_cfg)
    {
        scratch.assign(static_cast<std::size_t>(f.sample_rate_hz) * kMaxFrameMs
                           / 1000 * (f.channels ? f.channels : 1) + 64,
                       0.0f);
    }

    static DecoderConfig make_decoder_config(const plugins::AudioDecodeFormat& f) {
        DecoderConfig c;
        c.sample_rate_hz    = f.sample_rate_hz ? f.sample_rate_hz : kDefaultSampleRate;
        c.channels          = (f.channels == 2) ? 2 : 1;
        c.complexity       = 10;
        c.packet_loss_perc  = f.packet_loss_perc < 0 ? 0
                                 : (f.packet_loss_perc > 100 ? 100 : f.packet_loss_perc);
        c.use_inband_fec    = f.use_inband_fec;
        return c;
    }

    /** Append one frame to the output and describe it. */
    plugins::AudioDecodedFrame* emit(std::vector<plugins::AudioDecodedFrame>& out,
                                      std::size_t max_frames,
                                      const float* pcm,
                                      std::size_t samples,
                                      std::uint32_t ts,
                                      std::uint16_t duration_ms,
                                      bool concealed,
                                      bool fec_recovered) {
        if (out.size() >= max_frames) return nullptr;
        const std::size_t ch = fmt.channels ? fmt.channels : 1;
        frame_pcm.emplace_back(pcm, pcm + samples * ch);
        out.emplace_back();
        plugins::AudioDecodedFrame& f = out.back();
        f.pcm                = frame_pcm.back().data();
        f.samples_per_channel = samples;
        f.sample_rate_hz     = fmt.sample_rate_hz;
        f.channels           = fmt.channels;
        f.rtp_timestamp      = ts;
        f.duration_ms        = duration_ms;
        f.concealed          = concealed;
        f.fec_recovered      = fec_recovered;
        return &f;
    }
};

// ---------------------------------------------------------------------------
// AudioDecoderPlugin
// ---------------------------------------------------------------------------

AudioDecoderPlugin::AudioDecoderPlugin()
    : impl_(std::make_unique<Impl>(plugins::AudioDecodeFormat{})) {}

AudioDecoderPlugin::~AudioDecoderPlugin() = default;

const char* AudioDecoderPlugin::name() const noexcept {
    return "nimrtc::opus::AudioDecoderPlugin (Opus behind plugins::IAudioDecoder)";
}

plugins::Status AudioDecoderPlugin::open() noexcept {
    impl_->open = true;
    return plugins::kOk;
}

void AudioDecoderPlugin::close() noexcept {
    impl_->open = false;
    impl_->frame_pcm.clear();
    impl_->have_ts = false;
    impl_->pending_fec = false;
}

plugins::AudioDecodeFormat AudioDecoderPlugin::format() const noexcept {
    return impl_->fmt;
}

plugins::Status AudioDecoderPlugin::update_format(plugins::AudioDecodeFormat f) noexcept {
    if (f.channels != 1 && f.channels != 2) return plugins::kErrInvalidParam;
    if (f.sample_rate_hz == 0 || f.sample_rate_hz > 48000) {
        return plugins::kErrInvalidParam;
    }

    DecoderConfig cfg = Impl::make_decoder_config(f);
    if (!impl_->decoder.reconfigure(cfg)) return plugins::kErrInternal;

    impl_->fmt = f;
    impl_->scratch.assign(static_cast<std::size_t>(f.sample_rate_hz) * kMaxFrameMs
                              / 1000 * (f.channels ? f.channels : 1) + 64,
                          0.0f);
    return plugins::kOk;
}

std::size_t AudioDecoderPlugin::decode_packet(const std::uint8_t* payload,
                                              std::size_t  payload_len,
                                              std::uint32_t rtp_timestamp,
                                              plugins::AudioDecodedFrame* out,
                                              std::size_t  max_frames) noexcept {
    if (!impl_->open || !payload || payload_len == 0 || !out || max_frames == 0) {
        return 0;
    }

    PacketView views[kMaxFramesPerPacket];
    std::size_t num_views = 0;
    if (depacketise(payload, payload_len, views, kMaxFramesPerPacket, &num_views) == 0
        || num_views == 0) {
        impl_->stats.decode_errors++;
        NIMRTC_LOG_DEBUG("opus::AudioDecoderPlugin: malformed RTP payload ("
                         << payload_len << " bytes)");
        return 0;
    }

    impl_->frame_pcm.clear();
    std::vector<plugins::AudioDecodedFrame> frames;
    frames.reserve(num_views + 1);

    const std::uint32_t rate = impl_->fmt.sample_rate_hz ? impl_->fmt.sample_rate_hz
                                                          : kDefaultSampleRate;
    const std::size_t channels = impl_->fmt.channels ? impl_->fmt.channels : 1;

    // A gap is either signalled by the caller (a concealment was produced)
    // or visible in the RTP timestamp (the packet that should have carried
    // the next frame never arrived).
    bool gap = impl_->pending_fec;
    if (impl_->have_ts && rtp_timestamp > impl_->next_ts) gap = true;
    if (gap) {
        // Only the anomalies are logged: a per-packet trace would fire
        // fifty times a second and drown the log.
        NIMRTC_LOG_DEBUG("opus::AudioDecoderPlugin: gap before ts=" << rtp_timestamp
                         << " (next_ts=" << impl_->next_ts
                         << ", inband_fec=" << (impl_->fmt.use_inband_fec ? 1 : 0) << ")");
    }

    std::uint32_t ts = rtp_timestamp;
    for (std::size_t i = 0; i < num_views; ++i) {
        const PacketView& v = views[i];

        // ---- Rebuild the complete Opus packet -------------------------
        //
        // `depacketise()` strips the TOC byte, because the TOC describes
        // the *packet*, not the frame.  libopus, however, decodes
        // complete packets: it reads the TOC to learn the mode, the
        // bandwidth and the duration of the frame it is about to decode.
        // Handing it a body without its TOC is what made every inbound
        // packet fail with "corrupted stream" (defect F-1): the decoder
        // read the first body byte as a TOC and picked a configuration
        // the frame was never coded for.
        //
        // The rebuilt packet is a code-0 packet — one frame, the frame's
        // own config and stereo bit — which is exactly what the frame
        // was coded as.
        impl_->packet_scratch.resize(v.size + 1);
        impl_->packet_scratch[0] = make_toc_byte(v.config, v.s, /*code=*/0);
        if (v.size > 0) {
            std::memcpy(impl_->packet_scratch.data() + 1, v.data, v.size);
        }
        const std::uint8_t* packet = impl_->packet_scratch.data();
        const std::size_t packet_len = impl_->packet_scratch.size();

        // The first frame of the packet may reconstruct the frame that was
        // lost: the peer embeds a low-bitrate copy of the previous frame in
        // this packet (libopus's decode_fec = 1).
        if (i == 0 && gap && impl_->fmt.use_inband_fec) {
            const std::size_t n = impl_->decoder.decode_with_fec(
                packet, packet_len, impl_->scratch.data(), impl_->scratch.size() / channels);
            NIMRTC_LOG_DEBUG("opus::AudioDecoderPlugin: in-band FEC attempt on a "
                             << packet_len << " byte packet returned " << n);
            if (n > 0) {
                const std::uint32_t n32 = static_cast<std::uint32_t>(n);
                const std::uint32_t recovered_ts = (ts >= n32) ? (ts - n32) : 0u;
                const auto duration = static_cast<std::uint16_t>((n32 * 1000u) / rate);
                if (impl_->emit(frames, max_frames, impl_->scratch.data(), n,
                                recovered_ts, duration, false, true)) {
                    impl_->stats.fec_recovered_frames++;
                }
            }
            gap = false;
            impl_->pending_fec = false;
        }

        const std::size_t n = impl_->decoder.decode(
            packet, packet_len, impl_->scratch.data(), impl_->scratch.size() / channels);
        if (n == 0) {
            impl_->stats.decode_errors++;
            break;
        }
        const auto duration = static_cast<std::uint16_t>(
            (static_cast<std::uint32_t>(n) * 1000u) / rate);
        if (!impl_->emit(frames, max_frames, impl_->scratch.data(), n,
                         ts, duration, false, false)) {
            break;  // the caller's array is full
        }
        ts += static_cast<std::uint32_t>(n);
        impl_->stats.frames_decoded++;
    }

    // The bytes counted are the RTP payload bytes the caller handed over —
    // the seam is payload-level, so the TOC bytes of the RFC 7587 framing
    // count as decoded bytes too.
    impl_->stats.bytes_decoded += payload_len;

    // Copy the frames out — the seam's contract is that the PCM stays
    // valid until the next call, so the buffers live in the plugin.
    const std::size_t written = std::min(frames.size(), max_frames);
    for (std::size_t i = 0; i < written; ++i) {
        out[i] = frames[i];
    }

    impl_->next_ts = ts;
    impl_->have_ts = true;
    impl_->pending_fec = false;
    return written;
}

std::size_t AudioDecoderPlugin::decode_plc(std::uint32_t rtp_timestamp,
                                           std::uint16_t duration_ms,
                                           plugins::AudioDecodedFrame* out,
                                           std::size_t  max_frames) noexcept {
    if (!impl_->open || !out || max_frames == 0 || duration_ms == 0) return 0;

    const std::uint32_t rate = impl_->fmt.sample_rate_hz ? impl_->fmt.sample_rate_hz
                                                          : kDefaultSampleRate;
    const std::size_t channels = impl_->fmt.channels ? impl_->fmt.channels : 1;

    // libopus generates concealment in whole frames, so the requested
    // duration is rounded down to a multiple of 2.5 ms (the shortest Opus
    // frame).
    const std::uint32_t step = rate / 400;  // 2.5 ms
    std::uint32_t samples = (static_cast<std::uint32_t>(duration_ms) * rate) / 1000u;
    if (step > 0) samples = (samples / step) * step;
    if (samples == 0) return 0;

    impl_->frame_pcm.clear();
    std::vector<plugins::AudioDecodedFrame> frames;

    std::vector<float> pcm(static_cast<std::size_t>(samples) * channels, 0.0f);
    const std::size_t n = impl_->decoder.decode_plc(pcm.data(), samples);
    if (n == 0) {
        impl_->stats.decode_errors++;
        return 0;
    }

    const auto produced_ms = static_cast<std::uint16_t>(
        (static_cast<std::uint32_t>(n) * 1000u) / rate);
    if (!impl_->emit(frames, max_frames, pcm.data(), n, rtp_timestamp,
                     produced_ms, true, false)) {
        return 0;
    }
    impl_->stats.plc_frames++;

    // The next packet may carry the redundancy of the frame that was lost.
    impl_->pending_fec = true;
    impl_->next_ts = rtp_timestamp + static_cast<std::uint32_t>(n);
    impl_->have_ts = true;

    out[0] = frames[0];
    return 1;
}

void AudioDecoderPlugin::reset() noexcept {
    impl_->decoder.reset();
    impl_->frame_pcm.clear();
    impl_->next_ts = 0;
    impl_->have_ts = false;
    impl_->pending_fec = false;
    // The counters are part of the state that reset() drops -- the
    // concrete decoder clears its own counters too, so the seam counters
    // follow the same rule.
    impl_->stats = plugins::IAudioDecoder::Stats{};
}

AudioDecoderPlugin::Stats AudioDecoderPlugin::stats() const noexcept {
    return impl_->stats;
}

// ---------------------------------------------------------------------------
// AudioDecoderPluginFactory
// ---------------------------------------------------------------------------

std::string_view AudioDecoderPluginFactory::id() const noexcept {
    return NIMRTC_PLUGIN_ID("opus");
}

std::string_view AudioDecoderPluginFactory::display_name() const noexcept {
    return "Opus decoder (RFC 6716 / RFC 7587) — 48 kHz, mono or stereo";
}

plugins::IAudioDecoder* AudioDecoderPluginFactory::create() const {
    try {
        return new AudioDecoderPlugin();
    } catch (...) {
        // The concrete decoder could not be created (invalid format or an
        // exhausted libopus allocator) — the caller gets a null factory
        // product and falls back to the "plugin missing" policy (B-16).
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

namespace detail {

void do_register_audio_decoder_plugin() noexcept {
    static const struct Registrar {
        Registrar() {
            static nimrtc::opus::AudioDecoderPluginFactory s_factory{};
            nimrtc::core::PluginRegistry::instance().register_audio_decoder(
                std::string_view{s_factory.id()}, &s_factory);
        }
    } s_registrar;
    (void)s_registrar;
}

} // namespace detail

} // namespace nimrtc::opus
