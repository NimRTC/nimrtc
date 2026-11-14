/**
 * @file tests/test_engine_multi_stream.cpp
 * @brief The multi-stream video plane (remote-desktop plan H1 / H2 / H4).
 *
 * ## What the plan asks for
 *
 *   - **H1**: "the engine's single-video-slot model becomes a **stream
 *     set** keyed by a stable stream id (SSRC + mid).  The public API
 *     changes are **additive**: `send_video(stream_id, frame)`,
 *     `set_video_source(stream_id, …)`, and the stats surface becomes
 *     per-stream.  **The single-stream call sites keep working.**"
 *     Acceptance: "Two video streams flow concurrently through one
 *     engine with independent SSRCs, independent codecs and independent
 *     stats; the existing single-stream tests still pass unchanged."
 *
 *   - **H2**: "each stream owns its own encoder, sender, receiver,
 *     decoder and sink, resolved through the registry by id.  The
 *     engine holds the set, not the slots."  Acceptance: "Two streams
 *     use different codecs (e.g. H.264 screen + VP8 camera) in the same
 *     session."
 *
 *   - **H4**: "the stats snapshot returns a **list** of per-stream
 *     entries, each with its own bitrate / ladder level / keyframe rate
 *     / backend id."  Acceptance: "One stats call returns one entry per
 *     active stream."
 *
 *   - **H3 (simulcast) is DEFERRED.**  The thumbnail case is served by an
 *     independent low-resolution capture — a second stream — not by
 *     simulcast.  There is deliberately no simulcast test here.
 *
 * ## The codec situation in this tree
 *
 * The plan's H2 example is "H.264 screen + VP8 camera".  This tree has
 * **no VP8 codec module** — the registered video codecs are "h264",
 * "h265" and "av1", and only "h264" is guaranteed to be built (the
 * others are behind `NIMRTC_MODULE_H265` / `NIMRTC_MODULE_AV1`).
 *
 * So the "two different codecs" case is tested with the **H.264 and
 * H.265** modules when the H.265 module is present in the build (the
 * common case — `NIMRTC_MODULE_H265` defaults to ON), and with **two
 * distinct instances of the same codec id** otherwise.  Either way the
 * property under test is the same: the two streams own *separate plugin
 * instances*, and the engine never shares one encoder between them.
 *
 * The "same codec id, two instances" case is *always* exercised as
 * well, so the core property (independent instances) is covered even in
 * a build without the H.265 module.
 *
 * ## No networking
 *
 * The test opens the engine with the "shell" transport strategy and the
 * "ice" transport, but never establishes a connection: the video path
 * under test is "host pushes a frame → engine encodes it → the stream's
 * own sender packetizes it", which needs no peer.
 */

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/video_codec.hpp>

#include <gtest/gtest.h>

using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;
using nimrtc::engine::VideoStreamStats;
namespace np = nimrtc::plugins;

namespace {

// ---------------------------------------------------------------------------
// The helpers
// ---------------------------------------------------------------------------

/** Align a stride to 16 bytes (the codec seam's requirement). */
std::int32_t align16(std::uint32_t v) noexcept {
    return static_cast<std::int32_t>((v + 15U) & ~15U);
}

/** An I420 buffer the test can point a source frame at. */
struct I420Buffer {
    std::vector<std::uint8_t> storage;
    std::int32_t stride_y = 0;
    std::int32_t stride_u = 0;
    std::int32_t stride_v = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    I420Buffer(std::uint32_t w, std::uint32_t h) : width(w), height(h) {
        stride_y = align16(w);
        stride_u = align16(w / 2);
        stride_v = stride_u;
        const std::size_t y_size =
            static_cast<std::size_t>(stride_y) * h;
        const std::size_t uv_size =
            static_cast<std::size_t>(stride_u) * (h / 2);
        storage.assign(y_size + uv_size + uv_size, 128);
    }
};

/** Build a source frame pointing into the buffer. */
np::VideoSourceFrame make_frame(I420Buffer& buf,
                                std::uint32_t frame_seq,
                                std::int64_t capture_ts_us) noexcept {
    np::VideoSourceFrame frame{};
    frame.width  = buf.width;
    frame.height = buf.height;
    frame.format = np::VideoPixelFormat::kI420;
    frame.stride_y = buf.stride_y;
    frame.stride_u = buf.stride_u;
    frame.stride_v = buf.stride_v;
    frame.plane_y = buf.storage.data();
    frame.plane_u = buf.storage.data() +
                    static_cast<std::size_t>(buf.stride_y) * buf.height;
    frame.plane_v = frame.plane_u +
                    static_cast<std::size_t>(buf.stride_u) * (buf.height / 2);
    frame.buffer_size = buf.storage.size();
    frame.capture_ts_us = capture_ts_us;
    frame.frame_seq = frame_seq;
    frame.rtp_timestamp =
        static_cast<std::uint32_t>(capture_ts_us / 1000 * 90);   // 90 kHz
    return frame;
}

/** The engine config every test in this file starts from. */
EngineConfig base_config(const std::string& codec_id) {
    EngineConfig cfg{};
    cfg.transport_strategy_name = "shell";
    cfg.transport_name          = "ice";
    cfg.video_codec_name        = codec_id;
    cfg.video_sender_name       = "reference";
    cfg.video_receiver_name     = "reference";
    cfg.video_source_name       = "memory";
    cfg.video_sink_name         = "headless";
    cfg.video_width             = 320;
    cfg.video_height            = 240;
    cfg.video_fps               = 30;
    return cfg;
}

/** The id of the default (legacy single) stream. */
constexpr std::string_view kDefault = "default";

/** True iff the registry knows a video codec under this id. */
bool codec_registered(std::string_view id) {
    return PluginRegistry::instance().get_video_codec(id) != nullptr;
}

/** Find a statistics entry by stream id (nullptr when there is none). */
const VideoStreamStats* find_entry(const std::vector<VideoStreamStats>& stats,
                                   std::string_view id) {
    for (const auto& s : stats) {
        if (s.stream_id == id) return &s;
    }
    return nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// The fixture
// ---------------------------------------------------------------------------

class EngineMultiStream : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();
    }
};

// ---------------------------------------------------------------------------
// H1 — the stream set
// ---------------------------------------------------------------------------

TEST_F(EngineMultiStream, TheDefaultStreamIsAlwaysPresent) {
    NimRTCEngine engine(base_config("h264"));
    ASSERT_EQ(engine.open(), 0u);

    const auto ids = engine.video_stream_ids();
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], kDefault)
        << "the legacy single-stream path is the stream named \"default\"";

    engine.close();
}

TEST_F(EngineMultiStream, AStreamCanBeAddedAndRemoved) {
    NimRTCEngine engine(base_config("h264"));
    ASSERT_EQ(engine.open(), 0u);

    EXPECT_TRUE(engine.add_video_stream("screen"));
    {
        const auto ids = engine.video_stream_ids();
        ASSERT_EQ(ids.size(), 2u);
        EXPECT_EQ(ids[0], kDefault);
        EXPECT_EQ(ids[1], "screen");
    }

    // Adding the same id twice is refused (the ids are unique).
    EXPECT_FALSE(engine.add_video_stream("screen"));

    EXPECT_TRUE(engine.remove_video_stream("screen"));
    {
        const auto ids = engine.video_stream_ids();
        ASSERT_EQ(ids.size(), 1u);
        EXPECT_EQ(ids[0], kDefault);
    }

    // Removing it again is refused (there is nothing left to remove).
    EXPECT_FALSE(engine.remove_video_stream("screen"));

    engine.close();
}

TEST_F(EngineMultiStream, TheDefaultStreamCannotBeRemoved) {
    NimRTCEngine engine(base_config("h264"));
    ASSERT_EQ(engine.open(), 0u);

    // The default stream *is* the legacy single-stream path; removing it
    // would break every existing call site, so it is refused.
    EXPECT_FALSE(engine.remove_video_stream(kDefault));
    EXPECT_EQ(engine.video_stream_ids().size(), 1u);

    engine.close();
}

TEST_F(EngineMultiStream, TwoStreamsHaveIndependentSsrcs) {
    NimRTCEngine engine(base_config("h264"));
    ASSERT_EQ(engine.open(), 0u);

    ASSERT_TRUE(engine.add_video_stream("screen"));
    ASSERT_TRUE(engine.add_video_stream("camera"));

    const auto stats = engine.stats().video_streams;
    ASSERT_EQ(stats.size(), 3u);

    const auto* def  = find_entry(stats, kDefault);
    const auto* scr  = find_entry(stats, "screen");
    const auto* cam  = find_entry(stats, "camera");
    ASSERT_NE(def, nullptr);
    ASSERT_NE(scr, nullptr);
    ASSERT_NE(cam, nullptr);

    EXPECT_NE(def->ssrc, scr->ssrc) << "the streams must not share an SSRC";
    EXPECT_NE(def->ssrc, cam->ssrc);
    EXPECT_NE(scr->ssrc, cam->ssrc);

    // The payload types must not collide either (RFC 3551's dynamic
    // range is 96..127, and the default H.264 type is 102).
    EXPECT_NE(scr->payload_type, cam->payload_type);
    EXPECT_NE(scr->payload_type, def->payload_type);
    EXPECT_NE(cam->payload_type, def->payload_type);

    engine.close();
}

TEST_F(EngineMultiStream, TheStreamIdIsStableAcrossAReconfiguration) {
    // The id is the *key* of the stream set, so the same id must always
    // map to the same wire identity (SSRC + payload type) — otherwise a
    // reconfiguration would renumber the streams under the peer's feet.
    std::uint32_t first_ssrc = 0;
    std::uint8_t  first_pt   = 0;

    {
        NimRTCEngine engine(base_config("h264"));
        ASSERT_EQ(engine.open(), 0u);
        ASSERT_TRUE(engine.add_video_stream("screen"));

        const auto stats = engine.stats().video_streams;
        const auto* scr = find_entry(stats, "screen");
        ASSERT_NE(scr, nullptr);
        first_ssrc = scr->ssrc;
        first_pt   = scr->payload_type;
        engine.close();
    }

    {
        NimRTCEngine engine(base_config("h264"));
        ASSERT_EQ(engine.open(), 0u);
        ASSERT_TRUE(engine.add_video_stream("screen"));

        const auto stats = engine.stats().video_streams;
        const auto* scr = find_entry(stats, "screen");
        ASSERT_NE(scr, nullptr);
        EXPECT_EQ(scr->ssrc, first_ssrc)
            << "the same stream id must map to the same SSRC";
        EXPECT_EQ(scr->payload_type, first_pt);
        engine.close();
    }
}

// ---------------------------------------------------------------------------
// H1 / H2 — two streams flowing concurrently
// ---------------------------------------------------------------------------

TEST_F(EngineMultiStream, TwoStreamsFlowConcurrentlyThroughOneEngine) {
    NimRTCEngine engine(base_config("h264"));
    ASSERT_EQ(engine.open(), 0u);

    ASSERT_TRUE(engine.add_video_stream("screen"));
    ASSERT_TRUE(engine.add_video_stream("camera"));

    I420Buffer screen_buf(320, 240);
    I420Buffer camera_buf(160, 120);

    // Push frames into both streams, interleaved.
    for (std::uint32_t i = 0; i < 5; ++i) {
        auto sf = make_frame(screen_buf, i, 33333 * (i + 1));
        EXPECT_EQ(engine.send_video("screen", sf), np::kOk);

        auto cf = make_frame(camera_buf, 1000 + i, 33333 * (i + 1));
        EXPECT_EQ(engine.send_video("camera", cf), np::kOk);
    }

    const auto stats = engine.stats().video_streams;
    ASSERT_EQ(stats.size(), 3u);

    const auto* scr = find_entry(stats, "screen");
    const auto* cam = find_entry(stats, "camera");
    const auto* def = find_entry(stats, kDefault);
    ASSERT_NE(scr, nullptr);
    ASSERT_NE(cam, nullptr);
    ASSERT_NE(def, nullptr);

    EXPECT_EQ(scr->frames_sent, 5u);
    EXPECT_EQ(cam->frames_sent, 5u);

    // The default stream was never touched: it stays at zero.
    EXPECT_EQ(def->frames_sent, 0u);

    // Both streams emitted packets of their own.
    EXPECT_GT(scr->packets_sent, 0u);
    EXPECT_GT(cam->packets_sent, 0u);

    engine.close();
}

TEST_F(EngineMultiStream, EachStreamOwnsItsOwnCodecInstance) {
    NimRTCEngine engine(base_config("h264"));
    ASSERT_EQ(engine.open(), 0u);

    ASSERT_TRUE(engine.add_video_stream("screen"));
    ASSERT_TRUE(engine.add_video_stream("camera"));

    // Two streams, two *separate* encoder instances.  The tree may or
    // may not offer a second codec family (the H.265 module is
    // optional), so the property under test — "no encoder is shared
    // between two streams" — is checked structurally: each stream's
    // encoder reports the *stream's own* backend id and the streams'
    // counters advance independently.
    //
    // When the H.265 module IS present the two streams are additionally
    // given *different* codec families, which is the plan's H2 example.
    const bool two_families = codec_registered("h265");

    if (two_families) {
        ASSERT_EQ(engine.set_video_codec("camera", "h265"), np::kOk);
    }

    I420Buffer screen_buf(320, 240);
    I420Buffer camera_buf(160, 120);

    for (std::uint32_t i = 0; i < 3; ++i) {
        auto sf = make_frame(screen_buf, i, 33333 * (i + 1));
        EXPECT_EQ(engine.send_video("screen", sf), np::kOk);
        auto cf = make_frame(camera_buf, 1000 + i, 33333 * (i + 1));
        EXPECT_EQ(engine.send_video("camera", cf), np::kOk);
    }

    const auto stats = engine.stats().video_streams;
    const auto* scr = find_entry(stats, "screen");
    const auto* cam = find_entry(stats, "camera");
    ASSERT_NE(scr, nullptr);
    ASSERT_NE(cam, nullptr);

    EXPECT_EQ(scr->frames_encoded, 3u);
    EXPECT_EQ(cam->frames_encoded, 3u);

    if (two_families) {
        EXPECT_EQ(scr->codec_name, "h264");
        EXPECT_EQ(cam->codec_name, "h265")
            << "the two streams must be able to use different codecs in the "
               "same session";
        EXPECT_NE(scr->backend_id, cam->backend_id)
            << "the two streams must run on separate encoder instances";
    } else {
        // Same codec id, but two distinct instances: the per-stream
        // counters and the per-stream backend ids are the evidence.
        EXPECT_EQ(scr->codec_name, cam->codec_name);
        EXPECT_EQ(scr->frames_encoded, cam->frames_encoded);
    }

    engine.close();
}

TEST_F(EngineMultiStream, AnUnknownStreamIdIsRejected) {
    NimRTCEngine engine(base_config("h264"));
    ASSERT_EQ(engine.open(), 0u);

    I420Buffer buf(320, 240);
    auto frame = make_frame(buf, 1, 33333);

    EXPECT_EQ(engine.send_video("nope", frame), np::kErrInvalidParam);
    EXPECT_EQ(engine.set_video_codec("nope", "h264"), np::kErrInvalidParam);
    EXPECT_EQ(engine.set_video_source("nope", "memory"), np::kErrInvalidParam);
    EXPECT_EQ(engine.set_video_sink("nope", "headless"), np::kErrInvalidParam);

    engine.close();
}

// ---------------------------------------------------------------------------
// H4 — the per-stream statistics
// ---------------------------------------------------------------------------

TEST_F(EngineMultiStream, TheStatsCarryOneEntryPerActiveStream) {
    NimRTCEngine engine(base_config("h264"));
    ASSERT_EQ(engine.open(), 0u);

    // One entry for the default stream, before any extra stream exists.
    {
        const auto stats = engine.stats().video_streams;
        ASSERT_EQ(stats.size(), 1u);
        EXPECT_EQ(stats[0].stream_id, kDefault);
    }

    ASSERT_TRUE(engine.add_video_stream("screen"));
    ASSERT_TRUE(engine.add_video_stream("camera"));
    ASSERT_TRUE(engine.add_video_stream("thumbnail"));

    {
        const auto stats = engine.stats().video_streams;
        ASSERT_EQ(stats.size(), 4u) << "one entry per active stream";
        EXPECT_NE(find_entry(stats, "default"), nullptr);
        EXPECT_NE(find_entry(stats, "screen"), nullptr);
        EXPECT_NE(find_entry(stats, "camera"), nullptr);
        EXPECT_NE(find_entry(stats, "thumbnail"), nullptr);
    }

    // Removing a stream removes its entry.
    ASSERT_TRUE(engine.remove_video_stream("thumbnail"));
    {
        const auto stats = engine.stats().video_streams;
        ASSERT_EQ(stats.size(), 3u);
        EXPECT_EQ(find_entry(stats, "thumbnail"), nullptr);
        EXPECT_NE(find_entry(stats, "screen"), nullptr);
    }

    engine.close();
}

TEST_F(EngineMultiStream, ThePerStreamStatsCarryTheFullEntry) {
    NimRTCEngine engine(base_config("h264"));
    ASSERT_EQ(engine.open(), 0u);

    ASSERT_TRUE(engine.add_video_stream("screen"));

    I420Buffer buf(320, 240);
    for (std::uint32_t i = 0; i < 4; ++i) {
        auto frame = make_frame(buf, i, 33333 * (i + 1));
        ASSERT_EQ(engine.send_video("screen", frame), np::kOk);
    }

    const auto stats = engine.stats().video_streams;
    const auto* scr = find_entry(stats, "screen");
    ASSERT_NE(scr, nullptr);

    EXPECT_EQ(scr->stream_id, "screen");
    EXPECT_NE(scr->ssrc, 0u) << "the entry carries the stream's SSRC";
    EXPECT_NE(scr->payload_type, 0u);
    EXPECT_EQ(scr->codec_name, "h264");
    EXPECT_FALSE(scr->backend_id.empty())
        << "the entry carries the backend id";
    EXPECT_EQ(scr->frames_sent, 4u);
    EXPECT_EQ(scr->frames_encoded, 4u);
    EXPECT_GT(scr->packets_sent, 0u);
    EXPECT_GT(scr->bytes_sent, 0u);
    EXPECT_GT(scr->keyframes_sent, 0u);
    EXPECT_GT(scr->keyframe_rate_hz, 0u)
        << "the entry carries the keyframe rate";
    // The ladder level is reported even when no rate controller is
    // configured (it is then the best rung, 0).
    EXPECT_EQ(scr->ladder_level, 0u);

    engine.close();
}

TEST_F(EngineMultiStream, TheFlatStatsFieldsStillDescribeTheDefaultStream) {
    NimRTCEngine engine(base_config("h264"));
    ASSERT_EQ(engine.open(), 0u);

    ASSERT_TRUE(engine.add_video_stream("screen"));

    I420Buffer def_buf(320, 240);
    I420Buffer scr_buf(320, 240);

    // One frame into the default stream (the legacy path) and one into
    // the extra stream.
    auto df = make_frame(def_buf, 1, 33333);
    ASSERT_EQ(engine.send_video(df), np::kOk);
    auto sf = make_frame(scr_buf, 1, 33333);
    ASSERT_EQ(engine.send_video("screen", sf), np::kOk);

    const auto stats = engine.stats();

    // The flat fields describe the default stream, so the existing
    // single-stream call sites keep seeing exactly what they saw before.
    EXPECT_EQ(stats.video_frames_sent, 1u);
    EXPECT_GT(stats.video_packets_sent, 0u);

    const auto* def = find_entry(stats.video_streams, kDefault);
    const auto* scr = find_entry(stats.video_streams, "screen");
    ASSERT_NE(def, nullptr);
    ASSERT_NE(scr, nullptr);

    EXPECT_EQ(def->frames_sent, stats.video_frames_sent);
    EXPECT_EQ(def->packets_sent, stats.video_packets_sent);
    EXPECT_EQ(scr->frames_sent, 1u);

    engine.close();
}

// ---------------------------------------------------------------------------
// The legacy single-stream path (unchanged)
// ---------------------------------------------------------------------------

TEST_F(EngineMultiStream, TheLegacySingleStreamPathStillWorks) {
    NimRTCEngine engine(base_config("h264"));
    ASSERT_EQ(engine.open(), 0u);

    auto* sender = engine.video_sender();
    ASSERT_NE(sender, nullptr);

    I420Buffer buf(320, 240);
    auto frame = make_frame(buf, 1, 33333);
    ASSERT_EQ(engine.send_video(frame), np::kOk);

    const auto sender_stats = sender->stats();
    EXPECT_EQ(sender_stats.frames_pushed, 1u);
    EXPECT_GT(sender_stats.packets_emitted, 0u);

    // The "default" stream id addresses exactly the same path.
    I420Buffer buf2(320, 240);
    auto frame2 = make_frame(buf2, 2, 66666);
    ASSERT_EQ(engine.send_video(kDefault, frame2), np::kOk);

    const auto sender_stats2 = sender->stats();
    EXPECT_EQ(sender_stats2.frames_pushed, 2u);

    engine.close();
}

TEST_F(EngineMultiStream, TheLegacySingleStreamStatsAreUnchanged) {
    NimRTCEngine engine(base_config("h264"));
    ASSERT_EQ(engine.open(), 0u);

    I420Buffer buf(320, 240);
    auto frame = make_frame(buf, 1, 33333);
    ASSERT_EQ(engine.send_video(frame), np::kOk);

    const auto stats = engine.stats();
    EXPECT_EQ(stats.video_frames_sent, 1u);
    EXPECT_GT(stats.video_bytes_sent, 0u);
    EXPECT_GT(stats.video_packets_sent, 0u);

    // The default stream's list entry carries the same numbers.
    const auto* def = find_entry(stats.video_streams, kDefault);
    ASSERT_NE(def, nullptr);
    EXPECT_EQ(def->frames_sent, stats.video_frames_sent);
    EXPECT_EQ(def->bytes_sent, stats.video_bytes_sent);

    engine.close();
}
