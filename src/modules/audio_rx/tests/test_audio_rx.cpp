// modules/audio_rx/tests/test_audio_rx.cpp
/**
 * @file src/modules/audio_rx/tests/test_audio_rx.cpp
 * @brief Acceptance tests of the audio receive pipeline (v1.2.0, audio
 *        pipeline WP-3 / WP-4).
 *
 * The rows covered here are the ones of the specification's V-1 table that
 * belong to this module:
 *
 *   - *Receiver timeline* -- a timestamp gap produces exactly the missing
 *     number of concealed frames;
 *   - *Receiver cadence*  -- the output sample count equals the media time
 *     times the rate, received plus concealed;
 *   - *Receiver downmix*  -- stereo to mono halves the channel count and
 *     preserves the sample count.
 *
 * ## Why the test doubles
 *
 * The module depends on `nimrtc::core` and `nimrtc::plugins` only: the
 * jitter buffer and the decoder are *injected* (spec B-3 / B-15).  The
 * tests therefore do not use the Opus or the JB module either -- they
 * register their own doubles under test-only ids and point
 * `AudioReceiverConfig::jb_plugin_id` / `decoder_plugin_id` at them.  That
 * keeps the module independently buildable and testable, which is the
 * property the module layout demands (L3).
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include <nimrtc/audio_rx/audio_rx.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/plugins/audio_decoder.hpp>
#include <nimrtc/plugins/audio_receiver.hpp>
#include <nimrtc/plugins/jb.hpp>

namespace {

// ---------------------------------------------------------------------------
// The doubles
// ---------------------------------------------------------------------------

/** A jitter buffer that hands the pushed payloads back in order.
 *
 *  It is deliberately trivial: the pipeline is what owns the timeline, so
 *  the double only has to be a faithful *carrier* — it stores the payload
 *  bytes (the pipeline owns them only until the frame is popped) and
 *  returns them one by one.
 */
class FakeJitterBuffer : public nimrtc::plugins::IJB {
public:
    const char* name() const noexcept override { return "test::FakeJitterBuffer"; }

    nimrtc::plugins::Status open() noexcept override { return nimrtc::plugins::kOk; }

    void close() noexcept override { queue_.clear(); }

    void set_callbacks(nimrtc::plugins::FrameReadyCallback,
                       nimrtc::plugins::FrameDropCallback) noexcept override {}

    nimrtc::plugins::Status configure(
        const nimrtc::plugins::JBConfig& cfg) noexcept override {
        config_ = cfg;
        s_last_config = cfg;
        s_configured = true;
        return nimrtc::plugins::kOk;
    }

    nimrtc::plugins::Status push(const nimrtc::plugins::RtpPacket& pkt,
                                  nimrtc::plugins::TimestampUs) noexcept override {
        ++stats_.packets_received;
        Entry e;
        e.ssrc = pkt.header.ssrc;
        e.rtp_ts = pkt.header.ts;
        e.seq = pkt.header.seq;
        e.payload.assign(pkt.payload.data(),
                         pkt.payload.data() + pkt.payload.size());
        queue_.push_back(std::move(e));
        return nimrtc::plugins::kOk;
    }

    std::optional<nimrtc::plugins::MediaSample>
    pop(nimrtc::plugins::TimestampUs now_us,
        nimrtc::plugins::TimestampUs* play_time_us = nullptr) noexcept override {
        if (queue_.empty()) return std::nullopt;

        Entry e = std::move(queue_.front());
        queue_.pop_front();

        nimrtc::plugins::MediaSample s;
        s.kind = nimrtc::plugins::MediaSample::Kind::kAudio;
        s.ssrc = e.ssrc;
        s.rtp_ts = e.rtp_ts;
        s.seq = e.seq;
        s.payload = nimrtc::core::ByteSpan(e.payload.data(), e.payload.size());
        ++stats_.frames_out;
        if (play_time_us) *play_time_us = now_us;
        return s;
    }

    void flush() noexcept override { queue_.clear(); }

    void apply_feedback(const std::vector<nimrtc::plugins::RtcpPacket>&) noexcept override {}

    Stats stats() const noexcept override { return stats_; }

    int estimated_delay_ms(nimrtc::plugins::TimestampUs) const noexcept override {
        return 0;
    }

    /** The configuration the pipeline handed over (spec B-3). */
    const nimrtc::plugins::JBConfig& config() const noexcept { return config_; }

    /** The last configuration any instance received.
     *
     *  The double is created inside the pipeline, so the test cannot hold
     *  a pointer to it; the configuration is therefore recorded in a
     *  file-local static.  The tests are single-threaded.
     */
    static nimrtc::plugins::JBConfig s_last_config;
    static bool s_configured;

private:
    struct Entry {
        std::uint32_t ssrc = 0;
        std::uint32_t rtp_ts = 0;
        std::uint16_t seq = 0;
        std::vector<std::uint8_t> payload;
    };

    std::deque<Entry> queue_;
    nimrtc::plugins::JBConfig config_;
    Stats stats_{};
};

nimrtc::plugins::JBConfig FakeJitterBuffer::s_last_config{};
bool FakeJitterBuffer::s_configured = false;

/** A decoder that produces a fixed-size ramp.
 *
 *  Every decoded frame carries `samples_per_channel` samples whose value
 *  identifies the frame (the sample index), so a test can tell the frames
 *  apart and can check the downmix arithmetic.
 */
class FakeDecoder : public nimrtc::plugins::IAudioDecoder {
public:
    /** The sample count of one frame (20 ms at 48 kHz). */
    static constexpr std::size_t kSamplesPerFrame = 960;

    FakeDecoder() = default;

    const char* name() const noexcept override { return "test::FakeDecoder"; }

    nimrtc::plugins::Status open() noexcept override { return nimrtc::plugins::kOk; }

    void close() noexcept override { pcm_.clear(); }

    nimrtc::plugins::AudioDecodeFormat format() const noexcept override { return fmt_; }

    nimrtc::plugins::Status update_format(
        nimrtc::plugins::AudioDecodeFormat f) noexcept override {
        fmt_ = f;
        return nimrtc::plugins::kOk;
    }

    std::size_t decode_packet(const std::uint8_t* payload, std::size_t payload_len,
                              std::uint32_t rtp_timestamp,
                              nimrtc::plugins::AudioDecodedFrame* out,
                              std::size_t max_frames) noexcept override {
        if (!payload || payload_len == 0 || !out || max_frames == 0) return 0;

        const std::uint8_t ch = fmt_.channels ? fmt_.channels : 1;
        pcm_.assign(kSamplesPerFrame * ch, 0.0f);
        for (std::size_t s = 0; s < kSamplesPerFrame; ++s) {
            for (std::uint8_t c = 0; c < ch; ++c) {
                // The value encodes the sample index and the channel, so a
                // downmix is verifiable: channel 0 carries `s`, channel 1
                // carries `s + 1000`.
                pcm_[s * ch + c] = static_cast<float>(s + 1000u * c);
            }
        }

        nimrtc::plugins::AudioDecodedFrame f;
        f.pcm = pcm_.data();
        f.samples_per_channel = kSamplesPerFrame;
        f.sample_rate_hz = fmt_.sample_rate_hz;
        f.channels = ch;
        f.rtp_timestamp = rtp_timestamp;
        f.duration_ms = static_cast<std::uint16_t>(
            (kSamplesPerFrame * 1000u) / (fmt_.sample_rate_hz ? fmt_.sample_rate_hz : 1u));
        f.concealed = false;
        f.fec_recovered = false;
        out[0] = f;

        ++stats_.frames_decoded;
        stats_.bytes_decoded += payload_len;
        return 1;
    }

    std::size_t decode_plc(std::uint32_t rtp_timestamp, std::uint16_t duration_ms,
                           nimrtc::plugins::AudioDecodedFrame* out,
                           std::size_t max_frames) noexcept override {
        if (!out || max_frames == 0 || duration_ms == 0) return 0;

        const std::uint8_t ch = fmt_.channels ? fmt_.channels : 1;
        const std::size_t samples =
            (static_cast<std::size_t>(fmt_.sample_rate_hz) * duration_ms) / 1000u;
        pcm_.assign(samples * ch, -1.0f);  // the concealment marker

        nimrtc::plugins::AudioDecodedFrame f;
        f.pcm = pcm_.data();
        f.samples_per_channel = samples;
        f.sample_rate_hz = fmt_.sample_rate_hz;
        f.channels = ch;
        f.rtp_timestamp = rtp_timestamp;
        f.duration_ms = duration_ms;
        f.concealed = true;
        f.fec_recovered = false;
        out[0] = f;

        ++stats_.plc_frames;
        return 1;
    }

    void reset() noexcept override { stats_ = nimrtc::plugins::IAudioDecoder::Stats{}; }

    nimrtc::plugins::IAudioDecoder::Stats stats() const noexcept override { return stats_; }

private:
    nimrtc::plugins::AudioDecodeFormat fmt_{};
    std::vector<float> pcm_;
    nimrtc::plugins::IAudioDecoder::Stats stats_{};
};

/** The factory of the fake jitter buffer. */
class FakeJitterBufferFactory : public nimrtc::plugins::IJBFactory {
public:
    std::string_view id() const noexcept override { return "test_fake_jb"; }
    std::string_view display_name() const noexcept override { return "test fake jb"; }
    nimrtc::plugins::IJB* create() const override { return new FakeJitterBuffer(); }
};

/** The factory of the fake decoder. */
class FakeDecoderFactory : public nimrtc::plugins::IAudioDecoderFactory {
public:
    std::string_view id() const noexcept override { return "test_fake_decoder"; }
    std::string_view display_name() const noexcept override { return "test fake decoder"; }
    nimrtc::plugins::IAudioDecoder* create() const override { return new FakeDecoder(); }
};

// ---------------------------------------------------------------------------
// The fixture
// ---------------------------------------------------------------------------

/** One frame the pipeline emitted. */
struct Emitted {
    std::uint32_t rtp_timestamp = 0;
    std::size_t samples_per_channel = 0;
    std::uint8_t channels = 0;
    bool concealed = false;
    bool fec_recovered = false;
    std::vector<float> pcm;
};

class AudioRxTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto& reg = nimrtc::core::PluginRegistry::instance();

        static const FakeJitterBufferFactory s_jb_factory{};
        static const FakeDecoderFactory s_decoder_factory{};
        reg.register_jb(s_jb_factory.id(), &s_jb_factory);
        reg.register_audio_decoder(s_decoder_factory.id(), &s_decoder_factory);

        // The module under test registers itself.
        nimrtc::audio_rx::register_default_plugins();
    }

    void TearDown() override {
        // The registry is a process-wide singleton (L6): the doubles stay
        // registered, but the pipeline instances are per test.
        frames_.clear();
    }

    /** The configuration the tests use. */
    nimrtc::plugins::AudioReceiverConfig make_config() const {
        nimrtc::plugins::AudioReceiverConfig cfg;
        cfg.jb_plugin_id = "test_fake_jb";
        cfg.decoder_plugin_id = "test_fake_decoder";
        cfg.clock_rate_hz = 48000;
        cfg.decoder_sample_rate_hz = 48000;
        cfg.decoder_channels = 2;
        cfg.output_channels = 1;
        cfg.frame_duration_ms = 20;
        cfg.enable_downmix = true;
        cfg.enable_fec_recovery = true;
        cfg.max_concealed_frames = 10;
        return cfg;
    }

    /** Create the receiver under test through the registry (B-15). */
    std::unique_ptr<nimrtc::plugins::IAudioReceiver> make_receiver(
        const nimrtc::plugins::AudioReceiverConfig& cfg) {
        const auto* factory =
            nimrtc::core::PluginRegistry::instance().get_audio_receiver("nimrtc_audio_rx");
        EXPECT_NE(factory, nullptr);
        if (!factory) return nullptr;

        auto* receiver = factory->create(cfg);
        EXPECT_NE(receiver, nullptr);
        if (!receiver) return nullptr;

        std::unique_ptr<nimrtc::plugins::IAudioReceiver> owned(receiver);
        owned->set_callbacks(
            [this](const nimrtc::plugins::AudioOutputFrame& f) {
                Emitted e;
                e.rtp_timestamp = f.rtp_timestamp;
                e.samples_per_channel = f.samples_per_channel;
                e.channels = f.channels;
                e.concealed = f.concealed;
                e.fec_recovered = f.fec_recovered;
                if (f.pcm && f.samples_per_channel && f.channels) {
                    e.pcm.assign(f.pcm,
                                 f.pcm + f.samples_per_channel * f.channels);
                }
                frames_.push_back(std::move(e));
            },
            [](nimrtc::plugins::Status, std::string_view) {});
        return owned;
    }

    /** Feed one payload of @p len bytes at @p ts into the receiver. */
    void push(nimrtc::plugins::IAudioReceiver& r, std::uint32_t ts,
              std::uint16_t seq, std::size_t len = 4) {
        std::vector<std::uint8_t> payload(len, 0x42);
        r.push(payload.data(), payload.size(), ts, seq, 0x11223344u, 0);
    }

    std::vector<Emitted> frames_;
};

// ---------------------------------------------------------------------------
// V-1: Receiver timeline
// ---------------------------------------------------------------------------

TEST_F(AudioRxTest, TimelineGapProducesExactlyTheMissingConcealedFrames) {
    auto rx = make_receiver(make_config());
    ASSERT_TRUE(rx);

    // A continuous stream first: the first frame defines the origin.
    push(*rx, 0, 1);
    rx->tick(1000);
    ASSERT_EQ(frames_.size(), 1u);
    EXPECT_EQ(frames_[0].rtp_timestamp, 0u);
    EXPECT_FALSE(frames_[0].concealed);

    // Then a jump: the frames at 960 and 1920 never arrived.
    push(*rx, 2880, 4);
    rx->tick(2000);

    ASSERT_EQ(frames_.size(), 4u);
    EXPECT_EQ(frames_[1].rtp_timestamp, 960u);
    EXPECT_TRUE(frames_[1].concealed);
    EXPECT_EQ(frames_[2].rtp_timestamp, 1920u);
    EXPECT_TRUE(frames_[2].concealed);
    EXPECT_EQ(frames_[3].rtp_timestamp, 2880u);
    EXPECT_FALSE(frames_[3].concealed);

    // Exactly the missing number -- no more, no less.
    const std::size_t concealed =
        std::count_if(frames_.begin(), frames_.end(),
                      [](const Emitted& e) { return e.concealed; });
    EXPECT_EQ(concealed, 2u);
}

TEST_F(AudioRxTest, TimelineCountsTheLossInTheStatistics) {
    auto rx = make_receiver(make_config());
    ASSERT_TRUE(rx);

    push(*rx, 0, 1);
    rx->tick(1000);
    push(*rx, 2880, 4);
    rx->tick(2000);

    const auto stats = rx->stats();
    EXPECT_EQ(stats.packets_received, 2u);
    EXPECT_EQ(stats.frames_played_out, 4u);
    EXPECT_EQ(stats.frames_concealed, 2u);
    EXPECT_EQ(stats.packets_lost, 2u);
}

// ---------------------------------------------------------------------------
// V-1: Receiver cadence
// ---------------------------------------------------------------------------

TEST_F(AudioRxTest, CadenceOutputSampleCountEqualsMediaTimeTimesRate) {
    auto rx = make_receiver(make_config());
    ASSERT_TRUE(rx);

    // Three consecutive 20 ms frames at 48 kHz: 60 ms of media time.
    push(*rx, 0, 1);
    push(*rx, 960, 2);
    push(*rx, 1920, 3);
    rx->tick(3000);

    ASSERT_EQ(frames_.size(), 3u);

    std::size_t total = 0;
    for (const auto& f : frames_) total += f.samples_per_channel;

    // 60 ms * 48000 Hz = 2880 samples per channel.
    EXPECT_EQ(total, 3u * FakeDecoder::kSamplesPerFrame);
    EXPECT_EQ(total, 2880u);

    // The media time walked from 0 to 2880 RTP units at 48 kHz: the last
    // frame starts at 1920 and is 960 units long.
    EXPECT_EQ(frames_.back().rtp_timestamp + frames_.back().samples_per_channel, 2880u);
}

TEST_F(AudioRxTest, CadenceReceivedPlusConcealedEqualsTheTotalFrameCount) {
    auto rx = make_receiver(make_config());
    ASSERT_TRUE(rx);

    push(*rx, 0, 1);
    rx->tick(1000);
    push(*rx, 2880, 4);   // two frames are missing
    rx->tick(2000);

    ASSERT_EQ(frames_.size(), 4u);

    const std::size_t received =
        std::count_if(frames_.begin(), frames_.end(),
                      [](const Emitted& e) { return !e.concealed; });
    const std::size_t concealed =
        std::count_if(frames_.begin(), frames_.end(),
                      [](const Emitted& e) { return e.concealed; });
    EXPECT_EQ(received + concealed, frames_.size());
    EXPECT_EQ(received, 2u);
    EXPECT_EQ(concealed, 2u);
}

// ---------------------------------------------------------------------------
// V-1: Receiver downmix
// ---------------------------------------------------------------------------

TEST_F(AudioRxTest, DownmixStereoToMonoHalvesTheChannelsAndKeepsTheSamples) {
    auto rx = make_receiver(make_config());
    ASSERT_TRUE(rx);

    push(*rx, 0, 1);
    rx->tick(1000);

    ASSERT_EQ(frames_.size(), 1u);
    const Emitted& f = frames_[0];

    // The decoder produced stereo, the receiver delivers mono.
    EXPECT_EQ(f.channels, 1u);
    EXPECT_EQ(f.pcm.size(), FakeDecoder::kSamplesPerFrame);
    EXPECT_EQ(f.samples_per_channel, FakeDecoder::kSamplesPerFrame);

    // The mix is the average of the two channels: channel 0 carries `s`,
    // channel 1 carries `s + 1000`.
    for (std::size_t s = 0; s < f.samples_per_channel; ++s) {
        const float expected =
            (static_cast<float>(s) + static_cast<float>(s + 1000u)) / 2.0f;
        EXPECT_FLOAT_EQ(f.pcm[s], expected);
    }
}

TEST_F(AudioRxTest, DownmixIsANoOpWhenTheChannelCountsMatch) {
    auto cfg = make_config();
    cfg.decoder_channels = 1;
    cfg.output_channels = 1;

    auto rx = make_receiver(cfg);
    ASSERT_TRUE(rx);

    push(*rx, 0, 1);
    rx->tick(1000);

    ASSERT_EQ(frames_.size(), 1u);
    EXPECT_EQ(frames_[0].channels, 1u);
    EXPECT_EQ(frames_[0].samples_per_channel, FakeDecoder::kSamplesPerFrame);
    // Mono in, mono out -- the samples are untouched.
    EXPECT_FLOAT_EQ(frames_[0].pcm[0], 0.0f);
    EXPECT_FLOAT_EQ(frames_[0].pcm[1], 1.0f);
}

// ---------------------------------------------------------------------------
// The concealment cap (spec B-7)
// ---------------------------------------------------------------------------

TEST_F(AudioRxTest, BeyondTheConcealmentCapTheReceiverEmitsSilence) {
    auto cfg = make_config();
    cfg.max_concealed_frames = 2;

    auto rx = make_receiver(cfg);
    ASSERT_TRUE(rx);

    push(*rx, 0, 1);
    rx->tick(1000);
    push(*rx, 4800, 7);   // four frames are missing
    rx->tick(2000);

    // The first frame, four missing slots, the frame that arrived.
    ASSERT_EQ(frames_.size(), 6u);

    // The first two missing slots are concealment frames ...
    EXPECT_TRUE(frames_[1].concealed);
    EXPECT_TRUE(frames_[2].concealed);
    EXPECT_FLOAT_EQ(frames_[1].pcm[0], -1.0f);   // the PLC marker

    // ... the next two are silence, not synthetic audio.
    EXPECT_TRUE(frames_[3].concealed);
    EXPECT_TRUE(frames_[4].concealed);
    for (float v : frames_[3].pcm) EXPECT_FLOAT_EQ(v, 0.0f);
    for (float v : frames_[4].pcm) EXPECT_FLOAT_EQ(v, 0.0f);

    // The frame that finally arrived is a normal one.
    EXPECT_FALSE(frames_[5].concealed);
    EXPECT_EQ(frames_[5].rtp_timestamp, 4800u);

    // The counter only counts the frames that were really concealed.
    EXPECT_EQ(rx->stats().frames_concealed, 2u);
}

// ---------------------------------------------------------------------------
// The injection (spec B-3 / B-15 / B-16)
// ---------------------------------------------------------------------------

TEST_F(AudioRxTest, TheJitterBufferIsConfiguredWithTheStreamClockRate) {
    FakeJitterBuffer::s_configured = false;

    auto rx = make_receiver(make_config());
    ASSERT_TRUE(rx);

    push(*rx, 0, 1);
    rx->tick(1000);

    // The pipeline must have configured the jitter buffer with the
    // *stream* clock -- a buffer that assumes 90 kHz mis-measures the
    // jitter of a 48 kHz stream (spec B-3 / F-6).
    ASSERT_TRUE(FakeJitterBuffer::s_configured);
    EXPECT_EQ(FakeJitterBuffer::s_last_config.clock_rate_hz, 48000u);
    EXPECT_EQ(FakeJitterBuffer::s_last_config.ssrc, 0x11223344u);
}

TEST_F(AudioRxTest, AMissingDecoderPluginMakesTheFactoryReturnNoProduct) {
    auto cfg = make_config();
    cfg.decoder_plugin_id = "no_such_decoder";

    const auto* factory =
        nimrtc::core::PluginRegistry::instance().get_audio_receiver("nimrtc_audio_rx");
    ASSERT_NE(factory, nullptr);

    // Spec B-16: there is no fallback receiver -- the caller gets a null
    // product and turns it into a hard error.
    EXPECT_EQ(factory->create(cfg), nullptr);
}

TEST_F(AudioRxTest, AMissingJitterBufferPluginMakesTheFactoryReturnNoProduct) {
    auto cfg = make_config();
    cfg.jb_plugin_id = "no_such_jb";

    const auto* factory =
        nimrtc::core::PluginRegistry::instance().get_audio_receiver("nimrtc_audio_rx");
    ASSERT_NE(factory, nullptr);
    EXPECT_EQ(factory->create(cfg), nullptr);
}

TEST_F(AudioRxTest, TheModuleIsRegisteredUnderItsDocumentedId) {
    const auto* factory =
        nimrtc::core::PluginRegistry::instance().get_audio_receiver("nimrtc_audio_rx");
    ASSERT_NE(factory, nullptr);
    EXPECT_EQ(factory->id(), "nimrtc_audio_rx");
}

} // namespace
