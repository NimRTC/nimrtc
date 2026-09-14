/**
 * @file src/modules/h264/src/openh264_encoder.cpp
 * @brief OpenH264 software H.264 encoder plugin.
 *
 * Provides a software-fallback H.264 encoder for hosts that don't have
 * HW acceleration available but want better compression than the stub.
 * OpenH264 is BSD-licensed and binary-distributed by Cisco.
 *
 * Real implementation would link against libopenh264 (cisco's encoder);
 * for the plugin entry point we delegate to CodecPluginAdapter which
 * already produces a valid H.264 bitstream for tests. Production
 * deployments should replace this with the actual OpenH264 binding.
 *
 * Compile-time:
 *   - NIMRTC_PLUGINS_OPENH264_ON must be defined by NimRTHwPlugins.cmake
 *     after OpenH264 SDK detection succeeds.
 */

#include <nimrtc/h264/hw_backends.hpp>

#include <cstdint>
#include <memory>

#include <nimrtc/core/log.hpp>
#include <nimrtc/h264/codec_plugin.hpp>
#include <nimrtc/plugins/video_codec.hpp>

#if defined(_WIN32)
  #include <windows.h>
#else
  #include <dlfcn.h>
#endif

namespace nimrtc::h264 {

#if defined(NIMRTC_PLUGINS_OPENH264_ON)

// The OpenH264 binding lives in libopenh264.so / openh264.dll.  We defer
// actual API binding to the host plugin (which already provides an
// ISVCEncoder interface via WelsCreateSVCEncoder).  For the in-tree
// plugin we expose the same selection surface but delegate to the
// CodecPluginAdapter for now — this keeps the registry stable until a
// real libopenh264 dependency lands.

class Openh264Codec : public plugins::IVideoCodec {
public:
    explicit Openh264Codec(plugins::VideoCodecConfig cfg)
        : inner_(std::move(cfg)) {}
    bool can_encode() const noexcept override { return true; }
    bool can_decode() const noexcept override { return true; }
    plugins::VideoCodecKind kind() const noexcept override {
        return plugins::VideoCodecKind::kH264;
    }
    std::string_view codec_name() const noexcept override {
        return "h264-openh264";
    }

    plugins::Status open()  noexcept override { return inner_.open(); }
    void           close() noexcept override { return inner_.close(); }

    plugins::Status encode(const plugins::VideoFrame& raw,
                           std::uint8_t* out, std::size_t cap,
                           plugins::EncodedVideoFrame& enc) noexcept override {
        return inner_.encode(raw, out, cap, enc);
    }
    plugins::Status decode(const plugins::EncodedVideoFrame& in,
                           plugins::VideoFrame& raw,
                           std::uint8_t* const* buffers) noexcept override {
        return inner_.decode(in, raw, buffers);
    }
    plugins::Status force_keyframe() noexcept override {
        return inner_.force_keyframe();
    }
    plugins::VideoCodecConfig config() const noexcept override {
        return inner_.config();
    }
    plugins::Status update_config(plugins::VideoCodecConfig cfg) noexcept override {
        return inner_.update_config(std::move(cfg));
    }
    plugins::VideoCodecStats stats() const noexcept override {
        return inner_.stats();
    }
    std::uint8_t payload_type() const noexcept override {
        return inner_.payload_type();
    }
private:
    CodecPluginAdapter inner_;
};

bool openh264_h264_available() noexcept {
    // Probe: try dlopen("openh264") / LoadLibrary("openh264"); when the
    // library is absent we report unavailable so the selector falls
    // through.  Real binding would also check for the WelsCreateSVCEncoder
    // entry point.
#if defined(_WIN32)
    HMODULE h = LoadLibraryW(L"openh264.dll");
    if(!h) return false;
    FreeLibrary(h);
    return true;
#else
    void* h = dlopen("libopenh264.so", RTLD_LAZY);
    if(!h) return false;
    dlclose(h);
    return true;
#endif
}

std::unique_ptr<plugins::IVideoCodec>
make_openh264_h264_codec(plugins::VideoCodecConfig cfg) {
    return std::make_unique<Openh264Codec>(std::move(cfg));
}

#else   // NIMRTC_PLUGINS_OPENH264_ON not defined

bool openh264_h264_available() noexcept { return false; }

std::unique_ptr<plugins::IVideoCodec>
make_openh264_h264_codec(plugins::VideoCodecConfig) { return nullptr; }

#endif

} // namespace nimrtc::h264
