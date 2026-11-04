/**
 * @file src/modules/video_jb/src/video_jitter_buffer.cpp
 * @brief Video-aware jitter buffer implementation.
 *
 * Strategy:
 *   - One JB instance per (SSRC, payload_type) tuple.
 *   - Packets grouped by `rtp_timestamp` (one frame per timestamp).
 *   - Per-frame: track first_seq, last_seq, expected_count, received_count.
 *   - When received_count == expected_count OR marker bit seen AND all
 *     in-order packets received → emit frame.
 *   - When first_seq's arrival_us is older than max_wait_ms → force-emit
 *     (even if incomplete), tagging as `complete=false`.
 *
 * FU-A reassembly:
 *   - When a FU-A start fragment arrives, we accumulate subsequent
 *     continuation fragments (matched by SSRC + rtp_timestamp + non-start
 *     indicator) until the end fragment is seen OR timeout.
 *   - We then concatenate the fragments into a single NAL unit and
 *     insert it as a "synthesized single NAL packet" into the same
 *     per-frame bucket.
 *
 * Adaptive playout (v1.3 Q2):
 *   - The jitter estimator and the target-delay policy live once in
 *     nimrtc::jb_strategy (shared with the audio front-end).  This file
 *     only feeds it (arrival time, media clock) once per frame and
 *     applies the resulting target delay as a playout deadline.
 */

#include <nimrtc/video_jb/video_jitter_buffer.hpp>

#include <nimrtc/jb_strategy/adaptive_delay.hpp>

#include <nimrtc/video_payload/h264.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace nimrtc::video_jb {

namespace {

using video_payload::h264::FuAHeader;
using video_payload::h264::NaluType;
using video_payload::h264::parse_fu_a_header;

// kRtpSeqMod / kRtpTsMod are reserved for future sequence/timestamp
// wrap-around arithmetic; suppress -Werror=unused-const-variable on
// Clang so the symbols stay available.
#if defined(__clang__)
[[maybe_unused]]
#endif
constexpr std::uint32_t kRtpSeqMod = 65536u;
#if defined(__clang__)
[[maybe_unused]]
#endif
constexpr std::uint64_t kRtpTsMod  = 0x100000000ull;

// Video RTP clock rate (Hz). v1.3 Q2 decision: fixed 90000 — the video
// convention (90 kHz for H.264/VP8/VP9). Used to convert RTP timestamps
// to microseconds for the shared RFC 3550 §6.4.1 estimator.
constexpr std::uint32_t kVideoClockRateHz = 90000;

// Per-mode playout defaults. Zero Config fields fall back to these so
// existing `Config{mode}` constructions keep their behaviour while new
// code can override individual bounds (additive).
struct ModeDefaults {
    core::Milliseconds base_delay;
    core::Milliseconds min_delay;
    core::Milliseconds max_delay;
};

inline ModeDefaults mode_defaults(Mode mode) noexcept {
    switch (mode) {
        case Mode::kLive:
            return {core::Milliseconds{50}, core::Milliseconds{20},
                    core::Milliseconds{400}};
        case Mode::kInteractive:
            return {core::Milliseconds{80}, core::Milliseconds{50},
                    core::Milliseconds{800}};
        case Mode::kLowLatency:
        default:
            return {core::Milliseconds{20}, core::Milliseconds{20},
                    core::Milliseconds{200}};
    }
}

/** Signed 32-bit timestamp distance (handles wrap-around). */
inline std::int64_t ts_delta(std::uint32_t newer, std::uint32_t older) noexcept {
    return static_cast<std::int64_t>(static_cast<std::int32_t>(newer - older));
}

// ---------------------------------------------------------------------------
// Per-frame state
// ---------------------------------------------------------------------------

struct FrameBucket {
    std::uint32_t ssrc          = 0;
    std::uint32_t rtp_timestamp = 0;
    std::uint16_t first_seq     = 0;
    std::uint16_t last_seq      = 0;
    std::int64_t  first_arrival_us = 0;
    std::int64_t  last_arrival_us  = 0;
    video_frame::CodecKind codec = video_frame::CodecKind::kUnknown;

    /** Concatenated bitstream (NAL units for H.264; partition 0 for VP8). */
    std::vector<std::uint8_t> bitstream;

    /** Sequence numbers we've received (for dedup + NACK detection). */
    std::vector<std::uint16_t> received_seqs;

    /** Whether the marker bit was seen on any packet of this frame. */
    bool marker_seen = false;

    /** Whether the frame was already emitted. */
    bool emitted = false;

    /** FU-A accumulation state — only used while FU-A is being assembled. */
    bool          fu_a_in_progress = false;
    NaluType      fu_a_nalu_type   = NaluType::kUnspecified;
    std::uint8_t  fu_a_nri         = 0;
    std::vector<std::uint8_t> fu_a_buf;
    std::uint16_t fu_a_start_seq   = 0;
};

// ---------------------------------------------------------------------------
// Seq distance helper (handles 16-bit wrap-around)
// ---------------------------------------------------------------------------

inline std::int32_t seq_delta(std::uint16_t newer, std::uint16_t older) noexcept {
    std::int32_t d = static_cast<std::int32_t>(newer) - static_cast<std::int32_t>(older);
    if (d > 32767)  d -= 65536;
    if (d < -32768) d += 65536;
    return d;
}

// ---------------------------------------------------------------------------
// Detect if a payload is a FU-A fragment
// ---------------------------------------------------------------------------

inline bool is_fu_a(core::ByteSpan payload) noexcept {
    if (payload.empty()) return false;
    const std::uint8_t type = payload[0] & 0x1F;
    return type == 28;
}

// ---------------------------------------------------------------------------
// Reassemble a FU-A fragment into the frame's bitstream
// ---------------------------------------------------------------------------

inline void append_fu_a_fragment(FrameBucket& bucket,
                                 core::ByteSpan payload) noexcept {
    FuAHeader h;
    if (!parse_fu_a_header(payload, h)) return;
    const bool start = (h.fu_header & 0x80) != 0;
    const bool end   = (h.fu_header & 0x40) != 0;
    const std::uint8_t inner_type = h.fu_header & 0x1F;
    const std::uint8_t nri        = (h.fu_indicator & 0x60) >> 5;

    if (start) {
        bucket.fu_a_in_progress = true;
        bucket.fu_a_nalu_type   = static_cast<NaluType>(inner_type);
        bucket.fu_a_nri         = nri;
        bucket.fu_a_buf.clear();
        bucket.fu_a_start_seq   = 0;        // set by caller
    }
    if (!bucket.fu_a_in_progress) {
        // Stray continuation without a start — drop.
        return;
    }

    bucket.fu_a_buf.insert(bucket.fu_a_buf.end(),
                           h.fragment.begin(), h.fragment.end());

    if (end) {
        // Synthesize the original NAL header and append to frame bitstream.
        std::uint8_t nalu_hdr = static_cast<std::uint8_t>(
            (bucket.fu_a_nri << 5) | (static_cast<std::uint8_t>(bucket.fu_a_nalu_type) & 0x1F));
        bucket.bitstream.push_back(nalu_hdr);
        bucket.bitstream.insert(bucket.bitstream.end(),
                                bucket.fu_a_buf.begin(),
                                bucket.fu_a_buf.end());
        bucket.fu_a_in_progress = false;
    }
}

inline bool is_keyframe_payload(video_frame::CodecKind codec,
                                core::ByteSpan payload) noexcept {
    switch (codec) {
        case video_frame::CodecKind::kH264: {
            if (payload.empty()) return false;
            const std::uint8_t type = payload[0] & 0x1F;
            return type == 5 || type == 7 || type == 8;   // IDR / SPS / PPS
        }
        case video_frame::CodecKind::kVP8:
            // RFC 7741 §4.2: the P bit in the first byte of a partition is
            // set to 0 for key frames (intra) and 1 for inter-frames.
            // payload[0] & 0x01 == 0 → keyframe.
            return !payload.empty() && (payload[0] & 0x01) == 0;
        case video_frame::CodecKind::kVP9:
            // For VP9 we approximate via the picture-id presence: a non-empty
            // bitstream with D=0 in the descriptor is intra/key. Without
            // descriptor parse, we return false (let the receiver decide).
            return false;
        default:
            return false;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// VideoJitterBuffer — concrete impl
// ---------------------------------------------------------------------------

class VideoJitterBuffer final : public IVideoJitterBuffer {
public:
    explicit VideoJitterBuffer(Config cfg) : cfg_(cfg) {
        resolve_policy_defaults();
        sync_tracker_policy();
    }

    bool insert(const InboundPacket& pkt) noexcept override {
        // We accept empty payload if the marker bit is set — such a packet
        // is used to signal "this is the last packet of the frame" without
        // contributing any payload bytes (e.g. RFC 4585 feedback frames).
        if (pkt.payload.empty() && !pkt.marker) return false;

        stats_.packets_inserted++;

        // Locate or create the bucket for this rtp_timestamp.
        FrameBucket* bucket = find_or_create_bucket(pkt);
        if (!bucket) return false;

        // Track sequence number for NACK / dedup.
        if (!bucket->received_seqs.empty()) {
            const std::int32_t d = seq_delta(pkt.seq, bucket->last_seq);
            if (d <= 0) {
                // Out-of-order or duplicate; we still keep the payload but
                // mark reordered. For full reorder-correctness we'd buffer
                // here; for now we accept in-order or later packets.
                stats_.packets_reordered++;
            }
        }
        bucket->received_seqs.push_back(pkt.seq);
        bucket->last_seq = pkt.seq;
        bucket->last_arrival_us = pkt.arrival_us;
        if (pkt.marker) bucket->marker_seen = true;

        // Append payload (handle FU-A specially).
        if (pkt.codec == video_frame::CodecKind::kH264 && is_fu_a(pkt.payload)) {
            append_fu_a_fragment(*bucket, pkt.payload);
        } else {
            bucket->bitstream.insert(bucket->bitstream.end(),
                                     pkt.payload.begin(), pkt.payload.end());
        }

        // Keyframe detection.
        if (is_keyframe_payload(pkt.codec, pkt.payload)) {
            bucket_pending_keyframe_ = true;
        }
        return true;
    }

    std::unique_ptr<AssembledFrame> pop_frame() noexcept override {
        return pop_ready(/*gate_us=*/-1);
    }

    std::unique_ptr<AssembledFrame> pop_frame(std::int64_t now_us) noexcept override {
        return pop_ready(now_us);
    }

    std::vector<std::uint16_t> pop_nack_entries() noexcept override {
        std::vector<std::uint16_t> nacks;
        if (!cfg_.emit_nacks) return nacks;

        for (auto& [ts, bucket] : buckets_) {
            if (bucket.emitted) continue;
            if (bucket.received_seqs.size() < 2) continue;
            std::sort(bucket.received_seqs.begin(), bucket.received_seqs.end(),
                      [](std::uint16_t a, std::uint16_t b) {
                          // signed distance from bucket->first_seq
                          return static_cast<std::int32_t>(a) <
                                 static_cast<std::int32_t>(b);
                      });
            std::uint16_t expected = bucket.first_seq;
            for (std::uint16_t got : bucket.received_seqs) {
                while (seq_delta(got, expected) > 0) {
                    nacks.push_back(expected);
                    expected = static_cast<std::uint16_t>(expected + 1);
                    stats_.nacks_emitted++;
                }
                if (seq_delta(got, expected) == 0) {
                    expected = static_cast<std::uint16_t>(expected + 1);
                }
            }
        }
        return nacks;
    }

    void tick(std::int64_t now_us) noexcept override {
        // Emit buckets that are old enough.
        for (auto it = buckets_.begin(); it != buckets_.end(); ) {
            FrameBucket& b = it->second;
            if (b.emitted) {
                it = buckets_.erase(it);
                continue;
            }
            const std::int64_t age_ms =
                (now_us - b.first_arrival_us) / 1000;
            // Only frames that never completed are force-emitted.  A
            // complete frame is *not* dropped here: in adaptive mode it
            // may still be waiting for its playout deadline (which can
            // exceed max_wait_ms), and dropping it would lose a frame
            // that is ready to play out.
            const bool complete = b.marker_seen && !b.fu_a_in_progress;
            if (!complete && age_ms >= static_cast<std::int64_t>(cfg_.max_wait_ms)) {
                // Force-emit incomplete frame.
                stats_.frames_incomplete++;
                stats_.frames_emitted++;
                pending_incomplete_.push_back(make_assembled(b));
                b.emitted = true;
                stats_.packets_dropped++;
            }
            ++it;
        }

        // Drain pending incomplete frames.
        if (!pending_incomplete_.empty()) {
            // The first pending frame is emitted via pop_frame() — see below.
            // We stash them so pop_frame() can drain them in order.
        }

        // Hard cap on in-flight frames: drop oldest if exceeded.
        enforce_inflight_cap();
    }

    void reset() noexcept override {
        buckets_.clear();
        pending_incomplete_.clear();
        bucket_pending_keyframe_ = false;
        stats_ = Stats{};
        frame_seq_counter_ = 0;
        last_rtp_ts_.reset();
        prev_media_us_ = 0;
        have_prev_media_us_ = false;
        frame_interval_us_ = 0;
        tracker_.reset();
        sync_tracker_policy();
    }

    Stats stats() const noexcept override {
        Stats out = stats_;
        out.current_delay = current_target();
        out.current_jitter = tracker_.jitter();
        return out;
    }

private:
    // Zero Config delay fields fall back to per-mode defaults (additive:
    // explicit non-zero values win, so existing `Config{mode}` behaviour
    // is preserved while new code can override individual bounds).
    void resolve_policy_defaults() noexcept {
        const ModeDefaults d = mode_defaults(cfg_.mode);
        if (cfg_.base_delay.count() == 0) cfg_.base_delay = d.base_delay;
        if (cfg_.min_delay.count() == 0) cfg_.min_delay = d.min_delay;
        if (cfg_.max_delay.count() == 0) cfg_.max_delay = d.max_delay;
        if (cfg_.jitter_multiplier <= 0.0) cfg_.jitter_multiplier = 4.0;
    }

    void sync_tracker_policy() noexcept {
        jb_strategy::DelayPolicy policy;
        policy.base_delay = cfg_.base_delay;
        policy.min_delay = cfg_.min_delay;
        policy.max_delay = cfg_.max_delay;
        policy.jitter_multiplier = cfg_.jitter_multiplier;
        policy.adaptive = cfg_.adaptive;
        tracker_.set_policy(policy);
    }

    /** The playout delay the shared strategy currently asks for.
     *
     *  In fixed-window mode (Config::adaptive == false) there is no
     *  playout delay at all: a complete frame is emitted as soon as it
     *  is assembled — the behaviour the module had before the adaptive
     *  strategy was introduced.  In adaptive mode the delay follows the
     *  smoothed jitter estimate (base + multiplier · J, clamped to
     *  [min_delay, max_delay]). */
    core::Milliseconds current_target() const noexcept {
        if (!cfg_.adaptive) return core::Milliseconds{};
        return tracker_.target_delay();
    }

    // Shared pop path. `gate_us < 0` means immediate (legacy behaviour):
    // emit the first complete frame. Otherwise only emit a complete frame
    // whose playout time (first arrival + shared target delay) is due.
    // Forced-incomplete frames from tick() always drain immediately.
    //
    // A popped bucket is *marked* emitted but stays in the map until the
    // next tick() reaps it.  A late packet of an already-played frame (a
    // NACK retransmission, typically) must land in that bucket and be
    // discarded there; erasing it on pop would let the late packet create
    // a fresh bucket, which would then be delivered a second time under
    // the same RTP timestamp.
    std::unique_ptr<AssembledFrame> pop_ready(std::int64_t gate_us) noexcept {
        if (!pending_incomplete_.empty()) {
            auto out = std::move(pending_incomplete_.front());
            pending_incomplete_.erase(pending_incomplete_.begin());
            return out;
        }
        // The gate only applies in adaptive mode; in fixed-window mode
        // the frame is emitted as soon as it is complete.
        if (gate_us < 0 || !cfg_.adaptive) {
            for (auto& [ts, bucket] : buckets_) {
                if (bucket.emitted) continue;
                if (bucket.fu_a_in_progress) continue;
                if (!bucket.marker_seen) continue;
                bucket.emitted = true;
                stats_.frames_emitted++;
                return make_assembled(bucket);
            }
            return nullptr;
        }
        const std::int64_t target_us =
            static_cast<std::int64_t>(current_target().count()) * 1000LL;
        auto best = buckets_.end();
        for (auto it = buckets_.begin(); it != buckets_.end(); ++it) {
            FrameBucket& bucket = it->second;
            if (bucket.emitted) continue;
            if (bucket.fu_a_in_progress) continue;
            if (!bucket.marker_seen) continue;
            if (bucket.first_arrival_us + target_us > gate_us) continue;
            if (best == buckets_.end()
                || bucket.first_arrival_us < best->second.first_arrival_us) {
                best = it;
            }
        }
        if (best == buckets_.end()) return nullptr;
        best->second.emitted = true;
        stats_.frames_emitted++;
        return make_assembled(best->second);
    }
    FrameBucket* find_or_create_bucket(const InboundPacket& pkt) noexcept {
        const auto it = buckets_.find(pkt.rtp_timestamp);
        if (it != buckets_.end()) return &it->second;

        // Make room *before* inserting the new frame, so the frame that
        // was just inserted can never be the one the cap evicts.
        enforce_inflight_cap(/*incoming=*/1);

        FrameBucket b;
        b.ssrc             = pkt.ssrc;
        b.rtp_timestamp    = pkt.rtp_timestamp;
        b.first_seq        = pkt.seq;
        b.last_seq         = pkt.seq;
        b.first_arrival_us = pkt.arrival_us;
        b.last_arrival_us  = pkt.arrival_us;
        b.codec            = pkt.codec;
        // The marker bit belongs to the *frame*, so it is known as soon as
        // the bucket exists — the frame is complete right away.  Setting
        // it here (instead of only after the bucket is handed out) keeps
        // the in-flight cap from evicting a frame that is already
        // complete but still waiting for its playout deadline.
        b.marker_seen      = pkt.marker;
        auto [ins_it, _] = buckets_.emplace(pkt.rtp_timestamp, std::move(b));

        // ---- Shared RFC 3550 §6.4.1 interarrival jitter observation ----
        // One observation per *frame* (not per packet): a video frame —
        // not an RTP packet — is the unit the decoder plays out, and the
        // packets of one frame share a media-clock reading.  Observing
        // per packet would feed the estimator the intra-frame burst
        // spread (media clock frozen, arrival clock advancing), which
        // biases the jitter estimate upwards.
        //
        // Fixed 90 kHz video clock (v1.3 Q2 decision). RTP timestamps wrap
        // at 2^32, so unwrap against the previous observation to keep the
        // media clock monotonic for the tracker.
        observe_frame(pkt);

        const auto ins2 = buckets_.find(pkt.rtp_timestamp);
        return (ins2 == buckets_.end()) ? nullptr : &ins2->second;
    }

    /** Feed the shared jitter estimator with this frame's first packet. */
    void observe_frame(const InboundPacket& pkt) noexcept {
        const std::int64_t rtp_ticks =
            last_rtp_ts_.has_value()
                ? static_cast<std::int64_t>(*last_rtp_ts_)
                  + ts_delta(pkt.rtp_timestamp, *last_rtp_ts_)
                : static_cast<std::int64_t>(pkt.rtp_timestamp);
        last_rtp_ts_ = pkt.rtp_timestamp;

        const std::int64_t media_clock_us =
            (rtp_ticks * 1'000'000LL)
            / static_cast<std::int64_t>(kVideoClockRateHz);
        tracker_.observe(pkt.arrival_us, media_clock_us);

        // Frame interval estimate (media clock): used to size the
        // in-flight window in adaptive mode (a frame is held for up to
        // max_delay before it is released, so the buffer must be able to
        // hold that many frames).
        if (have_prev_media_us_) {
            const std::int64_t delta = media_clock_us - prev_media_us_;
            if (delta > 0) {
                frame_interval_us_ = (frame_interval_us_ > 0)
                    ? (frame_interval_us_ * 3 + delta) / 4
                    : delta;
            }
        }
        prev_media_us_ = media_clock_us;
        have_prev_media_us_ = true;
    }

    /** Effective in-flight frame cap.
     *
     *  In fixed-window mode this is the configured cap.  In adaptive
     *  mode a frame is held for up to `max_delay` before it is released,
     *  so the cap has to grow with the delay — otherwise the frames
     *  that are about to be played out get evicted by the very delay
     *  they are waiting for. */
    std::size_t effective_inflight_cap() const noexcept {
        std::size_t cap = cfg_.max_inflight_frames;
        if (!cfg_.adaptive || frame_interval_us_ <= 0) return cap;
        const std::int64_t max_delay_us =
            static_cast<std::int64_t>(cfg_.max_delay.count()) * 1000LL;
        const std::int64_t needed = max_delay_us / frame_interval_us_ + 2;
        if (needed > static_cast<std::int64_t>(cap)) {
            cap = static_cast<std::size_t>(needed);
        }
        return cap;
    }

    /** Drop frames until the in-flight cap is respected.
     *
     *  `incoming` is the number of frames that are about to be added, so
     *  the caller can make room *before* inserting instead of evicting
     *  the frame it just inserted.
     *
     *  Only frames that have not been delivered yet count towards the
     *  cap: a frame that was already handed out is kept in the map until
     *  the next tick() reaps it (so a late retransmission of it lands in
     *  that bucket and is discarded), but it does not occupy a slot.
     *
     *  A frame that never completed is dropped first — a complete frame
     *  that is only waiting for its playout deadline carries no loss. */
    void enforce_inflight_cap(std::size_t incoming = 0) noexcept {
        const std::size_t cap = effective_inflight_cap();
        for (;;) {
            std::size_t pending = 0;
            for (const auto& [ts, b] : buckets_) {
                if (!b.emitted) ++pending;
            }
            if (pending + incoming <= cap) return;

            // Prefer a frame that never completed; fall back to the
            // oldest pending frame.
            auto victim = buckets_.end();
            for (auto it = buckets_.begin(); it != buckets_.end(); ++it) {
                if (it->second.emitted) continue;
                if (!it->second.marker_seen || it->second.fu_a_in_progress) {
                    victim = it;
                    break;
                }
            }
            if (victim == buckets_.end()) {
                for (auto it = buckets_.begin(); it != buckets_.end(); ++it) {
                    if (it->second.emitted) continue;
                    victim = it;
                    break;
                }
            }
            if (victim == buckets_.end()) return;   // nothing pending left
            if (!victim->second.marker_seen || victim->second.fu_a_in_progress) {
                stats_.frames_incomplete++;
            }
            victim->second.emitted = true;
            stats_.packets_dropped++;
            buckets_.erase(victim);
        }
    }

    std::unique_ptr<AssembledFrame> make_assembled(FrameBucket& b) noexcept {
        auto out = std::make_unique<AssembledFrame>();
        out->ssrc          = b.ssrc;
        out->rtp_timestamp = b.rtp_timestamp;
        out->frame_seq     = ++frame_seq_counter_;
        out->capture_ts_us = static_cast<std::uint64_t>(b.first_arrival_us);   // simplified; real impl
                                                    // uses RTCP-SR NTP↔RTP
        out->bitstream     = std::move(b.bitstream);
        out->complete      = b.marker_seen;
        out->codec         = b.codec;
        // Determine is_keyframe based on the codec AND bitstream content;
        // the bit pattern alone is ambiguous (H.264 NRI bit 0 and VP8
        // keyframe bit 0 collide on the same byte).
        out->is_keyframe = is_keyframe_payload(b.codec, out->bitstream);
        return out;
    }

    Config                                cfg_;
    std::map<std::uint32_t, FrameBucket>  buckets_;
    std::vector<std::unique_ptr<AssembledFrame>> pending_incomplete_;
    bool                                   bucket_pending_keyframe_ = false;
    std::uint32_t                          frame_seq_counter_       = 0;
    Stats                                  stats_{};

    // Shared RFC 3550 §6.4.1 jitter estimator + target-delay policy
    // (v1.3 Q2: single implementation, audio/video dual front-ends).
    // Last RTP timestamp (raw 32-bit) for wrap-aware unwrapping.
    jb_strategy::AdaptiveDelayTracker tracker_;
    std::optional<std::uint32_t> last_rtp_ts_;

    // Media-clock bookkeeping for the frame-interval estimate.
    std::int64_t prev_media_us_      = 0;
    bool         have_prev_media_us_ = false;
    std::int64_t frame_interval_us_  = 0;
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

std::unique_ptr<IVideoJitterBuffer> create(Config cfg) {
    return std::make_unique<VideoJitterBuffer>(cfg);
}

} // namespace nimrtc::video_jb
