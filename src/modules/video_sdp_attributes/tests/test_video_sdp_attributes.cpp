/**
 * @file src/modules/video_sdp_attributes/tests/test_video_sdp_attributes.cpp
 * @brief Unit tests for video_sdp_attributes.
 *
 * Covers:
 *   1. split_fmtp_pairs (basic, edge cases)
 *   2. parse_int_value
 *   3. H.264 fmtp parse + build roundtrip
 *   4. VP8  fmtp parse + build roundtrip
 *   5. VP9  fmtp parse + build roundtrip
 *   6. codec_from_rtpmap_encoding auto-detect
 *   7. parse_fmtp dispatcher
 *   8. Error handling (malformed inputs)
 */

#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include <nimrtc/video_sdp_attributes/video_sdp_attributes.hpp>

using nimrtc::video_sdp_attributes::H264Fmtp;
using nimrtc::video_sdp_attributes::Vp8Fmtp;
using nimrtc::video_sdp_attributes::Vp9Fmtp;
using nimrtc::video_sdp_attributes::PacketizationMode;
using nimrtc::video_sdp_attributes::parse_h264;
using nimrtc::video_sdp_attributes::parse_vp8;
using nimrtc::video_sdp_attributes::parse_vp9;
using nimrtc::video_sdp_attributes::parse_fmtp;
using nimrtc::video_sdp_attributes::build_h264_fmtp;
using nimrtc::video_sdp_attributes::build_vp8_fmtp;
using nimrtc::video_sdp_attributes::build_vp9_fmtp;
using nimrtc::video_sdp_attributes::build_fmtp;
using nimrtc::video_sdp_attributes::split_fmtp_pairs;
using nimrtc::video_sdp_attributes::parse_int_value;
using nimrtc::video_sdp_attributes::codec_from_rtpmap_encoding;
using nimrtc::video_sdp_attributes::ParsedFmtp;
using nimrtc::video_sdp_attributes::CodecKind;
using nimrtc::core::ErrorCode;
// Note: nimrtc::video_sdp_attributes::CodecKind is a type alias for
// nimrtc::video_frame::CodecKind (see video_sdp_attributes.hpp), so there
// is no separate enum to test against here.

// -----------------------------------------------------------------------------
// split_fmtp_pairs
// -----------------------------------------------------------------------------

TEST(SplitFmtpPairs, BasicTwoPairs) {
    auto pairs = split_fmtp_pairs("packetization-mode=1;profile-level-id=42e01f");
    ASSERT_EQ(pairs.size(), 2u);
    EXPECT_EQ(pairs[0].first,  "packetization-mode");
    EXPECT_EQ(pairs[0].second, "1");
    EXPECT_EQ(pairs[1].first,  "profile-level-id");
    EXPECT_EQ(pairs[1].second, "42e01f");
}

TEST(SplitFmtpPairs, Empty) {
    EXPECT_TRUE(split_fmtp_pairs("").empty());
    EXPECT_TRUE(split_fmtp_pairs("   ").empty());
    EXPECT_TRUE(split_fmtp_pairs(";").empty());
}

TEST(SplitFmtpPairs, TrailingSemicolon) {
    auto pairs = split_fmtp_pairs("max-fr=30;");
    ASSERT_EQ(pairs.size(), 1u);
    EXPECT_EQ(pairs[0].first,  "max-fr");
    EXPECT_EQ(pairs[0].second, "30");
}

TEST(SplitFmtpPairs, BooleanAttribute) {
    auto pairs = split_fmtp_pairs("level-asymmetry-allowed");
    ASSERT_EQ(pairs.size(), 1u);
    EXPECT_EQ(pairs[0].first,  "level-asymmetry-allowed");
    EXPECT_EQ(pairs[0].second, "");
}

TEST(SplitFmtpPairs, UrlEncodedPercent) {
    auto pairs = split_fmtp_pairs("foo=%41%42");
    ASSERT_EQ(pairs.size(), 1u);
    EXPECT_EQ(pairs[0].second, "AB");
}

TEST(SplitFmtpPairs, EqualsInValueIsKept) {
    auto pairs = split_fmtp_pairs("a=b=c");
    ASSERT_EQ(pairs.size(), 1u);
    EXPECT_EQ(pairs[0].first,  "a");
    EXPECT_EQ(pairs[0].second, "b=c");
}

TEST(SplitFmtpPairs, WhitespaceTrimmed) {
    auto pairs = split_fmtp_pairs("  a = 1 ; b= 2  ;");
    ASSERT_EQ(pairs.size(), 2u);
    EXPECT_EQ(pairs[0].first,  "a");
    EXPECT_EQ(pairs[0].second, "1");
    EXPECT_EQ(pairs[1].first,  "b");
    EXPECT_EQ(pairs[1].second, "2");
}

// -----------------------------------------------------------------------------
// parse_int_value
// -----------------------------------------------------------------------------

TEST(ParseIntValue, Basic) {
    auto v = parse_int_value("123");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 123);
}

TEST(ParseIntValue, Negative) {
    auto v = parse_int_value("-7");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, -7);
}

TEST(ParseIntValue, Empty) {
    EXPECT_FALSE(parse_int_value("").has_value());
}

TEST(ParseIntValue, Garbage) {
    EXPECT_FALSE(parse_int_value("abc").has_value());
}

TEST(ParseIntValue, Partial) {
    EXPECT_FALSE(parse_int_value("12abc").has_value());
}

// -----------------------------------------------------------------------------
// codec_from_rtpmap_encoding
// -----------------------------------------------------------------------------

TEST(CodecFromRtpmap, KnownEncodings) {
    EXPECT_EQ(codec_from_rtpmap_encoding("H264"),  CodecKind::kH264);
    EXPECT_EQ(codec_from_rtpmap_encoding("VP8"),   CodecKind::kVP8);
    EXPECT_EQ(codec_from_rtpmap_encoding("VP9"),   CodecKind::kVP9);
    EXPECT_EQ(codec_from_rtpmap_encoding("AV1"),   CodecKind::kAV1);
}

TEST(CodecFromRtpmap, CaseInsensitive) {
    EXPECT_EQ(codec_from_rtpmap_encoding("h264"),  CodecKind::kH264);
    EXPECT_EQ(codec_from_rtpmap_encoding("vp8"),   CodecKind::kVP8);
    EXPECT_EQ(codec_from_rtpmap_encoding("Vp9"),   CodecKind::kVP9);
}

TEST(CodecFromRtpmap, Unknown) {
    EXPECT_EQ(codec_from_rtpmap_encoding("MPEG4"), CodecKind::kUnknown);
    EXPECT_EQ(codec_from_rtpmap_encoding(""),      CodecKind::kUnknown);
}

// -----------------------------------------------------------------------------
// H.264 parse / build
// -----------------------------------------------------------------------------

TEST(H264Fmtp, ParseTypicalWebRtCLine) {
    auto r = parse_h264("packetization-mode=1;profile-level-id=42e01f;"
                        "sprop-parameter-sets=Z0LAHtkA,aM4G4g==;max-mbps=2000;max-fs=1200");
    ASSERT_TRUE(r.ok()) << r.error().message();
    const auto& cfg = r.value();
    ASSERT_TRUE(cfg.packetization_mode.has_value());
    EXPECT_EQ(*cfg.packetization_mode, PacketizationMode::kNonInterleaved);
    ASSERT_TRUE(cfg.profile_level_id.has_value());
    EXPECT_EQ(*cfg.profile_level_id, "42e01f");
    ASSERT_TRUE(cfg.sprop_sps.has_value());
    EXPECT_EQ(*cfg.sprop_sps, "Z0LAHtkA");
    ASSERT_TRUE(cfg.sprop_pps.has_value());
    EXPECT_EQ(*cfg.sprop_pps, "aM4G4g==");
    ASSERT_TRUE(cfg.max_mbps.has_value());
    EXPECT_EQ(*cfg.max_mbps, 2000u);
    ASSERT_TRUE(cfg.max_fs.has_value());
    EXPECT_EQ(*cfg.max_fs, 1200u);
    EXPECT_TRUE(cfg.has_any());
}

TEST(H264Fmtp, ParseBaselineProfile) {
    auto r = parse_h264("profile-level-id=42e01f");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(*r.value().profile_level_id, "42e01f");
}

TEST(H264Fmtp, RejectsInvalidProfileLevelId) {
    // 5 chars (not 6) → reject
    auto r = parse_h264("profile-level-id=42e01");
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
}

TEST(H264Fmtp, RejectsNonHexProfileLevelId) {
    auto r = parse_h264("profile-level-id=zzzzzz");
    EXPECT_FALSE(r.ok());
}

TEST(H264Fmtp, RejectsInvalidPacketizationMode) {
    auto r = parse_h264("packetization-mode=99");
    EXPECT_FALSE(r.ok());
}

TEST(H264Fmtp, BuildRoundtrip) {
    H264Fmtp in;
    in.packetization_mode = PacketizationMode::kNonInterleaved;
    in.profile_level_id   = "42e01f";
    in.sprop_sps          = "Z0LAHtkA";
    in.sprop_pps          = "aM4G4g==";
    in.max_fs             = 1200;

    auto body = build_h264_fmtp(in);
    auto re_parsed = parse_h264(body);
    ASSERT_TRUE(re_parsed.ok()) << re_parsed.error().message();
    EXPECT_EQ(*re_parsed.value().profile_level_id, "42e01f");
    EXPECT_EQ(*re_parsed.value().packetization_mode,
              PacketizationMode::kNonInterleaved);
    EXPECT_EQ(*re_parsed.value().max_fs, 1200u);
}

TEST(H264Fmtp, BuildEmpty) {
    H264Fmtp empty;
    EXPECT_EQ(build_h264_fmtp(empty), "");
    EXPECT_FALSE(empty.has_any());
}

TEST(H264Fmtp, LevelAsymmetryAllowedBooleanTrue) {
    auto r = parse_h264("level-asymmetry-allowed");
    ASSERT_TRUE(r.ok());
    ASSERT_TRUE(r.value().level_asymmetry_allowed.has_value());
    EXPECT_TRUE(*r.value().level_asymmetry_allowed);
}

TEST(H264Fmtp, LevelAsymmetryAllowedBooleanFalse) {
    auto r = parse_h264("level-asymmetry-allowed=0");
    ASSERT_TRUE(r.ok());
    ASSERT_TRUE(r.value().level_asymmetry_allowed.has_value());
    EXPECT_FALSE(*r.value().level_asymmetry_allowed);
}

TEST(H264Fmtp, UnknownKeysIgnored) {
    // Permissive parse.
    auto r = parse_h264("max-fs=1200;unknown-key=hello");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(*r.value().max_fs, 1200u);
}

// -----------------------------------------------------------------------------
// VP8 parse / build
// -----------------------------------------------------------------------------

TEST(Vp8Fmtp, ParseMaxFrAndMaxFs) {
    auto r = parse_vp8("max-fr=15;max-fs=1200");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(*r.value().max_fr, 15u);
    EXPECT_EQ(*r.value().max_fs, 1200u);
}

TEST(Vp8Fmtp, BuildRoundtrip) {
    Vp8Fmtp in;
    in.max_fr = 30;
    in.max_fs = 3600;
    auto body = build_vp8_fmtp(in);
    auto re_parsed = parse_vp8(body);
    ASSERT_TRUE(re_parsed.ok());
    EXPECT_EQ(*re_parsed.value().max_fr, 30u);
    EXPECT_EQ(*re_parsed.value().max_fs, 3600u);
}

TEST(Vp8Fmtp, EmptyConfig) {
    Vp8Fmtp empty;
    EXPECT_EQ(build_vp8_fmtp(empty), "");
}

// -----------------------------------------------------------------------------
// VP9 parse / build
// -----------------------------------------------------------------------------

TEST(Vp9Fmtp, ParseProfileId) {
    auto r = parse_vp9("profile-id=0");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(*r.value().profile_id, 0u);
}

TEST(Vp9Fmtp, RejectInvalidProfileId) {
    auto r = parse_vp9("profile-id=9");
    EXPECT_FALSE(r.ok());
}

TEST(Vp9Fmtp, BuildRoundtrip) {
    Vp9Fmtp in;
    in.profile_id = 2;
    in.max_fr     = 30;
    in.max_fs     = 1200;
    in.max_br     = 1500;
    auto body = build_vp9_fmtp(in);
    auto re_parsed = parse_vp9(body);
    ASSERT_TRUE(re_parsed.ok());
    EXPECT_EQ(*re_parsed.value().profile_id, 2u);
    EXPECT_EQ(*re_parsed.value().max_fr, 30u);
    EXPECT_EQ(*re_parsed.value().max_fs, 1200u);
    EXPECT_EQ(*re_parsed.value().max_br, 1500u);
}

// -----------------------------------------------------------------------------
// parse_fmtp dispatcher
// -----------------------------------------------------------------------------

TEST(ParseFmtp, DispatchH264) {
    auto r = parse_fmtp(CodecKind::kH264,
                        "packetization-mode=1;profile-level-id=42e01f");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().kind, CodecKind::kH264);
    EXPECT_EQ(*r.value().h264.profile_level_id, "42e01f");
}

TEST(ParseFmtp, DispatchVP8) {
    auto r = parse_fmtp(CodecKind::kVP8, "max-fr=30");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().kind, CodecKind::kVP8);
}

TEST(ParseFmtp, DispatchUnsupported) {
    auto r = parse_fmtp(CodecKind::kAV1, "foo=bar");
    EXPECT_FALSE(r.ok());
}

TEST(BuildFmtp, DispatchByKind) {
    H264Fmtp h;
    h.profile_level_id = "42e01f";
    ParsedFmtp p;
    p.kind = CodecKind::kH264;
    p.h264 = h;
    auto body = build_fmtp(p);
    auto re = parse_h264(body);
    ASSERT_TRUE(re.ok());
    EXPECT_EQ(*re.value().profile_level_id, "42e01f");
}
