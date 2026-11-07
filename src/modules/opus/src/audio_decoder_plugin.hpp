/**
 * @file src/modules/opus/src/audio_decoder_plugin.hpp
 * @brief The Opus implementation of plugins::IAudioDecoder (audio pipeline WP-2).
 *
 * This header is **private to the module** — it is not installed and not
 * part of the public API.  The engine reaches the decoder through the
 * plugin seam (`core::PluginRegistry` → `plugins::IAudioDecoderFactory`),
 * so no consumer needs the concrete class; keeping it internal also keeps
 * the public header count of the API-freeze scan unchanged.
 *
 * The implementation is the Opus half of the audio pipeline
 * (`docs/plan/audio-pipeline-spec.md` B-10, `docs/plan/audio-pipeline-dev-plan.md`
 * WP-2):
 *
 *   - `decode_packet()` depacketises an RFC 7587 payload (RFC 6716 §3.2
 *     codes 0..3), decodes every Opus frame in it and reports one
 *     `AudioDecodedFrame` per frame;
 *   - `decode_plc()` produces concealment audio of the requested duration
 *     and flags it as concealed;
 *   - when the caller signals a gap (a concealment was produced, or the
 *     RTP timestamp jumped), the next packet is decoded in FEC mode first,
 *     which reconstructs the **lost** frame from the redundancy the peer
 *     embedded in it.  That frame is reported with `fec_recovered == true`.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include <nimrtc/opus/opus.hpp>
#include <nimrtc/plugins/audio_decoder.hpp>

namespace nimrtc::opus {

// ---------------------------------------------------------------------------
// AudioDecoderPlugin
// ---------------------------------------------------------------------------

/**
 * @brief Decodes one inbound Opus stream into PCM frames.
 *
 * One instance owns the decoder state of one stream.  Not thread-safe: the
 * engine serialises the calls on the engine thread (the seam's contract).
 */
class AudioDecoderPlugin : public plugins::IAudioDecoder {
public:
    AudioDecoderPlugin();
    ~AudioDecoderPlugin() override;

    AudioDecoderPlugin(const AudioDecoderPlugin&)            = delete;
    AudioDecoderPlugin& operator=(const AudioDecoderPlugin&) = delete;

    // ---- plugins::IPlugin --------------------------------------------------

    const char* name() const noexcept override;
    plugins::Status open() noexcept override;
    void close() noexcept override;

    // ---- plugins::IAudioDecoder --------------------------------------------

    plugins::AudioDecodeFormat format() const noexcept override;
    plugins::Status update_format(plugins::AudioDecodeFormat f) noexcept override;

    std::size_t decode_packet(const std::uint8_t* payload,
                              std::size_t  payload_len,
                              std::uint32_t rtp_timestamp,
                              plugins::AudioDecodedFrame* out,
                              std::size_t  max_frames) noexcept override;

    std::size_t decode_plc(std::uint32_t rtp_timestamp,
                           std::uint16_t duration_ms,
                           plugins::AudioDecodedFrame* out,
                           std::size_t  max_frames) noexcept override;

    void reset() noexcept override;
    Stats stats() const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// AudioDecoderPluginFactory
// ---------------------------------------------------------------------------

/** Factory producing AudioDecoderPlugin instances; registered as "opus". */
class AudioDecoderPluginFactory : public plugins::IAudioDecoderFactory {
public:
    std::string_view id() const noexcept override;
    std::string_view display_name() const noexcept override;
    plugins::IAudioDecoder* create() const override;
};

namespace detail {

/** Defined in audio_decoder_plugin.cpp; registers the factory under "opus". */
void do_register_audio_decoder_plugin() noexcept;

} // namespace detail

} // namespace nimrtc::opus
