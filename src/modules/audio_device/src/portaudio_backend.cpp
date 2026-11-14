// audio_device/src/portaudio_backend.cpp
// The PortAudio-backed capture + playback devices.
//
// The whole point of this file is to be THIN: PortAudio already knows
// how to talk to WASAPI / Core Audio / ALSA / PulseAudio, so the only
// job here is to translate between PortAudio's C API and NimRTC's
// plugin seam, and to make the *failure* modes clean (a device that
// was unplugged is a status code, not a crash).

#include <nimrtc/audio_device/audio_device.hpp>
#include <nimrtc/audio_device/ring_buffer.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if NIMRTC_USE_PORTAUDIO
#include <portaudio.h>
#endif

namespace nimrtc::audio_device {

namespace {

// ---------------------------------------------------------------------------
// Defaults
// ---------------------------------------------------------------------------

constexpr std::uint32_t kDefaultSampleRate = 48000;
constexpr std::uint16_t kDefaultChannels   = 1;
constexpr std::size_t   kDefaultQuantum     = 480;   // 10 ms at 48 kHz
constexpr std::size_t   kDefaultQueueFrames = 50;    // 500 ms

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
std::size_t queue_capacity(std::uint32_t requested_frames,
                            std::uint32_t requested_quantum) noexcept {
    const std::size_t quantum = effective_quantum(requested_quantum);
    const std::size_t requested = requested_frames != 0
        ? static_cast<std::size_t>(requested_frames)
        : kDefaultQueueFrames;
    const std::size_t minimum = quantum * 4;
    return requested < minimum ? minimum : requested;
}

std::int64_t now_us() noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

#if NIMRTC_USE_PORTAUDIO

/** The device id used by the seam: "portaudio:<index>". */
std::string device_id_for(PaDeviceIndex index) noexcept {
    return "portaudio:" + std::to_string(static_cast<int>(index));
}

/**
 * @brief Enumerate the PortAudio devices.
 *
 * @param playback true for the output direction, false for the input
 *                 direction.  A device that cannot serve the requested
 *                 direction is skipped.
 */
std::vector<plugins::AudioDevice> enumerate_portaudio_devices(
    bool playback) noexcept {
    std::vector<plugins::AudioDevice> out;

    // PortAudio needs to be initialised to enumerate.  The module keeps
    // a process-wide initialisation alive for the duration of the call.
    struct InitGuard {
        bool ok = false;
        InitGuard() { ok = (Pa_Initialize() == paNoError); }
        ~InitGuard() { if (ok) Pa_Terminate(); }
    } guard;

    if (!guard.ok) {
        return out;
    }

    const PaDeviceIndex count = Pa_GetDeviceCount();
    for (PaDeviceIndex i = 0; i < count; ++i) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (info == nullptr) {
            continue;
        }
        const bool can_in  = info->maxInputChannels > 0;
        const bool can_out = info->maxOutputChannels > 0;
        if (playback && !can_out) {
            continue;
        }
        if (!playback && !can_in) {
            continue;
        }

        plugins::AudioDevice d;
        d.id = device_id_for(i);
        d.name = info->name != nullptr ? info->name : "";
        d.can_capture = can_in;
        d.can_playback = can_out;
        d.max_input_channels = static_cast<std::uint16_t>(info->maxInputChannels);
        d.max_output_channels = static_cast<std::uint16_t>(info->maxOutputChannels);
        // PortAudio exposes one "default rate" per device; it is the
        // rate the device runs at natively.  (There is no separate
        // input/output rate accessor in the public API.)
        const double rate = info->defaultSampleRate;
        d.default_sample_rate_hz = rate > 0.0
            ? static_cast<std::uint32_t>(rate)
            : 0;
        out.push_back(std::move(d));
    }
    return out;
}

#endif  // NIMRTC_USE_PORTAUDIO

} // namespace

// ---------------------------------------------------------------------------
// PortAudio availability
// ---------------------------------------------------------------------------

bool portaudio_available() noexcept {
#if NIMRTC_USE_PORTAUDIO
    return Pa_GetVersion() != 0;
#else
    return false;
#endif
}

std::string portaudio_version() noexcept {
#if NIMRTC_USE_PORTAUDIO
    const PaVersionInfo* info = Pa_GetVersionInfo();
    if (info == nullptr || info->versionText == nullptr) {
        return {};
    }
    return std::string(info->versionText);
#else
    return {};
#endif
}

std::vector<std::string> portaudio_host_apis() noexcept {
    std::vector<std::string> out;
#if NIMRTC_USE_PORTAUDIO
    const PaHostApiIndex count = Pa_GetHostApiCount();
    for (PaHostApiIndex i = 0; i < count; ++i) {
        const PaHostApiInfo* info = Pa_GetHostApiInfo(i);
        if (info != nullptr && info->name != nullptr) {
            out.emplace_back(info->name);
        }
    }
#endif
    return out;
}

// ---------------------------------------------------------------------------
// PortAudio factories
// ---------------------------------------------------------------------------

#if NIMRTC_USE_PORTAUDIO

namespace {

/** Convert a PortAudio error code into a NimRTC status. */
plugins::Status pa_status(PaError err) noexcept {
    switch (err) {
        case paNoError:              return plugins::kOk;
        case paNotInitialized:       return plugins::kErrNotReady;
        case paUnanticipatedHostError:
        case paDeviceUnavailable:
        case paInternalError:        return plugins::kErrHardwareError;
        case paInvalidDevice:
        case paInvalidSampleRate:
        case paInvalidChannelCount:
        case paInvalidFlag:
        case paSampleFormatNotSupported:
        case paBadIODeviceCombination:
        case paInsufficientMemory:
        case paBufferTooBig:
        case paBufferTooSmall:
        case paBadStreamPtr:
        case paTimedOut:
        case paInputOverflowed:      return plugins::kErrInvalidParam;
        case paStreamIsStopped:
        case paStreamIsNotStopped:
        case paHostApiNotFound:
        case paInvalidHostApi:       return plugins::kErrUnsupported;
        default:                     return plugins::kErrInternal;
    }
}

/** The PaError's human-readable text, for the log. */
std::string pa_error_text(PaError err) noexcept {
    const char* text = Pa_GetErrorText(err);
    return text != nullptr ? std::string(text) : std::string("unknown");
}

/** Parse a device id back into a PortAudio index. */
bool parse_device_id(const std::string& id, PaDeviceIndex* out) noexcept {
    if (id.empty()) {
        return false;
    }
    const std::size_t colon = id.rfind(':');
    if (colon == std::string::npos) {
        return false;
    }
    try {
        const int value = std::stoi(id.substr(colon + 1));
        if (value < 0) {
            return false;
        }
        *out = static_cast<PaDeviceIndex>(value);
        return true;
    } catch (...) {
        return false;
    }
}

/** The default capture device, or paNoDevice when there is none. */
PaDeviceIndex default_input_device() noexcept {
    const PaDeviceIndex d = Pa_GetDefaultInputDevice();
    return d;
}

PaDeviceIndex default_output_device() noexcept {
    const PaDeviceIndex d = Pa_GetDefaultOutputDevice();
    return d;
}

/**
 * @brief The PortAudio capture device.
 *
 * A PortAudio callback runs on PortAudio's own thread.  It must not
 * block, so it copies the samples into the ring buffer and returns.
 */
class PortAudioSource final : public plugins::IAudioSource {
public:
    explicit PortAudioSource(plugins::AudioSourceConfig cfg) noexcept
        : cfg_(std::move(cfg)),
          queue_(queue_capacity(cfg_.queue_frames, cfg_.format.frames_per_buffer),
                 effective_channels(cfg_.format.channels)) {
        cfg_.format.sample_rate_hz = effective_rate(cfg_.format.sample_rate_hz);
        cfg_.format.channels = effective_channels(cfg_.format.channels);
        cfg_.format.frames_per_buffer =
            static_cast<std::uint32_t>(effective_quantum(cfg_.format.frames_per_buffer));
        quantum_frames_ = effective_quantum(cfg_.format.frames_per_buffer);
        scratch_.assign(quantum_frames_ * cfg_.format.channels, 0.0f);
    }

    ~PortAudioSource() override { close(); }

    const char* name() const noexcept override { return "portaudio-audio-source"; }

    plugins::Status open() noexcept override {
        if (open_.load()) {
            return plugins::kOk;
        }
        if (Pa_Initialize() != paNoError) {
            device_errors_.fetch_add(1);
            return plugins::kErrHardwareError;
        }
        initialised_.store(true);

        PaDeviceIndex index = paNoDevice;
        if (!cfg_.device_id.empty()) {
            if (!parse_device_id(cfg_.device_id, &index) ||
                index == paNoDevice) {
                close();
                return plugins::kErrInvalidParam;
            }
        } else {
            index = default_input_device();
            if (index == paNoDevice) {
                close();
                return plugins::kErrHardwareError;
            }
        }

        const PaDeviceInfo* info = Pa_GetDeviceInfo(index);
        if (info == nullptr) {
            close();
            return plugins::kErrHardwareError;
        }

        // The device may not support the requested rate / channel count.
        // PortAudio can convert, but the *seam* is explicit about what
        // is in force, so the device's native values win when the
        // request cannot be met exactly.
        const double rate = info->defaultSampleRate;
        if (rate > 0.0) {
            cfg_.format.sample_rate_hz = static_cast<std::uint32_t>(rate);
        }
        const int max_in = info->maxInputChannels;
        if (max_in > 0 && cfg_.format.channels > static_cast<std::uint16_t>(max_in)) {
            cfg_.format.channels = static_cast<std::uint16_t>(max_in);
        }

        PaStreamParameters params;
        params.device = index;
        params.channelCount = cfg_.format.channels;
        params.sampleFormat = paFloat32;
        params.suggestedLatency = 0.0;
        params.hostApiSpecificStreamInfo = nullptr;

        PaError err = Pa_OpenStream(&stream_,
                                     &params,
                                     nullptr,
                                     cfg_.format.sample_rate_hz,
                                     static_cast<unsigned long>(quantum_frames_),
                                     paNoFlag,
                                     &PortAudioSource::pa_callback,
                                     this);
        if (err != paNoError) {
            stream_ = nullptr;
            close();
            return pa_status(err);
        }

        err = Pa_StartStream(stream_);
        if (err != paNoError) {
            Pa_CloseStream(stream_);
            stream_ = nullptr;
            close();
            return pa_status(err);
        }

        device_lost_.store(false);
        open_.store(true);
        return plugins::kOk;
    }

    void close() noexcept override {
        if (stream_ != nullptr) {
            Pa_AbortStream(stream_);
            Pa_CloseStream(stream_);
            stream_ = nullptr;
        }
        open_.store(false);
        if (initialised_.exchange(false)) {
            Pa_Terminate();
        }
    }

    [[nodiscard]] std::vector<plugins::AudioDevice>
    enumerate_devices() const noexcept override {
        return enumerate_portaudio_devices(false);
    }

    [[nodiscard]] std::vector<plugins::AudioDeviceFormat>
    supported_formats(const std::string& device_id) const noexcept override {
        PaDeviceIndex index = paNoDevice;
        if (!parse_device_id(device_id, &index)) {
            return {};
        }
        const PaDeviceInfo* info = Pa_GetDeviceInfo(index);
        if (info == nullptr || info->maxInputChannels <= 0) {
            return {};
        }
        std::vector<plugins::AudioDeviceFormat> out;
        const double rate = info->defaultSampleRate;
        for (std::uint16_t ch = 1;
             ch <= static_cast<std::uint16_t>(info->maxInputChannels); ++ch) {
            plugins::AudioDeviceFormat f;
            f.sample_rate_hz = rate > 0.0 ? static_cast<std::uint32_t>(rate) : 0;
            f.channels = ch;
            f.frames_per_buffer = static_cast<std::uint32_t>(kDefaultQuantum);
            f.format = plugins::AudioSampleFormat::kFloat32;
            out.push_back(f);
        }
        return out;
    }

    [[nodiscard]] plugins::AudioDeviceCapabilities
    capabilities() const noexcept override {
        plugins::AudioDeviceCapabilities c;
        c.capture = true;
        c.playback = false;
        c.enumeration = true;
        c.hot_plug = true;
        c.max_sample_rate_hz = 192000;
        c.min_frames_per_buffer = 16;
        c.max_frames_per_buffer = 8192;
        c.backend = "portaudio";
        return c;
    }

    [[nodiscard]] bool is_open() const noexcept override { return open_.load(); }

    [[nodiscard]] plugins::AudioSourceConfig config() const noexcept override {
        return cfg_;
    }

    void set_on_audio_frame(plugins::AudioFrameCallback cb) noexcept override {
        cb_ = std::move(cb);
    }

    plugins::Status read_samples(float* dst,
                                std::size_t capacity,
                                std::size_t* out_frames) noexcept override {
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

    [[nodiscard]] plugins::AudioDeviceStats stats() const noexcept override {
        plugins::AudioDeviceStats s;
        s.frames_processed = frames_processed_.load();
        s.frames_dropped = queue_.dropped_frames();
        s.overruns = overruns_.load();
        s.underruns = 0;
        s.device_errors = device_errors_.load();
        s.device_restarts = device_restarts_.load();
        s.bytes_copied = queue_.written_frames() *
                         static_cast<std::uint64_t>(cfg_.format.channels) *
                         sizeof(float);
        s.device_lost = device_lost_.load();
        return s;
    }

private:
    static int pa_callback(const void* input,
                           void* output,
                           unsigned long frame_count,
                           const PaStreamCallbackTimeInfo* time_info,
                           PaStreamCallbackFlags status_flags,
                           void* user_data) {
        auto* self = static_cast<PortAudioSource*>(user_data);
        (void)output;
        (void)time_info;

        if (status_flags & paInputUnderflow) {
            self->overruns_.fetch_add(1);
        }
        if (input == nullptr) {
            // The device produced no data: usually a hot-unplug.  This
            // is a *clean* error, not a crash.
            self->device_lost_.store(true);
            self->device_errors_.fetch_add(1);
            return paContinue;
        }

        const std::size_t frames =
            std::min<std::size_t>(static_cast<std::size_t>(frame_count),
                                  self->quantum_frames_);
        const float* samples = static_cast<const float*>(input);

        self->frames_processed_.fetch_add(frames);
        self->queue_.write(samples, frames);

        if (self->cb_) {
            plugins::AudioFrame frame;
            frame.samples = samples;
            frame.num_samples = frames;
            frame.num_channels = self->cfg_.format.channels;
            frame.sample_rate_hz = self->cfg_.format.sample_rate_hz;
            frame.capture_ts_us = now_us();
            frame.frame_seq = self->frame_seq_.fetch_add(frames);
            self->cb_(frame);
        }
        return paContinue;
    }

    plugins::AudioSourceConfig cfg_;
    AudioRingBuffer queue_;
    plugins::AudioFrameCallback cb_;
    std::vector<float> scratch_;
    PaStream* stream_ = nullptr;
    std::size_t quantum_frames_ = kDefaultQuantum;
    std::atomic<bool> open_ {false};
    std::atomic<bool> initialised_ {false};
    std::atomic<bool> device_lost_ {false};
    std::atomic<std::uint64_t> frame_seq_ {0};
    std::atomic<std::uint64_t> frames_processed_ {0};
    std::atomic<std::uint64_t> overruns_ {0};
    std::atomic<std::uint64_t> device_errors_ {0};
    std::atomic<std::uint64_t> device_restarts_ {0};
};

/**
 * @brief The PortAudio playback device.
 *
 * The callback pulls from the ring buffer.  When the queue is empty it
 * plays silence and counts an underrun — the played-sample counter
 * still advances, because the device really did consume those samples
 * (they were silence).
 */
class PortAudioSink final : public plugins::IAudioSink {
public:
    explicit PortAudioSink(plugins::AudioSinkConfig cfg) noexcept
        : cfg_(std::move(cfg)),
          queue_(queue_capacity(cfg_.queue_frames, cfg_.format.frames_per_buffer),
                 effective_channels(cfg_.format.channels)) {
        cfg_.format.sample_rate_hz = effective_rate(cfg_.format.sample_rate_hz);
        cfg_.format.channels = effective_channels(cfg_.format.channels);
        cfg_.format.frames_per_buffer =
            static_cast<std::uint32_t>(effective_quantum(cfg_.format.frames_per_buffer));
        quantum_frames_ = effective_quantum(cfg_.format.frames_per_buffer);
        scratch_.assign(quantum_frames_ * cfg_.format.channels, 0.0f);
    }

    ~PortAudioSink() override { close(); }

    const char* name() const noexcept override { return "portaudio-audio-sink"; }

    plugins::Status open() noexcept override {
        if (open_.load()) {
            return plugins::kOk;
        }
        if (Pa_Initialize() != paNoError) {
            device_errors_.fetch_add(1);
            return plugins::kErrHardwareError;
        }
        initialised_.store(true);

        PaDeviceIndex index = paNoDevice;
        if (!cfg_.device_id.empty()) {
            if (!parse_device_id(cfg_.device_id, &index) ||
                index == paNoDevice) {
                close();
                return plugins::kErrInvalidParam;
            }
        } else {
            index = default_output_device();
            if (index == paNoDevice) {
                close();
                return plugins::kErrHardwareError;
            }
        }

        const PaDeviceInfo* info = Pa_GetDeviceInfo(index);
        if (info == nullptr) {
            close();
            return plugins::kErrHardwareError;
        }

        const double rate = info->defaultSampleRate;
        if (rate > 0.0) {
            cfg_.format.sample_rate_hz = static_cast<std::uint32_t>(rate);
        }
        const int max_out = info->maxOutputChannels;
        if (max_out > 0 && cfg_.format.channels > static_cast<std::uint16_t>(max_out)) {
            cfg_.format.channels = static_cast<std::uint16_t>(max_out);
        }

        PaStreamParameters params;
        params.device = index;
        params.channelCount = cfg_.format.channels;
        params.sampleFormat = paFloat32;
        params.suggestedLatency = 0.0;
        params.hostApiSpecificStreamInfo = nullptr;

        PaError err = Pa_OpenStream(&stream_,
                                     nullptr,
                                     &params,
                                     cfg_.format.sample_rate_hz,
                                     static_cast<unsigned long>(quantum_frames_),
                                     paNoFlag,
                                     &PortAudioSink::pa_callback,
                                     this);
        if (err != paNoError) {
            stream_ = nullptr;
            close();
            return pa_status(err);
        }

        err = Pa_StartStream(stream_);
        if (err != paNoError) {
            Pa_CloseStream(stream_);
            stream_ = nullptr;
            close();
            return pa_status(err);
        }

        device_lost_.store(false);
        open_.store(true);
        return plugins::kOk;
    }

    void close() noexcept override {
        if (stream_ != nullptr) {
            Pa_AbortStream(stream_);
            Pa_CloseStream(stream_);
            stream_ = nullptr;
        }
        open_.store(false);
        if (initialised_.exchange(false)) {
            Pa_Terminate();
        }
    }

    [[nodiscard]] std::vector<plugins::AudioDevice>
    enumerate_devices() const noexcept override {
        return enumerate_portaudio_devices(true);
    }

    [[nodiscard]] std::vector<plugins::AudioDeviceFormat>
    supported_formats(const std::string& device_id) const noexcept override {
        PaDeviceIndex index = paNoDevice;
        if (!parse_device_id(device_id, &index)) {
            return {};
        }
        const PaDeviceInfo* info = Pa_GetDeviceInfo(index);
        if (info == nullptr || info->maxOutputChannels <= 0) {
            return {};
        }
        std::vector<plugins::AudioDeviceFormat> out;
        const double rate = info->defaultSampleRate;
        for (std::uint16_t ch = 1;
             ch <= static_cast<std::uint16_t>(info->maxOutputChannels); ++ch) {
            plugins::AudioDeviceFormat f;
            f.sample_rate_hz = rate > 0.0 ? static_cast<std::uint32_t>(rate) : 0;
            f.channels = ch;
            f.frames_per_buffer = static_cast<std::uint32_t>(kDefaultQuantum);
            f.format = plugins::AudioSampleFormat::kFloat32;
            out.push_back(f);
        }
        return out;
    }

    [[nodiscard]] plugins::AudioDeviceCapabilities
    capabilities() const noexcept override {
        plugins::AudioDeviceCapabilities c;
        c.capture = false;
        c.playback = true;
        c.enumeration = true;
        c.hot_plug = true;
        c.max_sample_rate_hz = 192000;
        c.min_frames_per_buffer = 16;
        c.max_frames_per_buffer = 8192;
        c.backend = "portaudio";
        return c;
    }

    [[nodiscard]] bool is_open() const noexcept override { return open_.load(); }

    [[nodiscard]] plugins::AudioSinkConfig config() const noexcept override {
        return cfg_;
    }

    plugins::Status write(const float* samples,
                           std::size_t num_samples,
                           std::uint16_t channels) noexcept override {
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
        return plugins::kOk;
    }

    [[nodiscard]] std::uint64_t played_samples() const noexcept override {
        return played_.load();
    }

    void set_on_playback_progress(
        plugins::AudioPlaybackCallback cb) noexcept override {
        progress_cb_ = std::move(cb);
    }

    [[nodiscard]] plugins::AudioDeviceStats stats() const noexcept override {
        plugins::AudioDeviceStats s;
        s.frames_processed = frames_processed_.load();
        s.frames_dropped = queue_.dropped_frames();
        s.underruns = underruns_.load();
        s.overruns = 0;
        s.device_errors = device_errors_.load();
        s.device_restarts = device_restarts_.load();
        s.bytes_copied = played_.load() *
                         static_cast<std::uint64_t>(cfg_.format.channels) *
                         sizeof(float);
        s.device_lost = device_lost_.load();
        return s;
    }

private:
    static int pa_callback(const void* input,
                           void* output,
                           unsigned long frame_count,
                           const PaStreamCallbackTimeInfo* time_info,
                           PaStreamCallbackFlags status_flags,
                           void* user_data) {
        auto* self = static_cast<PortAudioSink*>(user_data);
        (void)input;
        (void)time_info;

        if (output == nullptr) {
            self->device_lost_.store(true);
            self->device_errors_.fetch_add(1);
            return paContinue;
        }

        float* out = static_cast<float*>(output);
        const std::size_t frames = static_cast<std::size_t>(frame_count);

        if (status_flags & paOutputUnderflow) {
            self->underruns_.fetch_add(1);
        }

        // The device consumes exactly `frame_count` frames.  Whatever it
        // cannot get from the queue is played as silence — and it
        // STILL counts towards the played-sample total, because the
        // device really did consume it.  That is what makes the
        // counter an honest "what the loudspeaker saw" number.
        const std::size_t got = self->queue_.read(out, frames);
        if (got < frames) {
            std::memset(out + got * self->cfg_.format.channels,
                        0,
                        (frames - got) * self->cfg_.format.channels *
                            sizeof(float));
            self->underruns_.fetch_add(1);
        }

        self->frames_processed_.fetch_add(frames);
        const std::uint64_t total = self->played_.fetch_add(frames) + frames;
        if (self->progress_cb_) {
            self->progress_cb_(total);
        }
        return paContinue;
    }

    plugins::AudioSinkConfig cfg_;
    AudioRingBuffer queue_;
    plugins::AudioPlaybackCallback progress_cb_;
    std::vector<float> scratch_;
    PaStream* stream_ = nullptr;
    std::size_t quantum_frames_ = kDefaultQuantum;
    std::atomic<bool> open_ {false};
    std::atomic<bool> initialised_ {false};
    std::atomic<bool> device_lost_ {false};
    std::atomic<std::uint64_t> played_ {0};
    std::atomic<std::uint64_t> frames_processed_ {0};
    std::atomic<std::uint64_t> underruns_ {0};
    std::atomic<std::uint64_t> device_errors_ {0};
    std::atomic<std::uint64_t> device_restarts_ {0};
};

} // namespace

// ---------------------------------------------------------------------------
// The device enumeration (shared by both directions) is defined above, in
// the anonymous namespace, so it is available to both the factory and
// the module-level entry points.
// ---------------------------------------------------------------------------

plugins::IAudioSource* PortAudioSourceFactory::create(
    plugins::AudioSourceConfig cfg) const {
    return new PortAudioSource(std::move(cfg));
}

plugins::IAudioSink* PortAudioSinkFactory::create(
    plugins::AudioSinkConfig cfg) const {
    return new PortAudioSink(std::move(cfg));
}

std::vector<std::string> PortAudioSourceFactory::device_ids() const {
    std::vector<std::string> out;
    for (const auto& d : enumerate_portaudio_devices(false)) {
        out.push_back(d.id);
    }
    return out;
}

std::vector<std::string> PortAudioSinkFactory::device_ids() const {
    std::vector<std::string> out;
    for (const auto& d : enumerate_portaudio_devices(true)) {
        out.push_back(d.id);
    }
    return out;
}

#else  // !NIMRTC_USE_PORTAUDIO

// ---------------------------------------------------------------------------
// The "not compiled in" path.
//
// The factories still exist and still answer honestly: the device list
// is empty, the capabilities report "no capture / no playback", and
// `open()` fails with a clean error.  A host that wants a device on a
// build without PortAudio gets a clear "no backend" rather than a
// link error.
// ---------------------------------------------------------------------------

namespace {

plugins::IAudioSource* unavailable_source(plugins::AudioSourceConfig) {
    return nullptr;
}

plugins::IAudioSink* unavailable_sink(plugins::AudioSinkConfig) {
    return nullptr;
}

} // namespace

plugins::IAudioSource* PortAudioSourceFactory::create(
    plugins::AudioSourceConfig cfg) const {
    return unavailable_source(std::move(cfg));
}

plugins::IAudioSink* PortAudioSinkFactory::create(
    plugins::AudioSinkConfig cfg) const {
    return unavailable_sink(std::move(cfg));
}

std::vector<std::string> PortAudioSourceFactory::device_ids() const {
    return {};
}

std::vector<std::string> PortAudioSinkFactory::device_ids() const {
    return {};
}

#endif  // NIMRTC_USE_PORTAUDIO

// ---------------------------------------------------------------------------
// Module-level enumeration
// ---------------------------------------------------------------------------

std::vector<plugins::AudioDevice> enumerate_capture_devices() noexcept {
#if NIMRTC_USE_PORTAUDIO
    return enumerate_portaudio_devices(false);
#else
    return {};
#endif
}

std::vector<plugins::AudioDevice> enumerate_playback_devices() noexcept {
#if NIMRTC_USE_PORTAUDIO
    return enumerate_portaudio_devices(true);
#else
    return {};
#endif
}

} // namespace nimrtc::audio_device
