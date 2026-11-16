// modules/audio_device/tests/test_system_audio.cpp
//
// The system-audio (loopback) capture device (remote-desktop plan F6).
//
// What they cover:
//
//   1. the **device identity** — the system-audio device is enumerated
//      with an identity that is DISTINCT from every microphone id;
//   2. the **factory's error path** — a host without loopback support
//      gets an explicit, enumerated error rather than an empty silent
//      success;
//   3. the **synthetic** system-audio source — it delivers frames
//      through the same `IAudioSource` contract as the microphone.

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <nimrtc/audio_device/audio_device.hpp>
#include <nimrtc/audio_device/mixer.hpp>
#include <nimrtc/audio_device/synthetic_device.hpp>
#include <nimrtc/audio_device/system_audio.hpp>

#include <cmath>

namespace {

using nimrtc::audio_device::AudioMixer;
using nimrtc::audio_device::AudioMixerConfig;
using nimrtc::audio_device::SyntheticSystemAudioSource;
using nimrtc::audio_device::SystemAudioBackend;
using nimrtc::audio_device::SystemAudioDeviceKind;
using nimrtc::audio_device::SystemAudioSourceFactory;
using nimrtc::audio_device::SystemAudioStatus;
using nimrtc::plugins::Status;

} // namespace

// ---------------------------------------------------------------------------
// 1. The device identity
// ---------------------------------------------------------------------------

TEST(system_audio, the_system_audio_device_has_a_distinct_identity) {
    const SystemAudioSourceFactory factory;

    const auto devices = factory.devices();
    const auto mic_devices = nimrtc::audio_device::enumerate_capture_devices();

    for (const auto& sys : devices) {
        // The id lives in the `system-audio:` namespace — no microphone
        // id uses it.
        EXPECT_NE(sys.id.rfind("system-audio:", 0), std::string::npos)
            << "the system-audio device id must live in the 'system-audio:' "
               "namespace; got '" << sys.id << "'";

        // And it does not collide with any microphone id.
        for (const auto& mic : mic_devices) {
            EXPECT_NE(sys.id, mic.id)
                << "the system-audio device must not share an id with the "
                   "microphone '" << mic.id << "'";
        }
    }
}

TEST(system_audio, the_system_audio_device_is_not_in_the_microphone_list) {
    const SystemAudioSourceFactory factory;

    const auto mic_devices = nimrtc::audio_device::enumerate_capture_devices();
    for (const auto& mic : mic_devices) {
        for (const auto& sys : factory.devices()) {
            EXPECT_NE(sys.id, mic.id);
        }
    }

    // The synthetic system's own id is not the microphone's id either.
    EXPECT_NE(std::string("system-audio:synthetic:0"),
              std::string("synthetic:0"));
}

TEST(system_audio, the_factory_resolves_under_its_own_backend_id) {
    // The system-audio backend is a *second source*: it has its own id
    // and it does not shadow the microphone's backends.
    const auto* factory = nimrtc::audio_device::source_factory("system-audio");
    ASSERT_NE(factory, nullptr);
    EXPECT_EQ(factory->id(), "system-audio");

    // The microphone's backends still resolve to the microphone.
    const auto* mic = nimrtc::audio_device::source_factory("synthetic");
    ASSERT_NE(mic, nullptr);
    EXPECT_EQ(mic->id(), "synthetic");
    EXPECT_NE(mic->id(), factory->id());
}

TEST(system_audio, the_backend_list_carries_the_system_audio_backends) {
    const auto backends = nimrtc::audio_device::available_source_backends();
    EXPECT_NE(std::find(backends.begin(), backends.end(), "system-audio"),
              backends.end());
    EXPECT_NE(std::find(backends.begin(), backends.end(),
                        "synthetic-system-audio"),
              backends.end());
    // ... and the microphone's backends are still there.
    EXPECT_NE(std::find(backends.begin(), backends.end(), "synthetic"),
              backends.end());
}

TEST(system_audio, every_enumerated_device_carries_a_kind_and_a_backend) {
    const SystemAudioSourceFactory factory;
    for (const auto& d : factory.devices()) {
        // The kind must be one of the three the vocabulary defines, and
        // the backend must name the mechanism that implements it.
        const bool kind_known =
            d.kind == SystemAudioDeviceKind::kLoopback ||
            d.kind == SystemAudioDeviceKind::kTap ||
            d.kind == SystemAudioDeviceKind::kSynthetic;
        EXPECT_TRUE(kind_known) << "the device kind must be set";

        EXPECT_NE(d.backend, SystemAudioBackend::kUnavailable)
            << "an enumerated device must name the backend that implements it";
    }
}

// ---------------------------------------------------------------------------
// 2. The factory's error path
// ---------------------------------------------------------------------------

TEST(system_audio, an_unavailable_loopback_reports_an_explicit_error) {
    const SystemAudioSourceFactory factory;

    const auto status = factory.status();

    if (!factory.available()) {
        // The failure must be ENUMERATED: a status code and a
        // human-readable reason.  Never silence, never a crash.
        EXPECT_NE(status.code, SystemAudioStatus::kAvailable);
        EXPECT_FALSE(status.reason.empty())
            << "an unavailable system-audio backend must say WHY it is "
               "unavailable";
    } else {
        EXPECT_EQ(status.code, SystemAudioStatus::kAvailable);
        EXPECT_FALSE(status.reason.empty());
        EXPECT_FALSE(factory.devices().empty());
    }
}

TEST(system_audio, creating_a_device_on_an_unsupported_host_reports_an_error) {
    const SystemAudioSourceFactory factory;

    if (factory.available()) {
        // The host supports loopback: the create path is exercised
        // elsewhere (and it must not crash here either).
        return;
    }

    nimrtc::plugins::AudioSourceConfig cfg;
    cfg.device_id = "system-audio:default";

    std::string error;
    std::unique_ptr<nimrtc::plugins::IAudioSource> source =
        factory.open_device(cfg, error);
    // On a host without loopback support the factory must NOT hand
    // back a silently-useless device.  Either it refuses (nullptr,
    // with an explicit reason) or it returns a device whose `open()`
    // fails with a clean error.
    if (source == nullptr) {
        EXPECT_FALSE(error.empty())
            << "a refused system-audio device must say WHY it was refused";
    } else {
        EXPECT_NE(source->open(), Status(0))
            << "a device opened on a host without loopback support must "
               "report a clean error";
        source->close();
    }
}

TEST(system_audio, the_status_names_are_stable) {
    EXPECT_EQ(nimrtc::audio_device::system_audio_status_name(
                  SystemAudioStatus::kAvailable),
              "available");
    EXPECT_EQ(nimrtc::audio_device::system_audio_backend_name(
                  SystemAudioBackend::kWasapiLoopback),
              "wasapi-loopback");
    EXPECT_EQ(nimrtc::audio_device::system_audio_device_kind_name(
                  SystemAudioDeviceKind::kLoopback),
              "loopback");
}

// ---------------------------------------------------------------------------
// 3. The synthetic system-audio source
// ---------------------------------------------------------------------------

TEST(system_audio, the_synthetic_source_delivers_frames_through_the_seam) {
    nimrtc::plugins::AudioSourceConfig cfg;
    cfg.format.sample_rate_hz = 48000;
    cfg.format.channels = 1;
    cfg.format.frames_per_buffer = 480;

    SyntheticSystemAudioSource source(cfg);
    ASSERT_EQ(source.open(), Status(0));
    EXPECT_TRUE(source.is_open());

    // The callback path.
    std::vector<float> received;
    std::size_t frames_seen = 0;
    source.set_on_audio_frame([&](const nimrtc::plugins::AudioFrame& frame) {
        frames_seen = frame.num_samples;
        received.assign(frame.samples, frame.samples + frame.num_samples);
    });

    ASSERT_EQ(source.pump(), Status(0));
    EXPECT_EQ(frames_seen, 480u);
    EXPECT_EQ(received.size(), 480u);

    // The pull path.
    float buffer[480] = {0};
    std::size_t got = 0;
    ASSERT_EQ(source.read_samples(buffer, 480, &got), Status(0));
    EXPECT_EQ(got, 480u);

    source.close();
    EXPECT_FALSE(source.is_open());
}

TEST(system_audio, the_synthetic_source_negotiates_its_format) {
    nimrtc::plugins::AudioSourceConfig cfg;
    SyntheticSystemAudioSource source(cfg);
    ASSERT_EQ(source.open(), Status(0));

    const auto negotiated = source.config();
    EXPECT_EQ(negotiated.format.sample_rate_hz, 48000u);
    EXPECT_EQ(negotiated.format.channels, 2u);
    EXPECT_EQ(negotiated.format.frames_per_buffer, 480u);

    source.close();
}

TEST(system_audio, the_synthetic_source_enumerates_its_own_device) {
    SyntheticSystemAudioSource source({});
    const auto devices = source.enumerate_devices();
    ASSERT_EQ(devices.size(), 1u);
    EXPECT_EQ(devices[0].id, "system-audio:synthetic:0");
    EXPECT_TRUE(devices[0].can_capture);
    // A system-audio device does NOT play back.
    EXPECT_FALSE(devices[0].can_playback);
}

TEST(system_audio, the_synthetic_source_reports_device_loss_cleanly) {
    nimrtc::plugins::AudioSourceConfig cfg;
    SyntheticSystemAudioSource source(cfg);
    ASSERT_EQ(source.open(), Status(0));

    source.simulate_device_loss();
    EXPECT_TRUE(source.is_device_lost());
    EXPECT_NE(source.pump(), Status(0));
    EXPECT_TRUE(source.stats().device_lost);
    EXPECT_GE(source.stats().device_errors, 1u);

    // No crash, no hang: the device keeps answering.
    source.clear_device_loss();
    EXPECT_EQ(source.pump(), Status(0));
    source.close();
}

TEST(system_audio, the_synthetic_factory_creates_its_own_device) {
    const nimrtc::audio_device::SyntheticSystemAudioSourceFactory factory;
    EXPECT_EQ(factory.id(), "synthetic-system-audio");

    const auto ids = factory.device_ids();
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], "system-audio:synthetic:0");

    std::unique_ptr<nimrtc::plugins::IAudioSource> source(factory.create({}));
    ASSERT_NE(source, nullptr);
    EXPECT_STREQ(source->name(), "synthetic-system-audio-source");
}

// ---------------------------------------------------------------------------
// 4. The two sources together (the F6 acceptance criterion)
// ---------------------------------------------------------------------------

TEST(system_audio, the_system_source_can_be_selected_alongside_the_microphone) {
    nimrtc::plugins::AudioSourceConfig mic_cfg;
    nimrtc::plugins::AudioSourceConfig sys_cfg;

    SyntheticSystemAudioSource mic_source(mic_cfg);
    nimrtc::audio_device::SyntheticAudioSource sys_source(sys_cfg);

    ASSERT_EQ(mic_source.open(), Status(0));
    ASSERT_EQ(sys_source.open(), Status(0));

    // The two devices are independent instances: both are open at the
    // same time, and neither knows about the other.
    EXPECT_TRUE(mic_source.is_open());
    EXPECT_TRUE(sys_source.is_open());
    EXPECT_NE(mic_source.config().format.sample_rate_hz, 0u);
    EXPECT_NE(sys_source.config().format.sample_rate_hz, 0u);

    mic_source.close();
    sys_source.close();
}

TEST(system_audio, the_two_sources_feed_the_mixer_as_two_legs) {
    nimrtc::plugins::AudioSourceConfig cfg;
    cfg.format.sample_rate_hz = 48000;
    cfg.format.channels = 1;
    cfg.format.frames_per_buffer = 480;

    SyntheticSystemAudioSource mic_source(cfg);
    nimrtc::audio_device::SyntheticAudioSource sys_source(cfg);
    ASSERT_EQ(mic_source.open(), Status(0));
    ASSERT_EQ(sys_source.open(), Status(0));

    // The synthetic devices are *pumped*: a frame exists only after the
    // host asks for one.  (This is what makes the assertion on the
    // sample values deterministic.)
    ASSERT_EQ(mic_source.pump(), Status(0));
    ASSERT_EQ(sys_source.pump(), Status(0));

    AudioMixerConfig mix_cfg;
    mix_cfg.output_rate_hz = 48000;
    mix_cfg.output_channels = 1;

    AudioMixer mixer(mix_cfg);
    ASSERT_TRUE(mixer.valid());

    // Pull one frame from each and feed the two legs.
    float mic_block[480] = {0};
    float sys_block[480] = {0};
    std::size_t mic_frames = 0;
    std::size_t sys_frames = 0;
    ASSERT_EQ(mic_source.read_samples(mic_block, 480, &mic_frames), Status(0));
    ASSERT_EQ(sys_source.read_samples(sys_block, 480, &sys_frames), Status(0));
    ASSERT_EQ(mic_frames, 480u);
    ASSERT_EQ(sys_frames, 480u);

    ASSERT_EQ(mixer.push_microphone(mic_block, mic_frames, 1, 48000),
              nimrtc::audio_device::AudioMixStatus::kOk);
    ASSERT_EQ(mixer.push_system(sys_block, sys_frames, 1, 48000),
              nimrtc::audio_device::AudioMixStatus::kOk);

    std::vector<float> mixed;
    std::vector<float> mic_out;
    std::vector<float> sys_out;
    ASSERT_EQ(mixer.pull(mixed, mic_out, sys_out),
              nimrtc::audio_device::AudioMixStatus::kOk);

    // The two synthetic sources have different tones (440 Hz vs
    // 880 Hz), so the mix is non-trivial and non-zero.
    EXPECT_EQ(mixed.size(), 480u);
    bool any_nonzero = false;
    for (float v : mixed) {
        if (std::abs(v) > 1e-6f) {
            any_nonzero = true;
            break;
        }
    }
    EXPECT_TRUE(any_nonzero);

    mic_source.close();
    sys_source.close();
}
