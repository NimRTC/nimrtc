// modules/audio_device/tests/test_audio_device.cpp
//
// The audio-device module's unit tests (remote-desktop plan F2).
//
// What they cover, and why:
//
//   1. the shared ring buffer — the bounded buffer with its explicit
//      "drop the oldest" overflow policy;
//   2. the synthetic device's lifecycle and its format negotiation;
//   3. the played-sample counter — the F3 substrate.  The acceptance
//      criterion is that the counter advances *monotonically* and
//      matches the device's consumed-sample count;
//   4. the device-loss path — an unplugged device is a clean error,
//      not a crash;
//   5. the PortAudio backend's *availability* reporting — the tests
//      never require an audio device, so they run on a CI machine with
//      no sound card.

#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <nimrtc/audio_device/audio_device.hpp>
#include <nimrtc/audio_device/ring_buffer.hpp>
#include <nimrtc/audio_device/synthetic_device.hpp>

namespace {

using nimrtc::audio_device::AudioRingBuffer;
using nimrtc::audio_device::SyntheticAudioSink;
using nimrtc::audio_device::SyntheticAudioSource;
using nimrtc::plugins::AudioDeviceFormat;
using nimrtc::plugins::AudioSampleFormat;
using nimrtc::plugins::Status;

// ---------------------------------------------------------------------------
// 1. The ring buffer
// ---------------------------------------------------------------------------

TEST(audio_ring_buffer, empty_buffer_reads_nothing) {
    AudioRingBuffer buf(16, 1);
    EXPECT_EQ(buf.available(), 0u);

    float out[8] = {0};
    EXPECT_EQ(buf.read(out, 8), 0u);
    EXPECT_EQ(buf.available(), 0u);
}

TEST(audio_ring_buffer, write_then_read_roundtrips) {
    AudioRingBuffer buf(16, 1);

    const float in[4] = {0.1f, 0.2f, 0.3f, 0.4f};
    EXPECT_EQ(buf.write(in, 4), 4u);
    EXPECT_EQ(buf.available(), 4u);
    EXPECT_EQ(buf.written_frames(), 4u);

    float out[4] = {0};
    EXPECT_EQ(buf.read(out, 4), 4u);
    EXPECT_FLOAT_EQ(out[0], 0.1f);
    EXPECT_FLOAT_EQ(out[1], 0.2f);
    EXPECT_FLOAT_EQ(out[2], 0.3f);
    EXPECT_FLOAT_EQ(out[3], 0.4f);
    EXPECT_EQ(buf.available(), 0u);
    EXPECT_EQ(buf.read_frames(), 4u);
}

TEST(audio_ring_buffer, read_returns_only_what_is_available) {
    AudioRingBuffer buf(16, 1);
    const float in[3] = {1.0f, 2.0f, 3.0f};
    ASSERT_EQ(buf.write(in, 3), 3u);

    float out[8] = {0};
    EXPECT_EQ(buf.read(out, 8), 3u);
    EXPECT_FLOAT_EQ(out[2], 3.0f);
}

TEST(audio_ring_buffer, interleaved_multichannel_is_frame_oriented) {
    // Two channels: a frame is two floats.
    AudioRingBuffer buf(8, 2);

    const float in[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    EXPECT_EQ(buf.write(in, 2), 2u);
    EXPECT_EQ(buf.available(), 2u);

    float out[4] = {0};
    EXPECT_EQ(buf.read(out, 2), 2u);
    EXPECT_FLOAT_EQ(out[0], 1.0f);
    EXPECT_FLOAT_EQ(out[1], 2.0f);
    EXPECT_FLOAT_EQ(out[2], 3.0f);
    EXPECT_FLOAT_EQ(out[3], 4.0f);
}

TEST(audio_ring_buffer, overflow_drops_the_oldest_frames) {
    AudioRingBuffer buf(4, 1);

    // Fill the buffer.
    const float first[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    ASSERT_EQ(buf.write(first, 4), 4u);
    EXPECT_EQ(buf.available(), 4u);

    // Overwrite it: the oldest frames are evicted.
    const float second[2] = {5.0f, 6.0f};
    ASSERT_EQ(buf.write(second, 2), 2u);

    EXPECT_EQ(buf.available(), 4u);
    EXPECT_EQ(buf.dropped_frames(), 2u);

    float out[4] = {0};
    ASSERT_EQ(buf.read(out, 4), 4u);
    // The two oldest (1, 2) are gone; what survives is (3, 4, 5, 6).
    EXPECT_FLOAT_EQ(out[0], 3.0f);
    EXPECT_FLOAT_EQ(out[1], 4.0f);
    EXPECT_FLOAT_EQ(out[2], 5.0f);
    EXPECT_FLOAT_EQ(out[3], 6.0f);
}

TEST(audio_ring_buffer, a_block_larger_than_the_buffer_keeps_its_tail) {
    AudioRingBuffer buf(4, 1);

    const float in[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    EXPECT_EQ(buf.write(in, 8), 4u);

    float out[4] = {0};
    ASSERT_EQ(buf.read(out, 4), 4u);
    EXPECT_FLOAT_EQ(out[0], 5.0f);
    EXPECT_FLOAT_EQ(out[3], 8.0f);
}

TEST(audio_ring_buffer, wraps_around_the_storage_end) {
    AudioRingBuffer buf(4, 1);

    // Three writes of three frames each: the storage wraps twice.
    for (int i = 0; i < 3; ++i) {
        const float in[3] = {static_cast<float>(i * 3 + 1),
                             static_cast<float>(i * 3 + 2),
                             static_cast<float>(i * 3 + 3)};
        ASSERT_EQ(buf.write(in, 3), 3u);
        float out[3] = {0};
        ASSERT_EQ(buf.read(out, 3), 3u);
        EXPECT_FLOAT_EQ(out[0], static_cast<float>(i * 3 + 1));
        EXPECT_FLOAT_EQ(out[2], static_cast<float>(i * 3 + 3));
    }
}

TEST(audio_ring_buffer, clear_discards_the_buffered_frames) {
    AudioRingBuffer buf(8, 1);
    const float in[4] = {1, 2, 3, 4};
    ASSERT_EQ(buf.write(in, 4), 4u);

    buf.clear();
    EXPECT_EQ(buf.available(), 0u);

    float out[4] = {0};
    EXPECT_EQ(buf.read(out, 4), 0u);
}

TEST(audio_ring_buffer, null_arguments_are_rejected) {
    AudioRingBuffer buf(8, 1);
    float out[4] = {0};
    EXPECT_EQ(buf.write(nullptr, 4), 0u);
    EXPECT_EQ(buf.read(nullptr, 4), 0u);
    EXPECT_EQ(buf.read(out, 0), 0u);
}

// ---------------------------------------------------------------------------
// 2. The synthetic capture device
// ---------------------------------------------------------------------------

TEST(synthetic_audio_source, lifecycle) {
    SyntheticAudioSource src(nimrtc::plugins::AudioSourceConfig{});
    EXPECT_FALSE(src.is_open());

    EXPECT_EQ(src.open(), nimrtc::plugins::kOk);
    EXPECT_TRUE(src.is_open());

    src.close();
    EXPECT_FALSE(src.is_open());
}

TEST(synthetic_audio_source, format_negotiation_fills_in_the_defaults) {
    nimrtc::plugins::AudioSourceConfig cfg;
    cfg.format.sample_rate_hz = 0;      // "device default"
    cfg.format.channels = 0;            // "device default"

    SyntheticAudioSource src(cfg);
    ASSERT_EQ(src.open(), nimrtc::plugins::kOk);

    const auto negotiated = src.config().format;
    EXPECT_EQ(negotiated.sample_rate_hz, 48000u);
    EXPECT_EQ(negotiated.channels, 1u);
    EXPECT_GT(negotiated.frames_per_buffer, 0u);
    EXPECT_EQ(negotiated.format, AudioSampleFormat::kFloat32);
}

TEST(synthetic_audio_source, an_explicit_format_is_kept) {
    nimrtc::plugins::AudioSourceConfig cfg;
    cfg.format.sample_rate_hz = 44100;
    cfg.format.channels = 2;

    SyntheticAudioSource src(cfg);
    ASSERT_EQ(src.open(), nimrtc::plugins::kOk);
    EXPECT_EQ(src.config().format.sample_rate_hz, 44100u);
    EXPECT_EQ(src.config().format.channels, 2u);
}

TEST(synthetic_audio_source, enumerates_its_own_device) {
    SyntheticAudioSource src(nimrtc::plugins::AudioSourceConfig{});
    const auto devices = src.enumerate_devices();
    ASSERT_EQ(devices.size(), 1u);
    EXPECT_EQ(devices[0].id, "synthetic:0");
    EXPECT_TRUE(devices[0].can_capture);
}

TEST(synthetic_audio_source, capabilities_report_the_backend) {
    SyntheticAudioSource src(nimrtc::plugins::AudioSourceConfig{});
    const auto caps = src.capabilities();
    EXPECT_TRUE(caps.capture);
    EXPECT_FALSE(caps.playback);
    EXPECT_FALSE(caps.enumeration);   // the list is fixed, not enumerated
    EXPECT_EQ(caps.backend, "synthetic");
}

TEST(synthetic_audio_source, pump_delivers_a_frame_to_the_callback) {
    SyntheticAudioSource src(nimrtc::plugins::AudioSourceConfig{});
    ASSERT_EQ(src.open(), nimrtc::plugins::kOk);

    std::atomic<int> calls {0};
    std::atomic<std::size_t> samples {0};
    src.set_on_audio_frame([&](const nimrtc::plugins::AudioFrame& frame) {
        calls.fetch_add(1);
        samples.store(frame.num_samples);
    });

    ASSERT_EQ(src.pump(), nimrtc::plugins::kOk);
    EXPECT_EQ(calls.load(), 1);
    EXPECT_EQ(samples.load(), 480u);   // 10 ms at 48 kHz
}

TEST(synthetic_audio_source, pump_delivers_a_frame_to_the_pull_path) {
    SyntheticAudioSource src(nimrtc::plugins::AudioSourceConfig{});
    ASSERT_EQ(src.open(), nimrtc::plugins::kOk);

    ASSERT_EQ(src.pump(), nimrtc::plugins::kOk);

    std::vector<float> out(512);
    std::size_t got = 0;
    EXPECT_EQ(src.read_samples(out.data(), out.size(), &got),
              nimrtc::plugins::kOk);
    EXPECT_EQ(got, 480u);
}

TEST(synthetic_audio_source, the_pull_path_reports_not_ready_when_empty) {
    SyntheticAudioSource src(nimrtc::plugins::AudioSourceConfig{});
    ASSERT_EQ(src.open(), nimrtc::plugins::kOk);

    std::vector<float> out(512);
    std::size_t got = 123;
    EXPECT_EQ(src.read_samples(out.data(), out.size(), &got),
              nimrtc::plugins::kErrNotReady);
    EXPECT_EQ(got, 0u);
}

TEST(synthetic_audio_source, the_synthesised_tone_is_a_real_signal) {
    SyntheticAudioSource src(nimrtc::plugins::AudioSourceConfig{});
    ASSERT_EQ(src.open(), nimrtc::plugins::kOk);

    std::vector<float> out(512);
    std::size_t got = 0;
    ASSERT_EQ(src.pump(), nimrtc::plugins::kOk);
    ASSERT_EQ(src.read_samples(out.data(), out.size(), &got),
              nimrtc::plugins::kOk);
    ASSERT_EQ(got, 480u);

    // The signal must not be all zeros, and it must stay inside
    // [-1.0, 1.0] (the engine's PCM contract).
    float peak = 0.0f;
    for (std::size_t i = 0; i < got; ++i) {
        peak = std::max(peak, std::fabs(out[i]));
    }
    EXPECT_GT(peak, 0.5f);
    EXPECT_LE(peak, 1.0f);
}

// ---------------------------------------------------------------------------
// 3. The device-loss path
// ---------------------------------------------------------------------------

TEST(synthetic_audio_source, device_loss_is_a_clean_error_not_a_crash) {
    SyntheticAudioSource src(nimrtc::plugins::AudioSourceConfig{});
    ASSERT_EQ(src.open(), nimrtc::plugins::kOk);
    ASSERT_EQ(src.pump(), nimrtc::plugins::kOk);

    src.simulate_device_loss();
    EXPECT_TRUE(src.is_device_lost());

    // The pump reports the loss as a hardware error, and the device
    // does NOT abort, does NOT throw and does NOT spin.
    EXPECT_EQ(src.pump(), nimrtc::plugins::kErrHardwareError);

    const auto stats = src.stats();
    EXPECT_TRUE(stats.device_lost);
    EXPECT_GT(stats.device_errors, 0u);

    // The pull path reports the same thing.
    std::vector<float> out(512);
    std::size_t got = 0;
    EXPECT_EQ(src.read_samples(out.data(), out.size(), &got),
              nimrtc::plugins::kErrHardwareError);

    // And the device is still usable afterwards.
    src.clear_device_loss();
    EXPECT_EQ(src.pump(), nimrtc::plugins::kOk);
}

TEST(synthetic_audio_sink, device_loss_is_a_clean_error_not_a_crash) {
    SyntheticAudioSink sink(nimrtc::plugins::AudioSinkConfig{});
    ASSERT_EQ(sink.open(), nimrtc::plugins::kOk);

    const float samples[480] = {0};
    ASSERT_EQ(sink.write(samples, 480, 1), nimrtc::plugins::kOk);
    ASSERT_EQ(sink.pump(), nimrtc::plugins::kOk);
    const std::uint64_t before = sink.played_samples();
    EXPECT_GT(before, 0u);

    sink.simulate_device_loss();
    EXPECT_EQ(sink.pump(), nimrtc::plugins::kErrHardwareError);

    const auto stats = sink.stats();
    EXPECT_TRUE(stats.device_lost);
    EXPECT_GT(stats.device_errors, 0u);

    // The played-sample counter does not advance while the device is
    // lost: the device consumed nothing.
    EXPECT_EQ(sink.played_samples(), before);
}

// ---------------------------------------------------------------------------
// 4. The played-sample counter — the F3 substrate
// ---------------------------------------------------------------------------

TEST(synthetic_audio_sink, played_samples_starts_at_zero) {
    SyntheticAudioSink sink(nimrtc::plugins::AudioSinkConfig{});
    EXPECT_EQ(sink.played_samples(), 0u);
}

TEST(synthetic_audio_sink, played_samples_matches_the_consumed_sample_count) {
    SyntheticAudioSink sink(nimrtc::plugins::AudioSinkConfig{});
    ASSERT_EQ(sink.open(), nimrtc::plugins::kOk);

    const std::size_t quantum = sink.config().format.frames_per_buffer;
    const float samples[480] = {0};
    ASSERT_EQ(sink.write(samples, quantum, 1), nimrtc::plugins::kOk);

    // Nothing has been played yet: writing is not playing.
    EXPECT_EQ(sink.played_samples(), 0u);

    ASSERT_EQ(sink.pump(), nimrtc::plugins::kOk);
    EXPECT_EQ(sink.played_samples(), quantum);

    // The queue is empty now: an underrun plays silence, and the
    // counter does NOT advance (nothing was consumed).
    EXPECT_EQ(sink.pump(), nimrtc::plugins::kErrNotReady);
    EXPECT_EQ(sink.played_samples(), quantum);
}

TEST(synthetic_audio_sink, played_samples_advances_monotonically) {
    SyntheticAudioSink sink(nimrtc::plugins::AudioSinkConfig{});
    ASSERT_EQ(sink.open(), nimrtc::plugins::kOk);

    const std::size_t quantum = sink.config().format.frames_per_buffer;
    const float samples[480] = {0};

    std::uint64_t previous = 0;
    for (int i = 0; i < 20; ++i) {
        ASSERT_EQ(sink.write(samples, quantum, 1), nimrtc::plugins::kOk);
        ASSERT_EQ(sink.pump(), nimrtc::plugins::kOk);
        const std::uint64_t now = sink.played_samples();
        EXPECT_GT(now, previous) << "the counter must advance";
        previous = now;
    }
    EXPECT_EQ(previous, static_cast<std::uint64_t>(quantum) * 20u);
}

TEST(synthetic_audio_sink, the_playback_progress_callback_reports_the_total) {
    SyntheticAudioSink sink(nimrtc::plugins::AudioSinkConfig{});
    ASSERT_EQ(sink.open(), nimrtc::plugins::kOk);

    std::vector<std::uint64_t> reported;
    sink.set_on_playback_progress(
        [&](std::uint64_t total) { reported.push_back(total); });

    const std::size_t quantum = sink.config().format.frames_per_buffer;
    const float samples[480] = {0};
    ASSERT_EQ(sink.write(samples, quantum, 1), nimrtc::plugins::kOk);
    ASSERT_EQ(sink.pump(), nimrtc::plugins::kOk);

    ASSERT_EQ(reported.size(), 1u);
    EXPECT_EQ(reported[0], quantum);
    EXPECT_EQ(reported[0], sink.played_samples());
}

TEST(synthetic_audio_sink, write_on_a_closed_device_is_refused) {
    SyntheticAudioSink sink(nimrtc::plugins::AudioSinkConfig{});
    const float samples[16] = {0};
    EXPECT_EQ(sink.write(samples, 16, 1), nimrtc::plugins::kErrClosed);
}

// ---------------------------------------------------------------------------
// 5. The module surface
// ---------------------------------------------------------------------------

TEST(audio_device_module, the_synthetic_backend_is_always_available) {
    const auto sources = nimrtc::audio_device::available_source_backends();
    const auto sinks = nimrtc::audio_device::available_sink_backends();

    EXPECT_NE(std::find(sources.begin(), sources.end(), "synthetic"),
              sources.end());
    EXPECT_NE(std::find(sinks.begin(), sinks.end(), "synthetic"), sinks.end());
}

TEST(audio_device_module, the_synthetic_factories_are_reachable) {
    const auto* src = nimrtc::audio_device::source_factory("synthetic");
    const auto* sink = nimrtc::audio_device::sink_factory("synthetic");
    ASSERT_NE(src, nullptr);
    ASSERT_NE(sink, nullptr);
    EXPECT_EQ(src->id(), "synthetic");
    EXPECT_EQ(sink->id(), "synthetic");
}

TEST(audio_device_module, an_unknown_backend_resolves_to_null) {
    EXPECT_EQ(nimrtc::audio_device::source_factory("no-such-backend"), nullptr);
    EXPECT_EQ(nimrtc::audio_device::sink_factory("no-such-backend"), nullptr);
}

TEST(audio_device_module, the_portaudio_backend_reports_its_availability) {
    // The test never *requires* an audio device: a CI machine with no
    // sound card must still pass.  What it checks is that the
    // availability flag and the backend list agree with each other.
    const bool available = nimrtc::audio_device::portaudio_available();
    const auto sources = nimrtc::audio_device::available_source_backends();

    const bool listed = std::find(sources.begin(), sources.end(), "portaudio") !=
                        sources.end();
    EXPECT_EQ(available, listed);

    if (available) {
        EXPECT_FALSE(nimrtc::audio_device::portaudio_version().empty());
    } else {
        EXPECT_TRUE(nimrtc::audio_device::portaudio_version().empty());
        EXPECT_TRUE(nimrtc::audio_device::portaudio_host_apis().empty());
    }
}

TEST(audio_device_module, the_portaudio_enumeration_does_not_crash) {
    // Even with no sound card, the enumeration must return cleanly (an
    // empty list is a legitimate answer).
    const auto devices = nimrtc::audio_device::enumerate_capture_devices();
    for (const auto& d : devices) {
        EXPECT_FALSE(d.id.empty());
    }
    const auto outputs = nimrtc::audio_device::enumerate_playback_devices();
    for (const auto& d : outputs) {
        EXPECT_FALSE(d.id.empty());
    }
}

} // namespace
