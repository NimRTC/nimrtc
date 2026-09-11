/**
 * @file src/modules/video_pipeline/src/video_pipeline.cpp
 * @brief VideoReceiver and VideoSender reference implementations.
 *
 * ## Receiver
 *
 *   push_rtp(packet, now_us):
 *     1. Track seq number (wrap-aware) → detect gaps → fill NACK list.
 *     2. If H.264:
 *        - peek_nalu_type(payload)
 *        - FU-A start : clear/initialise reassembly buffer (NRI, type).
 *        - FU-A middle: append fragment bytes to reassembly buffer.
 *        - FU-A end   : synthesise original NAL header + flush body to JB.
 *        - STAP-A     : extract all NAL units, concatenate, push to JB.
 *        - Single     : push payload directly to JB.
 *     3. If VP8/VP9: push the raw payload to JB (JB concatenates).
 *     4. Drain pop_frame() → invoke user frame_callback.
 *
 *   Sequence-number tracking is wrap-aware (signed distance), and we
 *   only report a gap when the new seq is strictly newer than the
 *   previous (so out-of-order packets don't generate spurious NACKs).
 *
 * ## Sender
 *
 *   push_frame(frame, capture_ts_us, rtp_timestamp, frame_seq):
 *     1. If H.264: parse bitstream into NAL units (Annex B start code or
 *        length-prefixed framing, per frame.nalu_format).
 *     2. For each NAL unit:
 *        - If size > mtu: emit FU-A fragments (start + middle + end).
 *        - Else: try STAP-A aggregation with neighbouring small NALs.
 *          Flush STAP-A when adding another NAL would exceed mtu.
 *        - Else: emit as a single-NAL packet.
 *     3. If VP8/VP9: emit as a single packet (no descriptor — minimal
 *        reference impl; the descriptor would be added by an interop-
 *        aware variant).
 *     4. For each emitted packet: assign seq, fire packet_callback.
 *
 *   `force_keyframe()` flips a one-shot flag.  The next push_frame()
 *   records it in `frames_marked_as_keyframe` and clears the flag.  The
 *   caller is responsible for emitting an actual keyframe at that point.
 */

#include <nimrtc/video_pipeline/video_pipeline.hpp>

#include <nimrtc/video_payload/h264.hpp>
#include <nimrtc/video_payload/vp8.hpp>
#include <nimrtc/video_payload/vp9.hpp>

#include <nimrtc/core/log.hpp>

#include <algorithm>
#include <cstring>
#include <unordered_set>
#include <vector>

namespace nimrtc::video_pipeline {

namespace {

using video_payload::h264::NaluType;
using video_payload::h264::build_fu_a;
using video_payload::h264::build_stap_a;
using video_payload::h264::FuAHeader;
using video_payload::h264::parse_fu_a_header;
using video_payload::h264::parse_stap_a;
using video_payload::h264::peek_nalu_type;

constexpr std::uint8_t kNaluTypeMask = 0x1F;
constexpr std::uint8_t kNriMask      = 0x60;
constexpr std::uint8_t kFuAType      = 28;
constexpr std::uint8_t kStapAType    = 24;

// ---------------------------------------------------------------------------
// Seq distance helper (16-bit wrap-around, signed).
// ---------------------------------------------------------------------------

inline std::int32_t seq_delta(std::uint16_t newer, std::uint16_t older) noexcept {
    std::int32_t d = static_cast<std::int32_t>(newer) -
                     static_cast<std::int32_t>(older);
    if (d >  32767) d -= 65536;
    if (d < -32768) d += 65536;
    return d;
}

// ---------------------------------------------------------------------------
// Annex B NALU parser (for the sender).
// ---------------------------------------------------------------------------

/** Extract NAL units from an Annex B bitstream.
 *  Accepts 3-byte (00 00 01) and 4-byte (00 00 00 01) start codes.
 *  Each returned vector includes the 1-byte NAL header (no start code). */
std::vector<std::vector<std::uint8_t>> parse_annex_b(core::ByteSpan bs) noexcept {
    std::vector<std::vector<std::uint8_t>> out;
    if (bs.empty()) return out;

    auto find_next_start = [&](std::size_t from) -> std::size_t {
        for (std::size_t j = from; j + 2 < bs.size(); ++j) {
            if (bs[j] == 0 && bs[j + 1] == 0 && bs[j + 2] == 1) return j;
        }
        return bs.size();
    };
    auto find_next_start4 = [&](std::size_t from) -> std::size_t {
        for (std::size_t j = from; j + 3 < bs.size(); ++j) {
            if (bs[j] == 0 && bs[j + 1] == 0 && bs[j + 2] == 0 && bs[j + 3] == 1)
                return j;
        }
        return bs.size();
    };

    // Find first start code.
    std::size_t start = find_next_start4(0);
    if (start == bs.size()) start = find_next_start(0);
    if (start == bs.size()) return out;

    // Skip the start code prefix (3 or 4 bytes).
    std::size_t prefix_len =
        (start + 3 < bs.size() && bs[start + 3] == 1) ? 4 : 3;
    std::size_t cursor = start + prefix_len;

    while (cursor < bs.size()) {
        std::size_t next4 = find_next_start4(cursor);
        std::size_t next3 = find_next_start(cursor);
        std::size_t next  = (next4 < next3) ? next4 : next3;
        std::size_t end   = (next == bs.size()) ? bs.size() : next;
        if (end > cursor) {
            std::vector<std::uint8_t> nalu(
                bs.begin() + static_cast<std::ptrdiff_t>(cursor),
                bs.begin() + static_cast<std::ptrdiff_t>(end));
            if (!nalu.empty()) out.push_back(std::move(nalu));
        }
        if (next == bs.size()) break;
        // Skip start code (3 or 4 bytes).
        prefix_len =
            (next + 3 < bs.size() && bs[next + 3] == 1) ? 4 : 3;
        cursor = next + prefix_len;
    }
    return out;
}

/** Extract NAL units from a length-prefixed bitstream (RFC 6184 Annex B /
 *  MP4-style framing).  Each NAL is prefixed by a 4-byte big-endian length. */
std::vector<std::vector<std::uint8_t>> parse_length_prefixed(core::ByteSpan bs) noexcept {
    std::vector<std::vector<std::uint8_t>> out;
    std::size_t cursor = 0;
    while (cursor + 4 <= bs.size()) {
        const std::uint32_t len =
            (static_cast<std::uint32_t>(bs[cursor])     << 24) |
            (static_cast<std::uint32_t>(bs[cursor + 1]) << 16) |
            (static_cast<std::uint32_t>(bs[cursor + 2]) <<  8) |
             static_cast<std::uint32_t>(bs[cursor + 3]);
        cursor += 4;
        if (len == 0 || cursor + len > bs.size()) break;
        std::vector<std::uint8_t> nalu(
            bs.begin() + static_cast<std::ptrdiff_t>(cursor),
            bs.begin() + static_cast<std::ptrdiff_t>(cursor + len));
        out.push_back(std::move(nalu));
        cursor += len;
    }
    return out;
}

} // namespace

// ===========================================================================
// VideoReceiver::Impl
// ===========================================================================

struct VideoReceiver::Impl {
    ReceiverConfig                 cfg{};
    ReceiverStats                  stats{};
    ReceiverFrameCallback          frame_cb;

    std::unique_ptr<video_jb::IVideoJitterBuffer> jb;

    // Sequence tracking — wrap-aware.
    bool        seq_initialised = false;
    std::uint16_t highest_seq     = 0;
    std::uint16_t expected_next   = 0;
    std::vector<std::uint16_t>    pending_nacks;

    // FU-A reassembly state.
    bool          fua_in_progress = false;
    std::uint8_t  fua_nalu_type   = 0;
    std::uint8_t  fua_nri         = 0;
    std::uint32_t fua_rtp_ts      = 0;
    std::uint16_t fua_start_seq   = 0;
    std::vector<std::uint8_t> fua_buf;
    // Marker bit to apply when we flush the reassembled NALU.
    bool          fua_end_marker  = false;

    explicit Impl(ReceiverConfig c) : cfg(std::move(c)) {
        jb = video_jb::create(cfg.jitter_buffer_config);
    }

    // ---------------------------------------------------------------------
    // Track the seq and detect gaps.
    // ---------------------------------------------------------------------
    void track_seq(std::uint16_t seq) noexcept {
        if (!seq_initialised) {
            highest_seq   = seq;
            expected_next = static_cast<std::uint16_t>(seq + 1);
            seq_initialised = true;
            return;
        }
        const std::int32_t d = seq_delta(seq, highest_seq);
        if (d == 0) {
            // Duplicate; ignore (we may have already seen this packet).
            return;
        }
        if (d < 0) {
            // Out-of-order / old packet; don't generate NACKs.
            return;
        }
        // d > 0: new packet, possibly with gaps.
        if (d > 1) {
            // gap of (d - 1) missing packets
            stats.sequence_gaps_seen += static_cast<std::uint64_t>(d - 1);
            std::uint16_t missing = expected_next;
            for (std::int32_t k = 0; k < d - 1; ++k) {
                pending_nacks.push_back(missing);
                missing = static_cast<std::uint16_t>(missing + 1);
            }
        }
        highest_seq   = seq;
        expected_next = static_cast<std::uint16_t>(seq + 1);
    }

    // ---------------------------------------------------------------------
    // Flush the current FU-A buffer as a single NALU into the JB.
    // Called when we see the E bit on the last fragment.
    // ---------------------------------------------------------------------
    void flush_fua(std::uint8_t  nri,
                   std::uint8_t  nalu_type,
                   core::ByteSpan body,
                   bool          marker) noexcept {
        if (body.empty()) {
            // Empty reassembly; nothing to emit.  This can happen if a
            // middle/end fragment arrived without a start — drop silently.
            return;
        }
        std::vector<std::uint8_t> nalu;
        nalu.reserve(1 + body.size());
        const std::uint8_t hdr = static_cast<std::uint8_t>(
            ((nri & 0x03) << 5) | (nalu_type & kNaluTypeMask));
        nalu.push_back(hdr);
        nalu.insert(nalu.end(), body.begin(), body.end());

        video_jb::InboundPacket pkt{};
        pkt.ssrc          = cfg.ssrc;
        pkt.seq           = fua_start_seq;   // representative seq
        pkt.rtp_timestamp = fua_rtp_ts;
        pkt.arrival_us    = 0;               // filled by caller? (set later)
        pkt.marker        = marker;
        pkt.payload_type  = cfg.payload_type;
        pkt.codec         = video_frame::CodecKind::kH264;
        pkt.payload       = core::ByteSpan{nalu.data(), nalu.size()};
        jb->insert(pkt);
    }

    // ---------------------------------------------------------------------
    // Handle a single RTP packet (after seq tracking).
    // ---------------------------------------------------------------------
    void handle_payload(core::ByteSpan payload,
                        bool          marker,
                        std::uint32_t rtp_ts,
                        std::int64_t  arrival_us) noexcept {
        if (cfg.codec == video_frame::CodecKind::kH264) {
            const NaluType nt = peek_nalu_type(payload);
            if (nt == static_cast<NaluType>(kFuAType) && cfg.expect_fu_a) {
                FuAHeader h;
                if (!parse_fu_a_header(payload, h)) return;
                const bool start = (h.fu_header & 0x80) != 0;
                const bool end   = (h.fu_header & 0x40) != 0;
                const std::uint8_t inner = h.fu_header & kNaluTypeMask;
                const std::uint8_t nri   = (h.fu_indicator & kNriMask) >> 5;

                stats.fua_fragments_seen++;
                if (start) {
                    fua_in_progress = true;
                    fua_nalu_type   = inner;
                    fua_nri         = nri;
                    fua_rtp_ts      = rtp_ts;
                    fua_start_seq   = highest_seq;
                    fua_buf.clear();
                    fua_end_marker  = marker;
                }
                if (!fua_in_progress) {
                    // Stray continuation; drop.
                    return;
                }
                fua_buf.insert(fua_buf.end(),
                               h.fragment.begin(),
                               h.fragment.end());
                if (end) {
                    flush_fua(fua_nri, fua_nalu_type,
                              core::ByteSpan{fua_buf.data(),
                                             fua_buf.size()},
                              marker);
                    fua_in_progress = false;
                    fua_buf.clear();
                    fua_end_marker  = false;
                }
                return;
            }
            if (nt == static_cast<NaluType>(kStapAType)) {
                stats.stap_a_packets_seen++;
                auto stap = parse_stap_a(payload);
                if (!stap.parsed_ok || stap.nalus.empty()) return;
                // Concatenate all NALUs (without start code) into one payload.
                std::vector<std::uint8_t> concat;
                for (const auto& n : stap.nalus) {
                    concat.insert(concat.end(), n.data.begin(), n.data.end());
                }
                video_jb::InboundPacket pkt{};
                pkt.ssrc          = cfg.ssrc;
                pkt.seq           = highest_seq;
                pkt.rtp_timestamp = rtp_ts;
                pkt.arrival_us    = arrival_us;
                pkt.marker        = marker;
                pkt.payload_type  = cfg.payload_type;
                pkt.codec         = video_frame::CodecKind::kH264;
                pkt.payload       = core::ByteSpan{concat.data(),
                                                   concat.size()};
                jb->insert(pkt);
                return;
            }
            // Single NALU (1..23) — pass through.
        }
        // Generic: VP8, VP9, or H.264 Single NALU.
        video_jb::InboundPacket pkt{};
        pkt.ssrc          = cfg.ssrc;
        pkt.seq           = highest_seq;
        pkt.rtp_timestamp = rtp_ts;
        pkt.arrival_us    = arrival_us;
        pkt.marker        = marker;
        pkt.payload_type  = cfg.payload_type;
        pkt.codec         = cfg.codec;
        pkt.payload       = payload;
        jb->insert(pkt);
    }

    // ---------------------------------------------------------------------
    // Drain popped frames from the JB and invoke the user callback.
    // ---------------------------------------------------------------------
    void drain_frames(std::int64_t now_us) noexcept {
        if (!frame_cb) return;
        for (;;) {
            auto f = jb->pop_frame();
            if (!f) break;
            stats.frames_emitted++;
            if (!f->complete) stats.frames_incomplete++;
            video_frame::EncodedVideoFrame out{};
            out.codec        = f->codec;
            out.nalu_format  = video_frame::NaluFormat::kAnnexBStartCode;
            out.payload      = core::ByteSpan{f->bitstream.data(),
                                              f->bitstream.size()};
            out.is_keyframe  = f->is_keyframe;
            out.payload_type = cfg.payload_type;
            out.info.capture_ts_us   = static_cast<std::int64_t>(f->capture_ts_us);
            out.info.rtp_timestamp   = f->rtp_timestamp;
            out.info.frame_seq       = f->frame_seq;
            frame_cb(out, now_us);
        }
    }
};

// ===========================================================================
// VideoReceiver public API
// ===========================================================================

VideoReceiver::VideoReceiver(ReceiverConfig cfg)
    : impl_(std::make_unique<Impl>(std::move(cfg))) {}

VideoReceiver::~VideoReceiver() = default;

void VideoReceiver::set_frame_callback(ReceiverFrameCallback cb) noexcept {
    impl_->frame_cb = std::move(cb);
}

bool VideoReceiver::push_rtp(const rtp::PacketView& packet,
                             std::int64_t now_us) noexcept {
    auto& s = *impl_;
    s.stats.packets_pushed++;
    if (packet.payload.empty() && !packet.marker) return false;

    // Sequence tracking.
    s.track_seq(packet.seq);

    // For H.264 FU-A, we may insert a synthesised NALU before draining
    // (so the frame can fire on this same push).  arrival_us is filled
    // with now_us; for fragments it stays at now_us — close enough for
    // the JB's purpose.
    s.handle_payload(packet.payload, packet.marker,
                     packet.timestamp, now_us);

    s.drain_frames(now_us);
    return true;
}

std::vector<std::uint16_t> VideoReceiver::pop_nack_entries() noexcept {
    auto& s = *impl_;
    auto out = std::move(s.pending_nacks);
    s.pending_nacks.clear();
    // Drain the JB's nacks too (covers cases where FU-A handling is off
    // and packets went through to the JB raw).
    auto from_jb = s.jb->pop_nack_entries();
    out.insert(out.end(), from_jb.begin(), from_jb.end());
    s.stats.nack_entries_emitted += out.size();
    return out;
}

void VideoReceiver::tick(std::int64_t now_us) noexcept {
    auto& s = *impl_;
    s.jb->tick(now_us);
    s.drain_frames(now_us);
}

void VideoReceiver::reset() noexcept {
    auto& s = *impl_;
    s.stats = ReceiverStats{};
    s.pending_nacks.clear();
    s.seq_initialised = false;
    s.highest_seq     = 0;
    s.expected_next   = 0;
    s.fua_in_progress = false;
    s.fua_buf.clear();
    s.fua_end_marker  = false;
    s.jb->reset();
}

ReceiverStats VideoReceiver::stats() const noexcept {
    return impl_->stats;
}

// ===========================================================================
// VideoSender::Impl
// ===========================================================================

struct VideoSender::Impl {
    SenderConfig            cfg{};
    SenderStats             stats{};
    SenderPacketCallback    packet_cb;

    std::uint16_t next_seq = 0;
    bool          keyframe_pending = false;

    // Reusable scratch buffer for emitted packet payloads.
    std::vector<std::uint8_t> scratch;

    explicit Impl(SenderConfig c) : cfg(std::move(c)) {
        next_seq = cfg.initial_seq;
    }

    void emit(core::ByteSpan payload,
              std::uint32_t   rtp_ts,
              bool           marker) noexcept {
        scratch.assign(payload.begin(), payload.end());
        std::uint16_t seq = next_seq;
        next_seq = static_cast<std::uint16_t>(next_seq + 1);
        stats.packets_emitted++;
        if (packet_cb) {
            packet_cb(core::ByteSpan{scratch.data(), scratch.size()},
                      rtp_ts, seq, marker);
        }
    }

    /** H.264 packetization: emit one frame's worth of NALUs. */
    core::Result<void> emit_h264(std::vector<std::vector<std::uint8_t>> nalus,
                                 std::uint32_t rtp_ts) noexcept {
        if (cfg.packetization_mode == PacketizationMode::kInterleaved) {
            return core::Result<void>::fail(
                core::ErrorCode::NotImplemented,
                "video_pipeline: H.264 interleaved mode (STAP-B/FU-B) "
                "not supported");
        }

        // Decide which NALUs to FU-A-packetize (oversized) and which to
        // consider for STAP-A aggregation.
        std::vector<bool> use_fua(nalus.size(), false);
        // large_fragments[i] = vector of FU-A fragment payloads for nalu i
        // (only populated when use_fua[i] is true).
        std::vector<std::vector<std::vector<std::uint8_t>>> large_fragments;

        for (std::size_t i = 0; i < nalus.size(); ++i) {
            const auto& n = nalus[i];
            if (n.size() > cfg.mtu) {
                use_fua[i] = true;
                const std::uint8_t nalu_type = n.empty() ? 0 : (n[0] & kNaluTypeMask);
                const std::uint8_t nri       = n.empty() ? 0 : ((n[0] & kNriMask) >> 5);
                const std::size_t  body_size = n.size() > 1 ? n.size() - 1 : 0;
                const std::size_t  max_body_per_frag = cfg.mtu > 2
                                                       ? cfg.mtu - 2
                                                       : 0;
                std::size_t offset = 0;
                std::vector<std::vector<std::uint8_t>> fragments;
                while (offset < body_size) {
                    const std::size_t remaining = body_size - offset;
                    const std::size_t this_len = remaining > max_body_per_frag
                                                  ? max_body_per_frag
                                                  : remaining;
                    const bool start = (offset == 0);
                    const bool end   = (offset + this_len == body_size);
                    const core::ByteSpan body_slice{
                        n.data() + 1 + offset, this_len};
                    std::vector<std::uint8_t> frag(cfg.mtu);
                    const std::size_t wrote = build_fu_a(
                        static_cast<NaluType>(nalu_type),
                        body_slice,
                        start,
                        end,
                        core::MutableByteSpan{frag.data(), frag.size()});
                    if (wrote == 0) return core::Result<void>::fail(
                        core::ErrorCode::InternalError,
                        "video_pipeline: build_fu_a failed");
                    frag.resize(wrote);
                    // Patch the NRI on the indicator byte (build_fu_a
                    // defaults to NRI=0).  Indicator layout:
                    //   F(1) NRI(2) Type(5) — type must stay 28.
                    frag[0] = static_cast<std::uint8_t>(
                        (frag[0] & ~kNriMask) | ((nri & 0x03) << 5));
                    fragments.push_back(std::move(frag));
                    offset += this_len;
                }
                large_fragments.push_back(std::move(fragments));
            }
        }

        // Walk through nalus; for each, either emit FU-A fragments OR
        // try to bundle with neighbouring small NALUs into a STAP-A.
        std::size_t i = 0;
        while (i < nalus.size()) {
            if (use_fua[i]) {
                const auto& frags = large_fragments[i];
                for (std::size_t k = 0; k < frags.size(); ++k) {
                    const bool is_last = (k + 1 == frags.size());
                    emit(core::ByteSpan{frags[k].data(), frags[k].size()},
                         rtp_ts, /*marker=*/is_last);
                }
                stats.fua_frames_packetized++;
                ++i;
                continue;
            }

            // Try STAP-A aggregation starting at i: pack as many small
            // NALUs as fit within mtu - 1 (STAP-A header) - 2*N (lengths).
            std::size_t budget = cfg.mtu > 1 ? cfg.mtu - 1 : 0;
            std::vector<std::vector<std::uint8_t>> group;
            std::size_t j = i;
            while (j < nalus.size() && !use_fua[j]) {
                if (budget < 2 + nalus[j].size()) {
                    // Would not fit.  Flush if we have a STAP-A group of
                    // 2+ NALUs; else just emit the single NALU as-is.
                    break;
                }
                if (group.empty() && budget < 2 + nalus[j].size()) {
                    // Only one candidate, but it doesn't fit in STAP-A —
                    // fall through and emit as single NALU.
                    break;
                }
                budget -= 2 + nalus[j].size();
                group.push_back(nalus[j]);
                ++j;
                if (j >= nalus.size()) break;
                // Stop if next NALU is FU-A (handled separately).
                if (use_fua[j]) break;
            }

            if (group.size() >= 2) {
                // Emit a STAP-A packet.
                std::vector<core::ByteSpan> nalu_spans;
                nalu_spans.reserve(group.size());
                for (auto& g : group) {
                    nalu_spans.push_back(core::ByteSpan{g.data(), g.size()});
                }
                std::vector<std::uint8_t> out(cfg.mtu);
                const std::size_t wrote = build_stap_a(
                    nalu_spans.data(), nalu_spans.size(),
                    core::MutableByteSpan{out.data(), out.size()});
                if (wrote == 0) {
                    // STAP-A build failed (e.g. too big).  Fall back to
                    // single NALUs one by one.
                    for (auto& g : group) {
                        emit(core::ByteSpan{g.data(), g.size()},
                             rtp_ts, /*marker=*/false);
                        stats.single_nal_frames_packetized++;
                    }
                } else {
                    out.resize(wrote);
                    emit(core::ByteSpan{out.data(), out.size()},
                         rtp_ts, /*marker=*/true);
                    stats.stapa_frames_packetized++;
                }
                i = j;
                continue;
            }

            // group.size() == 1: emit single NALU.
            if (group.size() == 1) {
                emit(core::ByteSpan{group[0].data(), group[0].size()},
                     rtp_ts, /*marker=*/true);
                stats.single_nal_frames_packetized++;
                i = j;
                continue;
            }

            // group empty — shouldn't happen, but be defensive.
            ++i;
        }
        return core::Result<void>::make_ok();
    }

    /** VP8 / VP9 minimal packetization: emit one packet per frame. */
    core::Result<void> emit_simple(core::ByteSpan bitstream,
                                   std::uint32_t  rtp_ts) noexcept {
        emit(bitstream, rtp_ts, /*marker=*/true);
        return core::Result<void>::make_ok();
    }
};

// ===========================================================================
// VideoSender public API
// ===========================================================================

VideoSender::VideoSender(SenderConfig cfg)
    : impl_(std::make_unique<Impl>(std::move(cfg))) {}

VideoSender::~VideoSender() = default;

void VideoSender::set_packet_callback(SenderPacketCallback cb) noexcept {
    impl_->packet_cb = std::move(cb);
}

core::Result<void> VideoSender::push_frame(video_frame::EncodedVideoFrame frame,
                                           std::int64_t  capture_ts_us,
                                           std::uint32_t rtp_timestamp,
                                           std::uint32_t frame_seq) noexcept {
    auto& s = *impl_;
    s.stats.frames_pushed++;

    if (s.keyframe_pending) {
        s.keyframe_pending = false;
        s.stats.frames_marked_as_keyframe++;
        // We don't override frame.is_keyframe — the caller is responsible
        // for emitting a real keyframe (IDR for H.264).  This is just a
        // bookkeeping marker for the receiver feedback roundtrip.
    }

    // Propagate capture metadata.
    frame.info.capture_ts_us = capture_ts_us;
    frame.info.rtp_timestamp = rtp_timestamp;
    frame.info.frame_seq     = frame_seq;
    (void)frame.info;       // used only when receiver emits callback

    switch (frame.codec) {
        case video_frame::CodecKind::kH264: {
            std::vector<std::vector<std::uint8_t>> nalus;
            switch (frame.nalu_format) {
                case video_frame::NaluFormat::kAnnexBStartCode:
                    nalus = parse_annex_b(frame.payload);
                    break;
                case video_frame::NaluFormat::kLengthPrefixed:
                    nalus = parse_length_prefixed(frame.payload);
                    break;
                default:
                    return core::Result<void>::fail(
                        core::ErrorCode::InvalidArgument,
                        "video_pipeline: H.264 frame nalu_format unsupported");
            }
            if (nalus.empty()) {
                return core::Result<void>::fail(
                    core::ErrorCode::InvalidArgument,
                    "video_pipeline: H.264 frame contained zero NAL units");
            }
            return s.emit_h264(std::move(nalus), rtp_timestamp);
        }
        case video_frame::CodecKind::kVP8:
        case video_frame::CodecKind::kVP9:
            return s.emit_simple(frame.payload, rtp_timestamp);
        default:
            return core::Result<void>::fail(
                core::ErrorCode::NotImplemented,
                "video_pipeline: codec not supported");
    }
}

void VideoSender::force_keyframe() noexcept {
    auto& s = *impl_;
    s.keyframe_pending = true;
    s.stats.force_keyframe_calls++;
}

SenderStats VideoSender::stats() const noexcept {
    return impl_->stats;
}

std::uint16_t VideoSender::next_seq() const noexcept {
    return impl_->next_seq;
}

} // namespace nimrtc::video_pipeline