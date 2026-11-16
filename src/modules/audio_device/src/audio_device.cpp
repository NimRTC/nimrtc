// audio_device/src/audio_device.cpp
// The module's backend lookup.

#include <nimrtc/audio_device/audio_device.hpp>
#include <nimrtc/audio_device/system_audio.hpp>

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

// The system-audio (loopback) factories — remote-desktop plan F6.
// They are *separate* from the microphone's factories, so a host can
// never resolve "the system output" through the microphone's id.
const SystemAudioSourceFactory g_system_audio_source_factory;
const SyntheticSystemAudioSourceFactory g_synthetic_system_audio_source_factory;

} // namespace

std::vector<std::string> available_source_backends() noexcept {
    std::vector<std::string> out;
    if (portaudio_available()) {
        out.emplace_back("portaudio");
    }
    out.emplace_back("synthetic");
    // The system-audio backend is a *second* source, not a variant of
    // the microphone: it has its own id, and it is always listed so a
    // host can discover it (its `status()` says whether it is usable).
    out.emplace_back("system-audio");
    out.emplace_back("synthetic-system-audio");
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
    // The system-audio backends are resolved by their OWN ids, so a
    // host that asks for the microphone never gets the system output
    // by accident.
    if (id == "system-audio") {
        return &g_system_audio_source_factory;
    }
    if (id == "synthetic-system-audio") {
        return &g_synthetic_system_audio_source_factory;
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

// ---------------------------------------------------------------------------
// Playback progress (remote-desktop plan F3)
// ---------------------------------------------------------------------------

const PlaybackProgressTracker*
playback_progress_tracker(const plugins::IAudioSink* sink) noexcept {
    if (sink == nullptr) {
        return nullptr;
    }
    const auto* progress_sink =
        dynamic_cast<const IPlaybackProgressSink*>(sink);
    if (progress_sink == nullptr) {
        return nullptr;
    }
    return &progress_sink->playback_progress();
}

std::uint64_t played_frames(const plugins::IAudioSink* sink) noexcept {
    const PlaybackProgressTracker* tracker =
        playback_progress_tracker(sink);
    if (tracker == nullptr) {
        return 0;
    }
    return tracker->frames_played();
}

std::size_t queued_frames(const plugins::IAudioSink* sink) noexcept {
    const PlaybackProgressTracker* tracker =
        playback_progress_tracker(sink);
    if (tracker == nullptr) {
        return 0;
    }
    return tracker->frames_pending();
}

// ---------------------------------------------------------------------------
// The AEC delay (remote-desktop plan F4)
// ---------------------------------------------------------------------------

bool attach_aec_delay_monitor(plugins::IAudioSink* sink,
                              AecDelayMonitor* monitor) noexcept {
    if (sink == nullptr) {
        return false;
    }
    auto* delay_sink = dynamic_cast<IAecDelaySink*>(sink);
    if (delay_sink == nullptr) {
        return false;
    }
    delay_sink->set_aec_delay_monitor(monitor);
    return true;
}

bool attach_aec_delay_monitor(plugins::IAudioSource* source,
                              AecDelayMonitor* monitor) noexcept {
    if (source == nullptr) {
        return false;
    }
    auto* delay_source = dynamic_cast<IAecDelaySource*>(source);
    if (delay_source == nullptr) {
        return false;
    }
    delay_source->set_aec_delay_monitor(monitor);
    return true;
}

namespace {

/** The monitor attached to @p sink, or nullptr. */
AecDelayMonitor* monitor_of(const plugins::IAudioSink* sink) noexcept {
    if (sink == nullptr) {
        return nullptr;
    }
    const auto* delay_sink = dynamic_cast<const IAecDelaySink*>(sink);
    if (delay_sink == nullptr) {
        return nullptr;
    }
    return delay_sink->aec_delay_monitor();
}

} // namespace

std::optional<std::int32_t>
aec_delay_ms(const plugins::IAudioSink* sink) noexcept {
    AecDelayMonitor* monitor = monitor_of(sink);
    if (monitor == nullptr) {
        return std::nullopt;
    }
    return monitor->delay_ms();
}

std::optional<std::int32_t>
aec_delay_headroom_ms(const plugins::IAudioSink* sink) noexcept {
    AecDelayMonitor* monitor = monitor_of(sink);
    if (monitor == nullptr) {
        return std::nullopt;
    }
    return monitor->aec_delay_headroom_ms();
}

float aec_delay_confidence(const plugins::IAudioSink* sink) noexcept {
    AecDelayMonitor* monitor = monitor_of(sink);
    if (monitor == nullptr) {
        return 0.0f;
    }
    return monitor->confidence();
}

} // namespace nimrtc::audio_device
