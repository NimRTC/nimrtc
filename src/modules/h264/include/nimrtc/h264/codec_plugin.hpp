/**
 * @file nimrtc/h264/codec_plugin.hpp
 * @brief Codec plugin adapter: wraps nimrtc::h264::Decoder behind plugins::IVideoCodec.
 *
 * Implements the video codec plugin interface defined in
 * <nimrtc/plugins/video_codec.hpp> by delegating to a concrete
 * h264::Decoder instance.
 *
 * The default shipped implementation uses StubDecoder (no-op).  Host
 * applications replace this with their own (libopenh264 / ffmpeg / HW).
 *
 * Registered as id `"h264"` (matches the EngineConfig::video_codec_name
 * default; see src/modules/assembly/include/nimrtc/assembly/profiles.hpp).
 *
 * @note P1. Replaces the engine's direct `make_unique<h264::Decoder>`
 *       instantiation with registry lookup.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <string_view>

#include <nimrtc/plugins/video_codec.hpp>
#include <nimrtc/h264/decoder.hpp>

namespace nimrtc::h264 {

// ---------------------------------------------------------------------------
// CodecPluginAdapter
// ---------------------------------------------------------------------------

/** Wraps a concrete h264::Decoder behind plugins::IVideoCodec. */
class CodecPluginAdapter : public plugins::IVideoCodec {
public:
    explicit CodecPluginAdapter(plugins::VideoCodecConfig cfg) noexcept;
    ~CodecPluginAdapter() override;

    CodecPluginAdapter(const CodecPluginAdapter&)            = delete;
    CodecPluginAdapter& operator=(const CodecPluginAdapter&) = delete;

    // ---- plugins::IPlugin --------------------------------------------------

    const char* name() const noexcept override;
    plugins::Status open() noexcept override;
    void          close() noexcept override;

    // ---- plugins::IVideoCodec ----------------------------------------------

    bool can_encode() const noexcept override { return true; }    // stub now emits a synthetic H.264 bitstream
    bool can_decode() const noexcept override { return true; }

    plugins::VideoCodecKind kind() const noexcept override {
        return plugins::VideoCodecKind::kH264;
    }
    std::string_view codec_name() const noexcept override { return "h264"; }

    plugins::Status encode(const plugins::VideoFrame& raw,
                           std::uint8_t* output, std::size_t output_capacity,
                           plugins::EncodedVideoFrame& encoded_out) noexcept override;

    plugins::Status decode(const plugins::EncodedVideoFrame& encoded,
                           plugins::VideoFrame& raw_out,
                           std::uint8_t* const* output_buffers) noexcept override;

    plugins::Status force_keyframe() noexcept override { return plugins::kOk; }

    /** Fill an I420 (or NV12) framebuffer with a recognisable synthetic
     *  pattern derived from @p frame_seq.  Used by the stub decoder so
     *  round-tripped frames are visually identifiable on the wire /
     *  on screen.  @p y, @p u, @p v are caller-owned. */
    static void paint_i420(std::uint8_t* y,
                           std::uint8_t* u,
                           std::uint8_t* v,
                           std::uint32_t width, std::uint32_t height,
                           std::uint32_t frame_seq) noexcept;

    plugins::VideoCodecConfig config() const noexcept override { return cfg_; }
    plugins::Status update_config(plugins::VideoCodecConfig cfg) noexcept override;
    plugins::VideoCodecStats stats() const noexcept override;
    std::uint8_t payload_type() const noexcept override;

private:
    plugins::VideoCodecConfig cfg_{};
    std::unique_ptr<Decoder>  decoder_;
    plugins::VideoCodecStats  stats_{};
};

// ---------------------------------------------------------------------------
// H264PluginFactory
// ---------------------------------------------------------------------------

/** Factory producing CodecPluginAdapter instances.
 *  Registered as id "h264".  Replace by registering your own
 *  factory with a different id and selecting it via
 *  `EngineConfig::video_codec_name`. */
class H264PluginFactory : public plugins::IVideoCodecFactory {
public:
    std::string_view id() const noexcept override { return "h264"; }
    std::string_view display_name() const noexcept override {
        return "H.264 (RFC 6184) — stub decoder; real decoder via host plugin";
    }
    plugins::IVideoCodec* create(plugins::VideoCodecConfig cfg) const override;
};

// ---------------------------------------------------------------------------
// Registration entry point
// ---------------------------------------------------------------------------

namespace detail {
void do_register_default_plugins() noexcept;
}

/** Register the built-in "h264" codec plugin with core::PluginRegistry.
 *  Idempotent (uses Meyers' singleton latch internally). */
void register_default_plugins() noexcept;

} // namespace nimrtc::h264
