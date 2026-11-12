/**
 * @file src/modules/audio_rx/src/receiver.cpp
 * @brief The audio receive pipeline (v1.2.0, audio pipeline WP-3 / WP-4).
 *
 * The pipeline turns inbound audio RTP payloads into PCM frames:
 *
 *   1. `push()` hands the payload to the **injected** jitter buffer
 *      (`plugins::IJB`, resolved through the registry by the id the
 *      engine put into `AudioReceiverConfig::jb_plugin_id`).
 *   2. `tick()` pops from that jitter buffer and hands the payload to the
 *      **injected** decoder (`plugins::IAudioDecoder`, id
 *      `AudioReceiverConfig::decoder_plugin_id`).
 *   3. The decoded frames are placed on the media timeline: the RTP
 *      timestamp progression says which media slot every frame belongs
 *      to.  A slot that never arrives is concealed with the decoder's
 *      PLC (spec B-7); a slot that arrives late is dropped.
 *
 * Two properties are worth stating explicitly:
 *
 * * **The pipeline owns no concrete plugin.**  The module depends on
 *   `nimrtc::core` and `nimrtc::plugins` only (L3) — the jitter buffer
 *   and the decoder are injected, which is also what lets the engine
 *   drop its concrete includes (L4, spec B-15 / B-16).
 * * **The jitter buffer is configured with the stream's clock rate.**
 *   The `jb` module's factory hands out an instance that defaults to
 *   90 kHz (the video clock); an audio stream that ran on it would
 *   measure its jitter in the wrong unit (spec B-3 / defect F-6).
 */

#include "receiver_internal.hpp"

#include <algorithm>
#include <utility>

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/registry.hpp>

namespace nimrtc::audio_rx {

namespace {

/** RFC 7587 payload type of Opus (the dynamic range starts at 96). */
constexpr std::uint8_t kOpusPayloadType = 111;

/** Upper bound on the frames one RFC 7587 payload may carry. */
constexpr std::size_t kMaxFramesPerPacket = 8;

/** Upper bound on the frames one tick may pop (guard against a loop). */
constexpr int kMaxPopsPerTick = 64;

} // anonymous namespace

// ---------------------------------------------------------------------------
// Pipeline — construction
// ---------------------------------------------------------------------------

Pipeline::Pipeline(const plugins::AudioReceiverConfig& cfg)
    : cfg_(cfg) {}

Pipeline::~Pipeline() {
    close();
}

void Pipeline::set_frame_callback(plugins::IAudioReceiver::FrameCallback cb) noexcept {
    on_frame_ = std::move(cb);
}

void Pipeline::set_error_callback(plugins::IAudioReceiver::ErrorCallback cb) noexcept {
    on_error_ = std::move(cb);
}

bool Pipeline::open() noexcept {
    auto& reg = core::PluginRegistry::instance();

    jb_factory_ = reg.get_jb(cfg_.jb_plugin_id);
    if (!jb_factory_) {
        if (!reported_missing_jb_) {
            reported_missing_jb_ = true;
            report_error(plugins::kErrNotReady,
                         "the jitter buffer plugin of the audio receiver is not registered");
        }
    }

    decoder_factory_ = reg.get_audio_decoder(cfg_.decoder_plugin_id);
    if (!decoder_factory_) {
        if (!reported_missing_decoder_) {
            reported_missing_decoder_ = true;
            report_error(plugins::kErrNotReady,
                         "the audio decoder plugin of the audio receiver is not registered");
        }
    }

    opened_ = (jb_factory_ != nullptr) && (decoder_factory_ != nullptr);
    return opened_;
}

void Pipeline::close() noexcept {
    for (auto& entry : streams_) {
        if (entry.second.decoder) entry.second.decoder->close();
        if (entry.second.jb) entry.second.jb->close();
        entry.second.decoder.reset();
        entry.second.jb.reset();
    }
    streams_.clear();
    jb_factory_ = nullptr;
    decoder_factory_ = nullptr;
    opened_ = false;
}

// ---------------------------------------------------------------------------
// Pipeline — stream management
// ---------------------------------------------------------------------------

bool Pipeline::open_stream(StreamState& st, std::uint32_t ssrc) noexcept {
    if (!jb_factory_ || !decoder_factory_) return false;

    // ---- the jitter buffer -------------------------------------------
    st.jb.reset(jb_factory_->create());
    if (!st.jb) {
        report_error(plugins::kErrInternal,
                     "the jitter buffer factory produced no instance");
        return false;
    }

    plugins::JBConfig jc;
    jc.name              = "audio";
    jc.ssrc              = ssrc;
    // The stream's own clock — never the module's 90 kHz default.
    jc.clock_rate_hz     = cfg_.clock_rate_hz;
    jc.initial_depth_ms  = static_cast<int>(cfg_.target_delay_ms);
    jc.min_depth_ms      = static_cast<int>(cfg_.min_delay_ms);
    jc.max_depth_ms      = static_cast<int>(cfg_.max_delay_ms);
    if (st.jb->configure(jc) != plugins::kOk) {
        report_error(plugins::kErrInternal,
                     "the jitter buffer rejected the audio stream configuration");
        st.jb.reset();
        return false;
    }
    if (st.jb->open() != plugins::kOk) {
        report_error(plugins::kErrInternal, "the jitter buffer could not be opened");
        st.jb.reset();
        return false;
    }

    // ---- the decoder -------------------------------------------------
    st.decoder.reset(decoder_factory_->create());
    if (!st.decoder) {
        report_error(plugins::kErrInternal,
                     "the audio decoder factory produced no instance");
        return false;
    }
    if (st.decoder->open() != plugins::kOk) {
        report_error(plugins::kErrInternal, "the audio decoder could not be opened");
        st.decoder.reset();
        return false;
    }

    plugins::AudioDecodeFormat fmt;
    fmt.sample_rate_hz = cfg_.decoder_sample_rate_hz;
    fmt.channels       = cfg_.decoder_channels;
    fmt.payload_type   = kOpusPayloadType;
    fmt.use_inband_fec  = cfg_.enable_fec_recovery;
    fmt.packet_loss_perc = cfg_.packet_loss_perc < 0 ? 0
                              : (cfg_.packet_loss_perc > 100 ? 100
                                                           : cfg_.packet_loss_perc);
    st.decoder->update_format(fmt);

    return true;
}

StreamState& Pipeline::stream_for(std::uint32_t ssrc) {
    auto it = streams_.find(ssrc);
    if (it != streams_.end()) return it->second;

    StreamState fresh;
    if (opened_) {
        open_stream(fresh, ssrc);
    }
    auto inserted = streams_.emplace(ssrc, std::move(fresh));
    return inserted.first->second;
}

void Pipeline::reset_stream(std::uint32_t ssrc) noexcept {
    auto it = streams_.find(ssrc);
    if (it == streams_.end()) return;
    if (it->second.decoder) it->second.decoder->reset();
    if (it->second.jb) it->second.jb->flush();
    streams_.erase(it);
}

// ---------------------------------------------------------------------------
// Pipeline — receive path
// ---------------------------------------------------------------------------

void Pipeline::push(const std::uint8_t* payload, std::size_t payload_len,
                    std::uint32_t rtp_timestamp, std::uint16_t seq,
                    std::uint32_t ssrc, plugins::TimestampUs recv_time_us) noexcept {
    if (!payload || payload_len == 0) return;

    StreamState& st = stream_for(ssrc);
    if (!st.jb) return;   // the stream could not be created (error reported)

    st.packets_received++;
    st.bytes_received += payload_len;

    // The jitter buffer keeps the *view* of the payload, so the bytes
    // have to stay alive until the frame is popped.
    st.owned_payloads.emplace_back(
        rtp_timestamp, std::vector<std::uint8_t>(payload, payload + payload_len));
    const std::vector<std::uint8_t>& bytes = st.owned_payloads.back().second;

    plugins::RtpPacket pkt;
    pkt.header.ssrc         = ssrc;
    pkt.header.seq          = seq;
    pkt.header.ts           = rtp_timestamp;
    pkt.header.payload_type = kOpusPayloadType;
    pkt.header.marker       = true;
    pkt.payload             = core::ByteSpan(bytes.data(), bytes.size());
    pkt.raw                 = pkt.payload;

    st.jb->push(pkt, recv_time_us);
}

void Pipeline::tick(plugins::TimestampUs now_us) noexcept {
    for (auto& entry : streams_) {
        StreamState& st = entry.second;
        if (!st.jb) continue;

        // The jitter buffer decides when a frame is due; the pipeline
        // only ever pops here (spec A-1).
        for (int guard = 0; guard < kMaxPopsPerTick; ++guard) {
            plugins::TimestampUs play_time_us = 0;
            std::optional<plugins::MediaSample> sample =
                st.jb->pop(now_us, &play_time_us);
            if (!sample.has_value()) break;

            consume(st, *sample, now_us);

            // The frame has been decoded — its bytes are no longer
            // referenced by the jitter buffer.
            const std::uint32_t ts = sample->rtp_ts;
            for (auto it = st.owned_payloads.begin(); it != st.owned_payloads.end();) {
                if (it->first == ts) {
                    it = st.owned_payloads.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Pipeline — the media timeline
// ---------------------------------------------------------------------------

void Pipeline::consume(StreamState& st, const plugins::MediaSample& sample,
                        plugins::TimestampUs now_us) noexcept {
    if (!st.decoder) return;

    plugins::AudioDecodedFrame frames[kMaxFramesPerPacket];
    const std::size_t n = st.decoder->decode_packet(
        sample.payload.data(), sample.payload.size(), sample.rtp_ts, frames,
        kMaxFramesPerPacket);
    if (n == 0) {
        st.decode_errors++;
        return;
    }

    const std::uint32_t step = rtp_units_per_frame(cfg_.clock_rate_hz,
                                                    cfg_.frame_duration_ms);

    for (std::size_t i = 0; i < n; ++i) {
        const plugins::AudioDecodedFrame& f = frames[i];
        if (!f.pcm || f.samples_per_channel == 0 || f.channels == 0) continue;

        // `f.pcm` is owned by the decoder and is only valid until the
        // next call into it (the IAudioDecoder seam's contract).  The
        // concealment path below calls decode_plc(), which invalidates
        // the buffer, so the samples are copied out before anything
        // else can touch the decoder.  Holding the pointer across the
        // conceal() call read freed memory (ASan: heap-use-after-free
        // in downmix_to()).
        std::vector<float> pcm(f.pcm,
                               f.pcm + f.samples_per_channel * f.channels);

        if (!st.have_expected) {
            // The first decoded frame defines the origin of the timeline.
            st.expected_ts = f.rtp_timestamp;
            st.have_expected = true;
        }

        if (f.rtp_timestamp > st.expected_ts) {
            // Missing media time: the slots between the expectation and
            // this frame never arrived (spec A-3).
            std::uint32_t ts = st.expected_ts;
            while (ts < f.rtp_timestamp) {
                conceal(st, ts, now_us);
                ts += step;
            }
        } else if (f.rtp_timestamp < st.expected_ts) {
            // The slot of this frame was already filled — the frame
            // arrived too late to be played out.
            continue;
        }

        DecodedSlot slot;
        slot.rtp_timestamp = f.rtp_timestamp;
        slot.sample_rate_hz = f.sample_rate_hz;
        slot.concealed = f.concealed;
        slot.fec_recovered = f.fec_recovered;

        const std::uint8_t out_channels =
            (cfg_.enable_downmix && cfg_.output_channels > 0) ? cfg_.output_channels
                                                              : f.channels;
        downmix_to(pcm.data(), f.samples_per_channel, f.channels, out_channels,
                   slot.pcm);
        slot.channels = out_channels;

        if (f.fec_recovered) st.frames_fec_recovered++;
        st.consecutive_concealed = 0;
        emit(st, slot, now_us);

        // The next slot starts where this frame ended — the frame's own
        // length, not the nominal step, so a stream whose frames are not
        // exactly `frame_duration_ms` long still advances correctly.
        const std::uint64_t units =
            (static_cast<std::uint64_t>(f.samples_per_channel) * cfg_.clock_rate_hz) /
            (f.sample_rate_hz ? f.sample_rate_hz : 1u);
        st.expected_ts = f.rtp_timestamp + static_cast<std::uint32_t>(units);
    }
}

void Pipeline::conceal(StreamState& st, std::uint32_t rtp_timestamp,
                        plugins::TimestampUs now_us) noexcept {
    const std::uint32_t step = rtp_units_per_frame(cfg_.clock_rate_hz,
                                                    cfg_.frame_duration_ms);
    st.packets_lost++;

    DecodedSlot slot;
    slot.rtp_timestamp = rtp_timestamp;
    slot.sample_rate_hz = cfg_.decoder_sample_rate_hz;
    slot.concealed = true;

    const std::uint8_t out_channels =
        (cfg_.enable_downmix && cfg_.output_channels > 0) ? cfg_.output_channels
                                                          : cfg_.decoder_channels;

    bool produced = false;
    if (st.decoder && st.consecutive_concealed < cfg_.max_concealed_frames) {
        plugins::AudioDecodedFrame frame{};
        const std::size_t n = st.decoder->decode_plc(
            rtp_timestamp, cfg_.frame_duration_ms, &frame, 1);
        if (n > 0 && frame.pcm && frame.samples_per_channel > 0 && frame.channels > 0) {
            const std::uint8_t target =
                (cfg_.enable_downmix && cfg_.output_channels > 0) ? cfg_.output_channels
                                                                 : frame.channels;
            downmix_to(frame.pcm, frame.samples_per_channel, frame.channels, target,
                       slot.pcm);
            slot.channels = target;
            slot.sample_rate_hz = frame.sample_rate_hz;
            produced = true;
        }
    }

    if (!produced) {
        // Beyond the cap the receiver stops generating synthetic audio
        // and emits silence instead (spec A-3).
        st.concealment_overflow++;
        const std::size_t samples =
            (static_cast<std::size_t>(cfg_.decoder_sample_rate_hz) *
             cfg_.frame_duration_ms) / 1000u;
        const std::uint8_t channels = out_channels ? out_channels : 1u;
        slot.pcm.assign(samples * channels, 0.0f);
        slot.channels = channels;
    } else {
        st.frames_concealed++;
        st.consecutive_concealed++;
    }

    emit(st, slot, now_us);
    st.expected_ts = rtp_timestamp + step;
}

void Pipeline::emit(StreamState& st, DecodedSlot& slot,
                     plugins::TimestampUs now_us) noexcept {
    st.frames_played_out++;
    if (!on_frame_) return;

    plugins::AudioOutputFrame frame;
    frame.pcm = slot.pcm.empty() ? nullptr : slot.pcm.data();
    frame.samples_per_channel =
        slot.channels ? slot.pcm.size() / slot.channels : slot.pcm.size();
    frame.sample_rate_hz = slot.sample_rate_hz;
    frame.channels = slot.channels;
    frame.rtp_timestamp = slot.rtp_timestamp;
    frame.playout_time_us = now_us;
    frame.concealed = slot.concealed;
    frame.fec_recovered = slot.fec_recovered;

    on_frame_(frame);
}

void Pipeline::report_error(plugins::Status status, const char* what) noexcept {
    if (!on_error_) return;
    on_error_(status, what);
}

// ---------------------------------------------------------------------------
// Pipeline — statistics
// ---------------------------------------------------------------------------

plugins::IAudioReceiver::Stats Pipeline::stats() const noexcept {
    plugins::IAudioReceiver::Stats out;
    for (const auto& entry : streams_) {
        const StreamState& st = entry.second;
        out.packets_received += st.packets_received;
        out.bytes_received += st.bytes_received;
        out.frames_played_out += st.frames_played_out;
        out.frames_concealed += st.frames_concealed;
        out.frames_fec_recovered += st.frames_fec_recovered;
        out.packets_lost += st.packets_lost;
        out.decode_errors += st.decode_errors;

        if (st.jb) {
            const plugins::IJB::Stats js = st.jb->stats();
            if (js.buffer_level_ms > 0) {
                out.current_delay_ms = static_cast<std::uint32_t>(js.buffer_level_ms);
            }
            if (js.jitter_ms > 0.0) {
                out.jitter_ms = static_cast<std::uint32_t>(js.jitter_ms);
            }
        }
    }
    return out;
}

} // namespace nimrtc::audio_rx
