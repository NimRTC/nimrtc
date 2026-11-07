/**
 * @file nimrtc/plugins/audio_receiver.hpp
 * @brief IAudioReceiver — the pluggable audio *receive pipeline* seam.
 *
 * The receive pipeline (jitter buffer, decoder, concealment, FEC
 * recovery, downmix, playout clock) lives behind this interface so the
 * engine can orchestrate it without including a concrete module header
 * (constraint L4, spec DEC-5 / B-2).
 *
 * The implementation lives in the self-contained module
 * `src/modules/audio_rx/`; the jitter buffer and the decoder are
 * *injected* as plugin interfaces (`IJB`, `IAudioDecoder`), so the
 * module depends on `nimrtc::core` and `nimrtc::plugins` only.
 *
 * ## Implementing a custom audio receiver
 *
 * 1. Implement `IAudioReceiver`.
 * 2. Register: `PluginRegistry::instance().register_audio_receiver("my_rx", &factory);`
 * 3. Select it at engine open: `cfg.audio_receiver_name = "my_rx";`
 *
 * @note v1.2.0 (audio pipeline, spec B-2). Not thread-safe — the engine
 * drives `push()` and `tick()` on the engine thread.
 */

// base.hpp must be before include guard — see transport.hpp for rationale.
#include <nimrtc/core/api_version.hpp>
#include "nimrtc/plugins/base.hpp"

#ifndef NIMRTC_PLUGINS_AUDIO_RECEIVER_HPP
#define NIMRTC_PLUGINS_AUDIO_RECEIVER_HPP

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace nimrtc::plugins {

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

/** Per-stream configuration of the receive pipeline. */
struct NIMRTC_API_STABLE AudioReceiverConfig {
    /** Id of the jitter-buffer plugin to inject (v1.2.0, WP-3).
     *
     *  The receiver owns no concrete jitter buffer: it resolves this id
     *  through `core::PluginRegistry` (the `kJb` category) and drives the
     *  result through `plugins::IJB`.  The default is the id the engine
     *  uses for its own jitter buffer (`EngineConfig::jb_name`), i.e.
     *  "adaptive".
     */
    std::string jb_plugin_id = "adaptive";
    /** Id of the audio-decoder plugin to inject (v1.2.0, WP-3).
     *
     *  Resolved through `core::PluginRegistry` (the `kAudioDecoder`
     *  category).  The default is the id the Opus decoder plugin
     *  registers under (`EngineConfig::audio_decoder_name`), i.e. "opus".
     */
    std::string decoder_plugin_id = "opus";
    /** RTP clock of the remote stream (48000 for Opus). */
    std::uint32_t clock_rate_hz = 48000;
    /** Sample rate the decoder produces. */
    std::uint32_t decoder_sample_rate_hz = 48000;
    /** Channel count as negotiated. */
    std::uint8_t  decoder_channels = 2;
    /** Channel count after downmix. */
    std::uint8_t  output_channels = 1;
    /** Negotiated packet time in milliseconds (10/20/40/60). */
    std::uint16_t frame_duration_ms = 20;
    /** Initial jitter buffer depth in milliseconds. */
    std::uint16_t target_delay_ms = 40;
    /** Minimum jitter buffer depth in milliseconds. */
    std::uint16_t min_delay_ms = 10;
    /** Maximum jitter buffer depth in milliseconds. */
    std::uint16_t max_delay_ms = 200;
    /** Cap on consecutive concealed frames before the stream is dropped. */
    std::uint16_t max_concealed_frames = 10;
    /** Enable in-band FEC recovery. */
    bool          enable_fec_recovery = true;
    /** In-band redundancy the peer is assumed to send, in percent (0..100).
     *
     *  Handed to the decoder as `AudioDecodeFormat::packet_loss_perc`:
     *  it tells libopus how much redundancy to expect, which drives its
     *  FEC / PLC decisions (spec B-8). */
    int           packet_loss_perc = 5;
    /** Downmix to `output_channels`. */
    bool          enable_downmix = true;
};

// ---------------------------------------------------------------------------
// Output frame
// ---------------------------------------------------------------------------

/** What the pipeline hands back to the engine. */
struct NIMRTC_API_STABLE AudioOutputFrame {
    /** Interleaved float PCM; owned by the receiver, valid during the callback. */
    const float*  pcm = nullptr;
    /** Number of samples per channel. */
    std::size_t   samples_per_channel = 0;
    /** Sample rate of `pcm`. */
    std::uint32_t sample_rate_hz = 48000;
    /** Channel count of `pcm`. */
    std::uint8_t  channels = 1;
    /** RTP timestamp of the first sample. */
    std::uint32_t rtp_timestamp = 0;
    /** Engine-thread playout instant in microseconds. */
    std::int64_t  playout_time_us = 0;
    /** The frame was produced by packet-loss concealment. */
    bool          concealed = false;
    /** The frame was reconstructed from in-band FEC. */
    bool          fec_recovered = false;
};

// ---------------------------------------------------------------------------
// IAudioReceiver
// ---------------------------------------------------------------------------

/**
 * @brief The audio receive pipeline for one peer connection.
 *
 * The receiver owns the jitter buffer instance, the decoder instance, the
 * timeline bookkeeping, concealment and FEC recovery. It does **not** own
 * 3A: the engine applies 3A render to the frames the receiver emits,
 * because 3A is already a plugin seam and the engine owns it.
 */
class NIMRTC_API_STABLE IAudioReceiver : public IPlugin {
public:
    /** Called for every frame that is due to be played out. */
    using FrameCallback = std::function<void(const AudioOutputFrame&)>;
    /** Called when the pipeline hits an error condition. */
    using ErrorCallback = std::function<void(Status, std::string_view)>;

    /** Install the frame and error callbacks. Called before `open()`. */
    virtual void set_callbacks(FrameCallback on_frame,
                               ErrorCallback on_error) noexcept = 0;

    /**
     * @brief Feed one inbound audio RTP packet (already SRTP-unprotected).
     *
     * @param ssrc Identifies the stream.
     */
    virtual void push(const std::uint8_t* payload, std::size_t payload_len,
                      std::uint32_t rtp_timestamp, std::uint16_t seq,
                      std::uint32_t ssrc, TimestampUs recv_time_us) noexcept = 0;

    /** Advance the playout clock; emits whatever is due. */
    virtual void tick(TimestampUs now_us) noexcept = 0;

    /** Drop all state for a stream (stream ended / renegotiated). */
    virtual void reset_stream(std::uint32_t ssrc) noexcept = 0;

    /** Receive-pipeline statistics. */
    struct Stats {
        std::uint64_t packets_received = 0;
        std::uint64_t bytes_received = 0;
        std::uint64_t frames_played_out = 0;
        std::uint64_t frames_concealed = 0;
        std::uint64_t frames_fec_recovered = 0;
        std::uint64_t packets_lost = 0;
        std::uint64_t decode_errors = 0;
        std::uint32_t current_delay_ms = 0;
        std::uint32_t jitter_ms = 0;
    };

    /** Snapshot of the receive-pipeline statistics. */
    virtual Stats stats() const noexcept = 0;
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

/** Factory for audio receivers. */
class NIMRTC_API_STABLE IAudioReceiverFactory {
public:
    virtual ~IAudioReceiverFactory() = default;
    /** Unique plugin id, e.g. "nimrtc_audio_rx". */
    virtual std::string_view id() const noexcept = 0;
    /** Short human-readable name. */
    virtual std::string_view display_name() const noexcept = 0;
    /** Create a receiver instance in the kConstructed state. */
    virtual IAudioReceiver* create(const AudioReceiverConfig& cfg) const = 0;
};

template<class T>
class NIMRTC_API_STABLE SimpleAudioReceiverFactory : public IAudioReceiverFactory {
    std::string_view id_;
    std::string_view name_;
public:
    explicit SimpleAudioReceiverFactory(std::string_view id,
                                        std::string_view name) noexcept
        : id_(id), name_(name) {}

    std::string_view id() const noexcept override { return id_; }
    std::string_view display_name() const noexcept override { return name_; }
    IAudioReceiver* create(const AudioReceiverConfig& cfg) const override {
        return new T(cfg);
    }
};

} // namespace nimrtc::plugins
#endif // NIMRTC_PLUGINS_AUDIO_RECEIVER_HPP
