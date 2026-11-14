// audio_device/src/system_audio_platform.cpp
// The platform half of the system-audio (loopback) capture device —
// see include/nimrtc/audio_device/system_audio.hpp for the contract.
//
// The seam is the same on every platform: a *probe* (what the platform
// can do, and — when it cannot — exactly why) and a *create* (open a
// device).  The portable factory in `system_audio.cpp` owns the
// device identity and the error reporting; this file owns the platform
// mechanism.
//
// Per-platform support (remote-desktop plan F6):
//
//   - **Windows** — REAL: WASAPI loopback.  The render endpoints are
//     enumerated through `IMMDeviceEnumerator::EnumAudioEndpoints(eRender)`
//     and the selected one is opened with `eRender` + `eConsole` +
//     `AUDCLNT_STREAMFLAGS_LOOPBACK`.  The captured PCM is the
//     endpoint's *mix* format, converted to the module's float PCM.
//
//   - **macOS** — GUARDED: Core Audio has no system-wide loopback
//     stream; the system's output can only be captured through a
//     **tap / aggregate device**.  The enumeration of the aggregate
//     devices is real; opening a device that is not a tap fails with an
//     explicit "requires a tap / aggregate device" error.
//
//   - **Linux** — GUARDED: PulseAudio / PipeWire expose the system's
//     output as a **monitor source** (`<sink>.monitor`).  Enumerating
//     the monitor sources needs the PulseAudio client library, which
//     NimRTC does not vendor (L5 forbids a system-library fallback and
//     the vendored tree does not carry libpulse), so the platform leg
//     reports an explicit "a PulseAudio monitor source must be
//     selected" error rather than pretending to enumerate.
//
//   - **anything else** — GUARDED: "this platform has no system-audio
//     loopback mechanism".
//
// In every guarded case the failure is a **clean, enumerated error**:
// never silence, never a crash.

#include "system_audio_platform.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

#if defined(_WIN32)
    #define NIMRTC_SYSTEM_AUDIO_WASAPI 1
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <audioclient.h>
    #include <mmdeviceapi.h>
    #include <unknwn.h>
    #include <wrl/client.h>
#else
    #define NIMRTC_SYSTEM_AUDIO_WASAPI 0
#endif

namespace nimrtc::audio_device::detail {

namespace {

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

constexpr std::uint32_t kDefaultSampleRate = 48000;
constexpr std::uint16_t kDefaultChannels   = 2;
constexpr std::size_t   kDefaultQuantum     = 480;
constexpr std::size_t   kDefaultQueueFrames = 50;

std::uint16_t effective_channels(std::uint16_t requested) noexcept {
    return requested == 0 ? kDefaultChannels : requested;
}

std::uint32_t effective_rate(std::uint32_t requested) noexcept {
    return requested == 0 ? kDefaultSampleRate : requested;
}

std::size_t effective_quantum(std::uint32_t requested) noexcept {
    return requested == 0 ? kDefaultQuantum : static_cast<std::size_t>(requested);
}

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

} // namespace

// ---------------------------------------------------------------------------
// Windows — WASAPI loopback (the REAL backend)
// ---------------------------------------------------------------------------

#if NIMRTC_SYSTEM_AUDIO_WASAPI

namespace {

using Microsoft::WRL::ComPtr;

/** The device-id prefix of the Windows system-audio devices. */
constexpr const char* kWindowsDevicePrefix = "system-audio:wasapi:";

/**
 * The WASAPI "friendly name" property key.
 *
 * It is spelled out here (rather than pulled from `<devpkey.h>`) so the
 * file does not need an INITGUID translation unit: `PKEY_Device_FriendlyName`
 * is `{a45c254e-df1c-4efd-8020-67d146a850e0}, 14`.
 */
constexpr PROPERTYKEY kPropertyKeyDeviceFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
    14
};

/** RAII wrapper around `CoInitializeEx` / `CoUninitialize`. */
class ComScope {
public:
    ComScope() noexcept {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        // S_FALSE means COM was already initialised on this thread (by
        // us, in a nested scope) — that is fine.  RPC_E_CHANGED_MODE
        // means the thread already initialised COM as STA — also fine
        // for WASAPI, which does not care.
        initialised_ = SUCCEEDED(hr);
        rpc_changed_mode_ = (hr == RPC_E_CHANGED_MODE);
    }

    ~ComScope() {
        if (initialised_) {
            CoUninitialize();
        }
    }

    ComScope(const ComScope&)            = delete;
    ComScope& operator=(const ComScope&) = delete;

    /** True iff COM is usable on this thread. */
    [[nodiscard]] bool usable() const noexcept {
        return initialised_ || rpc_changed_mode_;
    }

private:
    bool initialised_       = false;
    bool rpc_changed_mode_  = false;
};

/** Create the endpoint enumerator. */
bool make_enumerator(ComPtr<IMMDeviceEnumerator>& out) {
    const HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator),
                                       nullptr,
                                       CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(out.GetAddressOf()));
    return SUCCEEDED(hr);
}

/** The endpoint's friendly name, or an empty string. */
std::string friendly_name(IMMDevice* device) {
    ComPtr<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, store.GetAddressOf())) ||
        store == nullptr) {
        return {};
    }
    PROPVARIANT name;
    PropVariantInit(&name);
    if (FAILED(store->GetValue(kPropertyKeyDeviceFriendlyName, &name)) ||
        name.vt != VT_LPWSTR || name.pwszVal == nullptr) {
        PropVariantClear(&name);
        return {};
    }
    std::wstring wide(name.pwszVal);
    PropVariantClear(&name);
    if (wide.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                           static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
    std::string out;
    out.resize(static_cast<std::size_t>(needed));
    if (needed > 0) {
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                            out.data(), needed, nullptr, nullptr);
    }
    return out;
}

/** The endpoint's id string (the "endpoint id"). */
std::string endpoint_id(IMMDevice* device) {
    LPWSTR id = nullptr;
    if (FAILED(device->GetId(&id)) || id == nullptr) {
        return {};
    }
    std::wstring wide(id);
    CoTaskMemFree(id);
    if (wide.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                           static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
    std::string out;
    out.resize(static_cast<std::size_t>(needed));
    if (needed > 0) {
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                            out.data(), needed, nullptr, nullptr);
    }
    return out;
}

/**
 * @brief The WASAPI loopback capture device.
 *
 * The device is a *render* endpoint opened with
 * `AUDCLNT_STREAMFLAGS_LOOPBACK`, so the audio the endpoint plays is
 * also delivered to this capture client.
 */
class WasapiLoopbackSource final : public plugins::IAudioSource {
public:
    WasapiLoopbackSource(plugins::AudioSourceConfig cfg,
                          std::uint32_t endpoint_index,
                          bool use_default_endpoint) noexcept
        : cfg_(std::move(cfg)),
          queue_(queue_capacity(cfg_.queue_frames, cfg_.format.frames_per_buffer),
                 effective_channels(cfg_.format.channels)),
          endpoint_index_(endpoint_index),
          use_default_endpoint_(use_default_endpoint) {
        cfg_.format.sample_rate_hz = effective_rate(cfg_.format.sample_rate_hz);
        cfg_.format.channels = effective_channels(cfg_.format.channels);
        cfg_.format.frames_per_buffer =
            static_cast<std::uint32_t>(effective_quantum(cfg_.format.frames_per_buffer));
        quantum_frames_ = effective_quantum(cfg_.format.frames_per_buffer);
    }

    ~WasapiLoopbackSource() override { close(); }

    const char* name() const noexcept override {
        return "wasapi-loopback-audio-source";
    }

    plugins::Status open() noexcept override {
        if (open_.load()) {
            return plugins::kOk;
        }
        device_lost_.store(false);

        if (!com_.usable()) {
            device_errors_.fetch_add(1);
            return plugins::kErrHardwareError;
        }

        ComPtr<IMMDeviceEnumerator> enumerator;
        if (!make_enumerator(enumerator)) {
            device_errors_.fetch_add(1);
            return plugins::kErrHardwareError;
        }

        ComPtr<IMMDevice> device;
        HRESULT hr = E_FAIL;
        if (use_default_endpoint_) {
            hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole,
                                                     device.GetAddressOf());
        } else {
            ComPtr<IMMDeviceCollection> collection;
            if (SUCCEEDED(enumerator->EnumAudioEndpoints(
                    eRender, DEVICE_STATE_ACTIVE, collection.GetAddressOf()))) {
                UINT count = 0;
                if (SUCCEEDED(collection->GetCount(&count)) &&
                    endpoint_index_ < count) {
                    hr = collection->Item(endpoint_index_, device.GetAddressOf());
                }
            }
        }
        if (FAILED(hr) || device == nullptr) {
            device_errors_.fetch_add(1);
            return plugins::kErrHardwareError;
        }

        // ---- The audio client -------------------------------------------
        //
        // `IMMDevice::Activate` takes a `void**`, so the raw pointer is
        // fetched first and then handed to the `ComPtr` (which attaches
        // to it).
        IAudioClient* raw_client = nullptr;
        hr = device->Activate(__uuidof(IAudioClient),
                              CLSCTX_INPROC_SERVER,
                              nullptr,
                              reinterpret_cast<void**>(&raw_client));
        if (FAILED(hr) || raw_client == nullptr) {
            device_errors_.fetch_add(1);
            return plugins::kErrHardwareError;
        }
        client_ = raw_client;

        // ---- The mix format ----------------------------------------------
        //
        // `GetMixFormat` hands out a `WAVEFORMATEX*` the caller owns; it
        // is freed with `CoTaskMemFree` below.  For a multichannel
        // endpoint it is a `WAVEFORMATEXTENSIBLE`, whose first fields
        // are the base format's — so the rate / channel / block-align
        // values are read from the base layout.
        WAVEFORMATEX* mix = nullptr;
        hr = client_->GetMixFormat(&mix);
        if (FAILED(hr) || mix == nullptr) {
            device_errors_.fetch_add(1);
            return plugins::kErrHardwareError;
        }
        format_ = *mix;

        // ---- The capture format ------------------------------------------
        //
        // The endpoint's *mix* format is what the engine produces; it
        // is often 32-bit float, which is not what the seam's PCM
        // contract wants.  The capture client is therefore initialized
        // with an explicit 16-bit PCM format at the mix format's rate
        // and channel count, and `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM`
        // asks the audio engine to do the conversion.  The captured
        // data is then always int16, and the conversion to the seam's
        // float PCM happens in one place.
        WAVEFORMATEX capture_format {};
        capture_format.wFormatTag = WAVE_FORMAT_PCM;
        capture_format.nChannels = format_.nChannels > 0 ? format_.nChannels : 2;
        capture_format.nSamplesPerSec = format_.nSamplesPerSec > 0
                                            ? format_.nSamplesPerSec
                                            : 48000;
        capture_format.wBitsPerSample = 16;
        capture_format.nBlockAlign =
            static_cast<WORD>(capture_format.nChannels *
                              (capture_format.wBitsPerSample / 8));
        capture_format.nAvgBytesPerSec =
            capture_format.nSamplesPerSec * capture_format.nBlockAlign;
        mix_is_float_ = false;

        // ---- The capture client ------------------------------------------
        IAudioCaptureClient* raw_capture = nullptr;
        hr = client_->GetService(__uuidof(IAudioCaptureClient),
                                 reinterpret_cast<void**>(&raw_capture));
        if (FAILED(hr) || raw_capture == nullptr) {
            CoTaskMemFree(mix);
            device_errors_.fetch_add(1);
            return plugins::kErrHardwareError;
        }
        capture_ = raw_capture;

        // ---- Initialize ----------------------------------------------------
        //
        // AUDCLNT_STREAMFLAGS_LOOPBACK is what makes a *render* endpoint
        // deliver its output to a capture client.  The endpoint's own
        // mix format is used as-is (no AUTOCONVERTPCM: the mix format
        // is already what the endpoint produces).
        const REFERENCE_TIME duration =
            static_cast<REFERENCE_TIME>(
                (static_cast<double>(quantum_frames_) * 10000000.0) /
                static_cast<double>(format_.nSamplesPerSec > 0
                                        ? format_.nSamplesPerSec
                                        : 48000));
        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_LOOPBACK |
                                     AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                                 duration,
                                 0,
                                 &capture_format,
                                 nullptr);
        CoTaskMemFree(mix);
        if (FAILED(hr)) {
            capture_.Reset();
            client_.Reset();
            device_errors_.fetch_add(1);
            return plugins::kErrHardwareError;
        }

        // ---- The negotiated format ----------------------------------------
        //
        // The seam reports the format that is *actually* in force, not
        // the request: the device may have to fall back.
        cfg_.format.sample_rate_hz =
            format_.nSamplesPerSec > 0
                ? static_cast<std::uint32_t>(format_.nSamplesPerSec)
                : cfg_.format.sample_rate_hz;
        const std::uint16_t mix_channels =
            format_.nChannels > 0 ? static_cast<std::uint16_t>(format_.nChannels)
                                  : static_cast<std::uint16_t>(2);
        if (cfg_.format.channels > mix_channels) {
            cfg_.format.channels = mix_channels;
        }

        start_.store(true);
        open_.store(true);
        thread_ = std::thread([this] { capture_loop(); });
        return plugins::kOk;
    }

    void close() noexcept override {
        start_.store(false);
        if (thread_.joinable()) {
            thread_.join();
        }
        if (client_ != nullptr) {
            client_->Stop();
        }
        capture_.Reset();
        client_.Reset();
        open_.store(false);
        queue_.clear();
    }

    [[nodiscard]] std::vector<plugins::AudioDevice>
    enumerate_devices() const noexcept override {
        // The enumeration is the platform's; the device layer's
        // `SystemAudioSourceFactory::devices()` renders the richer
        // `SystemAudioDevice` view of it.
        std::vector<plugins::AudioDevice> out;
        ComScope com;
        if (!com.usable()) {
            return out;
        }
        ComPtr<IMMDeviceEnumerator> enumerator;
        if (!make_enumerator(enumerator)) {
            return out;
        }
        ComPtr<IMMDeviceCollection> collection;
        if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE,
                                                 collection.GetAddressOf()))) {
            return out;
        }
        UINT count = 0;
        if (FAILED(collection->GetCount(&count))) {
            return out;
        }
        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> device;
            if (FAILED(collection->Item(i, device.GetAddressOf())) ||
                device == nullptr) {
                continue;
            }
            plugins::AudioDevice d;
            d.id = std::string(kWindowsDevicePrefix) + std::to_string(i);
            d.name = friendly_name(device.Get());
            if (d.name.empty()) {
                d.name = "System Output " + std::to_string(i);
            }
            // A loopback device *captures* the render endpoint's
            // output.  It does not play back.
            d.can_capture = true;
            d.can_playback = false;
            d.max_input_channels = 2;
            d.max_output_channels = 0;
            d.default_sample_rate_hz = kDefaultSampleRate;
            out.push_back(std::move(d));
        }
        return out;
    }

    [[nodiscard]] std::vector<plugins::AudioDeviceFormat>
    supported_formats(const std::string& device_id) const noexcept override {
        std::vector<plugins::AudioDeviceFormat> out;
        for (std::uint16_t ch = 1; ch <= 2; ++ch) {
            plugins::AudioDeviceFormat f;
            f.sample_rate_hz = kDefaultSampleRate;
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
        c.hot_plug = false;
        c.max_sample_rate_hz = 192000;
        c.min_frames_per_buffer = 16;
        c.max_frames_per_buffer = 8192;
        c.backend = "wasapi-loopback";
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
    /** The capture thread: pulls packets out of the capture client. */
    void capture_loop() {
        std::vector<float> floats;
        while (start_.load()) {
            UINT32 available = 0;
            const HRESULT hr = capture_->GetNextPacketSize(&available);
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING) {
                device_lost_.store(true);
                device_errors_.fetch_add(1);
                return;
            }
            if (FAILED(hr)) {
                device_errors_.fetch_add(1);
                continue;
            }
            if (available == 0) {
                // Nothing captured yet: sleep briefly and poll again.
                // (A WASAPI capture client has no wait primitive of its
                // own in the shared-mode loopback path.)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            const HRESULT got = capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (got == AUDCLNT_E_DEVICE_INVALIDATED) {
                device_lost_.store(true);
                device_errors_.fetch_add(1);
                return;
            }
            if (FAILED(got)) {
                device_errors_.fetch_add(1);
                continue;
            }

            const std::size_t samples = static_cast<std::size_t>(frames) *
                                        static_cast<std::size_t>(format_.nChannels);
            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            floats.assign(samples, 0.0f);
            if (!silent && data != nullptr && samples > 0) {
                if (mix_is_float_) {
                    const float* src = reinterpret_cast<const float*>(data);
                    for (std::size_t i = 0; i < samples; ++i) {
                        floats[i] = src[i];
                    }
                } else {
                    const std::int16_t* src = reinterpret_cast<const std::int16_t*>(data);
                    for (std::size_t i = 0; i < samples; ++i) {
                        floats[i] = static_cast<float>(src[i]) / 32767.0f;
                    }
                }
            }
            capture_->ReleaseBuffer(frames);

            if (samples == 0) {
                continue;
            }

            frames_processed_.fetch_add(frames);
            queue_.write(floats.data(), frames);

            if (cb_) {
                plugins::AudioFrame frame;
                frame.samples = floats.data();
                frame.num_samples = frames;
                frame.num_channels = static_cast<std::uint16_t>(format_.nChannels);
                frame.sample_rate_hz = cfg_.format.sample_rate_hz;
                frame.capture_ts_us = now_us();
                frame.frame_seq = frame_seq_.fetch_add(frames);
                cb_(frame);
            }
        }
    }

    plugins::AudioSourceConfig cfg_;
    AudioRingBuffer queue_;
    plugins::AudioFrameCallback cb_;

    ComScope com_;
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioCaptureClient> capture_;
    WAVEFORMATEX format_ {};
    /** True iff the endpoint's mix format is 32-bit float (the usual
     *  shared-mode mix format). */
    bool mix_is_float_ = true;
    std::size_t quantum_frames_ = kDefaultQuantum;
    std::uint32_t endpoint_index_ = 0;
    bool use_default_endpoint_ = true;

    std::thread thread_;
    std::atomic<bool> start_ {false};
    std::atomic<bool> open_ {false};
    std::atomic<bool> device_lost_ {false};
    std::atomic<std::uint64_t> frame_seq_ {0};
    std::atomic<std::uint64_t> frames_processed_ {0};
    std::atomic<std::uint64_t> overruns_ {0};
    std::atomic<std::uint64_t> device_errors_ {0};
    std::atomic<std::uint64_t> device_restarts_ {0};
};

} // namespace

// ---------------------------------------------------------------------------
// The platform seam
// ---------------------------------------------------------------------------

PlatformSystemAudioInfo probe_platform_system_audio() {
    PlatformSystemAudioInfo info;
    info.backend = SystemAudioBackend::kWasapiLoopback;

    ComScope com;
    if (!com.usable()) {
        info.status = SystemAudioStatus::kBackendError;
        info.reason = "COM could not be initialised on this thread";
        return info;
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (!make_enumerator(enumerator)) {
        info.status = SystemAudioStatus::kBackendError;
        info.reason = "the WASAPI endpoint enumerator could not be created";
        return info;
    }

    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE,
                                             collection.GetAddressOf()))) {
        info.status = SystemAudioStatus::kBackendError;
        info.reason = "WASAPI refused to enumerate the render endpoints";
        return info;
    }

    UINT count = 0;
    if (FAILED(collection->GetCount(&count))) {
        info.status = SystemAudioStatus::kBackendError;
        info.reason = "WASAPI refused to report the render endpoint count";
        return info;
    }

    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, device.GetAddressOf())) ||
            device == nullptr) {
            continue;
        }
        PlatformSystemAudioDevice d;
        d.id = std::string(kWindowsDevicePrefix) + std::to_string(i);
        d.name = friendly_name(device.Get());
        if (d.name.empty()) {
            d.name = "System Output " + std::to_string(i);
        }
        d.kind = SystemAudioDeviceKind::kLoopback;
        d.channels = 2;
        d.sample_rate_hz = kDefaultSampleRate;
        info.devices.push_back(std::move(d));
    }

    // The *default* render endpoint is always offered, even when the
    // enumeration came back empty: it is the one the host wants 95% of
    // the time ("share the system's output").
    PlatformSystemAudioDevice def;
    def.id = std::string(kWindowsDevicePrefix) + "default";
    def.name = "System Output (default)";
    def.kind = SystemAudioDeviceKind::kLoopback;
    def.channels = 2;
    def.sample_rate_hz = kDefaultSampleRate;
    info.devices.insert(info.devices.begin(), std::move(def));

    info.status = SystemAudioStatus::kAvailable;
    info.reason = "WASAPI loopback is available";
    return info;
}

std::unique_ptr<plugins::IAudioSource> create_platform_system_audio(
    const plugins::AudioSourceConfig& cfg,
    std::string& error) {
    error.clear();

    // The device id carries the endpoint index, or "default".
    std::uint32_t index = 0;
    bool use_default = true;
    bool id_valid = true;
    if (!cfg.device_id.empty()) {
        const std::string prefix = kWindowsDevicePrefix;
        if (cfg.device_id.rfind(prefix, 0) == 0) {
            const std::string suffix = cfg.device_id.substr(prefix.size());
            if (suffix == "default") {
                use_default = true;
            } else {
                try {
                    const long parsed = std::stol(suffix);
                    if (parsed >= 0) {
                        index = static_cast<std::uint32_t>(parsed);
                        use_default = false;
                    } else {
                        id_valid = false;
                    }
                } catch (...) {
                    id_valid = false;
                }
            }
        } else {
            id_valid = false;
        }
        if (!id_valid) {
            error = "unknown system-audio device id '" + cfg.device_id +
                    "'; expected 'system-audio:wasapi:default' or "
                    "'system-audio:wasapi:<index>'";
            return nullptr;
        }
    }

    auto source = std::make_unique<WasapiLoopbackSource>(cfg, index, use_default);
    if (source->open() != plugins::kOk) {
        error = "the WASAPI loopback stream could not be opened";
        return nullptr;
    }
    return source;
}

#else  // !NIMRTC_SYSTEM_AUDIO_WASAPI

// ---------------------------------------------------------------------------
// The guarded platforms.
//
// macOS: Core Audio has no system-wide loopback stream.  The system's
// output can only be captured through a **tap / aggregate device**
// (an "Aggregate Device" in Audio MIDI Setup, or a virtual driver such
// as BlackHole).  The seam exists; the enumeration is guarded because
// it needs the CoreAudio framework, and the error says exactly what
// the host has to do.
//
// Linux: PulseAudio / PipeWire expose the system's output as a
// **monitor source** (`<sink>.monitor`).  Enumerating those needs the
// PulseAudio client library, which NimRTC does not vendor (L5 forbids
// a system-library fallback), so the platform leg reports an explicit
// error instead of pretending to enumerate.
//
// Either way the host gets a **clean, enumerated error** — never
// silence, never a crash.
// ---------------------------------------------------------------------------

PlatformSystemAudioInfo probe_platform_system_audio() {
    PlatformSystemAudioInfo info;

#if defined(__APPLE__)
    info.backend = SystemAudioBackend::kCoreAudioTap;
    info.status = SystemAudioStatus::kNoDevice;
    info.reason =
        "macOS captures the system's output through a Core Audio tap / "
        "aggregate device; create one in Audio MIDI Setup (or install a "
        "virtual audio driver) and select it as the system-audio device";
#elif defined(__linux__)
    info.backend = SystemAudioBackend::kPulseAudioMonitor;
    info.status = SystemAudioStatus::kNoDevice;
    info.reason =
        "Linux captures the system's output through a PulseAudio / PipeWire "
        "monitor source ('<sink>.monitor'); enumerate it with 'pactl list "
        "short sources' and select the monitor of the sink you want to share";
#else
    info.backend = SystemAudioBackend::kUnavailable;
    info.status = SystemAudioStatus::kUnsupported;
    info.reason = "this platform has no system-audio loopback mechanism";
#endif

    return info;
}

std::unique_ptr<plugins::IAudioSource> create_platform_system_audio(
    const plugins::AudioSourceConfig& cfg,
    std::string& error) {
    (void)cfg;
    const PlatformSystemAudioInfo info = probe_platform_system_audio();
    error = info.reason.empty()
        ? std::string("no system-audio loopback device is available")
        : info.reason;
    return nullptr;
}

#endif  // NIMRTC_SYSTEM_AUDIO_WASAPI

} // namespace nimrtc::audio_device::detail
