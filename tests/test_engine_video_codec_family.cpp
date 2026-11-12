/**
 * @file tests/test_engine_video_codec_family.cpp
 * @brief B6 — the video codec family contract.
 *
 * `IVideoCodec::kind()` is the *authoritative* statement of the wire
 * format.  The engine derives from it:
 *
 *   - the SDP `a=rtpmap:` encoding name ("H264" / "H265" / "AV1"),
 *   - the effective RTP payload type,
 *   - the packetizer / depacketizer the receiver and sender use.
 *
 * The plugin id is only a *hint* used to look the factory up.  This
 * test pins the three cases that matter:
 *
 *   1. `kind()` wins over the plugin id  — a plugin registered under
 *      "h264" that reports `kAV1` must produce an AV1 offer.
 *   2. The plugin id is the fallback      — a plugin that reports
 *      `kUnknown` but is registered under "h265" still works.
 *   3. Neither knows                      — the engine must fail the
 *      session loudly instead of advertising an empty encoding.
 *
 * @see docs/plan/architecture-remediation-plan.md §2.4 (B6).
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>

#include <nimrtc/core/engine_errors.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/video_codec.hpp>

namespace {

using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;
namespace np = nimrtc::plugins;

// ---------------------------------------------------------------------------
// A configurable fake codec — its `kind()` is chosen at factory time.
// ---------------------------------------------------------------------------

class ConfigurableVideoCodec final : public np::IVideoCodec {
public:
    explicit ConfigurableVideoCodec(np::VideoCodecKind kind) : kind_(kind) {}

    const char* name() const noexcept override { return "configurable-codec"; }
    np::Status open() noexcept override { return np::kOk; }
    void close() noexcept override {}

    bool can_encode() const noexcept override { return true; }
    bool can_decode() const noexcept override { return true; }

    np::VideoCodecKind kind() const noexcept override { return kind_; }
    std::string_view codec_name() const noexcept override { return "configurable"; }

    np::Status encode(const np::VideoFrame&,
                      std::uint8_t* output,
                      std::size_t output_capacity,
                      np::EncodedVideoFrame& out) noexcept override {
        if (output_capacity < 4) return np::kErrInvalidParam;
        output[0] = 0; output[1] = 0; output[2] = 0; output[3] = 1;
        out.payload = nimrtc::core::ByteSpan{output, 4};
        out.is_keyframe = true;
        return np::kOk;
    }

    np::Status decode(const np::EncodedVideoFrame&,
                      np::VideoFrame&,
                      std::uint8_t* const* output_buffers) noexcept override {
        (void)output_buffers;
        return np::kOk;
    }

    np::Status update_config(np::VideoCodecConfig cfg) noexcept override {
        cfg_ = std::move(cfg);
        return np::kOk;
    }
    np::VideoCodecConfig config() const noexcept override { return cfg_; }
    np::VideoCodecStats stats() const noexcept override { return {}; }
    std::uint8_t payload_type() const noexcept override { return cfg_.payload_type; }
    np::Status force_keyframe() noexcept override { return np::kOk; }

private:
    np::VideoCodecKind     kind_;
    np::VideoCodecConfig   cfg_{};
};

class ConfigurableVideoCodecFactory final : public np::IVideoCodecFactory {
public:
    ConfigurableVideoCodecFactory(std::string id, np::VideoCodecKind kind)
        : id_(std::move(id)), kind_(kind) {}

    std::string_view id() const noexcept override { return id_; }
    std::string_view display_name() const noexcept override {
        return "Configurable video codec (test)";
    }
    np::IVideoCodec* create(np::VideoCodecConfig) const override {
        return new ConfigurableVideoCodec(kind_);
    }

private:
    std::string        id_;
    np::VideoCodecKind kind_;
};

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class EngineVideoCodecFamily : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();

        // A plugin whose id says "h264" but whose `kind()` says AV1.
        static const ConfigurableVideoCodecFactory av1_under_h264_id(
            "av1-under-h264-id", np::VideoCodecKind::kAV1);
        PluginRegistry::instance().register_video_codec(
            "av1-under-h264-id", &av1_under_h264_id);

        // A plugin that does not know its family — the id has to carry it.
        // The id uses the "family prefix + separator" form the contract
        // allows ("h265-..."), so the engine can derive the family from
        // the id alone.
        static const ConfigurableVideoCodecFactory unknown_kind(
            "h265-unknown-kind", np::VideoCodecKind::kUnknown);
        PluginRegistry::instance().register_video_codec(
            "h265-unknown-kind", &unknown_kind);

        // A plugin that knows nothing: neither `kind()` nor the id.
        static const ConfigurableVideoCodecFactory nothing(
            "nothing-knows", np::VideoCodecKind::kUnknown);
        PluginRegistry::instance().register_video_codec(
            "nothing-knows", &nothing);
    }

    EngineConfig make_config() {
        EngineConfig cfg{};
        cfg.video_codec_name    = "h264";
        cfg.video_receiver_name = "reference";
        cfg.video_sender_name   = "reference";
        cfg.video_source_name   = "memory";
        cfg.video_sink_name     = "headless";
        cfg.datachannel_backend = "";
        cfg.bwe_name            = "";
        cfg.scheduler_config.enabled = false;
        return cfg;
    }
};

// ---------------------------------------------------------------------------
// Case 1: `kind()` wins over the plugin id.
// ---------------------------------------------------------------------------

TEST_F(EngineVideoCodecFamily, KindWinsOverThePluginId) {
    auto cfg = make_config();
    cfg.video_codec_name = "av1-under-h264-id";

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // The offer must name AV1 — the family the codec actually
    // produces — not H.264 (which is only the plugin's id).
    const std::string offer = engine.create_offer();
    EXPECT_NE(offer.find("AV1/90000"), std::string::npos)
        << "the offer must advertise the codec's own family:\n" << offer;
    EXPECT_EQ(offer.find("H264/90000"), std::string::npos)
        << "the plugin id must not decide the SDP encoding:\n" << offer;
}

// ---------------------------------------------------------------------------
// Case 2: the plugin id is the fallback when `kind()` is kUnknown.
// ---------------------------------------------------------------------------

TEST_F(EngineVideoCodecFamily, PluginIdIsTheFallbackWhenKindIsUnknown) {
    auto cfg = make_config();
    cfg.video_codec_name = "h265-unknown-kind";

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const std::string offer = engine.create_offer();
    EXPECT_NE(offer.find("H265/90000"), std::string::npos)
        << "the plugin id must be the fallback source of the family:\n"
        << offer;
}

// ---------------------------------------------------------------------------
// Case 3: neither `kind()` nor the id names a family → hard failure.
// ---------------------------------------------------------------------------

TEST_F(EngineVideoCodecFamily, UnknownFamilyFailsTheSession) {
    auto cfg = make_config();
    cfg.video_codec_name = "nothing-knows";

    std::uint32_t        last_code = 0;
    std::string          last_msg;
    NimRTCEngine engine(cfg);
    engine.set_on_error([&](std::uint32_t code, std::string_view msg) {
        last_code = code;
        last_msg  = std::string{msg};
    });

    const std::uint32_t rc = engine.open();

    EXPECT_NE(rc, 0u)
        << "a codec that identifies no family must not open the session — "
           "the SDP encoding name cannot be derived";
    EXPECT_EQ(last_code, nimrtc::core::kEngineInvalidParam)
        << "the failure must be reported as an invalid parameter";
    EXPECT_NE(last_msg.find("codec family"), std::string::npos)
        << "the error message must explain what is missing: " << last_msg;
}

// ---------------------------------------------------------------------------
// Case 4: the engine does not advertise a video m-line it cannot describe.
// ---------------------------------------------------------------------------

TEST_F(EngineVideoCodecFamily, UnknownFamilyAdvertisesNoVideoMLine) {
    auto cfg = make_config();
    cfg.video_codec_name = "nothing-knows";

    NimRTCEngine engine(cfg);
    // `open()` fails, so there is nothing to negotiate.
    EXPECT_NE(engine.open(), 0u);
    EXPECT_EQ(engine.video_codec(), nullptr)
        << "the codec plugin must be torn down when the family is unknown";
    EXPECT_EQ(engine.video_receiver(), nullptr);
    EXPECT_EQ(engine.video_sender(), nullptr);
}

}  // namespace
