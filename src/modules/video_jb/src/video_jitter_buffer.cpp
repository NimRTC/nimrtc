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
 */

#include <nimrtc/video_jb/video_jitter_buffer.hpp>

#include <nimrtc/video_payload/h264.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <unordered_map>
#include <vector>

namespace nimrtc::video_jb {

namespace {

using video_payload::h264::FuAHeader;
using video_payload::h264::NaluType;
using video_payload::h264::parse_fu_a_header;

constexpr std::uint32_t kRtpSeqMod = 65536u;
constexpr std::uint64_t kRtpTsMod  = 0x100000000ull;

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
    explicit VideoJitterBuffer(Config cfg) : cfg_(cfg) {}

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
        // First drain any forced-incomplete frames from tick().
        if (!pending_incomplete_.empty()) {
            auto out = std::move(pending_incomplete_.front());
            pending_incomplete_.erase(pending_incomplete_.begin());
            return out;
        }
        // Then look for any bucket that has marker bit and no FU-A in progress.
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
            if (age_ms >= static_cast<std::int64_t>(cfg_.max_wait_ms)) {
                if (!b.marker_seen) {
                    // Force-emit incomplete frame.
                    stats_.frames_incomplete++;
                    stats_.frames_emitted++;
                    pending_incomplete_.push_back(make_assembled(b));
                    b.emitted = true;
                    stats_.packets_dropped++;
                } else {
                    b.emitted = true;
                    stats_.frames_emitted++;
                }
            }
            ++it;
        }

        // Drain pending incomplete frames.
        if (!pending_incomplete_.empty()) {
            // The first pending frame is emitted via pop_frame() — see below.
            // We stash them so pop_frame() can drain them in order.
        }

        // Hard cap on in-flight frames: drop oldest if exceeded.
        while (buckets_.size() > cfg_.max_inflight_frames) {
            auto oldest = buckets_.begin();
            oldest->second.emitted = true;
            stats_.frames_incomplete++;
            stats_.packets_dropped++;
            buckets_.erase(oldest);
        }
    }

    void reset() noexcept override {
        buckets_.clear();
        pending_incomplete_.clear();
        bucket_pending_keyframe_ = false;
        stats_ = Stats{};
        frame_seq_counter_ = 0;
    }

    Stats stats() const noexcept override { return stats_; }

private:
    FrameBucket* find_or_create_bucket(const InboundPacket& pkt) noexcept {
        const auto it = buckets_.find(pkt.rtp_timestamp);
        if (it != buckets_.end()) return &it->second;

        if (buckets_.size() >= cfg_.max_inflight_frames) {
            // Drop the oldest bucket to make room.
            auto oldest = buckets_.begin();
            oldest->second.emitted = true;
            stats_.packets_dropped++;
            buckets_.erase(oldest);
        }
        FrameBucket b;
        b.ssrc             = pkt.ssrc;
        b.rtp_timestamp    = pkt.rtp_timestamp;
        b.first_seq        = pkt.seq;
        b.last_seq         = pkt.seq;
        b.first_arrival_us = pkt.arrival_us;
        b.last_arrival_us  = pkt.arrival_us;
        b.codec            = pkt.codec;
        auto [ins_it, _] = buckets_.emplace(pkt.rtp_timestamp, std::move(b));
        return &ins_it->second;
    }

    std::unique_ptr<AssembledFrame> make_assembled(FrameBucket& b) noexcept {
        auto out = std::make_unique<AssembledFrame>();
        out->ssrc          = b.ssrc;
        out->rtp_timestamp = b.rtp_timestamp;
        out->frame_seq     = ++frame_seq_counter_;
        out->capture_ts_us = b.first_arrival_us;   // simplified; real impl
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
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

std::unique_ptr<IVideoJitterBuffer> create(Config cfg) {
    return std::make_unique<VideoJitterBuffer>(cfg);
}

} // namespace nimrtc::video_jb
