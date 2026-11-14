/**
 * @file src/modules/nvcodec/tests/test_vt_common.cpp
 * @brief Unit tests for the platform-independent VideoToolbox helpers.
 *
 * The VideoToolbox encoder and decoder are macOS-only, but the *bitstream
 * plumbing* they depend on is pure byte manipulation and therefore
 * testable on every host:
 *
 *   - the AV1 low-overhead OBU <-> Annex-B conversion (VideoToolbox emits
 *     the AV1 spec's low-overhead form; this tree carries AV1 in
 *     Annex-B framing),
 *   - the AVCC (length-prefixed) -> Annex-B conversion (VideoToolbox
 *     emits length-prefixed NAL units for H.264 / HEVC),
 *   - the Annex-B scanner and the NAL unit type mapping,
 *   - the OS-version gate (which is what the availability probe uses to
 *     decide "the OS is too old for the required VTCompressionOutputSettings").
 *
 * These tests therefore give the macOS-only code a real, executable
 * verification of everything that *can* be verified off-Apple.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

// The shared helpers live in the module's *private* src/ tree.
#include "vt_common.hpp"

namespace vt = nimrtc::nvcodec::vt_detail;

// ---------------------------------------------------------------------------
// AV1 low-overhead OBU <-> Annex-B
// ---------------------------------------------------------------------------

namespace {

/** Build one low-overhead OBU: header + LEB128 size + payload. */
std::vector<std::uint8_t> make_low_overhead_obu(std::uint8_t header,
                                                const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> out;
    out.push_back(static_cast<std::uint8_t>(header | 0x02u));
    std::uint8_t leb[10] = {};
    const std::size_t n = vt::av1_write_leb128(payload.size(), leb);
    out.insert(out.end(), leb, leb + n);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

} // namespace

TEST(VtCommon, Leb128RoundTrips) {
    const std::uint64_t values[] = {0, 1, 127, 128, 255, 300, 16383, 16384, 100000};
    for (const std::uint64_t v : values) {
        std::uint8_t buf[10] = {};
        const std::size_t n = vt::av1_write_leb128(static_cast<std::size_t>(v), buf);
        std::size_t pos = 0;
        std::size_t decoded = 0;
        ASSERT_TRUE(vt::av1_read_leb128(buf, n, pos, decoded));
        EXPECT_EQ(decoded, static_cast<std::size_t>(v));
        EXPECT_EQ(pos, n);
    }
}

TEST(VtCommon, LowOverheadObuConvertsToAnnexB) {
    const std::vector<std::uint8_t> payload{0x12, 0x34, 0x56};
    const auto obu = make_low_overhead_obu(0x32u, payload);  // seq header + size

    std::vector<std::uint8_t> annex_b;
    ASSERT_TRUE(vt::av1_low_overhead_to_annex_b(obu.data(), obu.size(), annex_b));

    // Annex-B framing: a 4-byte start code, then the header with the
    // "has size field" bit cleared, then the payload.
    ASSERT_GE(annex_b.size(), 4u + 1u + payload.size());
    EXPECT_EQ(annex_b[0], 0x00);
    EXPECT_EQ(annex_b[1], 0x00);
    EXPECT_EQ(annex_b[2], 0x00);
    EXPECT_EQ(annex_b[3], 0x01);
    EXPECT_EQ(annex_b[4], 0x32u & ~0x02u);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        EXPECT_EQ(annex_b[5 + i], payload[i]);
    }
}

TEST(VtCommon, AnnexBToLowOverheadRoundTrips) {
    const std::vector<std::uint8_t> payload{0xAA, 0xBB, 0xCC, 0xDD};

    // Annex-B by hand: start code + header (size bit cleared) + payload.
    std::vector<std::uint8_t> annex_b{0x00, 0x00, 0x00, 0x01, 0x30u};
    annex_b.insert(annex_b.end(), payload.begin(), payload.end());

    std::vector<std::uint8_t> low;
    ASSERT_TRUE(vt::av1_annex_b_to_low_overhead(annex_b.data(), annex_b.size(), low));

    // Back to Annex-B: the two forms must agree.
    std::vector<std::uint8_t> back;
    ASSERT_TRUE(vt::av1_low_overhead_to_annex_b(low.data(), low.size(), back));
    EXPECT_EQ(back, annex_b);
}

TEST(VtCommon, LowOverheadConversionRejectsMalformedInput) {
    std::vector<std::uint8_t> out;

    // Empty input.
    EXPECT_FALSE(vt::av1_low_overhead_to_annex_b(nullptr, 0, out));

    // A header without the size field: the OBU's length is unknowable.
    const std::uint8_t no_size[] = {0x30};
    EXPECT_FALSE(vt::av1_low_overhead_to_annex_b(no_size, sizeof(no_size), out));

    // A truncated OBU (the declared size runs past the buffer).
    const std::uint8_t truncated[] = {0x32, 0x08, 0x01, 0x02};
    EXPECT_FALSE(vt::av1_low_overhead_to_annex_b(truncated, sizeof(truncated), out));
}

TEST(VtCommon, AlreadyAnnexBIsPassedThroughUnchanged) {
    const std::uint8_t annex_b[] = {0x00, 0x00, 0x00, 0x01, 0x30, 0x11, 0x22};
    std::vector<std::uint8_t> out;
    ASSERT_TRUE(vt::av1_low_overhead_to_annex_b(annex_b, sizeof(annex_b), out));
    EXPECT_EQ(out.size(), sizeof(annex_b));
    for (std::size_t i = 0; i < sizeof(annex_b); ++i) {
        EXPECT_EQ(out[i], annex_b[i]);
    }
}

// ---------------------------------------------------------------------------
// AVCC (length-prefixed) -> Annex-B
// ---------------------------------------------------------------------------

TEST(VtCommon, AvccConvertsToAnnexB) {
    // Two length-prefixed NAL units: 0x65 0x01 and 0x41 0x02 0x03.
    const std::uint8_t avcc[] = {
        0x00, 0x00, 0x00, 0x02, 0x65, 0x01,
        0x00, 0x00, 0x00, 0x03, 0x41, 0x02, 0x03,
    };
    std::vector<std::uint8_t> out;
    ASSERT_TRUE(vt::ensure_annex_b(avcc, sizeof(avcc), out));

    const std::uint8_t expected[] = {
        0x00, 0x00, 0x00, 0x01, 0x65, 0x01,
        0x00, 0x00, 0x00, 0x01, 0x41, 0x02, 0x03,
    };
    ASSERT_EQ(out.size(), sizeof(expected));
    for (std::size_t i = 0; i < sizeof(expected); ++i) {
        EXPECT_EQ(out[i], expected[i]);
    }
}

TEST(VtCommon, EnsureAnnexBRejectsGarbage) {
    std::vector<std::uint8_t> out;
    const std::uint8_t garbage[] = {0x11, 0x22, 0x33, 0x44, 0x55};
    EXPECT_FALSE(vt::ensure_annex_b(garbage, sizeof(garbage), out));
    EXPECT_FALSE(vt::ensure_annex_b(nullptr, 0, out));
}

// ---------------------------------------------------------------------------
// Annex-B -> AVCC (the form VideoToolbox's *decoder* session wants)
// ---------------------------------------------------------------------------

TEST(VtCommon, AnnexBConvertsToAvcc) {
    // Two NAL units: 0x65 0x01 and 0x41 0x02 0x03.
    const std::uint8_t annex_b[] = {
        0x00, 0x00, 0x00, 0x01, 0x65, 0x01,
        0x00, 0x00, 0x00, 0x01, 0x41, 0x02, 0x03,
    };
    std::vector<std::uint8_t> out;
    ASSERT_TRUE(vt::annex_b_to_avcc(annex_b, sizeof(annex_b), out));

    const std::uint8_t expected[] = {
        0x00, 0x00, 0x00, 0x02, 0x65, 0x01,
        0x00, 0x00, 0x00, 0x03, 0x41, 0x02, 0x03,
    };
    ASSERT_EQ(out.size(), sizeof(expected));
    for (std::size_t i = 0; i < sizeof(expected); ++i) {
        EXPECT_EQ(out[i], expected[i]);
    }
}

TEST(VtCommon, AnnexBToAvccRejectsNonAnnexB) {
    std::vector<std::uint8_t> out;
    const std::uint8_t not_annex_b[] = {0x11, 0x22, 0x33, 0x44};
    EXPECT_FALSE(vt::annex_b_to_avcc(not_annex_b, sizeof(not_annex_b), out));
    EXPECT_FALSE(vt::annex_b_to_avcc(nullptr, 0, out));
}

TEST(VtCommon, AvccAndAnnexBAreInverse) {
    const std::uint8_t annex_b[] = {
        0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x11,
        0x00, 0x00, 0x00, 0x01, 0x68, 0xCE,
    };
    std::vector<std::uint8_t> avcc;
    ASSERT_TRUE(vt::annex_b_to_avcc(annex_b, sizeof(annex_b), avcc));

    std::vector<std::uint8_t> back = vt::avcc_to_annex_b(avcc.data(), avcc.size());
    ASSERT_FALSE(back.empty());

    ASSERT_EQ(back.size(), sizeof(annex_b));
    for (std::size_t i = 0; i < sizeof(annex_b); ++i) {
        EXPECT_EQ(back[i], annex_b[i]);
    }
}

// ---------------------------------------------------------------------------
// Parameter-set extraction
//
// A VideoToolbox decoder's format description must carry the stream's own
// SPS / PPS (and the VPS for HEVC) — so the extraction must find them in
// an Annex-B stream.
// ---------------------------------------------------------------------------

TEST(VtCommon, ExtractsH264ParameterSets) {
    const std::uint8_t bs[] = {
        0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x11,  // SPS
        0x00, 0x00, 0x00, 0x01, 0x68, 0xCE,              // PPS
        0x00, 0x00, 0x00, 0x01, 0x65, 0xAA,              // IDR
    };
    vt::ParameterSets sets;
    ASSERT_TRUE(vt::extract_parameter_sets(vt::Codec::kH264, bs, sizeof(bs), sets));

    ASSERT_EQ(sets.sps.size(), 4u);
    EXPECT_EQ(sets.sps[0], 0x67);
    EXPECT_EQ(sets.sps[1], 0x42);
    ASSERT_EQ(sets.pps.size(), 2u);
    EXPECT_EQ(sets.pps[0], 0x68);
    EXPECT_TRUE(sets.vps.empty());
}

TEST(VtCommon, ExtractsHevcParameterSets) {
    // HEVC VPS (32), SPS (33), PPS (34).
    const std::uint8_t bs[] = {
        0x00, 0x00, 0x00, 0x01, 0x40, 0x01,  // VPS
        0x00, 0x00, 0x00, 0x01, 0x42, 0x02,  // SPS
        0x00, 0x00, 0x00, 0x01, 0x44, 0x03,  // PPS
    };
    vt::ParameterSets sets;
    ASSERT_TRUE(vt::extract_parameter_sets(vt::Codec::kH265, bs, sizeof(bs), sets));

    ASSERT_EQ(sets.vps.size(), 2u);
    EXPECT_EQ(sets.vps[0], 0x40);
    ASSERT_EQ(sets.sps.size(), 2u);
    EXPECT_EQ(sets.sps[0], 0x42);
    ASSERT_EQ(sets.pps.size(), 2u);
    EXPECT_EQ(sets.pps[0], 0x44);
}

TEST(VtCommon, ParameterSetExtractionFailsWithoutAnSps) {
    const std::uint8_t bs[] = {
        0x00, 0x00, 0x00, 0x01, 0x65, 0xAA,  // IDR only
    };
    vt::ParameterSets sets;
    EXPECT_FALSE(vt::extract_parameter_sets(vt::Codec::kH264, bs, sizeof(bs), sets));
    EXPECT_FALSE(sets.has_sps());
}

// ---------------------------------------------------------------------------
// Annex-B scanning
// ---------------------------------------------------------------------------

TEST(VtCommon, ScansAnnexBNalUnits) {
    // SPS (7), PPS (8), IDR (5) in H.264.
    const std::uint8_t bs[] = {
        0x00, 0x00, 0x00, 0x01, 0x67, 0x42,
        0x00, 0x00, 0x00, 0x01, 0x68, 0xCE,
        0x00, 0x00, 0x00, 0x01, 0x65, 0xAA, 0xBB,
    };
    const auto nalus = vt::scan_annex_b(vt::Codec::kH264, bs, sizeof(bs));
    ASSERT_EQ(nalus.size(), 3u);
    EXPECT_EQ(nalus[0].type, 7u);
    EXPECT_EQ(nalus[1].type, 8u);
    EXPECT_EQ(nalus[2].type, 5u);
    EXPECT_EQ(nalus[2].size, 3u);
}

TEST(VtCommon, HevcNaluTypeIsSixBits) {
    // HEVC VPS is NAL type 32 (0x40), which does not fit in H.264's
    // 5-bit type field — this is the mapping the packetizer depends on.
    const std::uint8_t bs[] = {
        0x00, 0x00, 0x00, 0x01, 0x40, 0x01,
        0x00, 0x00, 0x00, 0x01, 0x26, 0x01,
    };
    const auto nalus = vt::scan_annex_b(vt::Codec::kH265, bs, sizeof(bs));
    ASSERT_EQ(nalus.size(), 2u);
    EXPECT_EQ(nalus[0].type, 32u);
    EXPECT_EQ(nalus[1].type, 19u);
}

TEST(VtCommon, LooksLikeAnnexB) {
    const std::uint8_t four[] = {0x00, 0x00, 0x00, 0x01, 0x65};
    const std::uint8_t three[] = {0x00, 0x00, 0x01, 0x65};
    const std::uint8_t none[]  = {0x65, 0x00, 0x00, 0x00};
    EXPECT_TRUE(vt::looks_like_annex_b(four, sizeof(four)));
    EXPECT_TRUE(vt::looks_like_annex_b(three, sizeof(three)));
    EXPECT_FALSE(vt::looks_like_annex_b(none, sizeof(none)));
}

// ---------------------------------------------------------------------------
// The OS-version gate
//
// This is what the availability probe uses to answer "is the OS too old
// for the required VTCompressionOutputSettings?".
// ---------------------------------------------------------------------------

TEST(VtCommon, MinimumMacOsVersions) {
    // H.264: the hardware-accelerated specification keys and
    // `RealTime` are `API_AVAILABLE(macosx(10.9))`.
    EXPECT_EQ(vt::min_macos_version(vt::Codec::kH264).major, 10);
    EXPECT_EQ(vt::min_macos_version(vt::Codec::kH264).minor, 9);
    // HEVC landed in High Sierra.
    EXPECT_EQ(vt::min_macos_version(vt::Codec::kH265).major, 10);
    EXPECT_EQ(vt::min_macos_version(vt::Codec::kH265).minor, 13);
    // AV1 landed in Sonoma.
    EXPECT_EQ(vt::min_macos_version(vt::Codec::kAv1).major, 14);
}

TEST(VtCommon, OsVersionIsUnknownOffApple) {
    const auto v = vt::os_version();
#if defined(__APPLE__)
    EXPECT_GT(v.major, 0);
#else
    // Off Apple the version is unknown, and an unknown version is
    // treated as "too old" — the conservative answer.
    EXPECT_EQ(v.major, 0);
    EXPECT_FALSE(vt::os_at_least(vt::OsVersion{10, 13}));
#endif
}

TEST(VtCommon, UnavailableReasonsAreNeverEmpty) {
    for (const auto codec : {vt::Codec::kH264, vt::Codec::kH265, vt::Codec::kAv1}) {
        EXPECT_FALSE(vt::encoder_unavailable_reason(codec).empty());
        EXPECT_FALSE(vt::decoder_unavailable_reason(codec).empty());
    }
}
