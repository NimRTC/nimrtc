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
#include <nimrtc/video_payload/h265.hpp>
#include <nimrtc/video_payload/av1.hpp>
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

using H265NaluType       = video_payload::h265::NaluType;
using video_payload::h265::build_ap;
using video_payload::h265::build_fu;
using video_payload::h265::parse_ap;
using video_payload::h265::parse_fu_header;

constexpr std::uint8_t kNaluTypeMask = 0x1F;
constexpr std::uint8_t kNriMask      = 0x60;
constexpr std::uint8_t kFuAType      = 28;
constexpr std::uint8_t kStapAType    = 24;

// H.265 (RFC 7798): the NAL unit type is 6 bits wide and the payload
// header is two bytes long.
constexpr std::uint8_t kH265TypeMask = 0x3F;
constexpr std::uint8_t kH265ApType   = 48;
constexpr std::uint8_t kH265FuType   = 49;

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
//
// The NAL units are returned as **spans into the input buffer**, not as
// copies.  The caller owns the bitstream for the duration of the
// packetization (the engine's `video_enc_buf` lives across the whole
// `push_frame()` call), so a copy is pure waste — and on the hot path
// it is the dominant cost: a 1080p key frame is tens of kilobytes
// copied once per frame, on top of the per-NAL heap allocations the
// copy itself needed.
//
// The parser fills a caller-owned vector so the *vector* can also be
// reused across frames (see `VideoSender::Impl::nalus`).
// ---------------------------------------------------------------------------

/** Extract NAL units from an Annex B bitstream.
 *  Accepts 3-byte (00 00 01) and 4-byte (00 00 00 01) start codes.
 *  Each returned span includes the 1-byte NAL header (no start code). */
void parse_annex_b(core::ByteSpan bs,
                   std::vector<core::ByteSpan>& out) noexcept {
    out.clear();
    if (bs.empty()) return;

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
    if (start == bs.size()) return;

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
            out.push_back(core::ByteSpan{bs.data() + cursor, end - cursor});
        }
        if (next == bs.size()) break;
        // Skip start code (3 or 4 bytes).
        prefix_len =
            (next + 3 < bs.size() && bs[next + 3] == 1) ? 4 : 3;
        cursor = next + prefix_len;
    }
}

/** Extract NAL units from a length-prefixed bitstream (RFC 6184 Annex B /
 *  MP4-style framing).  Each NAL is prefixed by a 4-byte big-endian length.
 *  As with `parse_annex_b`, the NAL units are spans into the input. */
void parse_length_prefixed(core::ByteSpan bs,
                           std::vector<core::ByteSpan>& out) noexcept {
    out.clear();
    std::size_t cursor = 0;
    while (cursor + 4 <= bs.size()) {
        const std::uint32_t len =
            (static_cast<std::uint32_t>(bs[cursor])     << 24) |
            (static_cast<std::uint32_t>(bs[cursor + 1]) << 16) |
            (static_cast<std::uint32_t>(bs[cursor + 2]) <<  8) |
             static_cast<std::uint32_t>(bs[cursor + 3]);
        cursor += 4;
        if (len == 0 || cursor + len > bs.size()) break;
        out.push_back(core::ByteSpan{bs.data() + cursor, len});
        cursor += len;
    }
}

} // namespace

// ===========================================================================
// VideoReceiver::Impl
// ===========================================================================

struct VideoReceiver::Impl {
    ReceiverConfig                 cfg{};
    ReceiverStats                  stats{};
    ReceiverFrameCallback          frame_cb;
    ReceiverFeedbackCallback       feedback_cb;

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

    // ---- H.265 (RFC 7798) FU reassembly state ---------------------------
    //
    // H.265's FU packet carries a *two-byte* payload header and a
    // one-byte FU header; the original NAL unit type travels in the FU
    // header's FuType field.  The reassembled NAL unit therefore starts
    // with the original two-byte NAL header, which the FU payload header
    // does *not* carry (RFC 7798 §4.3.3: the FU header replaces the
    // original NAL header, and the type is carried in FuType).
    bool                   fu_h265_in_progress = false;
    H265NaluType           fu_h265_nalu_type   = H265NaluType::kUnspecified;
    std::uint8_t           fu_h265_layer_id    = 0;
    std::uint8_t           fu_h265_tid         = 1;
    std::uint32_t          fu_h265_rtp_ts      = 0;
    std::uint16_t          fu_h265_start_seq   = 0;
    std::vector<std::uint8_t> fu_h265_buf;

    // ---- AV1 reassembly state -------------------------------------------
    //
    // The AV1 depacketizer undoes the aggregation header and the
    // fragmentation (Z / Y), and re-emits the OBUs in Annex-B framing —
    // the form the AV1 decoder expects.
    video_payload::av1::ObuReassembler av1_reassembler;

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
            std::vector<std::uint16_t> lost;
            lost.reserve(static_cast<std::size_t>(d - 1));
            for (std::int32_t k = 0; k < d - 1; ++k) {
                pending_nacks.push_back(missing);
                lost.push_back(missing);
                missing = static_cast<std::uint16_t>(missing + 1);
            }
            // v1.1 Q3: hand the loss to the feedback callback so the owner
            // of the RTCP transport can answer it with a NACK.
            report_missing(std::move(lost));
        }
        highest_seq   = seq;
        expected_next = static_cast<std::uint16_t>(seq + 1);
    }

    // ---------------------------------------------------------------------
    // v1.1 Q3: report lost packets to the feedback callback.
    // ---------------------------------------------------------------------
    void report_missing(std::vector<std::uint16_t> lost) noexcept {
        if (!feedback_cb || lost.empty()) return;
        ReceiverFeedback fb;
        fb.missing_packets = std::move(lost);
        feedback_cb(fb);
    }

    // ---------------------------------------------------------------------
    // v1.1 Q3: picture data was lost -> ask the sender for a key frame.
    // ---------------------------------------------------------------------
    void request_keyframe() noexcept {
        stats.keyframe_requests_emitted++;
        if (!feedback_cb) return;
        ReceiverFeedback fb;
        fb.keyframe_requested = true;
        feedback_cb(fb);
    }

    // ---------------------------------------------------------------------
    // Flush the current FU-A buffer as a single NALU into the JB.
    // Called when we see the E bit on the last fragment.
    // ---------------------------------------------------------------------
    void flush_fua(std::uint8_t  nri,
                   std::uint8_t  nalu_type,
                   core::ByteSpan body,
                   bool          marker,
                   std::int64_t  arrival_us) noexcept {
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
        pkt.arrival_us    = arrival_us;
        pkt.marker        = marker;
        pkt.payload_type  = cfg.payload_type;
        pkt.codec         = video_frame::CodecKind::kH264;
        pkt.payload       = core::ByteSpan{nalu.data(), nalu.size()};
        jb->insert(pkt);
    }

    // ---------------------------------------------------------------------
    // Flush the current H.265 FU buffer as a single NALU into the JB.
    //
    // RFC 7798 §4.3.3: the FU payload header carries the *wrapper* type
    // (49) in place of the original NAL unit's type, and the original
    // type travels in the FU header's FuType field.  The reassembled NAL
    // unit therefore needs its two-byte header synthesised from the
    // original type plus the layer id / temporal id.
    // ---------------------------------------------------------------------
    void flush_fu_h265(bool          marker,
                       std::int64_t  arrival_us) noexcept {
        if (fu_h265_buf.empty()) {
            // Empty reassembly; nothing to emit.
            return;
        }
        std::vector<std::uint8_t> nalu;
        nalu.reserve(2 + fu_h265_buf.size());

        // The two-byte NAL unit header (RFC 7798 §1.1.4):
        //   byte 0: F(1) | Type(6) | LayerId MSB(1)
        //   byte 1: LayerId low 5 bits(5) | TID(3)
        const std::uint8_t t   = static_cast<std::uint8_t>(fu_h265_nalu_type) & kH265TypeMask;
        const std::uint8_t lid = fu_h265_layer_id & 0x3F;
        nalu.push_back(static_cast<std::uint8_t>((t << 1) | ((lid >> 5) & 0x01)));
        nalu.push_back(static_cast<std::uint8_t>(((lid & 0x1F) << 3) |
                                                  ((fu_h265_tid - 1) & 0x07)));
        nalu.insert(nalu.end(), fu_h265_buf.begin(), fu_h265_buf.end());

        video_jb::InboundPacket pkt{};
        pkt.ssrc          = cfg.ssrc;
        pkt.seq           = fu_h265_start_seq;   // representative seq
        pkt.rtp_timestamp = fu_h265_rtp_ts;
        pkt.arrival_us    = arrival_us;
        pkt.marker        = marker;
        pkt.payload_type  = cfg.payload_type;
        pkt.codec         = video_frame::CodecKind::kH265;
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
                              marker, arrival_us);
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

        // ---- H.265 (RFC 7798) ----------------------------------------------
        //
        // The payload format mirrors H.264's, but the payload header is
        // two bytes wide and the NAL unit type is six bits.
        if (cfg.codec == video_frame::CodecKind::kH265) {
            const H265NaluType pt =
                video_payload::h265::peek_payload_type(payload);
            if (pt == static_cast<H265NaluType>(kH265FuType) &&
                cfg.expect_fu_a) {
                video_payload::h265::FuHeader h;
                if (!parse_fu_header(payload, h)) return;
                const auto hdr = video_payload::h265::parse_nalu_header(payload);
                if (!hdr) return;

                stats.fua_fragments_seen++;
                if (h.start) {
                    fu_h265_in_progress = true;
                    fu_h265_nalu_type   = h.fu_type;
                    fu_h265_layer_id    = hdr->layer_id;
                    fu_h265_tid         = hdr->tid;
                    fu_h265_rtp_ts      = rtp_ts;
                    fu_h265_start_seq   = highest_seq;
                    fu_h265_buf.clear();
                }
                if (!fu_h265_in_progress) {
                    // Stray continuation; drop.
                    return;
                }
                // The FU payload starts after the two-byte payload header
                // and the one-byte FU header.
                fu_h265_buf.insert(fu_h265_buf.end(),
                                   payload.begin() + 3, payload.end());
                if (h.end) {
                    flush_fu_h265(marker, arrival_us);
                    fu_h265_in_progress = false;
                    fu_h265_buf.clear();
                }
                return;
            }
            if (pt == static_cast<H265NaluType>(kH265ApType)) {
                stats.stap_a_packets_seen++;
                std::vector<video_payload::h265::Nalu> nalus;
                if (!parse_ap(payload, /*has_donl=*/false, nalus) || nalus.empty()) {
                    return;
                }
                std::vector<std::uint8_t> concat;
                for (const auto& n : nalus) {
                    concat.insert(concat.end(), n.data.begin(), n.data.end());
                }
                video_jb::InboundPacket pkt{};
                pkt.ssrc          = cfg.ssrc;
                pkt.seq           = highest_seq;
                pkt.rtp_timestamp = rtp_ts;
                pkt.arrival_us    = arrival_us;
                pkt.marker        = marker;
                pkt.payload_type  = cfg.payload_type;
                pkt.codec         = video_frame::CodecKind::kH265;
                pkt.payload       = core::ByteSpan{concat.data(), concat.size()};
                jb->insert(pkt);
                return;
            }
            // Single NAL unit packet (types 0..47) — pass through.
        }

        // ---- AV1 (draft-ietf-payload-av1) ---------------------------------
        //
        // The aggregation header and the OBU fragmentation are undone
        // here; the OBUs are re-emitted in Annex-B framing, which is the
        // form the AV1 decoder consumes.
        if (cfg.codec == video_frame::CodecKind::kAV1) {
            std::vector<std::uint8_t> annex_b;
            if (!av1_reassembler.push(payload, annex_b)) return;
            if (annex_b.empty()) return;

            video_jb::InboundPacket pkt{};
            pkt.ssrc          = cfg.ssrc;
            pkt.seq           = highest_seq;
            pkt.rtp_timestamp = rtp_ts;
            pkt.arrival_us    = arrival_us;
            pkt.marker        = marker;
            pkt.payload_type  = cfg.payload_type;
            pkt.codec         = video_frame::CodecKind::kAV1;
            pkt.payload       = core::ByteSpan{annex_b.data(), annex_b.size()};
            jb->insert(pkt);
            return;
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
        for (;;) {
            // The time-aware pop lets the jitter buffer hold a frame until
            // its playout deadline (adaptive mode).  In fixed-window mode
            // it degrades to the immediate emit of the next complete frame.
            auto f = jb->pop_frame(now_us);
            if (!f) break;
            stats.frames_emitted++;
            if (!f->complete) {
                stats.frames_incomplete++;
                // v1.1 Q3: picture data was lost — ask the sender for a
                // fresh key frame so the decoder can resync (PLI / FIR).
                request_keyframe();
            }
            if (!frame_cb) continue;
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

void VideoReceiver::set_feedback_callback(ReceiverFeedbackCallback cb) noexcept {
    impl_->feedback_cb = std::move(cb);
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

    /// The maximum number of NAL units the packetizer works with in one
    /// frame.  The scratch vectors are reserved to this size once, so
    /// that a frame with an unusual number of NAL units does not grow
    /// them (which would be a heap allocation on the send path).
    static constexpr std::size_t kMaxScratchNalus = 64;

    std::uint16_t next_seq = 0;
    bool          keyframe_pending = false;

    // Reusable scratch buffer for emitted packet payloads.
    std::vector<std::uint8_t> scratch;

    // ---- Retransmission cache (v1.1 Q3) ---------------------------------
    // A NACK asks the sender to repeat one packet; the sender can only
    // do that if it still holds the packet.  We keep the most recently
    // sent packets in a small ring buffer (bounded by
    // `cfg.retransmit_cache_packets`) so a NACK can be answered without
    // re-encoding the frame.
    //
    // Phase 3: the slot's payload vector is pre-sized to the MTU when the
    // ring is built, so caching a packet is a memcpy into an already
    // allocated buffer instead of a fresh allocation per packet.
    struct CachedPacket {
        std::uint16_t           seq      = 0;
        std::uint32_t           rtp_ts   = 0;
        bool                    marker   = false;
        std::vector<std::uint8_t> payload;
    };
    std::vector<CachedPacket>  rtx_cache;   // ring buffer, oldest first
    std::size_t               rtx_head = 0; // next slot to overwrite

    // ---- Reusable packetization scratch (Phase 3 / C-class) ---------------
    //
    // The sender used to build a fresh set of vectors for every frame:
    // the NAL unit list, the "does this NAL need FU-A" flags, the FU-A
    // fragment groups, the STAP-A group, the STAP-A span list and the
    // STAP-A output buffer.  On a 30 fps video path that is a steady
    // stream of short-lived heap allocations — the last thing a real-time
    // path should have.
    //
    // All of them are now members whose capacity is kept across frames.
    // The vectors are *cleared*, not destroyed, so the storage stays.
    //
    // The NAL units are **spans into the source bitstream**, not copies:
    // the caller owns the bitstream for the whole `push_frame()` call, so
    // copying tens of kilobytes per frame bought nothing.

    /// The NAL units of the frame being packetized (spans into the source).
    std::vector<core::ByteSpan> nalus;

    /// Per-NAL flag: true when the NAL is too large for one packet and
    /// must be FU-A fragmented.
    std::vector<bool> use_fua;

    /// The NAL units of the current STAP-A / AP aggregation group.
    std::vector<core::ByteSpan> group;

    /// The FU-A / FU fragment buffer.  One fragment is built at a time:
    /// `emit()` copies the payload into its own scratch before firing
    /// the callback, so the buffer is free to be overwritten by the next
    /// fragment.
    std::vector<std::uint8_t> fua_buf;

    /// The STAP-A / AP output buffer, sized to the MTU.
    std::vector<std::uint8_t> group_out;

    /// The H.265 AP aggregation group (typed NAL units).
    std::vector<video_payload::h265::Nalu> ap_group;

    explicit Impl(SenderConfig c) : cfg(std::move(c)) {
        next_seq = cfg.initial_seq;
        rtx_cache.resize(cfg.retransmit_cache_packets);
        // Pre-size every retransmission slot to the MTU so caching a packet
        // is a memcpy into an existing buffer, not a fresh allocation.
        // Without this the RTX ring allocated one buffer per outgoing
        // packet — a per-frame allocation on the hot path.
        for (CachedPacket& slot : rtx_cache) {
            slot.payload.reserve(cfg.mtu);
        }
        // Reserve the packetization scratch vectors up-front.  Their
        // sizes depend on the *content* of the frame (how many NAL units
        // it has), so without a reservation the first frame that carries
        // more NAL units than any before it grows the vector — a
        // one-off allocation that shows up as a per-frame allocation in
        // the benchmark whenever the payload happens to contain a
        // start-code-like byte sequence.
        nalus.reserve(kMaxScratchNalus);
        group.reserve(kMaxScratchNalus);
        use_fua.reserve(kMaxScratchNalus);
        ap_group.reserve(kMaxScratchNalus);
        scratch.reserve(cfg.mtu);
    }

    void emit(core::ByteSpan payload,
              std::uint32_t   rtp_ts,
              bool           marker) noexcept {
        scratch.assign(payload.begin(), payload.end());
        std::uint16_t seq = next_seq;
        next_seq = static_cast<std::uint16_t>(next_seq + 1);
        stats.packets_emitted++;
        cache_packet(seq, rtp_ts, marker, scratch);
        if (packet_cb) {
            packet_cb(core::ByteSpan{scratch.data(), scratch.size()},
                      rtp_ts, seq, marker);
        }
    }

    /** Store a sent packet in the retransmission ring buffer. */
    void cache_packet(std::uint16_t           seq,
                      std::uint32_t           rtp_ts,
                      bool                    marker,
                      const std::vector<std::uint8_t>& payload) noexcept {
        if (cfg.retransmit_cache_packets == 0) return;
        CachedPacket& slot = rtx_cache[rtx_head];
        slot.seq    = seq;
        slot.rtp_ts = rtp_ts;
        slot.marker = marker;
        slot.payload.assign(payload.begin(), payload.end());
        rtx_head = (rtx_head + 1) % cfg.retransmit_cache_packets;
    }

    /** Look up a cached packet by sequence number. */
    const CachedPacket* find_cached(std::uint16_t seq) const noexcept {
        for (const auto& p : rtx_cache) {
            if (p.payload.empty()) continue;   // never filled
            if (p.seq == seq) return &p;
        }
        return nullptr;
    }

    /** H.264 packetization: emit one frame's worth of NALUs.
     *
     *  The NAL units are the `nalus` member (spans into the source
     *  bitstream, parsed by `parse_annex_b()`); every temporary this
     *  function used to build is now a reusable member, so the steady
     *  state performs **no** heap allocation.
     *
     *  The FU-A fragments are built lazily, one at a time, into a single
     *  reusable buffer: `emit()` copies the payload into its own scratch
     *  before firing the callback, so the fragment buffer is free to be
     *  overwritten by the next fragment.
     */
    core::Result<void> emit_h264(std::uint32_t rtp_ts) noexcept {
        if (cfg.packetization_mode == PacketizationMode::kInterleaved) {
            return core::Result<void>::fail(
                core::ErrorCode::NotImplemented,
                "video_pipeline: H.264 interleaved mode (STAP-B/FU-B) "
                "not supported");
        }

        // Decide which NALUs to FU-A-packetize (oversized) and which to
        // consider for STAP-A aggregation.  Only the *size* matters here,
        // so no fragment is built yet.
        use_fua.assign(nalus.size(), false);
        for (std::size_t i = 0; i < nalus.size(); ++i) {
            use_fua[i] = nalus[i].size() > cfg.mtu;
        }

        // Walk through nalus; for each, either emit FU-A fragments OR
        // try to bundle with neighbouring small NALUs into a STAP-A.
        std::size_t i = 0;
        while (i < nalus.size()) {
            if (use_fua[i]) {
                const core::ByteSpan n = nalus[i];
                const std::uint8_t nalu_type = n.empty() ? 0 : (n[0] & kNaluTypeMask);
                const std::uint8_t nri       = n.empty() ? 0 : ((n[0] & kNriMask) >> 5);
                const std::size_t  body_size = n.size() > 1 ? n.size() - 1 : 0;
                const std::size_t  max_body_per_frag = cfg.mtu > 2
                                                       ? cfg.mtu - 2
                                                       : 0;
                std::size_t offset = 0;
                while (offset < body_size) {
                    const std::size_t remaining = body_size - offset;
                    const std::size_t this_len = remaining > max_body_per_frag
                                                  ? max_body_per_frag
                                                  : remaining;
                    const bool start = (offset == 0);
                    const bool end   = (offset + this_len == body_size);
                    const core::ByteSpan body_slice{
                        n.data() + 1 + offset, this_len};
                    if (fua_buf.size() < cfg.mtu) fua_buf.resize(cfg.mtu);
                    const std::size_t wrote = build_fu_a(
                        static_cast<NaluType>(nalu_type),
                        body_slice,
                        start,
                        end,
                        core::MutableByteSpan{fua_buf.data(), fua_buf.size()});
                    if (wrote == 0) return core::Result<void>::fail(
                        core::ErrorCode::InternalError,
                        "video_pipeline: build_fu_a failed");
                    // Patch the NRI on the indicator byte (build_fu_a
                    // defaults to NRI=0).  Indicator layout:
                    //   F(1) NRI(2) Type(5) — type must stay 28.
                    fua_buf[0] = static_cast<std::uint8_t>(
                        (fua_buf[0] & ~kNriMask) | ((nri & 0x03) << 5));
                    emit(core::ByteSpan{fua_buf.data(), wrote},
                         rtp_ts, /*marker=*/end);
                    offset += this_len;
                }
                stats.fua_frames_packetized++;
                ++i;
                continue;
            }

            // Try STAP-A aggregation starting at i: pack as many small
            // NALUs as fit within mtu - 1 (STAP-A header) - 2*N (lengths).
            std::size_t budget = cfg.mtu > 1 ? cfg.mtu - 1 : 0;
            group.clear();
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
                if (group_out.size() < cfg.mtu) group_out.resize(cfg.mtu);
                const std::size_t wrote = build_stap_a(
                    group.data(), group.size(),
                    core::MutableByteSpan{group_out.data(), group_out.size()});
                if (wrote == 0) {
                    // STAP-A build failed (e.g. too big).  Fall back to
                    // single NALUs one by one.
                    for (auto& g : group) {
                        emit(g, rtp_ts, /*marker=*/false);
                        stats.single_nal_frames_packetized++;
                    }
                } else {
                    emit(core::ByteSpan{group_out.data(), wrote},
                         rtp_ts, /*marker=*/true);
                    stats.stapa_frames_packetized++;
                }
                i = j;
                continue;
            }

            // group.size() == 1: emit single NALU.
            if (group.size() == 1) {
                emit(group[0], rtp_ts, /*marker=*/true);
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

    // ---------------------------------------------------------------------
    // H.265 packetization (RFC 7798).
    //
    // The shape mirrors H.264's, with three differences:
    //
    //   * the NAL unit header is two bytes wide and the type is six
    //     bits;
    //   * the aggregation packet is **AP** (type 48), not STAP-A;
    //   * the fragmentation unit is **FU** (type 49) and carries a
    //     one-byte FU header (S | E | FuType).
    //
    // The AP packet does *not* carry a DONL field here: NimRTC does not
    // negotiate `sprop-max-don-diff`, so the field is absent.
    // ---------------------------------------------------------------------
    core::Result<void> emit_h265(std::uint32_t rtp_ts) noexcept {
        if (cfg.packetization_mode == PacketizationMode::kInterleaved) {
            return core::Result<void>::fail(
                core::ErrorCode::NotImplemented,
                "video_pipeline: H.265 interleaved mode (PACI) not supported");
        }

        // ---- Which NAL units are too large for one packet? ----
        //
        // A FU packet costs 3 bytes of overhead (two-byte payload header
        // + one-byte FU header) on top of the fragment payload, and the
        // fragment payload is the original NAL unit minus its two-byte
        // header.
        //
        // Only the *size* decides, so the fragments themselves are built
        // lazily, one at a time, during the walk below.
        use_fua.assign(nalus.size(), false);
        for (std::size_t i = 0; i < nalus.size(); ++i) {
            use_fua[i] = nalus[i].size() > cfg.mtu;
        }

        // ---- Walk the NAL units ----
        std::size_t i = 0;
        while (i < nalus.size()) {
            if (use_fua[i]) {
                const core::ByteSpan n = nalus[i];
                // The layer id / temporal id come from the original NAL
                // unit header (RFC 7798 §1.1.4).
                const auto hdr = video_payload::h265::parse_nalu_header(n);
                const std::uint8_t layer_id = hdr ? hdr->layer_id : 0;
                const std::uint8_t tid      = hdr ? hdr->tid : 1;
                const auto nalu_type = hdr
                    ? hdr->type
                    : static_cast<H265NaluType>((n[0] >> 1) & kH265TypeMask);

                const std::size_t body_size = n.size() > 2 ? n.size() - 2 : 0;
                const std::size_t max_body = cfg.mtu > 3 ? cfg.mtu - 3 : 0;
                if (max_body == 0) {
                    return core::Result<void>::fail(
                        core::ErrorCode::InvalidArgument,
                        "video_pipeline: H.265 MTU too small for a FU fragment");
                }

                std::size_t offset = 0;
                while (offset < body_size) {
                    const std::size_t remaining = body_size - offset;
                    const std::size_t this_len = remaining > max_body ? max_body
                                                                      : remaining;
                    const bool start = (offset == 0);
                    const bool end   = (offset + this_len == body_size);
                    const core::ByteSpan body_slice{
                        n.data() + 2 + offset, this_len};
                    if (fua_buf.size() < cfg.mtu) fua_buf.resize(cfg.mtu);
                    const std::size_t wrote = build_fu(
                        nalu_type, layer_id, tid, body_slice, start, end,
                        core::MutableByteSpan{fua_buf.data(), fua_buf.size()});
                    if (wrote == 0) {
                        return core::Result<void>::fail(
                            core::ErrorCode::InternalError,
                            "video_pipeline: build_fu failed");
                    }
                    emit(core::ByteSpan{fua_buf.data(), wrote},
                         rtp_ts, /*marker=*/end);
                    offset += this_len;
                }
                stats.fua_frames_packetized++;
                ++i;
                continue;
            }

            // ---- AP aggregation ----
            //
            // An AP packet costs 2 bytes of payload header, plus 2 bytes
            // of length prefix per aggregated NAL unit.
            std::size_t budget = cfg.mtu > 2 ? cfg.mtu - 2 : 0;
            ap_group.clear();
            std::size_t j = i;
            while (j < nalus.size() && !use_fua[j]) {
                const std::size_t cost = 2 + nalus[j].size();
                if (cost > budget) break;
                budget -= cost;
                video_payload::h265::Nalu n;
                n.type = video_payload::h265::peek_nalu_type(nalus[j]);
                n.data = nalus[j];
                ap_group.push_back(n);
                ++j;
            }

            if (ap_group.size() >= 2) {
                if (group_out.size() < cfg.mtu) group_out.resize(cfg.mtu);
                const std::size_t wrote = build_ap(
                    ap_group, /*layer_id=*/0, /*tid=*/1,
                    core::MutableByteSpan{group_out.data(), group_out.size()});
                if (wrote == 0) {
                    for (const auto& g : ap_group) {
                        emit(g.data, rtp_ts, /*marker=*/false);
                        stats.single_nal_frames_packetized++;
                    }
                } else {
                    emit(core::ByteSpan{group_out.data(), wrote},
                         rtp_ts, /*marker=*/true);
                    stats.stapa_frames_packetized++;
                }
                i = j;
                continue;
            }

            if (ap_group.size() == 1) {
                emit(ap_group[0].data, rtp_ts, /*marker=*/true);
                stats.single_nal_frames_packetized++;
                i = j;
                continue;
            }

            ++i;
        }
        return core::Result<void>::make_ok();
    }

    // ---------------------------------------------------------------------
    // AV1 packetization (draft-ietf-payload-av1).
    //
    // The frame arrives in Annex-B framing (the form the NVENC AV1
    // encoder emits); the OBUs are split out and framed by the
    // aggregation-header rules of the AV1 RTP payload format.
    // ---------------------------------------------------------------------
    core::Result<void> emit_av1(core::ByteSpan bitstream,
                                 std::uint32_t  rtp_ts,
                                 bool           is_keyframe) noexcept {
        std::vector<video_payload::av1::Obu> obus;
        if (!video_payload::av1::parse_annex_b_obus(bitstream, obus)) {
            return core::Result<void>::fail(
                core::ErrorCode::InvalidArgument,
                "video_pipeline: AV1 frame contained no OBU");
        }

        std::vector<video_payload::av1::Packet> packets;
        if (!video_payload::av1::packetize(obus, cfg.mtu, is_keyframe, packets)) {
            return core::Result<void>::fail(
                core::ErrorCode::InvalidArgument,
                "video_pipeline: AV1 frame could not be packetized");
        }

        for (std::size_t k = 0; k < packets.size(); ++k) {
            const bool is_last = (k + 1 == packets.size());
            emit(core::ByteSpan{packets[k].payload.data(),
                                packets[k].payload.size()},
                 rtp_ts, /*marker=*/is_last);
        }
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
            switch (frame.nalu_format) {
                case video_frame::NaluFormat::kAnnexBStartCode:
                    parse_annex_b(frame.payload, s.nalus);
                    break;
                case video_frame::NaluFormat::kLengthPrefixed:
                    parse_length_prefixed(frame.payload, s.nalus);
                    break;
                default:
                    return core::Result<void>::fail(
                        core::ErrorCode::InvalidArgument,
                        "video_pipeline: H.264 frame nalu_format unsupported");
            }
            if (s.nalus.empty()) {
                return core::Result<void>::fail(
                    core::ErrorCode::InvalidArgument,
                    "video_pipeline: H.264 frame contained zero NAL units");
            }
            return s.emit_h264(rtp_timestamp);
        }
        case video_frame::CodecKind::kH265: {
            // H.265 uses the same Annex-B / length-prefixed framing as
            // H.264 — only the NAL unit header differs (two bytes).
            switch (frame.nalu_format) {
                case video_frame::NaluFormat::kAnnexBStartCode:
                    parse_annex_b(frame.payload, s.nalus);
                    break;
                case video_frame::NaluFormat::kLengthPrefixed:
                    parse_length_prefixed(frame.payload, s.nalus);
                    break;
                default:
                    return core::Result<void>::fail(
                        core::ErrorCode::InvalidArgument,
                        "video_pipeline: H.265 frame nalu_format unsupported");
            }
            if (s.nalus.empty()) {
                return core::Result<void>::fail(
                    core::ErrorCode::InvalidArgument,
                    "video_pipeline: H.265 frame contained zero NAL units");
            }
            return s.emit_h265(rtp_timestamp);
        }
        case video_frame::CodecKind::kAV1:
            return s.emit_av1(frame.payload, rtp_timestamp, frame.is_keyframe);
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

bool VideoSender::keyframe_pending() const noexcept {
    return impl_->keyframe_pending;
}

bool VideoSender::retransmit(std::uint16_t seq) noexcept {
    auto& s = *impl_;
    const Impl::CachedPacket* p = s.find_cached(seq);
    if (!p) {
        s.stats.packets_retransmit_missed++;
        return false;
    }
    // Re-emit with the ORIGINAL sequence number / timestamp / marker so
    // the receiver can splice the packet back into the frame it belongs to.
    if (s.packet_cb) {
        s.packet_cb(core::ByteSpan{p->payload.data(), p->payload.size()},
                    p->rtp_ts, p->seq, p->marker);
    }
    s.stats.packets_retransmitted++;
    return true;
}

std::size_t VideoSender::cached_packet_count() const noexcept {
    const auto& s = *impl_;
    std::size_t n = 0;
    for (const auto& p : s.rtx_cache) {
        if (!p.payload.empty()) ++n;
    }
    return n;
}

SenderStats VideoSender::stats() const noexcept {
    return impl_->stats;
}

std::uint16_t VideoSender::next_seq() const noexcept {
    return impl_->next_seq;
}

// ===========================================================================
// Feedback bridge (v1.1 Q3)
// ===========================================================================

namespace fb = video_rtcp_feedback;

std::size_t feedback_rtcp_size(const ReceiverFeedback& feedback) noexcept {
    std::size_t n = 0;
    if (!feedback.missing_packets.empty()) {
        const auto entries = fb::pack_nack_entries(feedback.missing_packets);
        n += fb::kNackMinSize + entries.size() * fb::kNackEntrySize;
    }
    if (feedback.keyframe_requested) {
        n += fb::kPliPacketSize;
    }
    return n;
}

std::size_t build_feedback_rtcp(core::MutableByteSpan output,
                                const ReceiverFeedback& feedback,
                                std::uint32_t sender_ssrc,
                                std::uint32_t media_ssrc) noexcept {
    // Size the destination up-front: a report that does not fit is
    // rejected as a whole (nothing is written), so the caller never sees
    // a half-encoded feedback packet.
    if (output.size() < feedback_rtcp_size(feedback)) return 0;

    std::size_t written = 0;

    if (!feedback.missing_packets.empty()) {
        const auto entries = fb::pack_nack_entries(feedback.missing_packets);
        if (!entries.empty()) {
            fb::NackPacket nack;
            nack.sender_ssrc = sender_ssrc;
            nack.media_ssrc  = media_ssrc;
            nack.entries     = entries;
            const std::size_t n = fb::build_nack(
                output.subspan(written), nack);
            if (n == 0) return 0;      // destination too small
            written += n;
        }
    }

    if (feedback.keyframe_requested) {
        fb::PliPacket pli;
        pli.sender_ssrc = sender_ssrc;
        pli.media_ssrc  = media_ssrc;
        const std::size_t n = fb::build_pli(output.subspan(written), pli);
        if (n == 0) return 0;          // destination too small
        written += n;
    }

    return written;
}

core::Result<int> apply_feedback_rtcp(VideoSender& sender,
                                      core::ByteSpan rtcp) noexcept {
    int actions = 0;
    std::size_t offset = 0;

    while (offset < rtcp.size()) {
        if (rtcp.size() - offset < fb::kRtcpHeaderSize) {
            return core::Result<int>::fail(
                core::ErrorCode::ProtocolError,
                "video_pipeline: RTCP feedback truncated (header)");
        }
        const std::uint8_t b0 = rtcp[offset];
        const std::uint8_t pt = rtcp[offset + 1];
        const std::uint16_t length_words = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(rtcp[offset + 2]) << 8) |
             static_cast<std::uint16_t>(rtcp[offset + 3]));
        const std::size_t packet_size =
            (static_cast<std::size_t>(length_words) + 1u) * 4u;
        if (packet_size < fb::kRtcpHeaderSize ||
            offset + packet_size > rtcp.size()) {
            return core::Result<int>::fail(
                core::ErrorCode::ProtocolError,
                "video_pipeline: RTCP feedback length field invalid");
        }
        const core::ByteSpan packet = rtcp.subspan(offset, packet_size);
        const std::uint8_t fmt = static_cast<std::uint8_t>(b0 & 0x1Fu);

        if (pt == fb::kRtpfbPt && fmt == fb::kFmtNack) {
            if (packet_size < fb::kNackMinSize) {
                return core::Result<int>::fail(
                    core::ErrorCode::ProtocolError,
                    "video_pipeline: RTCP NACK too short");
            }
            auto parsed = fb::parse_nack(packet);
            if (!parsed.ok()) {
                return core::Result<int>::fail(parsed.error().code(),
                                                parsed.error().message());
            }
            const auto seqs = fb::unpack_nack_entries(parsed.value());
            for (std::uint16_t seq : seqs) {
                if (sender.retransmit(seq)) ++actions;
            }
        } else if (pt == fb::kPsfbPt && fmt == fb::kFmtPli) {
            if (packet_size < fb::kPliPacketSize) {
                return core::Result<int>::fail(
                    core::ErrorCode::ProtocolError,
                    "video_pipeline: RTCP PLI too short");
            }
            auto parsed = fb::parse_pli(packet);
            if (!parsed.ok()) {
                return core::Result<int>::fail(parsed.error().code(),
                                                parsed.error().message());
            }
            sender.force_keyframe();
            ++actions;
        } else if (pt == fb::kPsfbPt && fmt == fb::kFmtFir) {
            if (packet_size < fb::kPliPacketSize) {
                return core::Result<int>::fail(
                    core::ErrorCode::ProtocolError,
                    "video_pipeline: RTCP FIR too short");
            }
            auto parsed = fb::parse_fir(packet);
            if (!parsed.ok()) {
                return core::Result<int>::fail(parsed.error().code(),
                                                parsed.error().message());
            }
            sender.force_keyframe();
            ++actions;
        }
        // Other feedback kinds (REMB, transport-cc, …) are ignored on
        // purpose — the video pipeline has no use for them.  The same
        // holds for the non-feedback RTCP packets (SR / RR / SDES) that
        // share a compound datagram with a NACK: they are skipped, not
        // treated as a protocol error.

        offset += packet_size;
    }

    return core::Result<int>::ok(actions);
}

} // namespace nimrtc::video_pipeline