/**
 * @file tests/test_engine_video_codec_split.cpp
 * @brief The encoder / decoder split seam (remote-desktop plan B1).
 *
 * ## What the plan asks for
 *
 * B1's row says:
 *
 *   > "Codec split seam -- `IVideoEncoder` / `IVideoDecoder` split out of
 *   >  the combined `IVideoCodec`, with a **capability query** (supported
 *   >  profiles, resolutions, pixel formats, fps, max bitrate, zero-copy).
 *   >  `IVideoCodec` stays as a deprecated compatibility shim for one MINOR
 *   >  cycle (ADR-015 Decision 3)."
 *
 * and its acceptance criterion says:
 *
 *   > "Old code using `IVideoCodec` still compiles (deprecated marker
 *   >  present); the engine rejects an unsupported resolution at `open()`
 *   >  with a clear error instead of failing at the first frame."
 *
 * Two things are therefore tested here:
 *
 *   1. **The seam itself.**  A class that implements `IVideoEncoder` is
 *      an `IVideoEncoder`; a class that implements `IVideoDecoder` is an
 *      `IVideoDecoder`; a class that implements the deprecated combined
 *      `IVideoCodec` is *both* -- and it still compiles, because the
 *      deprecated shim derives from the two halves.
 *
 *   2. **The capability query and the engine's use of it.**  The engine
 *      must reject a geometry the codec cannot handle at `open()` time,
 *      with a distinct error code, and NOT at the first frame.
 *
 * ## The design
 *
 * The fake codecs live in this file and register under plugin ids that
 * no module in this tree registers a backend for, so they cannot collide
 * with a real backend and cannot be selected by the "auto" preference by
 * accident.
 *
 * The fake codecs report the **H.264** codec family.  That is not
 * cosmetic: the engine derives the SDP encoding name, the RTP payload
 * type and the packetizer / depacketizer selection from the codec's
 * `kind()`, and it refuses to open a session with a codec whose family
 * it cannot describe.  A fake that reported "unknown" would therefore
 * never reach the capability check at all -- the engine would fail
 * earlier, for an unrelated reason, and the test would prove nothing.
 *
 * The capability rejection is tested through the *engine*, not through a
 * unit of the check itself, because the acceptance criterion is about
 * the engine's behaviour: the error must arrive at `open()`.
 */

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/core/engine_errors.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/video_codec.hpp>

#include <gtest/gtest.h>

using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;
namespace np = nimrtc::plugins;

// The status vocabulary lives in `nimrtc::plugins`; the fake codecs below
// spell it unqualified, so pull it into scope once here.
using np::Status;
using np::kOk;
using np::kErrUnsupported;

namespace {

// ---------------------------------------------------------------------------
// A fake codec that implements ONLY the encode half
// ---------------------------------------------------------------------------

/**
 * @brief An encoder-only codec: it implements `IVideoEncoder` and
 *        nothing else.
 *
 * This is the shape the split enables: a camera gateway that encodes and
 * never decodes does not have to instantiate a decoder.
 */
class EncoderOnlyCodec final : public np::IVideoEncoder {
public:
    explicit EncoderOnlyCodec(np::VideoCodecConfig cfg) : cfg_(cfg) {}

    const char* name() const noexcept override { return "encoder-only"; }

    Status open() noexcept override {
        opened_ = true;
        return kOk;
    }
    void close() noexcept override { opened_ = false; }

    np::VideoCodecKind kind() const noexcept override {
        return np::VideoCodecKind::kH264;
    }
    std::string_view codec_name() const noexcept override { return "h264"; }

    Status encode(const np::VideoFrame&, std::uint8_t*, std::size_t,
                  np::EncodedVideoFrame&) noexcept override {
        return kErrUnsupported;
    }
    Status force_keyframe() noexcept override { return kOk; }

    Status set_params(const np::VideoEncoderParams&) noexcept override {
        return kOk;
    }
    np::VideoEncoderParams params() const noexcept override { return {}; }
    np::VideoCodecStats stats() const noexcept override { return {}; }

    bool opened() const noexcept { return opened_; }

private:
    np::VideoCodecConfig cfg_;
    bool opened_ = false;
};

// ---------------------------------------------------------------------------
// A fake codec that implements ONLY the decode half
// ---------------------------------------------------------------------------

class DecoderOnlyCodec final : public np::IVideoDecoder {
public:
    explicit DecoderOnlyCodec(np::VideoCodecConfig cfg) : cfg_(cfg) {}

    const char* name() const noexcept override { return "decoder-only"; }

    Status open() noexcept override {
        opened_ = true;
        return kOk;
    }
    void close() noexcept override { opened_ = false; }

    np::VideoCodecKind kind() const noexcept override {
        return np::VideoCodecKind::kH264;
    }
    std::string_view codec_name() const noexcept override { return "h264"; }

    Status decode(const np::EncodedVideoFrame&, np::VideoFrame&,
                  std::uint8_t* const*) noexcept override {
        return kErrUnsupported;
    }

    np::VideoCodecStats stats() const noexcept override { return {}; }

    bool opened() const noexcept { return opened_; }

private:
    np::VideoCodecConfig cfg_;
    bool opened_ = false;
};

// ---------------------------------------------------------------------------
// The factories for the two halves
// ---------------------------------------------------------------------------

/**
 * @brief The factory for the encode half.
 *
 * The split factories are *separate* interfaces from the combined
 * `IVideoCodecFactory`: a factory that can only hand out an encoder must
 * not be forced to pretend it can also hand out a decoder.
 */
class EncoderOnlyFactory final : public np::IVideoEncoderFactory {
public:
    std::string_view id() const noexcept override { return "split-enc"; }
    std::string_view display_name() const noexcept override {
        return "Split encoder only";
    }
    np::IVideoEncoder* create_encoder(np::VideoCodecConfig cfg) const override {
        last_cfg = cfg;
        return new EncoderOnlyCodec(cfg);
    }
    mutable np::VideoCodecConfig last_cfg{};
};

class DecoderOnlyFactory final : public np::IVideoDecoderFactory {
public:
    std::string_view id() const noexcept override { return "split-dec"; }
    std::string_view display_name() const noexcept override {
        return "Split decoder only";
    }
    np::IVideoDecoder* create_decoder(np::VideoCodecConfig cfg) const override {
        last_cfg = cfg;
        return new DecoderOnlyCodec(cfg);
    }
    mutable np::VideoCodecConfig last_cfg{};
};

// ---------------------------------------------------------------------------
// A fake codec that implements the DEPRECATED combined seam
// ---------------------------------------------------------------------------

/**
 * @brief A codec that implements the deprecated combined `IVideoCodec`.
 *
 * The point of this class is that it still *compiles*: the deprecated
 * shim derives from both halves, so an implementation of the combined
 * interface satisfies the two split interfaces at once.  This is what
 * "old code using `IVideoCodec` still compiles" means in practice.
 *
 * The deprecation warning is suppressed locally on purpose: the test
 * *wants* to use the deprecated interface (that is the behaviour under
 * test), and the warning is not an error.
 */
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

class LegacyCombinedCodec final : public np::IVideoCodec {
public:
    explicit LegacyCombinedCodec(np::VideoCodecConfig cfg) : cfg_(cfg) {}

    const char* name() const noexcept override { return "legacy-combined"; }

    Status open() noexcept override {
        opened_ = true;
        return kOk;
    }
    void close() noexcept override { opened_ = false; }

    bool can_encode() const noexcept override { return true; }
    bool can_decode() const noexcept override { return true; }

    np::VideoCodecKind kind() const noexcept override {
        return np::VideoCodecKind::kH264;
    }
    std::string_view codec_name() const noexcept override { return "h264"; }

    Status encode(const np::VideoFrame&, std::uint8_t*, std::size_t,
                  np::EncodedVideoFrame&) noexcept override {
        return kErrUnsupported;
    }
    Status decode(const np::EncodedVideoFrame&, np::VideoFrame&,
                  std::uint8_t* const*) noexcept override {
        return kErrUnsupported;
    }
    Status force_keyframe() noexcept override { return kOk; }

    np::VideoCodecConfig config() const noexcept override { return cfg_; }
    Status update_config(np::VideoCodecConfig cfg) noexcept override {
        cfg_ = cfg;
        return kOk;
    }
    np::VideoCodecStats stats() const noexcept override { return {}; }
    std::uint8_t payload_type() const noexcept override { return 102; }

    bool opened() const noexcept { return opened_; }

private:
    np::VideoCodecConfig cfg_;
    bool opened_ = false;
};

// ---------------------------------------------------------------------------
// A fake codec with a *bounded* capability set
// ---------------------------------------------------------------------------

/**
 * @brief A codec that knows its own limits -- a maximum resolution, a
 *        maximum frame rate and an enumerated pixel format list.
 *
 * The capability set is the whole point of B1's "capability query": a
 * codec that does not know its own limits is the reason the plan asks
 * for the query at all.
 */
class BoundedCodec final : public np::IVideoCodec {
public:
    explicit BoundedCodec(np::VideoCodecConfig cfg) : cfg_(cfg) {}

    const char* name() const noexcept override { return "bounded"; }

    Status open() noexcept override {
        opened_ = true;
        return kOk;
    }
    void close() noexcept override { opened_ = false; }

    bool can_encode() const noexcept override { return true; }
    bool can_decode() const noexcept override { return true; }

    np::VideoCodecKind kind() const noexcept override {
        return np::VideoCodecKind::kH264;
    }
    std::string_view codec_name() const noexcept override { return "h264"; }

    Status encode(const np::VideoFrame&, std::uint8_t*, std::size_t,
                  np::EncodedVideoFrame&) noexcept override {
        return kErrUnsupported;
    }
    Status decode(const np::EncodedVideoFrame&, np::VideoFrame&,
                  std::uint8_t* const*) noexcept override {
        return kErrUnsupported;
    }
    Status force_keyframe() noexcept override { return kOk; }

    np::VideoCodecConfig config() const noexcept override { return cfg_; }
    Status update_config(np::VideoCodecConfig cfg) noexcept override {
        cfg_ = cfg;
        return kOk;
    }
    np::VideoCodecStats stats() const noexcept override { return {}; }
    std::uint8_t payload_type() const noexcept override { return 102; }

    np::VideoCodecCapabilities capabilities() const noexcept override {
        np::VideoCodecCapabilities caps{};
        caps.max_width      = 1280;
        caps.max_height     = 720;
        caps.max_fps        = 30;
        caps.max_bitrate_bps = 2'000'000;
        caps.pixel_formats  = {np::VideoPixelFormat::kI420};
        caps.profiles       = {"baseline", "main"};
        return caps;
    }

    bool opened() const noexcept { return opened_; }

private:
    np::VideoCodecConfig cfg_;
    bool opened_ = false;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

// ---------------------------------------------------------------------------
// The combined factory (the deprecated shape still works)
// ---------------------------------------------------------------------------

class LegacyCombinedFactory final : public np::IVideoCodecFactory {
public:
    std::string_view id() const noexcept override { return "split-legacy"; }
    std::string_view display_name() const noexcept override {
        return "Split legacy combined";
    }
    np::IVideoCodec* create(np::VideoCodecConfig cfg) const override {
        last_cfg = cfg;
        return new LegacyCombinedCodec(cfg);
    }
    mutable np::VideoCodecConfig last_cfg{};
};

class BoundedFactory final : public np::IVideoCodecFactory {
public:
    std::string_view id() const noexcept override { return "split-bounded"; }
    std::string_view display_name() const noexcept override {
        return "Split bounded capability";
    }
    np::IVideoCodec* create(np::VideoCodecConfig cfg) const override {
        last_cfg = cfg;
        return new BoundedCodec(cfg);
    }
    mutable np::VideoCodecConfig last_cfg{};
};

/**
 * @brief Register a combined factory and remember it so it can be removed
 *        again.
 *
 * The plugin registry is a process-wide singleton, and the factories
 * registered here would otherwise leak into every later test in the
 * same binary.  The cleanup removes them again.
 */
class ScopedFactory {
public:
    explicit ScopedFactory(std::unique_ptr<np::IVideoCodecFactory> f) {
        id_ = std::string{f->id()};
        PluginRegistry::instance().register_video_codec(id_, f.get());
        factory_ = std::move(f);
    }
    ~ScopedFactory() {
        PluginRegistry::instance().unregister_video_codec(id_);
    }
    ScopedFactory(const ScopedFactory&) = delete;
    ScopedFactory& operator=(const ScopedFactory&) = delete;

    const std::string& id() const noexcept { return id_; }
    np::IVideoCodecFactory* get() const noexcept { return factory_.get(); }

private:
    std::string id_;
    std::unique_ptr<np::IVideoCodecFactory> factory_;
};

/// The engine config every test in this file starts from.
EngineConfig BaseConfig(const std::string& codec_id) {
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
    return cfg;
}

}  // namespace

// ---------------------------------------------------------------------------
// The seam
// ---------------------------------------------------------------------------

TEST(VideoCodecSplit, AnEncoderOnlyCodecIsAnIVideoEncoder) {
    np::VideoCodecConfig cfg{};
    EncoderOnlyCodec codec(cfg);

    // The point of the split: an encoder-only implementation needs no
    // decoder at all.
    np::IVideoEncoder* as_encoder = &codec;
    ASSERT_NE(as_encoder, nullptr);
    EXPECT_EQ(as_encoder->kind(), np::VideoCodecKind::kH264);
    EXPECT_EQ(as_encoder->codec_name(), "h264");

    EXPECT_EQ(as_encoder->open(), np::kOk);
    EXPECT_TRUE(codec.opened());
    as_encoder->close();
    EXPECT_FALSE(codec.opened());
}

TEST(VideoCodecSplit, ADecoderOnlyCodecIsAnIVideoDecoder) {
    np::VideoCodecConfig cfg{};
    DecoderOnlyCodec codec(cfg);

    np::IVideoDecoder* as_decoder = &codec;
    ASSERT_NE(as_decoder, nullptr);
    EXPECT_EQ(as_decoder->kind(), np::VideoCodecKind::kH264);

    EXPECT_EQ(as_decoder->open(), np::kOk);
    EXPECT_TRUE(codec.opened());
    as_decoder->close();
    EXPECT_FALSE(codec.opened());
}

TEST(VideoCodecSplit, TheCombinedCodecIsBothHalves) {
    np::VideoCodecConfig cfg{};
    LegacyCombinedCodec codec(cfg);

    // The compatibility promise: the deprecated combined interface is
    // still an encoder AND still a decoder.
    np::IVideoEncoder* as_encoder = &codec;
    np::IVideoDecoder* as_decoder = &codec;
    ASSERT_NE(as_encoder, nullptr);
    ASSERT_NE(as_decoder, nullptr);
    EXPECT_EQ(as_encoder->kind(), as_decoder->kind());
}

TEST(VideoCodecSplit, TheCapabilityQueryHasTheDocumentedShape) {
    np::VideoCodecCapabilities caps{};
    caps.max_width       = 1920;
    caps.max_height      = 1080;
    caps.min_width       = 320;
    caps.min_height      = 240;
    caps.max_fps         = 60;
    caps.max_bitrate_bps = 4'000'000;
    caps.pixel_formats   = {np::VideoPixelFormat::kI420};
    caps.profiles        = {"baseline"};

    EXPECT_TRUE(caps.accepts_size(1280, 720));
    EXPECT_FALSE(caps.accepts_size(3840, 2160)) << "above the maximum";
    EXPECT_FALSE(caps.accepts_size(160, 120))   << "below the minimum";
    EXPECT_FALSE(caps.accepts_size(0, 720))
        << "a zero dimension is a malformed request, not an accepted one";

    EXPECT_TRUE(caps.accepts_fps(30));
    EXPECT_FALSE(caps.accepts_fps(120));
    EXPECT_TRUE(caps.accepts_fps(0))
        << "zero means 'unconstrained' and must be accepted";

    EXPECT_TRUE(caps.accepts_bitrate(2'000'000));
    EXPECT_FALSE(caps.accepts_bitrate(8'000'000));
    EXPECT_TRUE(caps.accepts_bitrate(0));

    EXPECT_TRUE(caps.accepts_pixel_format(np::VideoPixelFormat::kI420));
    EXPECT_FALSE(caps.accepts_pixel_format(np::VideoPixelFormat::kNV12));
}

TEST(VideoCodecSplit, AnUnconstrainedCapabilityAcceptsEverything) {
    // The default capability set is *unconstrained*: a codec that does
    // not know its own limits must not break the engine.
    const np::VideoCodecCapabilities caps{};
    EXPECT_TRUE(caps.accepts_size(7680, 4320));
    EXPECT_TRUE(caps.accepts_fps(240));
    EXPECT_TRUE(caps.accepts_bitrate(100'000'000));
    EXPECT_TRUE(caps.accepts_pixel_format(np::VideoPixelFormat::kNV12));
}

// ---------------------------------------------------------------------------
// The engine's use of the capability query
// ---------------------------------------------------------------------------

TEST(EngineVideoCodecSplit, AnUnsupportedResolutionIsRejectedAtOpen) {
    nimrtc::core::register_all_default_plugins();

    auto factory = std::make_unique<BoundedFactory>();
    const std::string id{factory->id()};
    ScopedFactory scoped(std::move(factory));

    EngineConfig cfg = BaseConfig(id);
    // The bounded codec tops out at 1280x720 -- this asks for more.
    cfg.video_width  = 3840;
    cfg.video_height = 2160;

    std::uint32_t reported_err = 0;
    std::string   reported_msg;

    NimRTCEngine engine(cfg);
    engine.set_on_error([&](std::uint32_t err, std::string_view msg) {
        reported_err = err;
        reported_msg = std::string{msg};
    });

    const std::uint32_t rc = engine.open();

    EXPECT_NE(rc, 0u)
        << "an unsupported resolution must be rejected at open(), not "
           "deferred to the first frame";
    EXPECT_EQ(rc, nimrtc::core::kVideoCodecUnsupported);
    EXPECT_EQ(reported_err, nimrtc::core::kVideoCodecUnsupported);
    EXPECT_NE(reported_msg.find("3840"), std::string::npos)
        << "the error message must name the offending geometry: "
           << reported_msg;
}

TEST(EngineVideoCodecSplit, ASupportedResolutionOpensCleanly) {
    nimrtc::core::register_all_default_plugins();

    auto factory = std::make_unique<BoundedFactory>();
    const std::string id{factory->id()};
    ScopedFactory scoped(std::move(factory));

    EngineConfig cfg = BaseConfig(id);
    cfg.video_width  = 1280;
    cfg.video_height = 720;

    NimRTCEngine engine(cfg);
    EXPECT_EQ(engine.open(), 0u)
        << "a resolution inside the codec's range must open cleanly";
}

TEST(EngineVideoCodecSplit, TheSplitHalfFactoriesHandOutTheirOwnHalf) {
    EncoderOnlyFactory enc_factory;
    const std::string enc_id{enc_factory.id()};

    DecoderOnlyFactory dec_factory;
    const std::string dec_id{dec_factory.id()};

    {
        np::VideoCodecConfig cfg{};
        std::unique_ptr<np::IVideoEncoder> codec(enc_factory.create_encoder(cfg));
        ASSERT_NE(codec, nullptr);
        EXPECT_EQ(codec->kind(), np::VideoCodecKind::kH264);
        EXPECT_EQ(codec->codec_name(), "h264");
    }
    {
        np::VideoCodecConfig cfg{};
        std::unique_ptr<np::IVideoDecoder> codec(dec_factory.create_decoder(cfg));
        ASSERT_NE(codec, nullptr);
        EXPECT_EQ(codec->kind(), np::VideoCodecKind::kH264);
    }

    EXPECT_EQ(enc_id, "split-enc");
    EXPECT_EQ(dec_id, "split-dec");
}

TEST(EngineVideoCodecSplit, TheDeprecatedCombinedCodecStillWorks) {
    nimrtc::core::register_all_default_plugins();

    auto factory = std::make_unique<LegacyCombinedFactory>();
    const std::string id{factory->id()};
    ScopedFactory scoped(std::move(factory));

    EngineConfig cfg = BaseConfig(id);
    NimRTCEngine engine(cfg);
    EXPECT_EQ(engine.open(), 0u)
        << "a codec that implements the deprecated combined interface must "
           "still work";
}
