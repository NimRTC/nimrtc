/**
 * @file tests/test_hw_plugin_seam.cpp
 * @brief R3-Batch validation: HW plugin seam.
 *
 * What this test verifies:
 *   - The four reference plugin interfaces expose
 *     `is_hardware_accelerated()` and `hardware_backend()` capability
 *     methods, defaulting to "false" and "software".
 *   - A HW-flavored plugin (mock encoder that inherits both IVideoSender
 *     and IHwVideoEncoder) reports `is_hardware_accelerated()==true` and
 *     a non-software backend name.
 *   - `HwBackend` enum round-trips through `hw_backend_name()`.
 *   - The `is_hw_accelerated()` helper detects HW plugins via downcast.
 *
 * NOTE: No actual HW device is required — this test uses a stub plugin
 *       class that derives from the abstract HW seam interfaces to verify
 *       the layout and contract.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string_view>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/plugins/hw_seam.hpp>
#include <nimrtc/plugins/video_pipeline.hpp>
#include <nimrtc/plugins/video_sink.hpp>
#include <nimrtc/plugins/video_source.hpp>

#include <nimrtc/video_pipeline/video_pipeline_plugin.hpp>
#include <nimrtc/video_sink/video_sink.hpp>
#include <nimrtc/video_source/video_source_plugin.hpp>

namespace {

namespace np = nimrtc::plugins;

// ---------------------------------------------------------------------------
// Mock HW encoder: implements BOTH IVideoSender AND IHwVideoEncoder so the
// plugin-seam contract (multiple inheritance + capability flag) is exercised.
// ---------------------------------------------------------------------------
class MockHwEncoder : public np::IVideoSender, public np::IHwVideoEncoder {
public:
    MockHwEncoder() = default;

    // ---- IPlugin ---------------------------------------------------------
    const char* name()  const noexcept override { return "MockHwEncoder"; }
    np::Status  open()        noexcept override { return np::kOk; }
    void        close()       noexcept override {}

    // ---- IVideoSender (minimal stubs; just enough for the test) ---------
    void set_packet_callback(np::VideoSenderPacketCallback) noexcept override {}
    np::Status push_frame(const np::EncodedVideoFrame& /*f*/,
                          std::uint32_t /*rtp_ts*/) noexcept override {
        return np::kOk;
    }
    void force_keyframe() noexcept override {}
    np::VideoSenderStats stats()  const noexcept override { return {}; }
    std::uint16_t next_seq()      const noexcept override { return 0; }
    np::VideoSenderConfig config() const noexcept override { return {}; }

    // ---- R3-Batch: override capability flags to advertise HW ------------
    bool is_hardware_accelerated() const noexcept override { return true; }
    std::string_view hardware_backend() const noexcept override {
        return "mediacodec";
    }

    // ---- IHwVideoEncoder -------------------------------------------------
    np::HwBackend backend() const noexcept override {
        return np::HwBackend::MediaCodec;
    }
    void* target_surface_handle() noexcept override { return &dummy_handle_; }
    bool supports(np::VideoCodecKind codec,
                  std::uint32_t width, std::uint32_t height,
                  std::uint32_t fps) const noexcept override {
        return codec == np::VideoCodecKind::kH264
            && width  <= 1920
            && height <= 1080
            && fps    <= 60;
    }
    std::uint32_t encoding_latency_us() const noexcept override { return 5000; }

private:
    int dummy_handle_ = 0;
};

// ---------------------------------------------------------------------------
// HwBackend enum round-trip
// ---------------------------------------------------------------------------

TEST(HwBackend, RoundTripThroughHwBackendName) {
    EXPECT_EQ(np::hw_backend_name(np::HwBackend::Software),     "software");
    EXPECT_EQ(np::hw_backend_name(np::HwBackend::MediaCodec),   "mediacodec");
    EXPECT_EQ(np::hw_backend_name(np::HwBackend::VideoToolbox), "videotoolbox");
    EXPECT_EQ(np::hw_backend_name(np::HwBackend::Nvdec),        "nvdec");
    EXPECT_EQ(np::hw_backend_name(np::HwBackend::Nvenc),        "nvenc");
    EXPECT_EQ(np::hw_backend_name(np::HwBackend::Vaapi),        "vaapi");
    EXPECT_EQ(np::hw_backend_name(np::HwBackend::Dxva),         "dxva");
    EXPECT_EQ(np::hw_backend_name(np::HwBackend::Qsv),          "qsv");
    EXPECT_EQ(np::hw_backend_name(np::HwBackend::Amf),          "amf");
    EXPECT_EQ(np::hw_backend_name(np::HwBackend::V4l2),         "v4l2");
    EXPECT_EQ(np::hw_backend_name(np::HwBackend::AvFoundation), "avfoundation");
    EXPECT_EQ(np::hw_backend_name(np::HwBackend::DirectShow),   "directshow");
    EXPECT_EQ(np::hw_backend_name(np::HwBackend::AndroidCamera2), "android-camera2");
}

TEST(HwBackend, UnknownMapsToUnknown) {
    EXPECT_EQ(np::hw_backend_name(np::HwBackend::Unknown), "unknown");
    EXPECT_EQ(np::hw_backend_name(static_cast<np::HwBackend>(0xBEEF)),
              "unknown");
}

// ---------------------------------------------------------------------------
// Reference plugin defaults
// ---------------------------------------------------------------------------

TEST(HwSeam, ReferencePluginsDefaultToSoftware) {
    // Use a stub plugin via the same path the engine uses: create
    // through the registry.
    nimrtc::video_source::register_default_plugins();
    nimrtc::video_sink::register_default_plugins();
    nimrtc::video_pipeline::register_default_plugins();

    auto* src = nimrtc::core::PluginRegistry::instance()
                  .get_video_source("memory")->create(np::VideoSourceConfig{});
    ASSERT_NE(src, nullptr);
    EXPECT_FALSE(src->is_hardware_accelerated());
    EXPECT_EQ(src->hardware_backend(), "software");
    delete src;

    auto* sink = nimrtc::core::PluginRegistry::instance()
                  .get_video_sink("headless")->create(np::VideoSinkConfig{});
    ASSERT_NE(sink, nullptr);
    EXPECT_FALSE(sink->is_hardware_accelerated());
    EXPECT_EQ(sink->hardware_backend(), "software");
    delete sink;

    auto* rx = nimrtc::core::PluginRegistry::instance()
                  .get_video_receiver("reference")->create(np::VideoReceiverConfig{});
    ASSERT_NE(rx, nullptr);
    EXPECT_FALSE(rx->is_hardware_accelerated());
    EXPECT_EQ(rx->hardware_backend(), "software");
    delete rx;

    auto* tx = nimrtc::core::PluginRegistry::instance()
                  .get_video_sender("reference")->create(np::VideoSenderConfig{});
    ASSERT_NE(tx, nullptr);
    EXPECT_FALSE(tx->is_hardware_accelerated());
    EXPECT_EQ(tx->hardware_backend(), "software");
    delete tx;
}

// ---------------------------------------------------------------------------
// HW-flavored plugin: capability flag + backend enum
// ---------------------------------------------------------------------------

TEST(HwSeam, MockHwEncoderReportsAcceleration) {
    MockHwEncoder enc;
    EXPECT_TRUE(enc.is_hardware_accelerated());
    EXPECT_EQ(enc.hardware_backend(), "mediacodec");
    EXPECT_EQ(enc.backend(),           np::HwBackend::MediaCodec);
    EXPECT_EQ(enc.backend_name(),      "mediacodec");
    EXPECT_NE(enc.target_surface_handle(), nullptr);
    EXPECT_GT(enc.encoding_latency_us(), 0u);
}

TEST(HwSeam, MockHwEncoderCapabilityQuery) {
    MockHwEncoder enc;
    // Supported: H.264 1080p30.
    EXPECT_TRUE(enc.supports(np::VideoCodecKind::kH264, 1920, 1080, 30));
    // Not supported: VP8.
    EXPECT_FALSE(enc.supports(np::VideoCodecKind::kVP8,  1920, 1080, 30));
    // Not supported: 4K60 (exceeds the mock's 1920×1080 / 60 fps cap).
    EXPECT_FALSE(enc.supports(np::VideoCodecKind::kH264, 3840, 2160, 60));
}

// ---------------------------------------------------------------------------
// is_hw_accelerated() helper: identifies HW plugins via downcast
// ---------------------------------------------------------------------------
namespace {
// Alias to dodge GTest macro comma-splitting in template arg lists.
template <class A, class B>
bool is_hw_accelerated_typed(A* a, B* b) noexcept {
    return np::is_hw_accelerated<A, B>(a, b);
}
} // namespace

TEST(HwSeam, IsHwAcceleratedHelperDetectsHwPlugin) {
    // Reference plugins are registered on demand; mirror the pattern used by
    // the other tests in this file (e.g. ReferencePluginsDefaultToSoftware).
    nimrtc::video_source::register_default_plugins();
    nimrtc::video_sink::register_default_plugins();
    nimrtc::video_pipeline::register_default_plugins();

    MockHwEncoder enc;
    np::IHwVideoEncoder* as_hw = &enc;
    EXPECT_TRUE((is_hw_accelerated_typed<np::IVideoSender, np::IHwVideoEncoder>(
        &enc, as_hw)));

    // Software plugin via the reference registry should NOT match.
    auto* sw = nimrtc::core::PluginRegistry::instance()
                 .get_video_sender("reference")->create(np::VideoSenderConfig{});
    ASSERT_NE(sw, nullptr);
    EXPECT_FALSE((is_hw_accelerated_typed<np::IVideoSender, np::IHwVideoEncoder>(
        sw, nullptr)));
    delete sw;
}

TEST(HwSeam, IsHwAcceleratedHelperHandlesNull) {
    EXPECT_FALSE((is_hw_accelerated_typed<np::IVideoSender, np::IHwVideoEncoder>(
        nullptr, nullptr)));
}

// ---------------------------------------------------------------------------
// Registry-level capability introspection (R3-Batch)
// ---------------------------------------------------------------------------

namespace {

bool check_video_source_plugin(nimrtc::core::PluginRegistry& reg,
                               std::string_view id) {
    auto* p = reg.get_video_source(id)->create(np::VideoSourceConfig{});
    if (!p) return false;
    bool ok = !p->is_hardware_accelerated()
           && p->hardware_backend() == "software";
    delete p;
    return ok;
}

bool check_video_sink_plugin(nimrtc::core::PluginRegistry& reg,
                             std::string_view id) {
    auto* p = reg.get_video_sink(id)->create(np::VideoSinkConfig{});
    if (!p) return false;
    bool ok = !p->is_hardware_accelerated()
           && p->hardware_backend() == "software";
    delete p;
    return ok;
}

bool check_video_receiver_plugin(nimrtc::core::PluginRegistry& reg,
                                 std::string_view id) {
    auto* p = reg.get_video_receiver(id)->create(np::VideoReceiverConfig{});
    if (!p) return false;
    bool ok = !p->is_hardware_accelerated()
           && p->hardware_backend() == "software";
    delete p;
    return ok;
}

bool check_video_sender_plugin(nimrtc::core::PluginRegistry& reg,
                               std::string_view id) {
    auto* p = reg.get_video_sender(id)->create(np::VideoSenderConfig{});
    if (!p) return false;
    bool ok = !p->is_hardware_accelerated()
           && p->hardware_backend() == "software";
    delete p;
    return ok;
}

}  // namespace

TEST(HwSeam, RegistryPluginsAllReportSoftware) {
    nimrtc::video_source::register_default_plugins();
    nimrtc::video_sink::register_default_plugins();
    nimrtc::video_pipeline::register_default_plugins();
    auto& reg = nimrtc::core::PluginRegistry::instance();

    for (std::string_view id : reg.list_video_sources()) {
        EXPECT_TRUE(check_video_source_plugin(reg, id));
    }
    for (std::string_view id : reg.list_video_sinks()) {
        EXPECT_TRUE(check_video_sink_plugin(reg, id));
    }
    for (std::string_view id : reg.list_video_receivers()) {
        EXPECT_TRUE(check_video_receiver_plugin(reg, id));
    }
    for (std::string_view id : reg.list_video_senders()) {
        EXPECT_TRUE(check_video_sender_plugin(reg, id));
    }
}

} // namespace
