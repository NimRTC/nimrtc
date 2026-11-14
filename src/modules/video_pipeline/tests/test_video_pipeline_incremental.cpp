/**
 * @file src/modules/video_pipeline/tests/test_video_pipeline_incremental.cpp
 * @brief Remote-desktop plan B9 (dirty-rect incremental encode) and B14
 *        (the key-frame storm limiter).
 *
 * The tests use **synthetic doubles** — a fake encoder that implements the
 * `plugins::IVideoEncoder` seam — so they need no real codec and no
 * network.
 *
 * ## B14 — the key-frame storm limiter
 *
 *   1. a burst of 10 PLI requests produces exactly ONE key frame,
 *   2. the limiter reports 1 granted and 9 deferred,
 *   3. a request after the minimum interval is granted again,
 *   4. a resolution change is ALWAYS granted immediately and carries the
 *      parameter sets (a deferred parameter-set change breaks the decode),
 *   5. the limiter is wired to the `FirCoalescer` (the mechanism plan
 *      B14 says was "finally wired into the engine").
 *
 * ## B9 — the dirty-rect incremental encode
 *
 *   1. a codec that supports incremental encode codes the requested
 *      region, and the region reaches the peer as a region-refresh
 *      message,
 *   2. a codec that does NOT support it falls back to a full key frame,
 *      and the fallback counter increments,
 *   3. the screen-content gate: a camera stream never engages the
 *      mechanism, even with a codec that supports it.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/core/error.hpp>
#include <nimrtc/plugins/video_codec.hpp>
#include <nimrtc/video_frame/frame.hpp>
#include <nimrtc/video_pipeline/incremental_encode.hpp>
#include <nimrtc/video_pipeline/keyframe_limiter.hpp>
#include <nimrtc/video_pipeline/video_pipeline.hpp>
#include <nimrtc/video_rtcp_feedback/video_rtcp_feedback.hpp>

namespace {

namespace vp   = nimrtc::video_pipeline;
namespace vrf  = nimrtc::video_rtcp_feedback;
namespace core = nimrtc::core;
namespace np   = nimrtc::plugins;

constexpr std::uint32_t kMediaSsrc = 0xAABBCCDDu;
constexpr std::uint32_t kPeerSsrc  = 0x11112222u;
constexpr std::uint8_t  kPt        = 102;

// ===========================================================================
// A synthetic encoder — the codec-seam test double
// ===========================================================================

/**
 * @brief A fake encoder implementing the split codec seam.
 *
 * It records what it was asked to do:
 *   - `set_incremental_request()` — the incremental request (plan B9),
 *   - `force_keyframe()` — the key-frame requests (plan B14),
 *   - `last_encoded_region()` — the region it "coded".
 *
 * `supports_incremental` is the switch that decides between the
 * incremental path and the fallback.
 */
class SyntheticEncoder final : public np::IVideoEncoder {
public:
    explicit SyntheticEncoder(bool supports_incremental = false) noexcept
        : supports_incremental_(supports_incremental) {}

    // ---- plugins::IPlugin --------------------------------------------------
    const char* name() const noexcept override { return "synthetic-encoder"; }
    np::Status open() noexcept override { return np::kOk; }
    void close() noexcept override {}

    // ---- plugins::IVideoEncoder --------------------------------------------
    np::VideoCodecKind kind() const noexcept override {
        return np::VideoCodecKind::kH264;
    }
    std::string_view codec_name() const noexcept override { return "synthetic"; }

    np::Status encode(const np::VideoFrame&, std::uint8_t*,
                       std::size_t, np::EncodedVideoFrame&) noexcept override {
        ++encode_calls_;
        // A codec that supports incremental encode codes the region it was
        // asked for; the bounding box of the requested regions is what it
        // reports back.
        if (supports_incremental_ && request_.is_incremental()) {
            last_region_ = np::VideoRegion{};
            bool first = true;
            for (const auto& r : request_.regions) {
                if (first) {
                    last_region_ = r;
                    first = false;
                    continue;
                }
                const std::uint32_t x0 = std::min(last_region_.x, r.x);
                const std::uint32_t y0 = std::min(last_region_.y, r.y);
                const std::uint32_t x1 = std::max(last_region_.x + last_region_.width,
                                                 r.x + r.width);
                const std::uint32_t y1 = std::max(last_region_.y + last_region_.height,
                                                 r.y + r.height);
                last_region_.x      = x0;
                last_region_.y      = y0;
                last_region_.width  = x1 - x0;
                last_region_.height = y1 - y0;
            }
        }
        return np::kOk;
    }

    np::Status force_keyframe() noexcept override {
        ++force_keyframe_calls_;
        return np::kOk;
    }

    np::Status set_params(const np::VideoEncoderParams&) noexcept override {
        return np::kOk;
    }
    np::VideoEncoderParams params() const noexcept override { return {}; }
    np::VideoCodecStats stats() const noexcept override { return {}; }

    // ---- The incremental-encode seam (plan B9) ----------------------------
    bool supports_incremental_encode() const noexcept override {
        return supports_incremental_;
    }

    np::Status set_incremental_request(
            const np::VideoIncrementalEncodeRequest& request) noexcept override {
        if (!supports_incremental_) return np::kErrUnsupported;
        request_ = request;
        // Only a *meaningful* request counts: the driver pushes an inert
        // request to clear stale state, and that is not an incremental
        // encode.
        if (request.enabled) ++incremental_request_calls_;
        return np::kOk;
    }

    np::VideoIncrementalEncodeRequest incremental_request() const noexcept override {
        return request_;
    }

    np::VideoRegion last_encoded_region() const noexcept override {
        return last_region_;
    }

    // ---- The test's observations -----------------------------------------
    int force_keyframe_calls() const noexcept { return force_keyframe_calls_; }
    int encode_calls() const noexcept { return encode_calls_; }
    int incremental_request_calls() const noexcept {
        return incremental_request_calls_;
    }

private:
    bool supports_incremental_;
    np::VideoIncrementalEncodeRequest request_{};
    np::VideoRegion last_region_{};
    int force_keyframe_calls_ = 0;
    int encode_calls_ = 0;
    int incremental_request_calls_ = 0;
};

/** Build a PLI (RFC 4585 §6.3.1) for `media_ssrc`. */
std::vector<std::uint8_t> make_pli(std::uint32_t sender_ssrc,
                                    std::uint32_t media_ssrc) {
    vrf::PliPacket p;
    p.sender_ssrc = sender_ssrc;
    p.media_ssrc  = media_ssrc;
    std::vector<std::uint8_t> buf(vrf::kPliPacketSize, 0);
    const std::size_t n = vrf::build_pli(
        core::MutableByteSpan{buf.data(), buf.size()}, p);
    buf.resize(n);
    return buf;
}

np::VideoRegion region(std::uint32_t x, std::uint32_t y,
                        std::uint32_t w, std::uint32_t h) {
    np::VideoRegion r;
    r.x = x; r.y = y; r.width = w; r.height = h;
    return r;
}

} // namespace

// ===========================================================================
// B14 — the key-frame storm limiter
// ===========================================================================

TEST(KeyframeLimiter, ABurstOfPliRequestsProducesExactlyOneKeyframe) {
    // The plan's acceptance criterion: a sustained key-frame-request
    // storm produces a bounded IDR rate — not one IDR per request.
    vp::SenderConfig sc;
    sc.codec        = nimrtc::video_frame::CodecKind::kH264;
    sc.ssrc         = kMediaSsrc;
    sc.payload_type = kPt;

    vp::VideoSender tx(sc);

    // Ten PLIs, 10 ms apart — a storm well inside the 250 ms minimum
    // interval the FIR coalescer enforces.
    const auto pli = make_pli(kPeerSsrc, kMediaSsrc);
    for (int i = 0; i < 10; ++i) {
        const std::int64_t now_us = static_cast<std::int64_t>(i) * 10'000;
        auto applied = vp::apply_feedback_rtcp(
            tx, core::ByteSpan{pli.data(), pli.size()}, now_us);
        ASSERT_TRUE(applied.ok()) << applied.error().message();
    }

    const vp::KeyframeLimiterStats st = tx.keyframe_limiter_stats();
    EXPECT_EQ(st.requests, 10u);
    EXPECT_EQ(st.granted, 1u) << "a burst of 10 PLIs must produce ONE key frame";
    EXPECT_EQ(st.deferred, 9u);
    EXPECT_EQ(st.rejected, 0u);

    // The sender armed the encoder exactly once.
    EXPECT_EQ(tx.stats().force_keyframe_calls, 1u);
    EXPECT_TRUE(tx.keyframe_pending());

    // The sender's own counters mirror the limiter's.
    EXPECT_EQ(tx.stats().keyframe_requests, 10u);
    EXPECT_EQ(tx.stats().keyframes_granted, 1u);
    EXPECT_EQ(tx.stats().keyframes_deferred, 9u);
}

TEST(KeyframeLimiter, ASlowPliStormIsStillRateLimited) {
    // The plan's wording: "a peer that requests a keyframe every 100 ms
    // produces at most the rate-limited number of IDRs".  The default
    // policy is 4 Hz (250 ms), so a 1-second storm of 100 ms PLIs yields
    // at most 4 IDRs — not 10.
    vp::SenderConfig sc;
    sc.codec        = nimrtc::video_frame::CodecKind::kH264;
    sc.ssrc         = kMediaSsrc;
    sc.payload_type = kPt;

    vp::VideoSender tx(sc);
    const auto pli = make_pli(kPeerSsrc, kMediaSsrc);

    for (int i = 0; i < 10; ++i) {
        const std::int64_t now_us = static_cast<std::int64_t>(i) * 100'000;
        auto applied = vp::apply_feedback_rtcp(
            tx, core::ByteSpan{pli.data(), pli.size()}, now_us);
        ASSERT_TRUE(applied.ok()) << applied.error().message();
    }

    const vp::KeyframeLimiterStats st = tx.keyframe_limiter_stats();
    EXPECT_EQ(st.requests, 10u);
    EXPECT_LE(st.granted, 4u) << "the IDR rate must be rate limited";
    EXPECT_GT(st.deferred, 0u);
    EXPECT_EQ(st.granted + st.deferred + st.rejected, 10u);
}

TEST(KeyframeLimiter, ARequestAfterTheMinimumIntervalIsGrantedAgain) {
    vp::SenderConfig sc;
    sc.codec        = nimrtc::video_frame::CodecKind::kH264;
    sc.ssrc         = kMediaSsrc;
    sc.payload_type = kPt;

    vp::VideoSender tx(sc);
    const auto pli = make_pli(kPeerSsrc, kMediaSsrc);

    auto applied = vp::apply_feedback_rtcp(
        tx, core::ByteSpan{pli.data(), pli.size()}, /*now_us=*/0);
    ASSERT_TRUE(applied.ok());

    // Still inside the minimum interval (250 ms) -> deferred.
    applied = vp::apply_feedback_rtcp(
        tx, core::ByteSpan{pli.data(), pli.size()}, /*now_us=*/100'000);
    ASSERT_TRUE(applied.ok());
    EXPECT_EQ(tx.keyframe_limiter_stats().granted, 1u);
    EXPECT_EQ(tx.keyframe_limiter_stats().deferred, 1u);

    // Past the minimum interval -> granted again.
    applied = vp::apply_feedback_rtcp(
        tx, core::ByteSpan{pli.data(), pli.size()}, /*now_us=*/300'000);
    ASSERT_TRUE(applied.ok());
    EXPECT_EQ(tx.keyframe_limiter_stats().granted, 2u);
    EXPECT_EQ(tx.keyframe_limiter_stats().deferred, 1u);
    EXPECT_EQ(tx.stats().force_keyframe_calls, 2u);
}

TEST(KeyframeLimiter, AResolutionChangeIsAlwaysGrantedAndCarriesTheParameterSets) {
    // A resolution change forces a key frame *and* the parameter sets
    // (plan B13).  Deferring it breaks the decode: the peer receives a
    // frame whose geometry its parameter sets do not describe.
    vp::KeyframeLimiterConfig cfg;
    cfg.min_interval_us = 250'000;
    cfg.media_ssrc      = kMediaSsrc;

    vp::KeyframeLimiter limiter(cfg);

    // The first request is granted.
    EXPECT_EQ(limiter.request(vp::KeyframeReason::kPli, 0),
              vp::KeyframeDecision::kGranted);

    // A resolution change one millisecond later: still granted, and the
    // limiter says the parameter sets ride along.
    const vp::KeyframeDecision d = limiter.request(
        vp::KeyframeReason::kResolutionChange, 1'000,
        /*carries_parameter_sets=*/true);
    EXPECT_EQ(d, vp::KeyframeDecision::kGranted)
        << "a parameter-set change must never be deferred";
    EXPECT_EQ(limiter.last_reason(), vp::KeyframeDecisionReason::kResolutionChange);
    EXPECT_TRUE(limiter.last_carries_parameter_sets());

    // And the same request, with the flag alone, is also never deferred.
    const vp::KeyframeDecision d2 = limiter.request(
        vp::KeyframeReason::kPli, 2'000, /*carries_parameter_sets=*/true);
    EXPECT_EQ(d2, vp::KeyframeDecision::kGranted)
        << "a request that carries the parameter sets must never be deferred";

    // The counters separate "granted by the limiter" from "bypassed the
    // limiter because it was mandatory".
    const vp::KeyframeLimiterStats st = limiter.stats();
    EXPECT_EQ(st.requests, 3u);
    EXPECT_EQ(st.granted, 3u);
    EXPECT_EQ(st.deferred, 0u);
    EXPECT_EQ(st.forced_grants, 2u);
}

TEST(KeyframeLimiter, AResolutionChangeIsGrantedEvenInsideTheHardCap) {
    // The hard cap bounds the *voluntary* key frames.  A resolution
    // change is not voluntary: it must go through even when the cap is
    // exhausted, or the stream breaks.
    vp::KeyframeLimiterConfig cfg;
    cfg.min_interval_us           = 0;      // no interval gate
    cfg.max_keyframes_per_window  = 1;
    cfg.keyframe_budget           = 0;      // no cost gate
    cfg.window_us                 = 1'000'000;
    cfg.media_ssrc                = kMediaSsrc;

    vp::KeyframeLimiter limiter(cfg);

    EXPECT_EQ(limiter.request(vp::KeyframeReason::kPli, 0),
              vp::KeyframeDecision::kGranted);
    // The cap is now reached.
    EXPECT_EQ(limiter.request(vp::KeyframeReason::kPli, 1'000),
              vp::KeyframeDecision::kRejected);
    // The resolution change goes through anyway.
    EXPECT_EQ(limiter.request(vp::KeyframeReason::kResolutionChange, 2'000),
              vp::KeyframeDecision::kGranted);
}

TEST(KeyframeLimiter, TheCostBudgetBoundsTheKeyframeRate) {
    // The key-frame *cost* budget is the encoder-side counterpart of the
    // plan's D3 ("keyframe-cost budgeting" on the controller side).
    vp::KeyframeLimiterConfig cfg;
    cfg.min_interval_us          = 0;      // no interval gate
    cfg.max_keyframes_per_window = 0;      // no hard cap
    cfg.keyframe_budget          = 16;     // inter-frame equivalents
    cfg.keyframe_cost_ratio      = 8;      // a key frame costs 8 of them
    cfg.window_us                = 1'000'000;
    cfg.media_ssrc               = kMediaSsrc;

    vp::KeyframeLimiter limiter(cfg);

    // Two key frames fit in the budget (2 * 8 == 16); the third does not.
    EXPECT_EQ(limiter.request(vp::KeyframeReason::kPli, 0),
              vp::KeyframeDecision::kGranted);
    EXPECT_EQ(limiter.request(vp::KeyframeReason::kPli, 1'000),
              vp::KeyframeDecision::kGranted);
    EXPECT_EQ(limiter.request(vp::KeyframeReason::kPli, 2'000),
              vp::KeyframeDecision::kRejected);

    const vp::KeyframeLimiterStats st = limiter.stats();
    EXPECT_EQ(st.granted, 2u);
    EXPECT_EQ(st.rejected, 1u);
    EXPECT_EQ(limiter.last_reason(), vp::KeyframeDecisionReason::kCostBudget);
}

TEST(KeyframeLimiter, TheLimiterIsWiredToTheFirCoalescer) {
    // Plan B14: "the existing `FirCoalescer` is finally wired into the
    // engine".  The limiter must consult the coalescer, not keep a
    // parallel clock of its own.
    vrf::FirCoalescer coalescer(kPeerSsrc);
    vp::KeyframeLimiterConfig cfg;
    cfg.min_interval_us = 250'000;
    cfg.media_ssrc      = kMediaSsrc;

    vp::KeyframeLimiter limiter(coalescer, cfg);
    EXPECT_TRUE(limiter.uses_fir_coalescer());
    EXPECT_EQ(limiter.fir_coalescer(), &coalescer);

    // The limiter and the coalescer agree on the interval.
    EXPECT_EQ(limiter.config().min_interval_us,
              coalescer.min_interval_us());

    EXPECT_EQ(limiter.request(vp::KeyframeReason::kPli, 0),
              vp::KeyframeDecision::kGranted);
    // The coalescer now knows a key frame went out at t=0.
    EXPECT_FALSE(coalescer.should_send(kMediaSsrc, 100'000));
    EXPECT_TRUE(coalescer.should_send(kMediaSsrc, 300'000));

    // The limiter defers, because the coalescer says so.
    EXPECT_EQ(limiter.request(vp::KeyframeReason::kPli, 100'000),
              vp::KeyframeDecision::kDeferred);
    EXPECT_EQ(limiter.last_reason(), vp::KeyframeDecisionReason::kMinInterval);
    EXPECT_EQ(limiter.request(vp::KeyframeReason::kPli, 300'000),
              vp::KeyframeDecision::kGranted);
}

TEST(KeyframeLimiter, ADisabledLimiterGrantsEverything) {
    vp::KeyframeLimiterConfig cfg;
    cfg.disabled = true;
    vp::KeyframeLimiter limiter(cfg);

    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(limiter.request(vp::KeyframeReason::kPli, 0),
                  vp::KeyframeDecision::kGranted);
    }
    EXPECT_EQ(limiter.stats().granted, 10u);
    EXPECT_EQ(limiter.stats().deferred, 0u);
}

// ===========================================================================
// B9 — the dirty-rect incremental encode
// ===========================================================================

TEST(IncrementalEncode, ACodecThatSupportsItCodesTheRequestedRegion) {
    SyntheticEncoder encoder(/*supports_incremental=*/true);

    vp::IncrementalEncodeDriver driver;
    driver.set_encoder(&encoder);
    driver.controller().set_content_type(vp::VideoContentType::kScreen);
    driver.set_media_ssrc(kMediaSsrc);
    driver.set_feedback_ssrc(kPeerSsrc);

    std::vector<std::uint8_t> sent;
    driver.set_region_refresh_callback(
        [&sent](core::ByteSpan bytes) {
            sent.assign(bytes.begin(), bytes.end());
        });

    const std::vector<np::VideoRegion> dirty{region(100, 50, 200, 80)};
    const vp::IncrementalDecision decision =
        driver.on_frame_dirty(dirty, /*full_frame=*/false);

    EXPECT_EQ(decision, vp::IncrementalDecision::kIncremental);

    // The encoder was asked for the region.
    EXPECT_EQ(encoder.incremental_request_calls(), 1);
    const auto& req = encoder.incremental_request();
    EXPECT_TRUE(req.enabled);
    EXPECT_FALSE(req.full_frame);
    EXPECT_TRUE(req.allow_skip_unchanged);
    ASSERT_EQ(req.regions.size(), 1u);
    EXPECT_EQ(req.regions[0].x, 100u);
    EXPECT_EQ(req.regions[0].y, 50u);
    EXPECT_EQ(req.regions[0].width, 200u);
    EXPECT_EQ(req.regions[0].height, 80u);

    // And the region it coded is the region that was requested.
    np::VideoFrame raw{};
    np::EncodedVideoFrame out{};
    ASSERT_EQ(encoder.encode(raw, nullptr, 0, out), np::kOk);
    const np::VideoRegion coded = encoder.last_encoded_region();
    EXPECT_EQ(coded.x, 100u);
    EXPECT_EQ(coded.y, 50u);
    EXPECT_EQ(coded.width, 200u);
    EXPECT_EQ(coded.height, 80u);

    // The region reached the peer.
    ASSERT_EQ(sent.size(), 1u * vrf::region_refresh_size(1));
    auto parsed = vrf::parse_region_refresh(core::ByteSpan{sent.data(), sent.size()});
    ASSERT_TRUE(parsed.ok()) << parsed.error().message();
    EXPECT_EQ(parsed.value().media_ssrc, kMediaSsrc);
    ASSERT_EQ(parsed.value().regions.size(), 1u);
    EXPECT_EQ(parsed.value().regions[0].x, 100u);
    EXPECT_EQ(parsed.value().regions[0].y, 50u);
    EXPECT_EQ(parsed.value().regions[0].width, 200u);
    EXPECT_EQ(parsed.value().regions[0].height, 80u);

    EXPECT_EQ(driver.stats().frames_incremental, 1u);
    EXPECT_EQ(driver.stats().fallback_keyframes, 0u);
    EXPECT_EQ(driver.stats().region_refreshes_sent, 1u);
}

TEST(IncrementalEncode, TheRegionRefreshIsARtcpAppPacket) {
    // The region has to reach the peer over RTCP.  Neither a PLI nor a
    // FIR can carry a rectangle, so the message is an RTCP APP packet
    // with the "NRGN" name (RFC 3550 §6.1).
    vrf::RegionRefreshPacket pkt;
    pkt.sender_ssrc = kPeerSsrc;
    pkt.media_ssrc  = kMediaSsrc;
    pkt.regions.push_back(vrf::RegionRect{10, 20, 30, 40});

    std::vector<std::uint8_t> buf(
        vrf::region_refresh_size(pkt.regions.size()), 0);
    const std::size_t n = vrf::build_region_refresh(
        core::MutableByteSpan{buf.data(), buf.size()}, pkt);
    ASSERT_EQ(n, buf.size());

    // V=2, P=0, subtype=0, PT=204 (APP).
    EXPECT_EQ(buf[0] & 0xC0u, 0x80u);
    EXPECT_EQ(buf[1], vrf::kAppPt);
    EXPECT_TRUE(vrf::is_region_refresh(core::ByteSpan{buf.data(), buf.size()}));

    auto parsed = vrf::parse_region_refresh(core::ByteSpan{buf.data(), buf.size()});
    ASSERT_TRUE(parsed.ok()) << parsed.error().message();
    EXPECT_EQ(parsed.value().sender_ssrc, kPeerSsrc);
    EXPECT_EQ(parsed.value().media_ssrc, kMediaSsrc);
    ASSERT_EQ(parsed.value().regions.size(), 1u);
    EXPECT_EQ(parsed.value().regions[0].x, 10u);
    EXPECT_EQ(parsed.value().regions[0].y, 20u);
    EXPECT_EQ(parsed.value().regions[0].width, 30u);
    EXPECT_EQ(parsed.value().regions[0].height, 40u);

    // An ordinary APP packet is not a region refresh.
    std::vector<std::uint8_t> other = buf;
    other[8] = 'X';
    EXPECT_FALSE(vrf::is_region_refresh(
        core::ByteSpan{other.data(), other.size()}));
}

TEST(IncrementalEncode, ACodecThatCannotExpressItFallsBackToAFullKeyframe) {
    // Plan B9: "the mechanism is disabled when the codec does not
    // support it and the fallback is reported".
    SyntheticEncoder encoder(/*supports_incremental=*/false);

    vp::IncrementalEncodeDriver driver;
    driver.set_encoder(&encoder);
    driver.controller().set_content_type(vp::VideoContentType::kScreen);
    driver.set_media_ssrc(kMediaSsrc);
    driver.set_feedback_ssrc(kPeerSsrc);

    std::vector<std::uint8_t> sent;
    driver.set_region_refresh_callback(
        [&sent](core::ByteSpan bytes) {
            sent.assign(bytes.begin(), bytes.end());
        });

    const std::vector<np::VideoRegion> dirty{region(0, 0, 64, 64)};
    const vp::IncrementalDecision decision =
        driver.on_frame_dirty(dirty, /*full_frame=*/false);

    EXPECT_EQ(decision, vp::IncrementalDecision::kFallbackKeyframe);
    EXPECT_EQ(driver.stats().fallback_keyframes, 1u)
        << "the fallback must be reported";
    EXPECT_EQ(driver.stats().frames_incremental, 0u);

    // The fallback is a full key frame at the encoder.
    EXPECT_EQ(encoder.force_keyframe_calls(), 1);
    // And no region was signalled to the peer (there is nothing to
    // invalidate — the whole frame was coded).
    EXPECT_TRUE(sent.empty());
}

TEST(IncrementalEncode, ACameraStreamNeverEngagesTheMechanism) {
    // The mechanism is a *screen-content* optimisation (plan B9).  A
    // camera stream has no unchanged area worth skipping, so the
    // mechanism must not engage — even with a codec that supports it.
    SyntheticEncoder encoder(/*supports_incremental=*/true);

    vp::IncrementalEncodeDriver driver;
    driver.set_encoder(&encoder);
    driver.controller().set_content_type(vp::VideoContentType::kCamera);
    driver.set_media_ssrc(kMediaSsrc);

    std::vector<std::uint8_t> sent;
    driver.set_region_refresh_callback(
        [&sent](core::ByteSpan bytes) {
            sent.assign(bytes.begin(), bytes.end());
        });

    const std::vector<np::VideoRegion> dirty{region(10, 10, 100, 100)};
    const vp::IncrementalDecision decision =
        driver.on_frame_dirty(dirty, /*full_frame=*/false);

    EXPECT_EQ(decision, vp::IncrementalDecision::kNotScreenContent);
    EXPECT_EQ(driver.stats().frames_incremental, 0u);
    EXPECT_EQ(driver.stats().fallback_keyframes, 0u);
    EXPECT_EQ(driver.stats().skipped_not_screen, 1u);
    EXPECT_EQ(encoder.incremental_request_calls(), 0);
    EXPECT_TRUE(sent.empty());
}

TEST(IncrementalEncode, AStaticDesktopProducesNothingToEncode) {
    // The property plan B9 cares about most: a static desktop produces
    // no region at all, so nothing is encoded and nothing is signalled.
    SyntheticEncoder encoder(/*supports_incremental=*/true);

    vp::IncrementalEncodeDriver driver;
    driver.set_encoder(&encoder);
    driver.controller().set_content_type(vp::VideoContentType::kScreen);
    driver.set_media_ssrc(kMediaSsrc);

    std::vector<std::uint8_t> sent;
    driver.set_region_refresh_callback(
        [&sent](core::ByteSpan bytes) {
            sent.assign(bytes.begin(), bytes.end());
        });

    const std::vector<np::VideoRegion> dirty{};
    const vp::IncrementalDecision decision =
        driver.on_frame_dirty(dirty, /*full_frame=*/false);

    EXPECT_EQ(decision, vp::IncrementalDecision::kNoChange);
    EXPECT_EQ(driver.stats().frames_unchanged, 1u);
    EXPECT_EQ(encoder.incremental_request_calls(), 0);
    EXPECT_TRUE(sent.empty());
}

TEST(IncrementalEncode, AFullFrameNeedsNoIncrementalRequest) {
    SyntheticEncoder encoder(/*supports_incremental=*/true);

    vp::IncrementalEncodeDriver driver;
    driver.set_encoder(&encoder);
    driver.controller().set_content_type(vp::VideoContentType::kScreen);

    const std::vector<np::VideoRegion> dirty{region(0, 0, 10, 10)};
    EXPECT_EQ(driver.on_frame_dirty(dirty, /*full_frame=*/true),
              vp::IncrementalDecision::kFullFrame);
    EXPECT_EQ(driver.stats().frames_full, 1u);
    EXPECT_EQ(encoder.incremental_request_calls(), 0);
}

TEST(IncrementalEncode, TheDefaultCapabilityQueryIsFalse) {
    // A third-party codec that knows nothing about the mechanism must
    // behave exactly as before: the capability query defaults to false.
    class PlainCodec final : public np::IVideoEncoder {
    public:
        const char* name() const noexcept override { return "plain"; }
        np::Status open() noexcept override { return np::kOk; }
        void close() noexcept override {}
        np::VideoCodecKind kind() const noexcept override {
            return np::VideoCodecKind::kH264;
        }
        std::string_view codec_name() const noexcept override { return "plain"; }
        np::Status encode(const np::VideoFrame&, std::uint8_t*, std::size_t,
                           np::EncodedVideoFrame&) noexcept override {
            return np::kOk;
        }
        np::Status force_keyframe() noexcept override { return np::kOk; }
        np::Status set_params(const np::VideoEncoderParams&) noexcept override {
            return np::kOk;
        }
        np::VideoEncoderParams params() const noexcept override { return {}; }
        np::VideoCodecStats stats() const noexcept override { return {}; }
    };

    PlainCodec codec;
    EXPECT_FALSE(codec.supports_incremental_encode());
    EXPECT_EQ(codec.set_incremental_request(
                  np::VideoIncrementalEncodeRequest{}),
              np::kErrUnsupported);
    EXPECT_FALSE(codec.incremental_request().enabled);
}
