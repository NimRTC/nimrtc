// audio_device/src/system_audio.cpp
// The portable half of the system-audio (loopback) capture device —
// see include/nimrtc/audio_device/system_audio.hpp for the contract.
//
// This file owns:
//
//   - the **device identity** (the `system-audio:` namespace, the
//     kind, the backend);
//   - the **synthetic** system-audio source (the CI-testable device
//     that makes the whole path testable on every leg);
//   - the **factory** that ties the two together and reports the
//     platform probe's status verbatim.
//
// The platform mechanism lives in `system_audio_platform.cpp`.

#include <nimrtc/audio_device/system_audio.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "system_audio_platform.hpp"

namespace nimrtc::audio_device {

namespace {

// The synthetic system-audio device's identity.  It lives in the
// `system-audio:` namespace — the SAME namespace as the real loopback
// devices — but under the `synthetic` backend, so a host can never
// mistake it for a real capture device.
constexpr const char* kSyntheticDeviceId   = "system-audio:synthetic:0";
constexpr const char* kSyntheticDeviceName = "Synthetic System Audio (CI)";

// The synthetic device's defaults.
constexpr std::uint32_t kDefaultSampleRate = 48000;
constexpr std::uint16_t kDefaultChannels   = 2;
constexpr std::size_t   kDefaultQuantum     = 480;
constexpr std::size_t   kDefaultQueueFrames = 50;

constexpr double kTwoPi = 6.28318530717958647692;

std::uint16_t effective_channels(std::uint16_t requested) noexcept {
    return requested == 0 ? kDefaultChannels : requested;
}

std::uint32_t effective_rate(std::uint32_t requested) noexcept {
    return requested == 0 ? kDefaultSampleRate : requested;
}

std::size_t effective_quantum(std::uint32_t requested) noexcept {
    return requested == 0 ? kDefaultQuantum : static_cast<std::size_t>(requested);
}

std::size_t queue_capacity(const plugins::AudioSourceConfig& cfg) noexcept {
    const std::size_t quantum = effective_quantum(cfg.format.frames_per_buffer);
    const std::size_t requested = cfg.queue_frames != 0
        ? static_cast<std::size_t>(cfg.queue_frames)
        : kDefaultQueueFrames;
    const std::size_t minimum = quantum * 4;
    return requested < minimum ? minimum : requested;
}

std::int64_t now_us() noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::vector<plugins::AudioDeviceFormat> synthetic_formats() noexcept {
    std::vector<plugins::AudioDeviceFormat> out;
    for (std::uint32_t rate : {44100u, 48000u}) {
        for (std::uint16_t ch : {std::uint16_t{1}, std::uint16_t{2}}) {
            plugins::AudioDeviceFormat f;
            f.sample_rate_hz = rate;
            f.channels = ch;
            f.frames_per_buffer = kDefaultQuantum;
            f.format = plugins::AudioSampleFormat::kFloat32;
            out.push_back(f);
        }
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// SyntheticSystemAudioSource
// ---------------------------------------------------------------------------

SyntheticSystemAudioSource::SyntheticSystemAudioSource(
    plugins::AudioSourceConfig cfg) noexcept
    : cfg_(std::move(cfg)),
      queue_(queue_capacity(cfg_), effective_channels(cfg_.format.channels)) {
    cfg_.format.sample_rate_hz = effective_rate(cfg_.format.sample_rate_hz);
    cfg_.format.channels = effective_channels(cfg_.format.channels);
    cfg_.format.frames_per_buffer =
        static_cast<std::uint32_t>(effective_quantum(cfg_.format.frames_per_buffer));

    scratch_.assign(
        static_cast<std::size_t>(cfg_.format.frames_per_buffer) * cfg_.format.channels,
        0.0f);
}

SyntheticSystemAudioSource::~SyntheticSystemAudioSource() {
    stop_realtime();
}

plugins::Status SyntheticSystemAudioSource::open() noexcept {
    if (open_.load()) {
        return plugins::kOk;
    }
    device_lost_.store(false);
    open_.store(true);
    return plugins::kOk;
}

void SyntheticSystemAudioSource::close() noexcept {
    stop_realtime();
    open_.store(false);
    queue_.clear();
}

std::vector<plugins::AudioDevice>
SyntheticSystemAudioSource::enumerate_devices() const noexcept {
    std::vector<plugins::AudioDevice> out;
    plugins::AudioDevice d;
    d.id = kSyntheticDeviceId;
    d.name = kSyntheticDeviceName;
    d.can_capture = true;
    d.can_playback = false;
    d.max_input_channels = 2;
    d.max_output_channels = 0;
    d.default_sample_rate_hz = kDefaultSampleRate;
    out.push_back(std::move(d));
    return out;
}

std::vector<plugins::AudioDeviceFormat>
SyntheticSystemAudioSource::supported_formats(
    const std::string& device_id) const noexcept {
    if (!device_id.empty() && device_id != kSyntheticDeviceId) {
        return {};
    }
    return synthetic_formats();
}

plugins::AudioDeviceCapabilities
SyntheticSystemAudioSource::capabilities() const noexcept {
    plugins::AudioDeviceCapabilities c;
    c.capture = true;
    c.playback = false;
    // The device list is fixed, not enumerated from the platform.
    c.enumeration = false;
    c.hot_plug = false;
    c.max_sample_rate_hz = 48000;
    c.min_frames_per_buffer = 64;
    c.max_frames_per_buffer = 4096;
    c.backend = "synthetic-system-audio";
    return c;
}

void SyntheticSystemAudioSource::set_on_audio_frame(
    plugins::AudioFrameCallback cb) noexcept {
    cb_ = std::move(cb);
}

plugins::Status SyntheticSystemAudioSource::read_samples(
    float* dst,
    std::size_t capacity,
    std::size_t* out_frames) noexcept {
    if (dst == nullptr || out_frames == nullptr) {
        return plugins::kErrInvalidParam;
    }
    if (!open_.load()) {
        return plugins::kErrClosed;
    }
    if (device_lost_.load()) {
        return plugins::kErrHardwareError;
    }
    *out_frames = 0;
    const std::size_t got = queue_.read(dst, capacity);
    *out_frames = got;
    return got == 0 ? plugins::kErrNotReady : plugins::kOk;
}

plugins::AudioDeviceStats SyntheticSystemAudioSource::stats() const noexcept {
    plugins::AudioDeviceStats s;
    s.frames_processed = frames_processed_.load();
    s.frames_dropped = queue_.dropped_frames();
    s.overruns = overruns_.load();
    s.underruns = 0;
    s.device_errors = device_errors_.load();
    s.device_restarts = 0;
    s.bytes_copied = queue_.written_frames() *
                     static_cast<std::uint64_t>(cfg_.format.channels) *
                     sizeof(float);
    s.device_lost = device_lost_.load();
    return s;
}

plugins::Status SyntheticSystemAudioSource::pump() noexcept {
    if (!open_.load()) {
        return plugins::kErrNotReady;
    }
    if (device_lost_.load()) {
        device_errors_.fetch_add(1);
        return plugins::kErrHardwareError;
    }
    produce_frame();
    return plugins::kOk;
}

plugins::Status SyntheticSystemAudioSource::pump(std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        const plugins::Status st = pump();
        if (st != plugins::kOk) {
            return st;
        }
    }
    return plugins::kOk;
}

void SyntheticSystemAudioSource::produce_frame() noexcept {
    const std::size_t frames = scratch_.size() / cfg_.format.channels;
    if (frames == 0) {
        return;
    }

    // A pure tone at a frequency that is deliberately different from
    // the microphone's synthetic tone (880 Hz vs 440 Hz), so a test
    // can tell the two legs apart by ear *and* by value.
    const double rate = static_cast<double>(cfg_.format.sample_rate_hz);
    const double step = 2.0 * kTwoPi * tone_hz_ / rate;
    for (std::size_t i = 0; i < frames; ++i) {
        const float v = static_cast<float>(std::sin(phase_));
        for (std::uint16_t ch = 0; ch < cfg_.format.channels; ++ch) {
            scratch_[i * cfg_.format.channels + ch] = v;
        }
        phase_ += step;
        if (phase_ > 2.0 * kTwoPi) {
            phase_ -= 2.0 * kTwoPi;
        }
    }

    plugins::AudioFrame frame;
    frame.samples = scratch_.data();
    frame.num_samples = frames;
    frame.num_channels = cfg_.format.channels;
    frame.sample_rate_hz = cfg_.format.sample_rate_hz;
    frame.capture_ts_us = now_us();
    frame.frame_seq = frame_seq_.fetch_add(1);

    frames_processed_.fetch_add(1);

    // The pull path first (so a host that only pulls still sees the
    // frame), then the push path.
    queue_.write(scratch_.data(), frames);

    if (cb_) {
        cb_(frame);
    }
}

plugins::Status SyntheticSystemAudioSource::start_realtime(
    std::size_t frames_per_buffer) noexcept {
    if (!open_.load()) {
        return plugins::kErrClosed;
    }
    if (rt_running_.load()) {
        return plugins::kOk;
    }
    if (frames_per_buffer == 0) {
        frames_per_buffer = kDefaultQuantum;
    }
    scratch_.assign(frames_per_buffer * cfg_.format.channels, 0.0f);

    rt_running_.store(true);
    rt_thread_ = std::thread([this, frames_per_buffer] {
        realtime_loop(frames_per_buffer);
    });
    return plugins::kOk;
}

void SyntheticSystemAudioSource::stop_realtime() noexcept {
    if (!rt_running_.exchange(false)) {
        return;
    }
    if (rt_thread_.joinable()) {
        rt_thread_.join();
    }
}

void SyntheticSystemAudioSource::realtime_loop(
    std::size_t frames_per_buffer) noexcept {
    const auto period = std::chrono::microseconds(
        static_cast<std::int64_t>(static_cast<double>(frames_per_buffer) *
                                  1'000'000.0 /
                                  static_cast<double>(cfg_.format.sample_rate_hz)));
    auto next = std::chrono::steady_clock::now();
    while (rt_running_.load()) {
        next += period;
        (void)pump();
        std::this_thread::sleep_until(next);
    }
}

// ---------------------------------------------------------------------------
// The factories
// ---------------------------------------------------------------------------

plugins::IAudioSource* SystemAudioSourceFactory::create(
    plugins::AudioSourceConfig cfg) const {
    // The platform leg decides whether a real loopback can be opened.
    // When it cannot, the factory reports the failure explicitly
    // instead (see `open_device()` and `status()`).
    std::string error;
    auto source = detail::create_platform_system_audio(cfg, error);
    if (source != nullptr) {
        return source.release();
    }
    return nullptr;
}

std::unique_ptr<plugins::IAudioSource> SystemAudioSourceFactory::open_device(
    plugins::AudioSourceConfig cfg,
    std::string& error) const {
    error.clear();
    auto source = detail::create_platform_system_audio(cfg, error);
    if (source == nullptr && error.empty()) {
        error = "no system-audio loopback device could be opened";
    }
    return source;
}

std::vector<std::string> SystemAudioSourceFactory::device_ids() const {
    std::vector<std::string> out;
    for (const auto& d : devices()) {
        out.push_back(d.id);
    }
    return out;
}

std::vector<SystemAudioDevice> SystemAudioSourceFactory::devices() const {
    const detail::PlatformSystemAudioInfo info = detail::probe_platform_system_audio();

    std::vector<SystemAudioDevice> out;
    out.reserve(info.devices.size());
    for (const auto& d : info.devices) {
        SystemAudioDevice dev;
        dev.id = d.id;
        dev.name = d.name;
        dev.kind = d.kind;
        dev.backend = info.backend;
        dev.channels = d.channels;
        dev.sample_rate_hz = d.sample_rate_hz;
        out.push_back(std::move(dev));
    }
    return out;
}

SystemAudioBackend SystemAudioSourceFactory::backend() const noexcept {
    return detail::probe_platform_system_audio().backend;
}

bool SystemAudioSourceFactory::available() const noexcept {
    const detail::PlatformSystemAudioInfo info = detail::probe_platform_system_audio();
    return info.status == SystemAudioStatus::kAvailable &&
           !info.devices.empty();
}

SystemAudioFactoryStatus SystemAudioSourceFactory::status() const {
    const detail::PlatformSystemAudioInfo info = detail::probe_platform_system_audio();
    SystemAudioFactoryStatus st;
    st.backend = info.backend;
    st.code = info.status;
    st.reason = info.reason;
    return st;
}

plugins::IAudioSource* SyntheticSystemAudioSourceFactory::create(
    plugins::AudioSourceConfig cfg) const {
    return new SyntheticSystemAudioSource(std::move(cfg));
}

std::vector<std::string> SyntheticSystemAudioSourceFactory::device_ids() const {
    return {kSyntheticDeviceId};
}

// ---------------------------------------------------------------------------
// Free helpers
// ---------------------------------------------------------------------------

std::string_view system_audio_backend_name(SystemAudioBackend backend) noexcept {
    switch (backend) {
        case SystemAudioBackend::kUnavailable:       return "unavailable";
        case SystemAudioBackend::kWasapiLoopback:    return "wasapi-loopback";
        case SystemAudioBackend::kCoreAudioTap:      return "coreaudio-tap";
        case SystemAudioBackend::kPulseAudioMonitor: return "pulseaudio-monitor";
        case SystemAudioBackend::kSynthetic:         return "synthetic";
        default:                                     return "unknown";
    }
}

std::string_view system_audio_device_kind_name(
    SystemAudioDeviceKind kind) noexcept {
    switch (kind) {
        case SystemAudioDeviceKind::kLoopback: return "loopback";
        case SystemAudioDeviceKind::kTap:     return "tap";
        case SystemAudioDeviceKind::kSynthetic: return "synthetic";
        default:                               return "unknown";
    }
}

std::string_view system_audio_status_name(SystemAudioStatus status) noexcept {
    switch (status) {
        case SystemAudioStatus::kAvailable:    return "available";
        case SystemAudioStatus::kUnsupported:  return "unsupported";
        case SystemAudioStatus::kNoDevice:     return "no device";
        case SystemAudioStatus::kBackendError: return "backend error";
        default:                               return "unknown";
    }
}

} // namespace nimrtc::audio_device
