// audio_device/src/audio_device.cpp
// The module's backend lookup.

#include <nimrtc/audio_device/audio_device.hpp>

#include <string_view>

namespace nimrtc::audio_device {

namespace {

// The factories are `const` objects with static storage duration, so
// the pointers the lookup hands out stay valid for the lifetime of the
// process and the caller must not delete them.
//
// They are NOT global *mutable* state: they are immutable objects whose
// address is published through a lookup function (L6).
const PortAudioSourceFactory g_portaudio_source_factory;
const PortAudioSinkFactory   g_portaudio_sink_factory;
const SyntheticSourceFactory g_synthetic_source_factory;
const SyntheticSinkFactory   g_synthetic_sink_factory;

} // namespace

std::vector<std::string> available_source_backends() noexcept {
    std::vector<std::string> out;
    if (portaudio_available()) {
        out.emplace_back("portaudio");
    }
    out.emplace_back("synthetic");
    return out;
}

std::vector<std::string> available_sink_backends() noexcept {
    std::vector<std::string> out;
    if (portaudio_available()) {
        out.emplace_back("portaudio");
    }
    out.emplace_back("synthetic");
    return out;
}

const plugins::IAudioSourceFactory* source_factory(
    std::string_view id) noexcept {
    if (id == "synthetic") {
        return &g_synthetic_source_factory;
    }
    if (id == "portaudio" && portaudio_available()) {
        return &g_portaudio_source_factory;
    }
    return nullptr;
}

const plugins::IAudioSinkFactory* sink_factory(std::string_view id) noexcept {
    if (id == "synthetic") {
        return &g_synthetic_sink_factory;
    }
    if (id == "portaudio" && portaudio_available()) {
        return &g_portaudio_sink_factory;
    }
    return nullptr;
}

} // namespace nimrtc::audio_device
