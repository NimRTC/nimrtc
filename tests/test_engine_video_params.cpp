/**
 * @file tests/test_engine_video_params.cpp
 * @brief The encoder's parameter set on the engine side
 *        (remote-desktop plan B12 / B13 / C5 / B7).
 *
 * The engine is the *only* place that computes the encoder's parameter
 * set (see `engine_video_params.hpp`), and it is the only place that
 * pushes it through `IVideoCodec::set_params()`.  These tests pin that
 * contract from the outside:
 *
 *   1. **B12** ? the engine pushes a parameter set, and the codec
 *      receives it (the codec is the only one that can know).
 *   2. **B13** ? when the frame's geometry changes, the engine tells
 *      the codec, and the codec restarts (its `params()` reports the
 *      new geometry).  This is what puts the new SPS / PPS on the wire
 *      with the next frame.
 *   3. **B7** ? the content hint reaches the SDP, in both the standard
 *      `a=content-hint:` spelling and the `x-google-content-hint`
 *      fmtp parameter, and the two agree.
 *   4. **C5** ? the colour description travels with the picture: the
 *      frame's description reaches the encoder's parameter set.
 *
 * No ICE/DTLS/SRTP networking is involved.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/video_codec.hpp>

namespace {

using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;
namespace np = nimrtc::plugins;

// ---------------------------------------------------------------------------
// The recorder
// ---------------------------------------------------------------------------
//
// The engine owns the codec plugin (it takes ownership of the factory's
// raw pointer and deletes it on teardown), so the recorder cannot be
// owned by the test ? it has to outlive the engine.  A process-level
// singleton is the right shape here: the tests are sequential, and each
// one resets the recorder before it starts.

/** What the engine pushed to the codec. */
struct PushRecord {
    int calls = 0;
    np::VideoEncoderParams last{};
};

/** The process-wide recorder. */
PushRecord& recorder() {
    static PushRecord r;
    return r;
}

/** A codec plugin that records what the engine pushed to it.
 *
 *  The engine's job is to *push* the parameter set; whether the codec
 *  honours it is the codec's business.  A recording codec is the only
 *  way to observe the push from the outside ? the real H.264 codec
 *  applies the parameters and reports them back, but it cannot tell
 *  "the engine never pushed" from "the engine pushed the same values
 *  again". */
class RecordingVideoCodec : public np::IVideoCodec {
public:
    const char* name() const noexcept override { return "recording"; }
    np::Status open() noexcept override { return np::kOk; }
    void close() noexcept override {}

    np::VideoCodecConfig config() const noexcept override { return cfg_; }
    np::Status update_config(np::VideoCodecConfig cfg) noexcept override {
        cfg_ = std::move(cfg);
        return np::kOk;
    }
    np::Status set_params(const np::VideoEncoderParams& p) noexcept override {
        PushRecord& r = recorder();
        ++r.calls;
        r.last = p;
        return np::kOk;
    }
    np::VideoEncoderParams params() const noexcept override { return recorder().last; }

    bool can_encode() const noexcept override { return true; }
    bool can_decode() const noexcept override { return true; }
    np::VideoCodecKind kind() const noexcept override { return np::VideoCodecKind::kH264; }
    std::string_view codec_name() const noexcept override { return "h264"; }

    np::Status encode(const np::VideoFrame& /*f*/, std::uint8_t* /*out*/,
                      std::size_t /*cap*/, np::EncodedVideoFrame& /*e*/) noexcept override {
        return np::kErrUnsupported;
    }
    np::Status decode(const np::EncodedVideoFrame& /*e*/, np::VideoFrame& /*out*/,
                      std::uint8_t* const* /*bufs*/) noexcept override {
        return np::kErrUnsupported;
    }
    np::Status force_keyframe() noexcept override { return np::kOk; }

    np::VideoCodecStats stats() const noexcept override { return stats_; }
    std::uint8_t payload_type() const noexcept override { return 102; }

private:
    np::VideoCodecConfig cfg_{};
    np::VideoCodecStats stats_{};
};

/** The factory that hands out the recorder.
 *
 *  The engine resolves the codec through the registry, so the test
 *  registers this factory under a private id. */
class RecordingCodecFactory : public np::IVideoCodecFactory {
public:
    std::string_view id() const noexcept override { return "recording-h264"; }
    std::string_view display_name() const noexcept override { return "recording"; }
    np::IVideoCodec* create(np::VideoCodecConfig cfg) const override {
        created_with = cfg;
        return new RecordingVideoCodec();
        // The engine owns the returned pointer (it wraps it in a
        // `unique_ptr` and deletes it on teardown), so the factory
        // must hand over a *fresh* instance every time.
    }

    /** The configuration the engine handed to `create()`. */
    mutable np::VideoCodecConfig created_with{};

private:
};

class EngineVideoParams : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();
    }

    void SetUp() override {
        recorder() = PushRecord{};
        // The factory is registered once, for the whole suite: the
        // registry owns the pointer, and the engine only ever asks it
        // for a *new* codec instance.
        static RecordingCodecFactory factory;
        PluginRegistry::instance().register_video_codec("recording-h264", &factory);
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// B12 ? the engine pushes the parameter set
// ---------------------------------------------------------------------------

TEST_F(EngineVideoParams, TheEnginePushesTheParameterSet) {
    // The engine computes the parameter set in one place and pushes it.
    // Without this the codec would be running on whatever it guessed at
    // `open()` time.
    EngineConfig cfg{};
    cfg.video_codec_name    = "recording-h264";
    cfg.video_sender_name   = "reference";
    cfg.video_receiver_name = "reference";
    cfg.video_source_name   = "memory";
    cfg.video_sink_name     = "headless";
    cfg.video_width         = 800;
    cfg.video_height        = 600;
    cfg.video_fps           = 24;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // The engine's first push happens when the first frame is encoded.
    std::vector<std::uint8_t> buffer(800 * 600 * 3 / 2);
    np::VideoSourceFrame frame{};
    frame.width    = 800;
    frame.height   = 600;
    frame.format   = np::VideoPixelFormat::kI420;
    frame.stride_y = 800;
    frame.stride_u = 400;
    frame.stride_v = 400;
    frame.plane_y  = buffer.data();
    frame.plane_u  = buffer.data() + 800 * 600;
    frame.plane_v  = buffer.data() + 800 * 600 + 400 * 300;

    engine.send_video(frame);

    EXPECT_GE(recorder().calls, 1)
        << "the engine must push the parameter set to the codec";
    EXPECT_EQ(recorder().last.width, 800u);
    EXPECT_EQ(recorder().last.height, 600u);
    EXPECT_EQ(recorder().last.fps, 24u);
    EXPECT_TRUE(recorder().last.has_geometry());

    engine.close();
}

// ---------------------------------------------------------------------------
// B13 ? a geometry change restarts the encoder
// ---------------------------------------------------------------------------

TEST_F(EngineVideoParams, AGeometryChangePushesANewParameterSet) {
    // The frame is the authority on what is being sent: when the ladder
    // steps the resolution down, the engine must tell the codec ? the
    // codec then restarts and puts the new parameter sets on the wire
    // with the next frame.
    EngineConfig cfg{};
    cfg.video_codec_name    = "recording-h264";
    cfg.video_sender_name   = "reference";
    cfg.video_receiver_name = "reference";
    cfg.video_source_name   = "memory";
    cfg.video_sink_name     = "headless";
    cfg.video_width         = 640;
    cfg.video_height        = 480;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    std::vector<std::uint8_t> big(640 * 480 * 3 / 2);
    np::VideoSourceFrame frame{};
    frame.format       = np::VideoPixelFormat::kI420;
    frame.capture_ts_us = 1000;

    // ---- The first frame, at 640x480 ------------------------------------
    frame.width    = 640;
    frame.height   = 480;
    frame.stride_y = 640;
    frame.stride_u = 320;
    frame.stride_v = 320;
    frame.plane_y  = big.data();
    frame.plane_u  = big.data() + 640 * 480;
    frame.plane_v  = big.data() + 640 * 480 + 320 * 240;

    engine.send_video(frame);
    ASSERT_GE(recorder().calls, 1);
    EXPECT_EQ(recorder().last.width, 640u);
    EXPECT_EQ(recorder().last.height, 480u);

    // ---- The geometry changes -------------------------------------------
    std::vector<std::uint8_t> small(320 * 240 * 3 / 2);
    const int calls_before = recorder().calls;

    frame.width    = 320;
    frame.height   = 240;
    frame.stride_y = 320;
    frame.stride_u = 160;
    frame.stride_v = 160;
    frame.plane_y  = small.data();
    frame.plane_u  = small.data() + 320 * 240;
    frame.plane_v  = small.data() + 320 * 240 + 160 * 120;

    engine.send_video(frame);

    EXPECT_GT(recorder().calls, calls_before)
        << "a geometry change must reach the codec";
    EXPECT_EQ(recorder().last.width, 320u);
    EXPECT_EQ(recorder().last.height, 240u);
}

// ---------------------------------------------------------------------------
// C5 ? the colour description travels with the picture
// ---------------------------------------------------------------------------

TEST_F(EngineVideoParams, TheColourDescriptionReachesTheEncoder) {
    // A desktop compositor produces full-range pixels; a camera produces
    // limited-range ones.  The encoder has to know which it is coding,
    // or the picture comes out with the wrong contrast.
    EngineConfig cfg{};
    cfg.video_codec_name    = "recording-h264";
    cfg.video_sender_name   = "reference";
    cfg.video_receiver_name = "reference";
    cfg.video_source_name   = "memory";
    cfg.video_sink_name     = "headless";

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    std::vector<std::uint8_t> buffer(320 * 240 * 3 / 2);
    np::VideoSourceFrame frame{};
    frame.width    = 320;
    frame.height   = 240;
    frame.format   = np::VideoPixelFormat::kI420;
    frame.stride_y = 320;
    frame.stride_u = 160;
    frame.stride_v = 160;
    frame.plane_y  = buffer.data();
    frame.plane_u  = buffer.data() + 320 * 240;
    frame.plane_v  = buffer.data() + 320 * 240 + 160 * 120;
    frame.color.range  = np::VideoColorRange::kFull;
    frame.color.matrix = np::VideoColorMatrix::kBt709;

    engine.send_video(frame);

    ASSERT_GE(recorder().calls, 1);
    EXPECT_EQ(recorder().last.color.range, np::VideoColorRange::kFull);
    EXPECT_EQ(recorder().last.color.matrix, np::VideoColorMatrix::kBt709);
    EXPECT_TRUE(recorder().last.color.known());
}

// ---------------------------------------------------------------------------
// B7 ? the content hint reaches the SDP
// ---------------------------------------------------------------------------

TEST_F(EngineVideoParams, TheContentHintReachesTheSdp) {
    // RFC 8692 §3 defines three values; both the media-level
    // `a=content-hint:` attribute and the `x-google-content-hint` fmtp
    // parameter carry them, and they must agree.
    EngineConfig cfg{};
    cfg.video_codec_name    = "h264";
    cfg.video_sender_name   = "reference";
    cfg.video_receiver_name = "reference";
    cfg.video_source_name   = "memory";
    cfg.video_sink_name     = "headless";
    cfg.video_content_hint  = np::VideoEncoderParams::ContentHint::kText;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const std::string offer = engine.create_offer();
    EXPECT_NE(offer.find("a=content-hint:text"), std::string::npos)
        << "the media-level content hint must be advertised: " << offer;
    EXPECT_NE(offer.find("x-google-content-hint=text"), std::string::npos)
        << "the fmtp content hint must be advertised: " << offer;

    engine.close();
}

TEST_F(EngineVideoParams, NoContentHintMeansNoAttribute) {
    // "No opinion" must produce *no* attribute ? an `a=content-hint` line
    // with no value is not a thing RFC 8692 defines.
    EngineConfig cfg{};
    cfg.video_codec_name    = "h264";
    cfg.video_sender_name   = "reference";
    cfg.video_receiver_name = "reference";
    cfg.video_source_name   = "memory";
    cfg.video_sink_name     = "headless";

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const std::string offer = engine.create_offer();
    EXPECT_EQ(offer.find("a=content-hint"), std::string::npos) << offer;
    EXPECT_EQ(offer.find("x-google-content-hint"), std::string::npos) << offer;

    engine.close();
}
