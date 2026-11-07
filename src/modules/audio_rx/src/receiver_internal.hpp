/**
 * @file src/modules/audio_rx/src/receiver_internal.hpp
 * @brief Internal declarations of the audio receive pipeline (v1.2.0, WP-3).
 *
 * This is a **private** header: it lives under `src/`, not under
 * `include/`, so it is not part of the public API surface (the API
 * freeze scan counts only the headers under `include/`).  Nothing outside
 * this module includes it.
 *
 * The module implements `plugins::IAudioReceiver`: it owns the jitter
 * buffer instance, the decoder instance, the timeline bookkeeping, the
 * concealment and the FEC-recovery bookkeeping for one inbound audio
 * stream.  Both plugins are *injected* through the registry, so the
 * module depends on `nimrtc::core` and `nimrtc::plugins` only (L3).
 */

#ifndef NIMRTC_AUDIO_RX_RECEIVER_INTERNAL_HPP
#define NIMRTC_AUDIO_RX_RECEIVER_INTERNAL_HPP

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include <nimrtc/plugins/audio_decoder.hpp>
#include <nimrtc/plugins/audio_receiver.hpp>
#include <nimrtc/plugins/jb.hpp>

namespace nimrtc::audio_rx {

/** One decoded frame, on its way to the host. */
struct DecodedSlot {
    /** Interleaved float PCM, already downmixed to the output channels. */
    std::vector<float> pcm;
    /** RTP timestamp of the first sample of the frame. */
    std::uint32_t rtp_timestamp = 0;
    /** Sample rate of `pcm`. */
    std::uint32_t sample_rate_hz = 48000;
    /** Channel count of `pcm` (after the downmix). */
    std::uint8_t  channels = 1;
    /** The frame was produced by packet-loss concealment. */
    bool concealed = false;
    /** The frame was reconstructed from in-band FEC. */
    bool fec_recovered = false;
};

/** The state of one inbound audio stream (one remote SSRC). */
struct StreamState {
    /** The injected jitter buffer (one instance per stream). */
    std::unique_ptr<plugins::IJB> jb;
    /** The injected decoder (one instance per stream). */
    std::unique_ptr<plugins::IAudioDecoder> decoder;

    /** Payload bytes the jitter buffer still references.
     *
     *  The jitter buffer stores the *view* of the payload, not a copy
     *  (see `src/modules/jb/src/jitter_buffer.cpp`: the frame keeps the
     *  `rtp::PacketView` as it was handed in), so the receiver has to
     *  own the bytes until the frame is popped.  The list keeps the
     *  allocations stable while entries are appended.
     */
    std::list<std::pair<std::uint32_t, std::vector<std::uint8_t>>> owned_payloads;

    /** RTP timestamp of the next slot to play out. */
    std::uint32_t expected_ts = 0;
    /** Whether `expected_ts` is meaningful (a frame was decoded before). */
    bool have_expected = false;

    /** Consecutive concealed slots. */
    std::uint32_t consecutive_concealed = 0;
    /** Slots concealed beyond the cap (silence was emitted instead). */
    std::uint64_t concealment_overflow = 0;

    // ---- counters ----
    std::uint64_t packets_received = 0;
    std::uint64_t bytes_received = 0;
    std::uint64_t frames_played_out = 0;
    std::uint64_t frames_concealed = 0;
    std::uint64_t frames_fec_recovered = 0;
    std::uint64_t packets_lost = 0;
    std::uint64_t decode_errors = 0;
};

/**
 * @brief The audio receive pipeline.
 *
 * The pipeline is clock-driven: the host's `tick()` is the only thing
 * that makes it produce output (spec A-1).  It owns no concrete jitter
 * buffer and no concrete decoder — both are resolved through
 * `core::PluginRegistry` from the ids the engine put into
 * `plugins::AudioReceiverConfig` (spec B-15 / WP-3).
 */
class Pipeline {
public:
    explicit Pipeline(const plugins::AudioReceiverConfig& cfg);
    ~Pipeline();

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    /** Install the frame callback. */
    void set_frame_callback(plugins::IAudioReceiver::FrameCallback cb) noexcept;
    /** Install the error callback. */
    void set_error_callback(plugins::IAudioReceiver::ErrorCallback cb) noexcept;

    /** Resolve the injected plugins.  Returns false when one is missing. */
    bool open() noexcept;
    /** Drop every stream and release the injected plugins. */
    void close() noexcept;

    /** Feed one inbound audio RTP payload. */
    void push(const std::uint8_t* payload, std::size_t payload_len,
              std::uint32_t rtp_timestamp, std::uint16_t seq, std::uint32_t ssrc,
              plugins::TimestampUs recv_time_us) noexcept;

    /** Advance the playout clock; emits whatever is due. */
    void tick(plugins::TimestampUs now_us) noexcept;

    /** Drop the state of one stream. */
    void reset_stream(std::uint32_t ssrc) noexcept;

    /** Snapshot of the receive-pipeline statistics. */
    plugins::IAudioReceiver::Stats stats() const noexcept;

private:
    /** The stream of `ssrc`, created on first use. */
    StreamState& stream_for(std::uint32_t ssrc);
    /** Create the injected plugins of a fresh stream. */
    bool open_stream(StreamState& st, std::uint32_t ssrc) noexcept;
    /** Decode one popped sample and play out what it implies. */
    void consume(StreamState& st, const plugins::MediaSample& sample,
                 plugins::TimestampUs now_us) noexcept;
    /** Play out one decoded frame. */
    void emit(StreamState& st, DecodedSlot& slot, plugins::TimestampUs now_us) noexcept;
    /** Conceal one missing slot (and count it). */
    void conceal(StreamState& st, std::uint32_t rtp_timestamp,
                 plugins::TimestampUs now_us) noexcept;
    /** Report an error to the host (at most once per kind). */
    void report_error(plugins::Status status, const char* what) noexcept;

    plugins::AudioReceiverConfig cfg_;
    plugins::IAudioReceiver::FrameCallback on_frame_;
    plugins::IAudioReceiver::ErrorCallback on_error_;

    const plugins::IJBFactory*         jb_factory_ = nullptr;
    const plugins::IAudioDecoderFactory* decoder_factory_ = nullptr;

    std::map<std::uint32_t, StreamState> streams_;

    bool opened_ = false;
    bool reported_missing_jb_ = false;
    bool reported_missing_decoder_ = false;
};

/**
 * @brief Mix `in` down to `out_channels` channels.
 *
 * The mix is rate-preserving (spec B-9): the sample count per channel
 * never changes, only the number of channels.  When the channel counts
 * already match, the input is copied verbatim.
 */
void downmix_to(const float* in, std::size_t samples_per_channel,
                std::uint8_t in_channels, std::uint8_t out_channels,
                std::vector<float>& out);

/**
 * @brief The number of RTP clock units one media frame spans.
 *
 * 48000 Hz and 20 ms give 960 units — the step of the media timeline
 * the receiver walks.
 */
std::uint32_t rtp_units_per_frame(std::uint32_t clock_rate_hz,
                                   std::uint16_t frame_duration_ms) noexcept;

} // namespace nimrtc::audio_rx

#endif // NIMRTC_AUDIO_RX_RECEIVER_INTERNAL_HPP
