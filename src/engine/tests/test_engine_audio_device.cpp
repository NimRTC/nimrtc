/**
 * @file src/engine/tests/test_engine_audio_device.cpp
 * @brief v1-audio-device-engine (D1) — the engine-driven audio device layer.
 *
 * Pins acceptance criteria 1–14 and 22 of
 * `specs/slices/v1-audio-device-engine.md`:
 *
 *   | #  | criterion                                        | test(s)                        |
 *   |----|--------------------------------------------------|--------------------------------|
 *   | 1  | registry has two audio device slots              | RegistrySlotsReturnSamePointer |
 *   | 2  | the module registers its backends                | AudioDeviceModuleRegistersSynthetic |
 *   | 3  | registry and module factories are the same       | PalResolverMatchesModuleFactories |
 *   | 4  | Category values are untouched                    | CategoryValuesUnchanged        |
 *   | 5  | empty `audio_source_name` keeps today's behavior | EmptyAudioDeviceNamesKeepTodayBehavior |
 *   | 6  | "auto" prefers portaudio, falls back to synthetic| AutoResolutionPrefersPortAudioWhenRegistered / AutoResolutionFallsBackToSyntheticWhenPortAudioAbsent |
 *   | 6b | "auto" rule accepts an injected `is_registered`  | (the two AutoResolution* tests) |
 *   | 6c | unresolved "auto" degrades, does not error       | AutoResolutionWithNothingRegisteredDegradesToEmpty |
 *   | 7  | "none" is the explicit opt-out                   | NoneIsExplicitOptOut           |
 *   | 8  | the engine drives capture into the outbound path | EnginePumpsCaptureIntoOutboundPath |
 *   | 9  | decoded PCM is written into the sink             | EngineWritesDecodedPcmIntoSink |
 *   | 10 | 3A reference uses played_samples()               | RenderDeliveredUsesPlayedDeltaWithSink / RenderDeliveredUsesFrameSamplesWithoutSink |
 *   | 11 | "auto" open failure degrades                     | AutoOpenFailureDegrades        |
 *   | 12 | an explicit id that cannot open fails open()     | ExplicitIdOpenFailureFailsOpen |
 *   | 13 | Stats reports the *negotiated* device ids        | StatsReportsNegotiatedDeviceIds |
 *   | 14 | the pump counters exist and move                 | EnginePumpsCaptureIntoOutboundPath / PumpErrorsIncrementOnDeviceReadFailure |
 *   | 22 | no sound card → "auto" resolves to synthetic     | AutoResolutionFallsBackToSyntheticWhenPortAudioAbsent |
 *
 * ## Why the fake transport / DTLS / codec set
 *
 * `send_audio()` refuses to emit RTP before the DTLS handshake completed,
 * and the pump feeds `send_audio()`.  The tests therefore drive the engine
 * through a fake ICE transport and a fake DTLS session (the same pattern
 * as `tests/test_engine_audio_alloc.cpp`): the fake DTLS reports the
 * handshake as connected and exports no SRTP keying material, so the RTP
 * travels in cleartext and the fake ICE transport counts it.
 *
 * ## Why the module registers itself
 *
 * `nimrtc::core::register_all_default_plugins()` iterates
 * `detail::kDefaultRegistrars[]`; the audio-device entry is guarded by
 * `NIMRTC_HAS_AUDIO_DEVICE`, which is not defined for
 * `nimrtc_core_objects`, so the entry is inert.  The tests call
 * `nimrtc::audio_device::register_default_plugins()` directly — the
 * documented "call the module-specific entry point directly" pattern.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/dtls/dtls_session_factory.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/engine/pal/engine_plugin_resolver.hpp>
#include <nimrtc/plugins/audio3a.hpp>
#include <nimrtc/plugins/audio_decoder.hpp>
#include <nimrtc/plugins/audio_receiver.hpp>
#include <nimrtc/plugins/audio_sink.hpp>
#include <nimrtc/plugins/audio_source.hpp>
#include <nimrtc/plugins/codec.hpp>
#include <nimrtc/plugins/ice_transport.hpp>

#include "engine_device_policy.hpp"  // detail::resolve_auto_audio_backend

// The engine test reaches the audio_device module's *entry points* (its
// unified registrar and its two factory lookups) without including the
// module header -- Layout Invariant 4 / ADR-025: the engine tree may
// include only the enumerated exemption list, and `audio_device` is not
// on it.  The symbols are resolvable at link time because
// `nimrtc_engine` PRIVATE-links the module; the declarations below just
// give the TU their prototypes.
namespace nimrtc::audio_device {
void register_default_plugins() noexcept;
[[nodiscard]] const plugins::IAudioSourceFactory*
source_factory(std::string_view id) noexcept;
[[nodiscard]] const plugins::IAudioSinkFactory*
sink_factory(std::string_view id) noexcept;
}  // namespace nimrtc::audio_device

namespace {

namespace np = nimrtc::plugins;
namespace nd = nimrtc::dtls;

using nimrtc::core::PluginRegistry;
using nimrtc::core::detail::Registrar;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;
using nimrtc::engine::detail::resolve_auto_audio_backend;

/// One 20 ms frame of mono PCM at 48 kHz — exactly one RTP packet.
constexpr std::size_t kFrameSamples = 48000 / 50;

// ---------------------------------------------------------------------------
// Fake ICE transport (the audio_alloc pattern)
// ---------------------------------------------------------------------------

class FakeIceTransport final : public np::IICETransport {
public:
    const char* name() const noexcept override { return "test-fake-ice"; }
    np::Status open() noexcept override { return np::kOk; }
    void close() noexcept override {}

    void set_callbacks(np::RecvCallback on_recv,
                       np::ErrorCallback on_error) noexcept override {
        on_recv_  = std::move(on_recv);
        on_error_ = std::move(on_error);
    }

    np::Status send(np::BufferView view, np::Addr = {}) noexcept override {
        sent_bytes_ += view.size();
        ++sent_packets_;
        return np::kOk;
    }

    std::size_t sent_bytes() const noexcept { return sent_bytes_; }
    std::size_t sent_packets() const noexcept { return sent_packets_; }

    int recv() noexcept override {
        int n = 0;
        while (!inbound_.empty()) {
            std::vector<std::uint8_t> pkt = std::move(inbound_.front());
            inbound_.erase(inbound_.begin());
            if (on_recv_) on_recv_(np::BufferView(pkt.data(), pkt.size()));
            ++n;
        }
        return n;
    }

    np::Addr local_addr() const noexcept override { return {}; }
    np::Addr remote_addr() const noexcept override { return {}; }

    np::IceState state() const noexcept override { return np::IceState::Connected; }
    std::string local_ufrag() const noexcept override { return "testufrag"; }
    std::string local_password() const noexcept override { return "testpassword"; }
    bool wait_for_gathering(int) noexcept override { return true; }
    std::vector<std::string> gathered_local_candidates() const noexcept override {
        return {};
    }
    np::Status set_remote_description(std::string_view) noexcept override {
        return np::kOk;
    }
    np::Status add_remote_candidate(std::string_view) noexcept override {
        return np::kOk;
    }
    void set_bind_address(std::string_view) noexcept override {}
    void set_stun_server(std::string_view, std::uint16_t) noexcept override {}
    void set_local_port_range(std::uint16_t, std::uint16_t) noexcept override {}
    void add_turn_server(std::string_view, std::uint16_t, std::string_view,
                         std::string_view) noexcept override {}
    void set_bwe(np::IBwe*) noexcept override {}
    void set_scheduler(np::IScheduler*) noexcept override {}

private:
    np::RecvCallback  on_recv_;
    np::ErrorCallback on_error_;
    std::vector<std::vector<std::uint8_t>> inbound_;
    std::size_t sent_bytes_ = 0;
    std::size_t sent_packets_ = 0;
};

class FakeIceFactory final : public np::IICETransportFactory {
public:
    std::string_view id() const noexcept override { return "test-fake-ice"; }
    std::string_view display_name() const noexcept override {
        return "Fake ICE transport (audio-device test)";
    }
    np::IICETransport* create_ice() const override { return new FakeIceTransport(); }
};

// ---------------------------------------------------------------------------
// Fake DTLS session (the audio_alloc pattern)
// ---------------------------------------------------------------------------

class FakeDtlsSession final : public nd::IDtlsSession {
public:
    void set_role(nd::DtlsRole) noexcept override {}
    void set_peer_fingerprint(std::span<const std::uint8_t>) noexcept override {}
    void start() noexcept override {}
    void pump() noexcept override {}
    void on_handshake_complete(nd::IDtlsSession::OnCompleteCb) noexcept override {}
    np::Status export_srtp_key_material(std::span<std::uint8_t, 60>) noexcept override {
        return np::kErrNotReady;
    }

    nimrtc::core::Result<void> open() noexcept override {
        return nimrtc::core::Result<void>::make_ok();
    }
    void set_role(nd::Role) noexcept override {}
    void set_peer_fingerprint(std::string, std::vector<std::uint8_t>) noexcept override {}
    std::size_t feed_inbound(std::span<const std::uint8_t>,
                             const nd::DtlsAddr&) noexcept override {
        return 0;
    }
    std::vector<nd::DtlsRecord> take_outbound() noexcept override { return {}; }
    void tick() noexcept override {}
    nd::DtlsState state() const noexcept override { return nd::DtlsState::Connected; }
    bool is_connected() const noexcept override { return true; }
    const nd::Fingerprint& local_fingerprint() const noexcept override {
        return fingerprint_;
    }
    std::optional<nd::SrtpKeyingMaterial>
    srtp_keying_material() const noexcept override {
        return std::nullopt;
    }

private:
    nd::Fingerprint fingerprint_{"sha-256", {}, {}, ""};
};

class FakeDtlsFactory final : public nd::IDtlsSessionFactory {
public:
    std::string_view id() const noexcept override { return "test-fake-dtls"; }
    std::string_view display_name() const noexcept override {
        return "Fake DTLS session (audio-device test)";
    }
    std::unique_ptr<nd::IDtlsSession> create(const nd::DtlsConfig&) const override {
        return std::make_unique<FakeDtlsSession>();
    }
};

// ---------------------------------------------------------------------------
// Fake audio codec
// ---------------------------------------------------------------------------

class FakeAudioCodec final : public np::ICodec {
public:
    explicit FakeAudioCodec(np::CodecConfig cfg) : cfg_(cfg) {}
    const char* name() const noexcept override { return "FakeAudioCodec"; }
    np::Status open() noexcept override { return np::kOk; }
    void close() noexcept override {}
    std::size_t encode(const float*, std::size_t, std::uint8_t* output,
                       std::size_t output_capacity) noexcept override {
        if (output_capacity < 2) return 0;
        output[0] = 0xF8;  // Opus TOC: 48 kHz, mono, 20 ms
        output[1] = 0xFF;
        ++frames_encoded_;
        return 2;
    }
    std::size_t decode(const std::uint8_t*, std::size_t, float*,
                       std::size_t) noexcept override {
        return 0;
    }
    std::size_t decode_plc(float*, std::size_t) noexcept override { return 0; }
    np::CodecConfig config() const noexcept override { return cfg_; }
    np::Status update_config(np::CodecConfig cfg) noexcept override {
        cfg_ = cfg;
        return np::kOk;
    }
    np::CodecStats stats() const noexcept override {
        np::CodecStats s;
        s.frames_encoded = frames_encoded_;
        return s;
    }
    std::string_view codec_name() const noexcept override { return "test-fake-codec"; }
    std::uint8_t payload_type() const noexcept override { return 111; }

private:
    np::CodecConfig cfg_;
    std::uint64_t frames_encoded_ = 0;
};

class FakeAudioCodecFactory final : public np::ICodecFactory {
public:
    std::string_view id() const noexcept override { return "test-fake-codec"; }
    std::string_view display_name() const noexcept override {
        return "Fake audio codec (audio-device test)";
    }
    np::ICodec* create(np::CodecConfig cfg) const override {
        return new FakeAudioCodec(std::move(cfg));
    }
};

// ---------------------------------------------------------------------------
// Fake audio decoder
// ---------------------------------------------------------------------------

class FakeAudioDecoder final : public np::IAudioDecoder {
public:
    const char* name() const noexcept override { return "FakeAudioDecoder"; }
    np::Status open() noexcept override { return np::kOk; }
    void close() noexcept override {}
    np::AudioDecodeFormat format() const noexcept override { return {}; }
    np::Status update_format(np::AudioDecodeFormat) noexcept override { return np::kOk; }
    std::size_t decode_packet(const std::uint8_t*, std::size_t, std::uint32_t,
                              np::AudioDecodedFrame*, std::size_t) noexcept override {
        return 0;
    }
    std::size_t decode_plc(std::uint32_t, std::uint16_t,
                           np::AudioDecodedFrame*, std::size_t) noexcept override {
        return 0;
    }
    void reset() noexcept override {}
    np::IAudioDecoder::Stats stats() const noexcept override { return {}; }
};

class FakeAudioDecoderFactory final : public np::IAudioDecoderFactory {
public:
    std::string_view id() const noexcept override { return "test-fake-decoder"; }
    std::string_view display_name() const noexcept override {
        return "Fake audio decoder (audio-device test)";
    }
    np::IAudioDecoder* create() const override { return new FakeAudioDecoder(); }
};

// ---------------------------------------------------------------------------
// Fake audio receiver — fires its frame callback immediately on push()
// ---------------------------------------------------------------------------

class FakeAudioReceiver final : public np::IAudioReceiver {
public:
    const char* name() const noexcept override { return "FakeAudioReceiver"; }
    np::Status open() noexcept override { return np::kOk; }
    void close() noexcept override {}
    void set_callbacks(np::IAudioReceiver::FrameCallback on_frame,
                       np::IAudioReceiver::ErrorCallback) noexcept override {
        on_frame_ = std::move(on_frame);
    }
    void push(const std::uint8_t*, std::size_t, std::uint32_t rtp_timestamp,
              std::uint16_t, std::uint32_t, np::TimestampUs) noexcept override {
        ++pushed_;
        if (on_frame_) {
            np::AudioOutputFrame frame;
            frame.pcm = pcm_.data();
            frame.samples_per_channel = kFrameSamples;
            frame.sample_rate_hz = 48000;
            frame.channels = 1;
            frame.rtp_timestamp = rtp_timestamp;
            frame.playout_time_us = 0;
            frame.concealed = false;
            frame.fec_recovered = false;
            on_frame_(frame);
        }
    }
    void tick(np::TimestampUs) noexcept override {}
    void reset_stream(std::uint32_t) noexcept override {}
    np::IAudioReceiver::Stats stats() const noexcept override { return {}; }
    std::uint64_t pushed() const noexcept { return pushed_; }

private:
    np::IAudioReceiver::FrameCallback on_frame_;
    std::array<float, kFrameSamples> pcm_{};
    std::uint64_t pushed_ = 0;
};

class FakeAudioReceiverFactory final : public np::IAudioReceiverFactory {
public:
    std::string_view id() const noexcept override { return "test-fake-receiver"; }
    std::string_view display_name() const noexcept override {
        return "Fake audio receiver (audio-device test)";
    }
    np::IAudioReceiver* create(const np::AudioReceiverConfig&) const override {
        return new FakeAudioReceiver();
    }
};

// ---------------------------------------------------------------------------
// Fake 3A
// ---------------------------------------------------------------------------

class FakeAudio3A final : public np::IAudio3A {
public:
    const char* name() const noexcept override { return "FakeAudio3A"; }
    np::Status open() noexcept override { return np::kOk; }
    void close() noexcept override {}
    void set_callbacks(np::VadCallback, np::LevelCallback,
                       np::AudioErrorCallback) noexcept override {}
    np::Status process_capture(float*, std::size_t, std::size_t) noexcept override {
        ++capture_calls_;
        return np::kOk;
    }
    np::Status process_render(const float*, std::size_t, std::size_t) noexcept override {
        ++render_calls_;
        return np::kOk;
    }
    void set_render_delivered(std::size_t n) noexcept override {
        ++render_delivered_calls_;
        last_render_delivered_ = n;
    }
    void request_vad_report() noexcept override {}
    void request_level_report() noexcept override {}
    void set_pre_process_tap(np::PcmTapCallback) noexcept override {}
    void set_post_process_tap(np::PcmTapCallback,
                              np::PcmTapCallbackI16) noexcept override {}
    np::IAudio3A::Stats stats() const noexcept override { return {}; }

    std::size_t render_delivered_calls() const noexcept { return render_delivered_calls_; }
    std::size_t last_render_delivered() const noexcept { return last_render_delivered_; }
    std::size_t render_calls() const noexcept { return render_calls_; }

private:
    std::size_t render_delivered_calls_ = 0;
    std::size_t last_render_delivered_ = 0;
    std::size_t capture_calls_ = 0;
    std::size_t render_calls_ = 0;
};

class FakeAudio3AFactory final : public np::IAudio3AFactory {
public:
    std::string_view id() const noexcept override { return "test-fake-3a"; }
    std::string_view display_name() const noexcept override {
        return "Fake 3A (audio-device test)";
    }
    np::IAudio3A* create() const override { return new FakeAudio3A(); }
};

// ---------------------------------------------------------------------------
// Fake capture device
//
// One class covers every failure mode the pump tests need: the open result
// and the read result are knobs on the factory.
// ---------------------------------------------------------------------------

class FakeAudioSource final : public np::IAudioSource {
public:
    FakeAudioSource(np::AudioSourceConfig cfg, np::Status open_result,
                    np::Status read_result)
        : cfg_(std::move(cfg)), open_result_(open_result), read_result_(read_result) {}

    const char* name() const noexcept override { return "FakeAudioSource"; }
    np::Status open() noexcept override {
        if (open_result_ != np::kOk) return open_result_;
        open_ = true;
        return np::kOk;
    }
    void close() noexcept override { open_ = false; }
    bool is_open() const noexcept override { return open_; }

    // The seam's contract: after open(), config() reports what was
    // actually negotiated, not what the host requested (criterion 13).
    np::AudioSourceConfig config() const noexcept override {
        np::AudioSourceConfig c = cfg_;
        c.device_id = "negotiated-source-device";
        return c;
    }

    std::vector<np::AudioDevice> enumerate_devices() const noexcept override { return {}; }
    std::vector<np::AudioDeviceFormat> supported_formats(
        const std::string&) const noexcept override {
        return {};
    }
    np::AudioDeviceCapabilities capabilities() const noexcept override {
        np::AudioDeviceCapabilities c;
        c.capture = true;
        c.backend = "test-fake-source";
        return c;
    }
    void set_on_audio_frame(np::AudioFrameCallback) noexcept override {}
    np::Status read_samples(float* dst, std::size_t capacity,
                            std::size_t* out_frames) noexcept override {
        if (read_result_ != np::kOk) return read_result_;
        const std::size_t n = std::min<std::size_t>(kFrameSamples, capacity);
        for (std::size_t i = 0; i < n; ++i) dst[i] = 0.25f;
        *out_frames = n;
        ++reads_;
        return np::kOk;
    }
    np::AudioDeviceStats stats() const noexcept override { return {}; }
    std::uint64_t reads() const noexcept { return reads_; }

private:
    np::AudioSourceConfig cfg_;
    np::Status open_result_ = np::kOk;
    np::Status read_result_ = np::kOk;
    bool open_ = false;
    std::uint64_t reads_ = 0;
};

class FakeAudioSourceFactory final : public np::IAudioSourceFactory {
public:
    FakeAudioSourceFactory(std::string_view id, np::Status open_result = np::kOk,
                           np::Status read_result = np::kOk)
        : id_(id), open_result_(open_result), read_result_(read_result) {}

    std::string_view id() const noexcept override { return id_; }
    std::string_view display_name() const noexcept override {
        return "Fake capture device (audio-device test)";
    }
    np::IAudioSource* create(np::AudioSourceConfig cfg) const override {
        return new FakeAudioSource(std::move(cfg), open_result_, read_result_);
    }

private:
    std::string_view id_;
    np::Status open_result_;
    np::Status read_result_;
};

// ---------------------------------------------------------------------------
// Fake playback device
// ---------------------------------------------------------------------------

class FakeAudioSink final : public np::IAudioSink {
public:
    FakeAudioSink(np::AudioSinkConfig cfg, np::Status open_result)
        : cfg_(std::move(cfg)), open_result_(open_result) {}

    const char* name() const noexcept override { return "FakeAudioSink"; }
    np::Status open() noexcept override {
        if (open_result_ != np::kOk) return open_result_;
        open_ = true;
        return np::kOk;
    }
    void close() noexcept override { open_ = false; }
    bool is_open() const noexcept override { return open_; }

    // The negotiated device id, per the seam's contract (criterion 13).
    np::AudioSinkConfig config() const noexcept override {
        np::AudioSinkConfig c = cfg_;
        c.device_id = "negotiated-sink-device";
        return c;
    }

    std::vector<np::AudioDevice> enumerate_devices() const noexcept override { return {}; }
    std::vector<np::AudioDeviceFormat> supported_formats(
        const std::string&) const noexcept override {
        return {};
    }
    np::AudioDeviceCapabilities capabilities() const noexcept override {
        np::AudioDeviceCapabilities c;
        c.playback = true;
        c.backend = "test-fake-sink";
        return c;
    }
    np::Status write(const float*, std::size_t num_samples,
                     std::uint16_t channels) noexcept override {
        if (!open_) return np::kErrClosed;
        ++writes_;
        last_samples_ = num_samples;
        last_channels_ = channels;
        return np::kOk;
    }
    std::uint64_t played_samples() const noexcept override { return played_; }
    void set_on_playback_progress(np::AudioPlaybackCallback) noexcept override {}
    np::AudioDeviceStats stats() const noexcept override { return {}; }

    void set_played(std::uint64_t v) noexcept { played_ = v; }
    std::uint64_t writes() const noexcept { return writes_; }
    std::size_t last_samples() const noexcept { return last_samples_; }
    std::uint16_t last_channels() const noexcept { return last_channels_; }

private:
    np::AudioSinkConfig cfg_;
    np::Status open_result_ = np::kOk;
    bool open_ = false;
    std::uint64_t played_ = 0;
    std::uint64_t writes_ = 0;
    std::size_t last_samples_ = 0;
    std::uint16_t last_channels_ = 0;
};

class FakeAudioSinkFactory final : public np::IAudioSinkFactory {
public:
    FakeAudioSinkFactory(std::string_view id, np::Status open_result = np::kOk)
        : id_(id), open_result_(open_result) {}

    std::string_view id() const noexcept override { return id_; }
    std::string_view display_name() const noexcept override {
        return "Fake playback device (audio-device test)";
    }
    np::IAudioSink* create(np::AudioSinkConfig cfg) const override {
        return new FakeAudioSink(std::move(cfg), open_result_);
    }

private:
    std::string_view id_;
    np::Status open_result_;
};

// ---------------------------------------------------------------------------
// Minimal RTP helper — a 12-byte header plus one payload byte.
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> make_minimal_rtp(std::uint8_t pt, std::uint16_t seq,
                                           std::uint32_t ts,
                                           std::uint32_t ssrc) {
    std::vector<std::uint8_t> pkt(13, 0);
    pkt[0] = 0x80;  // v=2, no padding, no extension, no CSRC
    pkt[1] = pt;
    pkt[2] = static_cast<std::uint8_t>(seq >> 8);
    pkt[3] = static_cast<std::uint8_t>(seq);
    pkt[4] = static_cast<std::uint8_t>(ts >> 24);
    pkt[5] = static_cast<std::uint8_t>(ts >> 16);
    pkt[6] = static_cast<std::uint8_t>(ts >> 8);
    pkt[7] = static_cast<std::uint8_t>(ts);
    pkt[8] = static_cast<std::uint8_t>(ssrc >> 24);
    pkt[9] = static_cast<std::uint8_t>(ssrc >> 16);
    pkt[10] = static_cast<std::uint8_t>(ssrc >> 8);
    pkt[11] = static_cast<std::uint8_t>(ssrc);
    pkt[12] = 0x00;  // one payload byte
    return pkt;
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class EngineAudioDevice : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();
        // The kDefaultRegistrars audio-device entry is guarded by
        // NIMRTC_HAS_AUDIO_DEVICE, which is not defined for
        // nimrtc_core_objects, so the module registers itself here.
        nimrtc::audio_device::register_default_plugins();

        static FakeIceFactory              ice_factory;
        static FakeDtlsFactory             dtls_factory;
        static FakeAudioCodecFactory       codec_factory;
        static FakeAudioDecoderFactory     decoder_factory;
        static FakeAudioReceiverFactory    receiver_factory;
        static FakeAudio3AFactory          a3a_factory;
        static FakeAudioSourceFactory      source_factory("test-fake-source");
        static FakeAudioSinkFactory        sink_factory("test-fake-sink");
        static FakeAudioSourceFactory      failing_source("test-failing-source",
                                                          np::kErrHardwareError);
        static FakeAudioSinkFactory        failing_sink("test-failing-sink",
                                                       np::kErrHardwareError);
        static FakeAudioSourceFactory      hw_fail_source("test-hwfail-source",
                                                         np::kOk,
                                                         np::kErrHardwareError);

        auto& reg = PluginRegistry::instance();
        reg.register_ice_transport("test-fake-ice", &ice_factory);
        reg.register_dtls_session("test-fake-dtls", &dtls_factory);
        reg.register_codec("test-fake-codec", &codec_factory);
        reg.register_audio_decoder("test-fake-decoder", &decoder_factory);
        reg.register_audio_receiver("test-fake-receiver", &receiver_factory);
        reg.register_audio3a("test-fake-3a", &a3a_factory);
        reg.register_audio_source("test-fake-source", &source_factory);
        reg.register_audio_sink("test-fake-sink", &sink_factory);
        reg.register_audio_source("test-failing-source", &failing_source);
        reg.register_audio_sink("test-failing-sink", &failing_sink);
        reg.register_audio_source("test-hwfail-source", &hw_fail_source);
    }

    static EngineConfig make_config() {
        EngineConfig cfg{};
        cfg.transport_name       = "test-fake-ice";
        cfg.dtls_name            = "test-fake-dtls";
        cfg.datachannel_backend  = "";
        cfg.bwe_name             = "";
        cfg.scheduler_config.enabled = false;
        cfg.pcm_sample_rate_hz   = 48000;
        cfg.pcm_channels         = 1;
        cfg.codec_name           = "test-fake-codec";
        cfg.audio3a_name         = "test-fake-3a";
        cfg.audio_decoder_name   = "test-fake-decoder";
        cfg.audio_receiver_name  = "test-fake-receiver";
        return cfg;
    }
};

}  // namespace

// ===========================================================================
// Criterion 1 — the registry has two audio device slots.
// ===========================================================================

TEST(EngineAudioDeviceRegistry, RegistrySlotsReturnSamePointer) {
    static FakeAudioSourceFactory src_factory("regtest-fake-source");
    static FakeAudioSinkFactory   sink_factory("regtest-fake-sink");
    auto& reg = PluginRegistry::instance();
    reg.register_audio_source("regtest-fake-source", &src_factory);
    reg.register_audio_sink("regtest-fake-sink", &sink_factory);

    EXPECT_EQ(reg.get_audio_source("regtest-fake-source"), &src_factory);
    EXPECT_NE(std::find(reg.list_audio_sources().begin(),
                        reg.list_audio_sources().end(),
                        "regtest-fake-source"),
              reg.list_audio_sources().end());

    EXPECT_EQ(reg.get_audio_sink("regtest-fake-sink"), &sink_factory);
    EXPECT_NE(std::find(reg.list_audio_sinks().begin(),
                        reg.list_audio_sinks().end(),
                        "regtest-fake-sink"),
              reg.list_audio_sinks().end());
}

// ===========================================================================
// Criterion 2 — the audio_device module registers its backends.
// ===========================================================================

TEST_F(EngineAudioDevice, AudioDeviceModuleRegistersSynthetic) {
    const auto src_ids = PluginRegistry::instance().list_audio_sources();
    EXPECT_FALSE(src_ids.empty());
    EXPECT_NE(std::find(src_ids.begin(), src_ids.end(), "synthetic"),
              src_ids.end());

    const auto sink_ids = PluginRegistry::instance().list_audio_sinks();
    EXPECT_FALSE(sink_ids.empty());
    EXPECT_NE(std::find(sink_ids.begin(), sink_ids.end(), "synthetic"),
              sink_ids.end());
}

// ===========================================================================
// Criterion 3 — the registry and the module resolve the same factories.
// ===========================================================================

TEST_F(EngineAudioDevice, PalResolverMatchesModuleFactories) {
    const auto* mod_src = nimrtc::audio_device::source_factory("synthetic");
    ASSERT_NE(mod_src, nullptr);
    EXPECT_EQ(nimrtc::engine::pal::resolve_audio_source("synthetic"), mod_src);

    const auto* mod_sink = nimrtc::audio_device::sink_factory("synthetic");
    ASSERT_NE(mod_sink, nullptr);
    EXPECT_EQ(nimrtc::engine::pal::resolve_audio_sink("synthetic"), mod_sink);
}

// ===========================================================================
// Criterion 4 — the Category enum values are untouched by the new slots.
// ===========================================================================

TEST(EngineAudioDeviceRegistry, CategoryValuesUnchanged) {
    static_assert(static_cast<int>(Registrar::Category::kTransport) == 0);
    // The pre-existing device slot keeps its value...
    static_assert(static_cast<int>(Registrar::Category::kVideoCapture) == 19);
    // ...and the two new audio slots are appended after the pre-existing
    // ones, so no pre-existing category shifts.
    static_assert(static_cast<int>(Registrar::Category::kAudioSource) == 34);
    static_assert(static_cast<int>(Registrar::Category::kAudioSink) == 35);
    static_assert(static_cast<int>(Registrar::Category::kAudioSource) <
                  static_cast<int>(Registrar::Category::kAudioSink));
    // The spec (`specs/slices/v1-audio-device-engine.md`, §"枚举值既不是外部
    // 契约") requires that adding the audio slots leaves every pre-existing
    // category's value untouched.  The audio slots are therefore *appended*
    // at the end of the enum rather than inserted next to `kVideoCapture`:
    // inserting them mid-enum would renumber `kBwe` and the fourteen
    // categories after it.
    static_assert(static_cast<int>(Registrar::Category::kBwe) == 20);
    static_assert(static_cast<int>(Registrar::Category::kAudioSink) >
                  static_cast<int>(Registrar::Category::kBwe));
}

// ===========================================================================
// Criteria 6 / 6b / 6c / 22 — the "auto" rule, tested through an injected
// is_registered callback (so the rule is testable without a registry, 6b).
// ===========================================================================

TEST(EngineAudioDeviceRegistry, AutoResolutionPrefersPortAudioWhenRegistered) {
    const auto is_registered = [](std::string_view id) noexcept {
        return id == "portaudio" || id == "synthetic";
    };
    EXPECT_EQ(resolve_auto_audio_backend(/*capture=*/true, is_registered),
              "portaudio");
    EXPECT_EQ(resolve_auto_audio_backend(/*capture=*/false, is_registered),
              "portaudio");
}

TEST(EngineAudioDeviceRegistry, AutoResolutionFallsBackToSyntheticWhenPortAudioAbsent) {
    // A CI machine with no sound card: "portaudio" is not registered.
    const auto is_registered = [](std::string_view id) noexcept {
        return id == "synthetic";
    };
    EXPECT_EQ(resolve_auto_audio_backend(/*capture=*/true, is_registered),
              "synthetic");
    EXPECT_EQ(resolve_auto_audio_backend(/*capture=*/false, is_registered),
              "synthetic");
}

TEST(EngineAudioDeviceRegistry, AutoResolutionWithNothingRegisteredDegradesToEmpty) {
    const auto is_registered = [](std::string_view) noexcept { return false; };
    EXPECT_EQ(resolve_auto_audio_backend(/*capture=*/true, is_registered), "");
    EXPECT_EQ(resolve_auto_audio_backend(/*capture=*/false, is_registered), "");
}

// ===========================================================================
// Criterion 5 — an empty audio device name keeps today's behavior: the
// engine installs no device and the host-driven send path is untouched.
// ===========================================================================

TEST_F(EngineAudioDevice, EmptyAudioDeviceNamesKeepTodayBehavior) {
    EngineConfig cfg = make_config();
    cfg.audio_source_name = "";
    cfg.audio_sink_name   = "";
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    EXPECT_EQ(engine.audio_source(), nullptr);
    EXPECT_EQ(engine.audio_sink(), nullptr);

    // The host-driven path works exactly as before.
    const std::vector<float> pcm(kFrameSamples, 0.1f);
    const std::uint64_t before = engine.stats().audio_frames_sent;
    ASSERT_EQ(engine.send_audio(pcm.data(), pcm.size()), 0u);
    EXPECT_EQ(engine.stats().audio_frames_sent - before, 1u);
    engine.close();
}

// ===========================================================================
// Criterion 7 — "none" is the explicit opt-out.
// ===========================================================================

TEST_F(EngineAudioDevice, NoneIsExplicitOptOut) {
    EngineConfig cfg = make_config();
    cfg.audio_source_name = "none";
    cfg.audio_sink_name   = "none";
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    EXPECT_EQ(engine.audio_source(), nullptr);
    EXPECT_EQ(engine.audio_sink(), nullptr);
    engine.close();
}

// ===========================================================================
// Criteria 8 and 14 — the engine pump drives capture into the outbound
// path, and the pump counters move.
// ===========================================================================

TEST_F(EngineAudioDevice, EnginePumpsCaptureIntoOutboundPath) {
    EngineConfig cfg = make_config();
    cfg.audio_source_name = "test-fake-source";
    cfg.audio_source_config.device_id = "requested-source";
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    ASSERT_NE(engine.audio_source(), nullptr);

    const auto pumped_before = engine.stats().audio_device_pumped_frames;
    const auto sent_before   = engine.stats().audio_frames_sent;
    engine.tick();
    const auto pumped_after = engine.stats().audio_device_pumped_frames;
    const auto sent_after   = engine.stats().audio_frames_sent;

    EXPECT_EQ(pumped_after - pumped_before, 1u)
        << "one tick must pull one frame from the capture device";
    EXPECT_EQ(sent_after - sent_before, 1u)
        << "the pulled frame must reach the outbound path as one RTP packet";
    engine.close();
}

TEST_F(EngineAudioDevice, PumpErrorsIncrementOnDeviceReadFailure) {
    EngineConfig cfg = make_config();
    cfg.audio_source_name = "test-hwfail-source";
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const auto errors_before = engine.stats().audio_device_pump_errors;
    engine.tick();
    EXPECT_GT(engine.stats().audio_device_pump_errors, errors_before);
    engine.close();
}

// ===========================================================================
// Criterion 9 — decoded PCM is written into the sink without the host
// touching on_audio_frame().
// ===========================================================================

TEST_F(EngineAudioDevice, EngineWritesDecodedPcmIntoSink) {
    EngineConfig cfg = make_config();
    cfg.audio_sink_name   = "test-fake-sink";
    cfg.video_receiver_name = "";  // force the inbound packet to the audio path
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    auto* sink = dynamic_cast<FakeAudioSink*>(engine.audio_sink());
    ASSERT_NE(sink, nullptr);
    EXPECT_EQ(sink->writes(), 0u);

    const auto pkt = make_minimal_rtp(/*pt=*/111, /*seq=*/100,
                                      /*ts=*/1000, /*ssrc=*/0xDEADBEEF);
    engine.feed_media_inbound(pkt.data(), pkt.size());

    EXPECT_EQ(sink->writes(), 1u);
    EXPECT_EQ(sink->last_samples(), kFrameSamples);
    EXPECT_EQ(sink->last_channels(), 1u);
    engine.close();
}

// ===========================================================================
// Criterion 10 — the 3A render reference.
// ===========================================================================

TEST_F(EngineAudioDevice, RenderDeliveredUsesPlayedDeltaWithSink) {
    EngineConfig cfg = make_config();
    cfg.audio_sink_name   = "test-fake-sink";
    cfg.video_receiver_name = "";
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    auto* sink = dynamic_cast<FakeAudioSink*>(engine.audio_sink());
    ASSERT_NE(sink, nullptr);
    auto* a3a  = dynamic_cast<FakeAudio3A*>(engine.audio3a());
    ASSERT_NE(a3a, nullptr);

    // The baseline was read at open() (0).  The loudspeaker then consumed
    // half of the first frame before the render path observed it.
    sink->set_played(480);

    const auto pkt = make_minimal_rtp(111, 100, 1000, 0xDEADBEEF);
    engine.feed_media_inbound(pkt.data(), pkt.size());

    EXPECT_GT(a3a->render_delivered_calls(), 0u);
    EXPECT_EQ(a3a->last_render_delivered(), 480u)
        << "with a sink, set_render_delivered() must receive the played "
           "delta, not the frame's sample count";
    engine.close();
}

TEST_F(EngineAudioDevice, RenderDeliveredUsesFrameSamplesWithoutSink) {
    EngineConfig cfg = make_config();
    cfg.audio_sink_name   = "";
    cfg.video_receiver_name = "";
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    auto* a3a = dynamic_cast<FakeAudio3A*>(engine.audio3a());
    ASSERT_NE(a3a, nullptr);

    const auto pkt = make_minimal_rtp(111, 100, 1000, 0xDEADBEEF);
    engine.feed_media_inbound(pkt.data(), pkt.size());

    EXPECT_GT(a3a->render_delivered_calls(), 0u);
    EXPECT_EQ(a3a->last_render_delivered(), kFrameSamples)
        << "without a sink, set_render_delivered() keeps the pre-D1 "
           "behavior: the frame's sample count";
    engine.close();
}

// ===========================================================================
// Criteria 11 and 12 — device open failures.
// ===========================================================================

TEST_F(EngineAudioDevice, AutoOpenFailureDegrades) {
    // Overwrite "portaudio" with a factory whose open() always fails,
    // simulating a machine where the default device cannot be opened.
    static FakeAudioSourceFactory failing_portaudio("portaudio",
                                                    np::kErrHardwareError);
    static FakeAudioSinkFactory   failing_portaudio_sink("portaudio",
                                                         np::kErrHardwareError);
    auto& reg = PluginRegistry::instance();
    reg.register_audio_source("portaudio", &failing_portaudio);
    reg.register_audio_sink("portaudio", &failing_portaudio_sink);

    EngineConfig cfg = make_config();
    cfg.audio_source_name = "auto";
    cfg.audio_sink_name   = "";
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u)
        << "\"auto\" opening a failing device must degrade, not fail open()";
    EXPECT_EQ(engine.audio_source(), nullptr);

    const auto st = engine.stats();
    EXPECT_TRUE(st.audio_source_device_id.empty());
    EXPECT_TRUE(st.audio_source_backend.empty());
    engine.close();
}

TEST_F(EngineAudioDevice, ExplicitIdOpenFailureFailsOpen) {
    static FakeAudioSourceFactory failing_portaudio("portaudio",
                                                    np::kErrHardwareError);
    static FakeAudioSinkFactory   failing_portaudio_sink("portaudio",
                                                         np::kErrHardwareError);
    auto& reg = PluginRegistry::instance();
    reg.register_audio_source("portaudio", &failing_portaudio);
    reg.register_audio_sink("portaudio", &failing_portaudio_sink);

    EngineConfig cfg = make_config();
    cfg.audio_source_name = "portaudio";
    NimRTCEngine engine(cfg);
    EXPECT_NE(engine.open(), 0u)
        << "an explicit id that cannot open is a promise: open() must fail";

    // Restore the module's real factories so a later "auto" test (if any)
    // sees the machine's true backend table.
    if (const auto* real = nimrtc::audio_device::source_factory("portaudio")) {
        reg.register_audio_source("portaudio", real);
    }
    if (const auto* real = nimrtc::audio_device::sink_factory("portaudio")) {
        reg.register_audio_sink("portaudio", real);
    }
}

// ===========================================================================
// Criterion 13 — Stats reports the *negotiated* device ids.
// ===========================================================================

TEST_F(EngineAudioDevice, StatsReportsNegotiatedDeviceIds) {
    EngineConfig cfg = make_config();
    cfg.audio_source_name = "test-fake-source";
    cfg.audio_source_config.device_id = "requested-source";
    cfg.audio_sink_name   = "test-fake-sink";
    cfg.audio_sink_config.device_id   = "requested-sink";
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const auto st = engine.stats();
    EXPECT_EQ(st.audio_source_device_id, "negotiated-source-device");
    EXPECT_EQ(st.audio_source_backend, "test-fake-source");
    EXPECT_EQ(st.audio_sink_device_id, "negotiated-sink-device");
    EXPECT_EQ(st.audio_sink_backend, "test-fake-sink");
    engine.close();
}
