/**
 * @file tests/test_engine_video_stage_timing.cpp
 * @brief The per-stage CPU-time instrumentation (remote-desktop plan J1)
 *        and the format normalisation counters (plan C3).
 *
 * ## Why these two are tested together
 *
 * Both are *measurements* the plan's exit criterion is written in terms
 * of:
 *
 *   - SB.6: "CPU time per frame (capture ? encode ? packetize) ? within
 *     SB.6's budget on the hardware path; within 2x of it on the
 *     software path".  Without a measurement the budget is a sentence
 *     in a document rather than something the code checks (J1).
 *   - C3: "the encoder receives a single documented format; the
 *     conversion count is in the stats and is zero on the zero-copy
 *     path".
 *
 * A measurement that is not asserted is a decoration, so this test
 * asserts both: that the stage counters *move* when the pipeline runs,
 * that the per-stage means are individually plausible, and that the
 * format counters distinguish "a colour conversion happened" from "the
 * frame was already canonical" from "nothing happened at all".
 */

#include <cstdint>
#include <string>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/video_capture.hpp>
#include <nimrtc/plugins/video_sink.hpp>

#include <gtest/gtest.h>

using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;
namespace np = nimrtc::plugins;

namespace {

constexpr std::uint32_t kWidth  = 320;
constexpr std::uint32_t kHeight = 240;

// ---------------------------------------------------------------------------
// A capture device that hands out a picture in a chosen format.
// ---------------------------------------------------------------------------

/**
 * @brief A capture device that produces frames in a configurable format.
 *
 * The point of the configurable format is C3: the same device can
 * deliver BGRA (which needs a colour conversion) or I420 (which does
 * not), so the test can tell the two apart in the stats.
 */
class TimedCaptureDevice final : public np::IVideoCapture {
public:
    TimedCaptureDevice(np::CaptureConfig cfg, np::VideoPixelFormat fmt)
        : cfg_(std::move(cfg)), fmt_(fmt) {
        if (fmt_ == np::VideoPixelFormat::kBGRA) {
            pixels_.resize(static_cast<std::size_t>(kWidth) * kHeight * 4u);
            for (std::size_t i = 0; i < pixels_.size(); i += 4u) {
                pixels_[i + 0] = 64;   // B
                pixels_[i + 1] = 128;  // G
                pixels_[i + 2] = 192;  // R
                pixels_[i + 3] = 255;  // A
            }
        } else {
            const std::size_t luma = static_cast<std::size_t>(kWidth) * kHeight;
            const std::size_t chroma = luma / 4u;
            pixels_.assign(luma + 2u * chroma, 96);
        }
    }

    ~TimedCaptureDevice() override { close(); }

    const char* name() const noexcept override { return "TimedCaptureDevice"; }
    np::Status open() noexcept override { open_ = true; return np::kOk; }
    void close() noexcept override { open_ = false; }

    std::vector<np::CaptureDevice> enumerate_devices() const noexcept override {
        return {};
    }
    std::vector<np::CaptureFormat> supported_formats(
        const std::string&) const noexcept override {
        return {};
    }
    np::CaptureCapabilities capabilities() const noexcept override {
        np::CaptureCapabilities c;
        c.backend = "timed";
        return c;
    }

    bool is_open() const noexcept override { return open_; }
    np::CaptureConfig config() const noexcept override { return cfg_; }

    np::Status pop_frame(np::CaptureFrame& out) noexcept override {
        if (!open_) return np::kErrNotReady;
        out.width     = kWidth;
        out.height    = kHeight;
        out.format    = fmt_;
        out.pixels    = pixels_.data();
        out.size      = pixels_.size();
        out.stride    = static_cast<std::int32_t>(
            fmt_ == np::VideoPixelFormat::kBGRA ? kWidth * 4u : kWidth);
        out.frame_seq = static_cast<std::uint32_t>(produced_++);
        out.capture_ts_us = 1'000 + static_cast<std::int64_t>(produced_) * 33'333;
        return np::kOk;
    }

    void set_frame_callback(np::CaptureFrameCallback) noexcept override {}
    np::CaptureStats stats() const noexcept override {
        np::CaptureStats s;
        s.frames_captured = produced_;
        return s;
    }

private:
    np::CaptureConfig cfg_{};
    np::VideoPixelFormat fmt_ = np::VideoPixelFormat::kI420;
    std::vector<std::uint8_t> pixels_;
    std::uint64_t produced_ = 0;
    bool open_ = false;
};

class TimedCaptureFactory final : public np::IVideoCaptureFactory {
public:
    explicit TimedCaptureFactory(np::VideoPixelFormat fmt) : fmt_(fmt) {}

    std::string_view id() const noexcept override { return "timed-capture"; }
    std::string_view display_name() const noexcept override {
        return "Timed capture device (stage-timing test)";
    }
    np::IVideoCapture* create(np::CaptureConfig cfg) const override {
        return new TimedCaptureDevice(std::move(cfg), fmt_);
    }

private:
    np::VideoPixelFormat fmt_;
};

// ---------------------------------------------------------------------------
// A sink that counts the frames it drew.
// ---------------------------------------------------------------------------

class CountingSink final : public np::IVideoSink {
public:
    const char* name() const noexcept override { return "CountingSink"; }
    np::Status open() noexcept override { open_ = true; return np::kOk; }
    void close() noexcept override { open_ = false; }

    np::Status render(const np::VideoSourceFrame&) noexcept override {
        ++rendered_;
        return np::kOk;
    }
    np::Status render(const np::GpuBuffer&) noexcept override {
        return np::kErrUnsupported;
    }
    np::VideoSinkStats stats() const noexcept override {
        np::VideoSinkStats s;
        s.frames_rendered = rendered_;
        return s;
    }
    np::VideoSinkConfig config() const noexcept override { return {}; }

    std::uint64_t rendered() const noexcept { return rendered_; }

private:
    std::uint64_t rendered_ = 0;
    bool open_ = false;
};

class CountingSinkFactory final : public np::IVideoSinkFactory {
public:
    std::string_view id() const noexcept override { return "counting-sink"; }
    std::string_view display_name() const noexcept override {
        return "Counting sink (stage-timing test)";
    }
    np::IVideoSink* create(np::VideoSinkConfig) const override {
        return new CountingSink();
    }
};

// ---------------------------------------------------------------------------
// The engine, driven through the capture pump.
// ---------------------------------------------------------------------------

/**
 * @brief Open an engine, pump N frames through it, and return the stats.
 *
 * The engine uses the "shell" transport strategy so the test never
 * touches the network: the measurement is about the *pipeline*, not the
 * wire.
 */
nimrtc::engine::NimRTCEngine::Stats RunPipeline(int frames) {
    EngineConfig cfg{};
    cfg.transport_strategy_name  = "shell";
    cfg.transport_name           = "ice";
    cfg.datachannel_backend     = "";
    cfg.bwe_name                 = "";
    cfg.scheduler_config.enabled = false;

    cfg.video_codec_name    = "h264";
    cfg.video_sender_name   = "reference";
    cfg.video_receiver_name = "reference";
    cfg.video_source_name   = "memory";
    cfg.video_sink_name     = "counting-sink";
    cfg.video_capture_name  = "timed-capture";
    cfg.video_width         = kWidth;
    cfg.video_height        = kHeight;
    cfg.video_fps           = 30;

    NimRTCEngine engine(cfg);
    const std::uint32_t rc = engine.open();
    EXPECT_EQ(rc, 0u) << "the engine must open";

    for (int i = 0; i < frames; ++i) {
        engine.tick();
    }
    return engine.stats();
}

}  // namespace


// The stage counters are in `nimrtc::engine::detail`, which is a PRIVATE
// header (Layout Invariant 4) ? so the test drives the *public* surface
// and asserts on the public `Stats` fields.  That is the point: a
// measurement the host cannot read is not a measurement.

// ---------------------------------------------------------------------------
// J1 ? the per-stage timings
// ---------------------------------------------------------------------------

TEST(EngineVideoStageTiming, TheCaptureStageIsMeasured) {
    nimrtc::core::register_all_default_plugins();
    static TimedCaptureFactory capture(np::VideoPixelFormat::kI420);
    static CountingSinkFactory sink;
    PluginRegistry::instance().register_video_capture(capture.id(), &capture);
    PluginRegistry::instance().register_video_sink(sink.id(), &sink);

    const auto stats = RunPipeline(20);

    // The capture stage runs once per pump tick that found a frame.
    EXPECT_GT(stats.video_stage_capture.samples, 0u)
        << "the capture stage must have been measured";
    EXPECT_GT(stats.video_stage_capture.total_us, 0u)
        << "the capture stage must have consumed some time";
    // The mean is a real number, not an artefact of a zero division.
    EXPECT_LE(stats.video_stage_capture.mean_us,
              stats.video_stage_capture.max_us);
}

TEST(EngineVideoStageTiming, TheEncodeAndPacketizeStagesAreMeasured) {
    nimrtc::core::register_all_default_plugins();
    static TimedCaptureFactory capture(np::VideoPixelFormat::kI420);
    static CountingSinkFactory sink;
    PluginRegistry::instance().register_video_capture(capture.id(), &capture);
    PluginRegistry::instance().register_video_sink(sink.id(), &sink);

    const auto stats = RunPipeline(30);

    EXPECT_GT(stats.video_stage_encode.samples, 0u)
        << "the encode stage must have been measured";
    EXPECT_GT(stats.video_stage_packetize.samples, 0u)
        << "the packetize stage must have been measured";

    // The convert stage must also have run: the source was I420, so the
    // engine copied the planes into a codec-facing buffer.
    EXPECT_GT(stats.video_stage_convert.samples, 0u)
        << "the convert stage must have been measured";

    // The budget number is the sum of the four outbound stages.
    EXPECT_GT(stats.video_stage_total_us, 0u)
        << "the outbound frame cost must be a positive number";

    // The p95 is a percentile of the *recent* samples, so it must lie
    // between the minimum and the maximum.  It is deliberately NOT
    // asserted to be >= the mean: the mean covers the whole session
    // (including the first, slow, codec-warm-up frame) while the p95
    // covers only the recent window, so the mean may legitimately be
    // the larger of the two.
    EXPECT_LE(stats.video_stage_encode.p95_us, stats.video_stage_encode.max_us);
    EXPECT_LE(stats.video_stage_packetize.p95_us,
              stats.video_stage_packetize.max_us);
}

TEST(EngineVideoStageTiming, TheConvertStageIsMeasuredForAPackedSource) {
    nimrtc::core::register_all_default_plugins();
    static TimedCaptureFactory capture(np::VideoPixelFormat::kBGRA);
    static CountingSinkFactory sink;
    PluginRegistry::instance().register_video_capture(capture.id(), &capture);
    PluginRegistry::instance().register_video_sink(sink.id(), &sink);

    const auto stats = RunPipeline(30);

    EXPECT_GT(stats.video_stage_convert.samples, 0u)
        << "the BGRA ? I420 conversion must have been measured";
    EXPECT_GT(stats.video_stage_convert.mean_us, 0u)
        << "a colour conversion at 320x240 is not free ? it must cost "
           "some measurable time";
}

// ---------------------------------------------------------------------------
// C3 ? the format normalisation counters
// ---------------------------------------------------------------------------

TEST(EngineVideoFormatNormalisation, APackedSourceCountsAsAConversion) {
    nimrtc::core::register_all_default_plugins();
    static TimedCaptureFactory capture(np::VideoPixelFormat::kBGRA);
    static CountingSinkFactory sink;
    PluginRegistry::instance().register_video_capture(capture.id(), &capture);
    PluginRegistry::instance().register_video_sink(sink.id(), &sink);

    const auto stats = RunPipeline(20);

    EXPECT_GT(stats.video_format_conversions, 0u)
        << "a BGRA source must be counted as a colour conversion";
    EXPECT_EQ(stats.video_format_copies, 0u)
        << "a BGRA source is not a plain copy";
    EXPECT_EQ(stats.video_format_source, "BGRA")
        << "the source format must be reported";
    EXPECT_EQ(stats.video_format_canonical, "I420")
        << "the canonical (codec-facing) format must be reported";
}

TEST(EngineVideoFormatNormalisation, APlanarSourceCountsAsACopy) {
    nimrtc::core::register_all_default_plugins();
    static TimedCaptureFactory capture(np::VideoPixelFormat::kI420);
    static CountingSinkFactory sink;
    PluginRegistry::instance().register_video_capture(capture.id(), &capture);
    PluginRegistry::instance().register_video_sink(sink.id(), &sink);

    const auto stats = RunPipeline(20);

    EXPECT_EQ(stats.video_format_conversions, 0u)
        << "an I420 source needs no colour conversion";
    EXPECT_GT(stats.video_format_copies, 0u)
        << "an I420 source is still copied into the codec's buffer";
    EXPECT_EQ(stats.video_format_source, "I420");
    EXPECT_EQ(stats.video_format_canonical, "I420");
}
