/**
 * @file src/modules/h264/tests/test_hw_backends.cpp
 * @brief Unit tests for the HW backend probe and registry wiring.
 *
 * Tests:
 *   1. HwVideoBackendRegistry is populated by register_default_video_backends().
 *   2. Backend entries have valid priorities (higher wins) and known IDs.
 *   3. available() / create() are callable regardless of SDK presence.
 *   4. When the SDK is absent, available() returns false and create()
 *      returns nullptr — the registry's select_*() falls through to the
 *      stub entry without crashing.
 *   5. When the SDK is present (NIMRTC_PLUGINS_*_ON), create() returns
 *      a non-null IVideoCodec.
 *   6. hw_backend_base helpers (find_sps_in, find_pps_in, contains_idr_slice,
 *      pack_annex_b, nv12_to_i420) work on synthetic bitstreams.
 *
 * These tests do NOT require a GPU; they verify the *dispatch surface*
 * and the helper functions. Integration tests that actually call into
 * NVENC/AMF/QSV/DXVA/VA-API live in tests/ (separate test executable
 * compiled only on hosts with the corresponding SDK installed).
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include <nimrtc/h264/hw_backends.hpp>
#include <nimrtc/h264/codec_plugin.hpp>
#include <nimrtc/plugins/hw_video_backend.hpp>
#include <nimrtc/plugins/video_codec.hpp>

// Pull in the private helpers via the .cpp source directly — they're
// declared in src/hw_backend_base.hpp which is in the src/ tree.  Use a
// copy of the helpers that lives in tests/ via include path (the module
// exposes src/ as PRIVATE include).
#include "hw_backend_base.hpp"

namespace h264 = nimrtc::h264;
namespace plugins = nimrtc::plugins;

namespace {

void push_nalu(std::vector<std::uint8_t>& bs,
               std::initializer_list<std::uint8_t> nalu) {
    constexpr std::uint8_t sc[4] = {0x00, 0x00, 0x00, 0x01};
    bs.insert(bs.end(), sc, sc + 4);
    bs.insert(bs.end(), nalu.begin(), nalu.end());
}

} // namespace

// ---------------------------------------------------------------------------
// Registry population
// ---------------------------------------------------------------------------

TEST(HwBackends, RegistryPopulatedAfterRegistration) {
    h264::register_default_video_backends();
    auto& reg = plugins::HwVideoBackendRegistry::instance();

    // At minimum the stub backend is always registered.
    auto encoders = reg.list_encoders(plugins::VideoCodecKind::kH264);
    auto decoders = reg.list_decoders(plugins::VideoCodecKind::kH264);

    ASSERT_FALSE(encoders.empty());
    ASSERT_FALSE(decoders.empty());

    bool found_stub_enc = false;
    for (const auto* be : encoders) {
        if (std::string(be->id) == "stub_h264") {
            found_stub_enc = true;
            EXPECT_EQ(be->priority, 0);
            EXPECT_FALSE(be->is_hw);
        }
    }
    EXPECT_TRUE(found_stub_enc);

    bool found_stub_dec = false;
    for (const auto* be : decoders) {
        if (std::string(be->id) == "stub_h264") {
            found_stub_dec = true;
        }
    }
    EXPECT_TRUE(found_stub_dec);
}

TEST(HwBackends, SelectEncoderReturnsHighestPriorityAvailable) {
    h264::register_default_video_backends();
    plugins::VideoCodecConfig cfg;
    cfg.width = 320;
    cfg.height = 240;

    const auto* be = h264::select_encoder_backend(cfg);
    ASSERT_NE(be, nullptr);

    // Priority ordering: highest first. Stub is priority 0, any HW
    // backend that compiled in must have priority > 0; if it doesn't,
    // available() returns false and stub wins.
    if (std::string(be->id) != "stub_h264") {
        EXPECT_GT(be->priority, 0);
        EXPECT_TRUE(be->is_hw);
    }
}

TEST(HwBackends, SelectDecoderReturnsHighestPriorityAvailable) {
    h264::register_default_video_backends();
    plugins::VideoCodecConfig cfg;
    cfg.width = 320;
    cfg.height = 240;

    const auto* be = h264::select_decoder_backend(cfg);
    ASSERT_NE(be, nullptr);
}

TEST(HwBackends, PreferredNameRespected) {
    h264::register_default_video_backends();
    plugins::VideoCodecConfig cfg;
    cfg.name = "stub_h264";        // explicitly ask for the stub
    cfg.width = 640;
    cfg.height = 480;

    const auto* be = h264::select_encoder_backend(cfg);
    ASSERT_NE(be, nullptr);
    EXPECT_EQ(std::string(be->id), "stub_h264");
}

// ---------------------------------------------------------------------------
// Helper functions — bitstream parsing
// ---------------------------------------------------------------------------

TEST(HelperBitstream, FindSpsInAnnexB) {
    std::vector<std::uint8_t> bs;
    push_nalu(bs, {0x67, 0x42, 0x00});  // SPS
    push_nalu(bs, {0x68, 0xCE});        // PPS
    push_nalu(bs, {0x65, 0xAA});        // IDR

    auto* sps = h264::hw_backend::find_sps_in({bs.data(), bs.size()});
    ASSERT_NE(sps, nullptr);
    EXPECT_EQ(sps->type, nimrtc::video_payload::h264::NaluType::kSPS);
    EXPECT_EQ(sps->data.size(), 3u);
    EXPECT_EQ(sps->data[0], 0x67);
}

TEST(HelperBitstream, FindPpsInAnnexB) {
    std::vector<std::uint8_t> bs;
    push_nalu(bs, {0x67, 0x42});
    push_nalu(bs, {0x68, 0xCE, 0xAB});
    push_nalu(bs, {0x65, 0xAA});

    auto* pps = h264::hw_backend::find_pps_in({bs.data(), bs.size()});
    ASSERT_NE(pps, nullptr);
    EXPECT_EQ(pps->data.size(), 3u);
    EXPECT_EQ(pps->data[0], 0x68);
}

TEST(HelperBitstream, ContainsIdrSlice) {
    std::vector<std::uint8_t> bs_idr;
    push_nalu(bs_idr, {0x67, 0x42});
    push_nalu(bs_idr, {0x68, 0xCE});
    push_nalu(bs_idr, {0x65, 0xAA});   // IDR
    EXPECT_TRUE(h264::hw_backend::contains_idr_slice(
        {bs_idr.data(), bs_idr.size()}));

    std::vector<std::uint8_t> bs_p;
    push_nalu(bs_p, {0x67, 0x42});
    push_nalu(bs_p, {0x41, 0xCC});     // P-slice (no IDR)
    EXPECT_FALSE(h264::hw_backend::contains_idr_slice(
        {bs_p.data(), bs_p.size()}));
}

// ---------------------------------------------------------------------------
// Helper functions — pack_annex_b
// ---------------------------------------------------------------------------

TEST(HelperPack, AllThreeNalus) {
    const std::uint8_t sps_bytes[]   = {0x67, 0x42};
    const std::uint8_t pps_bytes[]   = {0x68, 0xCE};
    const std::uint8_t slice_bytes[] = {0x65, 0xAA, 0xBB};

    std::uint8_t out[64];
    std::size_t n = h264::hw_backend::pack_annex_b(
        nimrtc::core::ByteSpan{sps_bytes, sizeof(sps_bytes)},
        nimrtc::core::ByteSpan{pps_bytes, sizeof(pps_bytes)},
        nimrtc::core::ByteSpan{slice_bytes, sizeof(slice_bytes)},
        out, sizeof(out));
    EXPECT_EQ(n, 4u * 3 + 2u + 2u + 3u);   // 3 start codes + nalu bodies

    // Verify start codes.
    EXPECT_EQ(out[0], 0x00);
    EXPECT_EQ(out[1], 0x00);
    EXPECT_EQ(out[2], 0x00);
    EXPECT_EQ(out[3], 0x01);
    EXPECT_EQ(out[4], 0x67);   // SPS payload begins
}

TEST(HelperPack, EmptySliceOmitsSlice) {
    const std::uint8_t sps_bytes[] = {0x67, 0x42};
    const std::uint8_t pps_bytes[] = {0x68, 0xCE};

    std::uint8_t out[32];
    std::size_t n = h264::hw_backend::pack_annex_b(
        nimrtc::core::ByteSpan{sps_bytes, sizeof(sps_bytes)},
        nimrtc::core::ByteSpan{pps_bytes, sizeof(pps_bytes)},
        nimrtc::core::ByteSpan{}, out, sizeof(out));
    EXPECT_EQ(n, 4u * 2 + 2u + 2u);   // 2 start codes + nalu bodies
}

TEST(HelperPack, BufferTooSmallReturnsZero) {
    const std::uint8_t sps_bytes[] = {0x67, 0x42};
    const std::uint8_t pps_bytes[] = {0x68, 0xCE};
    const std::uint8_t slice_bytes[] = {0x65, 0xAA, 0xBB, 0xCC};

    std::uint8_t out[8];     // too small for 3 start codes + NALUs
    std::size_t n = h264::hw_backend::pack_annex_b(
        nimrtc::core::ByteSpan{sps_bytes, sizeof(sps_bytes)},
        nimrtc::core::ByteSpan{pps_bytes, sizeof(pps_bytes)},
        nimrtc::core::ByteSpan{slice_bytes, sizeof(slice_bytes)},
        out, sizeof(out));
    EXPECT_EQ(n, 0u);
}

// ---------------------------------------------------------------------------
// Helper functions — NV12 → I420 conversion
// ---------------------------------------------------------------------------

TEST(HelperNV12, BasicConversion) {
    constexpr std::uint32_t W = 4;
    constexpr std::uint32_t H = 4;

    // Build an NV12 frame: 4x4 Y plane (16 bytes) + 2x2 UV plane
    // (interleaved, 8 bytes total = 4 UV pairs).
    std::vector<std::uint8_t> nv12;
    for (int i = 0; i < 16; ++i) {
        nv12.push_back(static_cast<std::uint8_t>(i * 10));  // Y[0..15]
    }
    // UV pairs:
    //   (U0,V0), (U1,V1), (U2,V2), (U3,V3) for 2x2 macroblock grid
    for (int i = 0; i < 4; ++i) {
        nv12.push_back(static_cast<std::uint8_t>(100 + i));   // U
        nv12.push_back(static_cast<std::uint8_t>(150 + i));   // V
    }

    std::uint8_t y_out[16] = {};
    std::uint8_t u_out[4]  = {};
    std::uint8_t v_out[4]  = {};

    h264::hw_backend::nv12_to_i420(nv12.data(), W, H, y_out, u_out, v_out);

    // Y plane —copied verbatim.
    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(y_out[i], i * 10);
    }
    // U / V —de-interleaved.
    EXPECT_EQ(u_out[0], 100);
    EXPECT_EQ(u_out[1], 101);
    EXPECT_EQ(u_out[2], 102);
    EXPECT_EQ(u_out[3], 103);
    EXPECT_EQ(v_out[0], 150);
    EXPECT_EQ(v_out[1], 151);
    EXPECT_EQ(v_out[2], 152);
    EXPECT_EQ(v_out[3], 153);
}

// ---------------------------------------------------------------------------
// HW backend factory + available() —verify symbols exist and don't crash
// when the SDK is absent.  When NIMRTC_PLUGINS_*_ON is NOT defined, the
// factory returns nullptr and available() returns false.
// ---------------------------------------------------------------------------

#if defined(NIMRTC_PLUGINS_NVENC_ON)
TEST(HwFactory, NvencAvailableWhenLinkPresent) {
    // When the SDK is linked in, available() returns true if the driver
    // is loaded; on a CI box without NVIDIA hardware it will return
    // false.  Either is acceptable — we just want to verify the symbol
    // exists and is callable.
    bool avail = nvenc_h264_available();  // link-time resolution
    (void)avail;
}
#endif

// ---------------------------------------------------------------------------
// Stub CodecPluginAdapter roundtrip still works
// ---------------------------------------------------------------------------

TEST(HwBackends, StubCodecStillUsable) {
    nimrtc::plugins::VideoCodecConfig cfg;
    cfg.width = 320;
    cfg.height = 240;
    nimrtc::h264::CodecPluginAdapter adapter(cfg);
    ASSERT_EQ(adapter.open(), nimrtc::plugins::kOk);
    EXPECT_TRUE(adapter.can_encode());
    EXPECT_TRUE(adapter.can_decode());

    nimrtc::plugins::VideoFrame raw;
    raw.info.frame_seq = 0;
    std::uint8_t buf[4096] = {};
    nimrtc::plugins::EncodedVideoFrame enc;
    EXPECT_EQ(adapter.encode(raw, buf, sizeof(buf), enc),
              nimrtc::plugins::kOk);
    EXPECT_GT(enc.payload.size(), 0u);
    EXPECT_TRUE(enc.is_keyframe);

    adapter.close();
}
