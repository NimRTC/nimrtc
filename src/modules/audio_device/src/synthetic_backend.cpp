// audio_device/src/synthetic_backend.cpp
// The synthetic capture + playback devices — see
// include/nimrtc/audio_device/synthetic_device.hpp.

#include <nimrtc/audio_device/audio_device.hpp>
#include <nimrtc/audio_device/synthetic_device.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace nimrtc::audio_device {

namespace {

// The synthetic device's defaults.  48 kHz mono at a 10 ms quantum is
// what every WebRTC deployment uses, and it matches the engine's PCM
// contract exactly — so a host can pipe the device straight into
// `send_audio()` with no conversion.
constexpr std::uint32_t kDefaultSampleRate = 48000;
constexpr std::uint16_t kDefaultChannels   = 1;
constexpr std::size_t   kDefaultQuantum     = 480;   // 10 ms at 48 kHz
constexpr std::size_t   kDefaultQueueFrames = 50;    // 500 ms

// `M_PI` is not in the C++ standard headers, so the module carries its
// own constant rather than depending on a platform-specific macro.
constexpr double kTwoPi = 6.28318530717958647692;

constexpr const char* kSyntheticDeviceId   = "synthetic:0";
constexpr const char* kSyntheticDeviceName = "Synthetic Audio Device (CI)";

std::uint16_t effective_channels(std::uint16_t requested) noexcept {
    return requested == 0 ? kDefaultChannels : requested;
}

std::uint32_t effective_rate(std::uint32_t requested) noexcept {
    return requested == 0 ? kDefaultSampleRate : requested;
}

std::size_t effective_quantum(std::uint32_t requested) noexcept {
    return requested == 0 ? kDefaultQuantum : static_cast<std::size_t>(requested);
}

/**
 * The bounded queue's capacity, in frames.
 *
 * The queue must hold *at least one quantum* — a device that hands
 * over a 480-frame block into a 50-frame queue would lose most of it
 * before the consumer ever sees it.  Four quanta of headroom is the
 * minimum that keeps a consumer that is one block behind alive.
 */
std::size_t queue_capacity(const plugins::AudioSourceConfig& cfg) noexcept {
    const std::size_t quantum = effective_quantum(cfg.format.frames_per_buffer);
    const std::size_t requested = cfg.queue_frames != 0
        ? static_cast<std::size_t>(cfg.queue_frames)
        : kDefaultQueueFrames;
    const std::size_t minimum = quantum * 4;
    return requested < minimum ? minimum : requested;
}

std::size_t queue_capacity(const plugins::AudioSinkConfig& cfg) noexcept {
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

plugins::AudioDevice synthetic_device() noexcept {
    plugins::AudioDevice d;
    d.id = kSyntheticDeviceId;
    d.name = kSyntheticDeviceName;
    d.can_capture = true;
    d.can_playback = true;
    d.max_input_channels = 2;
    d.max_output_channels = 2;
    d.default_sample_rate_hz = kDefaultSampleRate;
    return d;
}

std::vector<plugins::AudioDeviceFormat> synthetic_formats() noexcept {
    std::vector<plugins::AudioDeviceFormat> out;
    for (std::uint32_t rate : {8000u, 16000u, 24000u, 44100u, 48000u}) {
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
// SyntheticAudioSource
// ---------------------------------------------------------------------------

SyntheticAudioSource::SyntheticAudioSource(plugins::AudioSourceConfig cfg) noexcept
    : cfg_(std::move(cfg)),
      queue_(queue_capacity(cfg_), effective_channels(cfg_.format.channels)) {
    // The negotiation happens at construction: the request's zero
    // fields become the device's defaults, and the *result* is what
    // `config()` reports.
    cfg_.format.sample_rate_hz = effective_rate(cfg_.format.sample_rate_hz);
    cfg_.format.channels = effective_channels(cfg_.format.channels);
    cfg_.format.frames_per_buffer =
        static_cast<std::uint32_t>(effective_quantum(cfg_.format.frames_per_buffer));

    scratch_.assign(
        static_cast<std::size_t>(cfg_.format.frames_per_buffer) * cfg_.format.channels,
        0.0f);
}

SyntheticAudioSource::~SyntheticAudioSource() {
    stop_realtime();
}

plugins::Status SyntheticAudioSource::open() noexcept {
    if (open_.load()) {
        return plugins::kOk;
    }
    device_lost_.store(false);
    open_.store(true);
    return plugins::kOk;
}

void SyntheticAudioSource::close() noexcept {
    stop_realtime();
    open_.store(false);
    queue_.clear();
}

std::vector<plugins::AudioDevice>
SyntheticAudioSource::enumerate_devices() const noexcept {
    return {synthetic_device()};
}

std::vector<plugins::AudioDeviceFormat>
SyntheticAudioSource::supported_formats(const std::string& device_id) const noexcept {
    if (!device_id.empty() && device_id != kSyntheticDeviceId) {
        return {};
    }
    return synthetic_formats();
}

plugins::AudioDeviceCapabilities
SyntheticAudioSource::capabilities() const noexcept {
    plugins::AudioDeviceCapabilities c;
    c.capture = true;
    c.playback = false;
    // The device list is fixed, not enumerated from the platform.
    c.enumeration = false;
    c.hot_plug = false;
    c.max_sample_rate_hz = 48000;
    c.min_frames_per_buffer = 64;
    c.max_frames_per_buffer = 4096;
    c.backend = "synthetic";
    return c;
}

void SyntheticAudioSource::set_on_audio_frame(plugins::AudioFrameCallback cb) noexcept {
    cb_ = std::move(cb);
}

plugins::Status SyntheticAudioSource::read_samples(float* dst,
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

plugins::AudioDeviceStats SyntheticAudioSource::stats() const noexcept {
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

plugins::Status SyntheticAudioSource::pump() noexcept {
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

plugins::Status SyntheticAudioSource::pump(std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        const plugins::Status st = pump();
        if (st != plugins::kOk) {
            return st;
        }
    }
    return plugins::kOk;
}

void SyntheticAudioSource::produce_frame() noexcept {
    const std::size_t frames = scratch_.size() / cfg_.format.channels;
    if (frames == 0) {
        return;
    }

    // A pure tone.  The phase is advanced by exactly one quantum's
    // worth of samples, so the synthesised signal is continuous across
    // frames — a test can assert on the waveform, not just on "some
    // samples arrived".
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

    // The signal the microphone captured is the *other* half of the AEC
    // correlation (F4).
    if (AecDelayMonitor* monitor = aec_monitor_.load(std::memory_order_relaxed)) {
        monitor->on_capture(scratch_.data(), frames, frame.capture_ts_us);
    }

    // The pull path first (so a host that only pulls still sees the
    // frame), then the push path.
    queue_.write(scratch_.data(), frames);

    if (cb_) {
        cb_(frame);
    }
}

plugins::Status SyntheticAudioSource::start_realtime(
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

void SyntheticAudioSource::stop_realtime() noexcept {
    if (!rt_running_.exchange(false)) {
        return;
    }
    if (rt_thread_.joinable()) {
        rt_thread_.join();
    }
}

void SyntheticAudioSource::realtime_loop(std::size_t frames_per_buffer) noexcept {
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
// SyntheticAudioSink
// ---------------------------------------------------------------------------

SyntheticAudioSink::SyntheticAudioSink(plugins::AudioSinkConfig cfg) noexcept
    : cfg_(std::move(cfg)),
      queue_(queue_capacity(cfg_), effective_channels(cfg_.format.channels)) {
    cfg_.format.sample_rate_hz = effective_rate(cfg_.format.sample_rate_hz);
    cfg_.format.channels = effective_channels(cfg_.format.channels);
    cfg_.format.frames_per_buffer =
        static_cast<std::uint32_t>(effective_quantum(cfg_.format.frames_per_buffer));
    quantum_frames_ = effective_quantum(cfg_.format.frames_per_buffer);
    scratch_.assign(quantum_frames_ * cfg_.format.channels, 0.0f);
}

SyntheticAudioSink::~SyntheticAudioSink() {
    stop_realtime();
}

plugins::Status SyntheticAudioSink::open() noexcept {
    if (open_.load()) {
        return plugins::kOk;
    }
    device_lost_.store(false);
    open_.store(true);
    return plugins::kOk;
}

void SyntheticAudioSink::close() noexcept {
    stop_realtime();
    open_.store(false);
    queue_.clear();
}

std::vector<plugins::AudioDevice>
SyntheticAudioSink::enumerate_devices() const noexcept {
    return {synthetic_device()};
}

std::vector<plugins::AudioDeviceFormat>
SyntheticAudioSink::supported_formats(const std::string& device_id) const noexcept {
    if (!device_id.empty() && device_id != kSyntheticDeviceId) {
        return {};
    }
    return synthetic_formats();
}

plugins::AudioDeviceCapabilities
SyntheticAudioSink::capabilities() const noexcept {
    plugins::AudioDeviceCapabilities c;
    c.capture = false;
    c.playback = true;
    c.enumeration = false;
    c.hot_plug = false;
    c.max_sample_rate_hz = 48000;
    c.min_frames_per_buffer = 64;
    c.max_frames_per_buffer = 4096;
    c.backend = "synthetic";
    return c;
}

plugins::Status SyntheticAudioSink::write(const float* samples,
                                          std::size_t num_samples,
                                          std::uint16_t channels) noexcept {
    if (samples == nullptr || num_samples == 0) {
        return plugins::kErrInvalidParam;
    }
    if (!open_.load()) {
        return plugins::kErrClosed;
    }
    if (device_lost_.load()) {
        device_errors_.fetch_add(1);
        return plugins::kErrHardwareError;
    }
    if (channels != 0 && channels != cfg_.format.channels) {
        return plugins::kErrInvalidParam;
    }
    queue_.write(samples, num_samples);
    // The sink accepted the frames: they are now "queued" as far as the
    // AEC reference is concerned (F3).  The device has not played them
    // yet, so the played counter does NOT move here.
    progress_.on_samples_queued(num_samples);
    return plugins::kOk;
}

void SyntheticAudioSink::set_on_playback_progress(
    plugins::AudioPlaybackCallback cb) noexcept {
    progress_cb_ = std::move(cb);
}

plugins::AudioDeviceStats SyntheticAudioSink::stats() const noexcept {
    plugins::AudioDeviceStats s;
    s.frames_processed = frames_processed_.load();
    s.frames_dropped = frames_dropped_.load() + queue_.dropped_frames();
    s.underruns = underruns_.load();
    s.overruns = 0;
    s.device_errors = device_errors_.load();
    s.device_restarts = 0;
    s.bytes_copied = played_.load() *
                     static_cast<std::uint64_t>(cfg_.format.channels) *
                     sizeof(float);
    s.device_lost = device_lost_.load();
    return s;
}

plugins::Status SyntheticAudioSink::pump() noexcept {
    if (!open_.load()) {
        return plugins::kErrNotReady;
    }
    if (device_lost_.load()) {
        device_errors_.fetch_add(1);
        return plugins::kErrHardwareError;
    }

    const std::size_t got = queue_.read(scratch_.data(), quantum_frames_);
    if (got == 0) {
        // Nothing to play: the device plays silence and counts an
        // underrun.  This is what a real device does, and it is what
        // the 3A reference alignment has to cope with.
        underruns_.fetch_add(1);
        return plugins::kErrNotReady;
    }

    frames_processed_.fetch_add(1);
    const std::uint64_t total = played_.fetch_add(got) + got;

    // The device consumed `got` frames: that is what the AEC reference
    // must be aligned to (F3).
    progress_.on_samples_played(got);

    // The signal that reached the loudspeaker is the *other* half of
    // the AEC correlation (F4).
    if (AecDelayMonitor* monitor = aec_monitor_.load(std::memory_order_relaxed)) {
        monitor->on_render_played(scratch_.data(), got, now_us());
    }

    if (progress_cb_) {
        progress_cb_(total);
    }
    return plugins::kOk;
}

plugins::Status SyntheticAudioSink::pump(std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        const plugins::Status st = pump();
        if (st != plugins::kOk) {
            return st;
        }
    }
    return plugins::kOk;
}

plugins::Status SyntheticAudioSink::start_realtime(
    std::size_t frames_per_buffer) noexcept {
    if (!open_.load()) {
        return plugins::kErrClosed;
    }
    if (rt_running_.load()) {
        return plugins::kOk;
    }
    if (frames_per_buffer == 0) {
        frames_per_buffer = quantum_frames_;
    }
    quantum_frames_ = frames_per_buffer;
    scratch_.assign(frames_per_buffer * cfg_.format.channels, 0.0f);

    rt_running_.store(true);
    rt_thread_ = std::thread([this, frames_per_buffer] {
        realtime_loop(frames_per_buffer);
    });
    return plugins::kOk;
}

void SyntheticAudioSink::stop_realtime() noexcept {
    if (!rt_running_.exchange(false)) {
        return;
    }
    if (rt_thread_.joinable()) {
        rt_thread_.join();
    }
}

void SyntheticAudioSink::realtime_loop(std::size_t frames_per_buffer) noexcept {
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
// Factories
// ---------------------------------------------------------------------------

plugins::IAudioSource* SyntheticSourceFactory::create(
    plugins::AudioSourceConfig cfg) const {
    return new SyntheticAudioSource(std::move(cfg));
}

plugins::IAudioSink* SyntheticSinkFactory::create(
    plugins::AudioSinkConfig cfg) const {
    return new SyntheticAudioSink(std::move(cfg));
}

} // namespace nimrtc::audio_device
