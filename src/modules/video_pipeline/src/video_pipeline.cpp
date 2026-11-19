/**
 * @file src/modules/video_pipeline/src/video_pipeline.cpp
 * @brief VideoReceiver and VideoSender reference implementations.
 *
 * ## Receiver
 *
 *   push_rtp(packet, now_us):
 *     1. Track seq number (wrap-aware) 鈫?detect gaps 鈫?fill NACK list.
 *     2. If H.264:
 *        - peek_nalu_type(payload)
 *        - FU-A start : clear/initialise reassembly buffer (NRI, type).
 *        - FU-A middle: append fragment bytes to reassembly buffer.
 *        - FU-A end   : synthesise original NAL header + flush body to JB.
 *        - STAP-A     : extract all NAL units, concatenate, push to JB.
 *        - Single     : push payload directly to JB.
 *     3. If VP8/VP9: push the raw payload to JB (JB concatenates).
 *     4. Drain pop_frame() 鈫?invoke user frame_callback.
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
 *     3. If VP8/VP9: emit as a single packet (no descriptor 鈥?minimal
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
#include <nimrtc/rtx/rtx.hpp>

#include <algorithm>
#include <chrono>
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

// The Annex-B start code (ITU-T H.264 Annex B / ISO-IEC 14496-10 搂7.4.1.2.4):
// four zero bytes followed by a one.
//
// RFC 6184 搂5.1 carries *bare* NAL units in the RTP payload 鈥?the start
// code is not on the wire.  The receiver therefore has to put it back,
// because the decoder consumes the Annex-B framing that
// `video_frame::NaluFormat::kAnnexBStartCode` promises:
//
//     00 00 00 01 [nalu] 00 00 00 01 [nalu] ...
//
// The receiver is the only place that knows where one NAL unit ends and
// the next begins: the single-NAL packet format carries exactly one NAL
// unit, STAP-A carries several, and FU-A carries a fragment that the
// receiver reassembles.  The jitter buffer below it only concatenates.
//
// So the start code is inserted *here*, at the point where the NAL
// boundary is still known, and never in `video_jb` 鈥?that module is
// codec-agnostic (VP8 / VP9 carry no NAL units at all).
constexpr std::uint8_t kAnnexBStartCode[4] = {0x00, 0x00, 0x00, 0x01};

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
// `push_frame()` call), so a copy is pure waste 鈥?and on the hot path
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

    // Sequence tracking 鈥?wrap-aware.
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
    // Which access unit the FU belongs to: 0 for a parameter set / SEI /
    // delimiter (emitted at once, ahead of the slice), 1 for a slice
    // (emitted when the picture's end arrives).
    std::uint8_t  fua_au          = 0;
    std::vector<std::uint8_t> fua_buf;
    // The end fragment of a slice carried the marker bit: the picture is
    // complete and the next timestamp boundary flushes the NAL unit.
    bool          fua_end_marker  = false;


    // ---- H.265 (RFC 7798) FU reassembly state ---------------------------
    //
    // H.265's FU packet carries a *two-byte* payload header and a
    // one-byte FU header; the original NAL unit type travels in the FU
    // header's FuType field.  The reassembled NAL unit therefore starts
    // with the original two-byte NAL header, which the FU payload header
    // does *not* carry (RFC 7798 搂4.3.3: the FU header replaces the
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
    // fragmentation (Z / Y), and re-emits the OBUs in Annex-B framing 鈥?    // the form the AV1 decoder expects.
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
    // RFC 4588: a retransmission has arrived, so the packet is no longer
    // missing.  Erase it from the pending-NACK list.
    //
    // Without this the receiver would keep NACKing a packet it already
    // has 鈥?the peer would keep retransmitting it, and the loop would
    // run until the packet fell out of the retransmission cache.  This
    // is the "the restored number must be used *everywhere* the
    // original would have been" requirement of the plan.
    // ---------------------------------------------------------------------
    void erase_pending_nack(std::uint16_t seq) noexcept {
        for (auto it = pending_nacks.begin(); it != pending_nacks.end(); ++it) {
            if (*it == seq) {
                pending_nacks.erase(it);
                return;
            }
        }
    }

    // ---------------------------------------------------------------------
    // Hand one complete NAL unit to the jitter buffer.
    //
    // The bitstream is Annex-B, and every NAL unit in it is delimited by
    // exactly the start code that preceded the *RTP packet it arrived in*
    // - nothing more.  The sender emitted each packet as an independent
    // payload, and its receiver is the party that knows where a NAL unit
    // begins and ends (RFC 6184 s5.1 carries bare NAL units).  For the
    // common single-NAL, single-packet case that framing is
    // byte-for-byte what the sender was handed (`00 00 00 01 | nalu`),
    // so only a packet that was actually reassembled (FU-A) or unpacked
    // (STAP-A) needs the start code synthesised around it.
    // ---------------------------------------------------------------------
    void insert_h264_annex_b(std::uint16_t                      seq,
                             std::uint32_t                      rtp_ts,
                             bool                               marker,
                             std::int64_t                       arrival_us,
                             const std::vector<std::uint8_t>&   bytes) noexcept {
        if (bytes.empty()) return;
        video_jb::InboundPacket pkt{};
        pkt.ssrc          = cfg.ssrc;
        pkt.seq           = seq;
        pkt.rtp_timestamp = rtp_ts;
        pkt.arrival_us    = arrival_us;
        pkt.marker        = marker;
        pkt.payload_type  = cfg.payload_type;
        pkt.codec         = video_frame::CodecKind::kH264;
        pkt.payload       = core::ByteSpan{bytes.data(), bytes.size()};
        jb->insert(pkt);
    }

    /// Append `00 00 00 01` + @p nalu to @p out.
    static void append_annex_b(std::vector<std::uint8_t>&  out,
                               core::ByteSpan              nalu) noexcept {
        out.insert(out.end(), kAnnexBStartCode,
                   kAnnexBStartCode + sizeof(kAnnexBStartCode));
        out.insert(out.end(), nalu.begin(), nalu.end());
    }

    // Scratch for the Annex-B NAL units of one packet.  Cleared, never
    // destroyed, so the steady state performs no allocation.
    std::vector<std::uint8_t> rx_annex_b_scratch;

    /** Hand one H.264 RTP payload to the jitter buffer in Annex-B framing.
     *
     *  The bitstream the sender was handed is Annex-B, and a single NAL
     *  unit that already carries its start code is forwarded byte for
     *  byte: the jitter buffer only *concatenates* the packets of a
     *  picture, so the framing is the sender's.  A payload without a
     *  start code gets one; an unexercised FU-A fragment is handed on
     *  as-is (the jitter buffer reassembles it). */
    void insert_h264_payload_annex_b(core::ByteSpan payload,
                                     std::uint32_t rtp_ts,
                                     bool          marker,
                                     std::uint16_t seq,
                                     std::int64_t  arrival_us) noexcept {
        const bool framed =
            payload.size() >= 4 && payload[0] == 0x00 && payload[1] == 0x00 &&
            payload[2] == 0x00 && payload[3] == 0x01;
        if (cfg.h264_validate_nalus) {
            // Validate every NAL unit the packet carries.  A unit whose
            // header is a reserved type means the bytes are not what the
            // sender wrote (a hole stitched by the jitter buffer, or a
            // misplaced start code): the packet is dropped whole, because
            // half a picture is not a picture.  The check costs one byte
            // per unit and only runs when the caller asked for it.
            bool bad = false;
            if (framed) {
                std::size_t o = 0;
                while (!bad && o + 4 < payload.size()) {
                    const std::uint8_t type = payload[o + 4] & kNaluTypeMask;
                    if (type == 0 || (type >= 24 && type <= 31)) {
                        bad = true;
                        break;
                    }
                    // The next Annex-B start code, or the payload's end.
                    std::size_t next = o + 5;
                    while (next + 3 < payload.size() &&
                           !(payload[next] == 0x00 && payload[next + 1] == 0x00 &&
                             payload[next + 2] == 0x00 && payload[next + 3] == 0x01)) {
                        ++next;
                    }
                    o = next;
                }
            } else if (!payload.empty()) {
                const std::uint8_t type = payload[0] & kNaluTypeMask;
                bad = (type == 0 || (type >= 24 && type <= 31));
            }
            if (bad) {
                stats.nalus_dropped_invalid++;
                return;
            }
        }
        const bool fu_a = !payload.empty() &&
                          (payload[0] & kNaluTypeMask) == kFuAType;
        rx_annex_b_scratch.clear();
        if (framed || fu_a) {
            rx_annex_b_scratch.insert(rx_annex_b_scratch.end(),
                                      payload.begin(), payload.end());
        } else {
            append_annex_b(rx_annex_b_scratch, payload);
        }
        insert_h264_annex_b(seq, rtp_ts, marker, arrival_us,
                            rx_annex_b_scratch);
    }

    /// Flush the completed FU-A reassembly as one Annex-B NAL unit.
    ///
    /// Only the reassembled NAL unit is emitted 鈥?the picture's other
    /// NAL units were emitted by their own packets and the jitter buffer
    /// concatenates them behind this one in sequence order.
    void flush_fua(std::uint8_t  nri,
                   std::uint8_t  nalu_type,
                   core::ByteSpan body,
                   std::uint16_t start_seq,
                   std::uint32_t rtp_ts,
                   bool          marker,
                   std::int64_t  arrival_us) noexcept {
        fua_in_progress = false;
        if (body.empty()) {
            // Empty reassembly; nothing to emit.  This can happen if a
            // middle/end fragment arrived without a start 鈥?drop silently.
            return;
        }
        rx_annex_b_scratch.clear();
        rx_annex_b_scratch.insert(rx_annex_b_scratch.end(), kAnnexBStartCode,
                                  kAnnexBStartCode + sizeof(kAnnexBStartCode));
        // The FU header replaces the original NAL header (RFC 6184
        // 搂5.8): synthesise it from the start fragment's NRI and the
        // FU header's inner type.
        rx_annex_b_scratch.push_back(static_cast<std::uint8_t>(
            ((nri & 0x03) << 5) | (nalu_type & kNaluTypeMask)));
        rx_annex_b_scratch.insert(rx_annex_b_scratch.end(),
                                  body.begin(), body.end());
        insert_h264_annex_b(start_seq, rtp_ts, marker, arrival_us,
                            rx_annex_b_scratch);
    }
    // ---------------------------------------------------------------------
    // Flush the current H.265 FU buffer as a single NALU into the JB.
    //
    // RFC 7798 搂4.3.3: the FU payload header carries the *wrapper* type
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
        nalu.reserve(sizeof(kAnnexBStartCode) + 2 + fu_h265_buf.size());
        nalu.insert(nalu.end(), kAnnexBStartCode,
                    kAnnexBStartCode + sizeof(kAnnexBStartCode));

        // The two-byte NAL unit header (RFC 7798 搂1.1.4):
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
    //
    // `seq` is the *effective* sequence number 鈥?the one the packet is
    // accounted under.  For an ordinary packet that is its own sequence
    // number; for a retransmission (RFC 4588) it is the **restored
    // original** number, because everything downstream (the jitter
    // buffer's frame assembly, the NACK bookkeeping) is keyed on the
    // original's sequence space.
    // ---------------------------------------------------------------------
    void handle_payload(core::ByteSpan payload,
                        bool          marker,
                        std::uint32_t rtp_ts,
                        std::uint16_t seq,
                        std::int64_t  arrival_us) noexcept {
        if (cfg.codec == video_frame::CodecKind::kH264) {
            const NaluType nt = peek_nalu_type(payload);
            // A new picture (different RTP timestamp) started before the
            // in-flight FU-A slice completed: that slice never finished,
            // so its partial bytes are dropped rather than framed as a
            // complete NAL unit the decoder would parse half-way.  The
            // timestamp is remembered so the packets that follow (they
            // carry the new picture's) are not mistaken for fragments of
            // the dropped one.
            if (fua_in_progress && fua_au != 0 && rtp_ts != fua_rtp_ts) {
                fua_in_progress = false;
                fua_buf.clear();
                fua_end_marker  = false;
            }
            if (nt == static_cast<NaluType>(kFuAType) && cfg.expect_fu_a) {
                FuAHeader h;
                if (!parse_fu_a_header(payload, h)) return;
                const bool start = (h.fu_header & 0x80) != 0;
                const bool end   = (h.fu_header & 0x40) != 0;
                const std::uint8_t inner = h.fu_header & kNaluTypeMask;
                const std::uint8_t nri   = (h.fu_indicator & kNriMask) >> 5;

                stats.fua_fragments_seen++;
                const std::uint8_t inner_type = static_cast<std::uint8_t>(
                    inner & kNaluTypeMask);
                const auto au_of = [&]() {
                    // The access unit a NAL unit belongs to.  Everything
                    // between a picture's first slice NAL and its last
                    // shares one RTP timestamp, so it is the same AU.
                    switch (inner_type) {
                        case 6: case 7: case 8: case 9:
                        case 14: case 15: case 16: case 17: case 18:
                            return std::uint8_t{0};
                        default:
                            return std::uint8_t{1};
                    }
                };
                const auto au = au_of();

                // Parameter sets and an end-of-sequence NAL can share the
                // picture's RTP timestamp.  They belong to the *next*
                // access unit's decoder state, so they are emitted at
                // once, ahead of the slice they precede - a decoder that
                // starts on the SPS would otherwise never see it, because
                // the slice's fragments own the marker bit.
                //
                // A slice NAL must wait for the picture's end (a
                // different timestamp, or the last fragment's marker
                // bit): only then are all of its fragments in.  A new
                // slice start while one is still in flight means the
                // in-flight one never completed - it is dropped, not
                // framed half-way.
                if (au != fua_au || (au != 0 && start)) {
                    const bool flush = fua_in_progress &&
                                       (au != fua_au ||
                                        rtp_ts != fua_rtp_ts);
                    if (flush) {
                        flush_fua(fua_nri, fua_nalu_type,
                                  core::ByteSpan{fua_buf.data(),
                                                 fua_buf.size()},
                                  fua_start_seq, fua_rtp_ts,
                                  fua_end_marker, arrival_us);
                    }
                    fua_buf.clear();
                }
                if (start) {
                    fua_in_progress = true;
                    fua_nalu_type   = inner;
                    fua_nri         = nri;
                    fua_rtp_ts      = rtp_ts;
                    fua_start_seq   = seq;
                    fua_au          = au;
                    fua_end_marker  = false;
                }
                if (!fua_in_progress) {
                    // Stray continuation (no start fragment seen for this
                    // picture); drop.
                    return;
                }
                fua_buf.insert(fua_buf.end(),
                               h.fragment.begin(),
                               h.fragment.end());
                if (end && au != 0) {
                    flush_fua(fua_nri, fua_nalu_type,
                              core::ByteSpan{fua_buf.data(),
                                             fua_buf.size()},
                              fua_start_seq, fua_rtp_ts,
                              marker, arrival_us);
                    fua_buf.clear();
                    fua_end_marker  = false;
                } else if (end) {
                    // A parameter set: emit it now, so it lands in the
                    // frame *before* the slice (see above).
                    flush_fua(fua_nri, fua_nalu_type,
                              core::ByteSpan{fua_buf.data(),
                                             fua_buf.size()},
                              fua_start_seq, fua_rtp_ts,
                              false, arrival_us);
                    fua_buf.clear();
                } else if (au != 0 && marker) {
                    fua_end_marker = true;
                }
                return;
            }
            if (nt == static_cast<NaluType>(kStapAType)) {
                stats.stap_a_packets_seen++;
                auto stap = parse_stap_a(payload);
                if (!stap.parsed_ok || stap.nalus.empty()) return;
                // Unpack the aggregation here: the jitter buffer
                // concatenates a frame's payloads into one Annex-B
                // stream, and the length prefixes would land inside it.
                // Each aggregated NAL unit thus gets its own start code,
                // exactly like a FU-A-reassembled one.
                rx_annex_b_scratch.clear();
                for (const auto& n : stap.nalus) {
                    append_annex_b(rx_annex_b_scratch,
                                   core::ByteSpan{n.data.data(), n.data.size()});
                }
                insert_h264_annex_b(seq, rtp_ts, marker, arrival_us,
                                    rx_annex_b_scratch);
                return;
            }
            // A single NAL unit packet (and the FU-A fallback when the
            // receiver is not reassembling) falls through to the generic
            // path below.  When the sender was declared Annex-B the
            // payload is re-framed there (see
            // `insert_h264_payload_annex_b`); otherwise it is forwarded
            // byte for byte, the RFC 6184 §5.1 default.
            //
            // `h264_validate_nalus` refuses a unit whose header is a
            // reserved type *before* that re-framing: a stitched frame can
            // carry such a byte where a lost packet left a hole, and it
            // must not be given a start code that makes it look legitimate.
            if (cfg.h264_validate_nalus && !payload.empty()) {
                const std::size_t off =
                    (payload.size() >= 4 && payload[0] == 0 && payload[1] == 0 &&
                     payload[2] == 0 && payload[3] == 1) ? 4 :
                    (payload.size() >= 3 && payload[0] == 0 && payload[1] == 0 &&
                     payload[2] == 1) ? 3 : 0;
                if (off < payload.size()) {
                    const std::uint8_t type = payload[off] & kNaluTypeMask;
                    if (type == 0 || (type >= 24 && type <= 31)) {
                        stats.nalus_dropped_invalid++;
                        return;
                    }
                }
            }
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
                    fu_h265_start_seq   = seq;
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
                    concat.insert(concat.end(), kAnnexBStartCode,
                                  kAnnexBStartCode + sizeof(kAnnexBStartCode));
                    concat.insert(concat.end(), n.data.begin(), n.data.end());
                }
                video_jb::InboundPacket pkt{};
                pkt.ssrc          = cfg.ssrc;
                pkt.seq           = seq;
                pkt.rtp_timestamp = rtp_ts;
                pkt.arrival_us    = arrival_us;
                pkt.marker        = marker;
                pkt.payload_type  = cfg.payload_type;
                pkt.codec         = video_frame::CodecKind::kH265;
                pkt.payload       = core::ByteSpan{concat.data(), concat.size()};
                jb->insert(pkt);
                return;
            }
            // Single NAL unit packet (types 0..47) 鈥?pass through.
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
            pkt.seq           = seq;
            pkt.rtp_timestamp = rtp_ts;
            pkt.arrival_us    = arrival_us;
            pkt.marker        = marker;
            pkt.payload_type  = cfg.payload_type;
            pkt.codec         = video_frame::CodecKind::kAV1;
            pkt.payload       = core::ByteSpan{annex_b.data(), annex_b.size()};
            jb->insert(pkt);
            return;
        }

        // Generic: VP8, VP9, or a single NAL unit of H.264 / H.265.
        //
        // A single NAL unit packet carries exactly one NAL unit, framed
        // (for the Annex-B codecs) exactly as the sender was handed it:
        // `00 00 00 01 | nalu`.  The jitter buffer concatenates the
        // packets of one picture, so the start codes are what delimit
        // the NAL units - re-adding one here would double the framing.
        //
        // The exception is an FU-A fragment: the receiver did not
        // reassemble it (`ReceiverConfig::expect_fu_a` false), so the
        // jitter buffer does the reassembly itself and puts the start
        // code on the reassembled NAL unit.  Prepending one here would
        // hide the FU indicator from the jitter buffer, which detects
        // the fragment by its NAL unit type.
        if (cfg.codec == video_frame::CodecKind::kH264 ||
            cfg.codec == video_frame::CodecKind::kH265) {
            const bool is_fu_fragment =
                (cfg.codec == video_frame::CodecKind::kH264 &&
                 peek_nalu_type(payload) == static_cast<NaluType>(kFuAType)) ||
                (cfg.codec == video_frame::CodecKind::kH265 &&
                 video_payload::h265::peek_payload_type(payload) ==
                     static_cast<H265NaluType>(kH265FuType));

            if (!is_fu_fragment) {
                if (cfg.codec == video_frame::CodecKind::kH264 &&
                    cfg.h264_annex_b) {
                    // The sender was declared to produce an Annex-B
                    // stream, and the frame contract is Annex-B: a unit
                    // that already carries its start code is forwarded
                    // as-is, a bare one (RFC 6184 §5.1 — what a sender
                    // that was handed an Annex-B frame still emits on the
                    // wire) gets one, exactly like a reassembled FU-A
                    // does.  Without this, a frame assembled from a bare
                    // SPS + PPS + slice reaches the decoder as one
                    // unparsable blob.
                    //
                    // With the flag off the receiver keeps the RFC 6184
                    // default: a single NAL unit packet is forwarded
                    // byte for byte, bare or framed.
                    insert_h264_payload_annex_b(payload, rtp_ts, marker,
                                                seq, arrival_us);
                    return;
                }
                // H.265 (and H.264 with the default framing): pass the NAL
                // unit through untouched.  Its start code, if the sender
                // put one there, is part of the framing.
                video_jb::InboundPacket pkt{};
                pkt.ssrc          = cfg.ssrc;
                pkt.seq           = seq;
                pkt.rtp_timestamp = rtp_ts;
                pkt.arrival_us    = arrival_us;
                pkt.marker        = marker;
                pkt.payload_type  = cfg.payload_type;
                pkt.codec         = cfg.codec;
                pkt.payload       = payload;
                jb->insert(pkt);
                return;
            }

            // A FU-A fragment the receiver did not reassemble is handed to
            // the jitter buffer as-is: it detects the fragment by its NAL
            // unit type, and re-frames the reassembled NAL unit itself.
            video_jb::InboundPacket pkt{};
            pkt.ssrc          = cfg.ssrc;
            pkt.seq           = seq;
            pkt.rtp_timestamp = rtp_ts;
            pkt.arrival_us    = arrival_us;
            pkt.marker        = marker;
            pkt.payload_type  = cfg.payload_type;
            pkt.codec         = cfg.codec;
            pkt.payload       = payload;
            jb->insert(pkt);
            return;
        }

        // VP8 / VP9: the raw payload, no NAL framing.
        {
            video_jb::InboundPacket pkt{};
            pkt.ssrc          = cfg.ssrc;
            pkt.seq           = seq;
            pkt.rtp_timestamp = rtp_ts;
            pkt.arrival_us    = arrival_us;
            pkt.marker        = marker;
            pkt.payload_type  = cfg.payload_type;
            pkt.codec         = cfg.codec;
            pkt.payload       = payload;
            jb->insert(pkt);
        }
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
                // v1.1 Q3: picture data was lost 鈥?ask the sender for a
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
    // with now_us; for fragments it stays at now_us 鈥?close enough for
    // the JB's purpose.
    s.handle_payload(packet.payload, packet.marker,
                     packet.timestamp, packet.seq, now_us);

    s.drain_frames(now_us);
    return true;
}

void VideoReceiver::set_rtx_payload_type(std::uint8_t rtx_pt) noexcept {
    impl_->cfg.rtx_payload_type = rtx_pt;
}

bool VideoReceiver::push_rtp_rtx(const rtp::PacketView& packet,
                                 std::int64_t now_us) noexcept {
    auto& s = *impl_;

    // The RTX path is only live when a retransmission payload type was
    // negotiated.  Without one, a packet arriving on an unknown payload
    // type is not ours to unwrap.
    if (s.cfg.rtx_payload_type == 0) return false;

    // RFC 4588 搂4.1: the retransmission payload is the two-octet OSN
    // header followed by the original packet's payload.  The header is
    // exactly two octets 鈥?there is no marker byte in front of it (see
    // the module's header comment), so a peer that sent a marker would
    // have its sequence number restored 0x9000 too large.
    const auto parsed = rtx::parse_payload(packet.payload);
    if (!parsed.ok()) {
        s.stats.rtx_packets_malformed++;
        return false;
    }

    const std::uint16_t osn = parsed.value().osn;
    const core::ByteSpan original = parsed.value().original;

    s.stats.rtx_packets_received++;
    s.stats.packets_pushed++;

    // ---- The restored sequence number is what everything else sees ----
    //
    // The retransmission stream has its own sequence space (RFC 4588
    // 搂4), so `packet.seq` is *not* the number the payload belongs
    // under.  The OSN is.  Everything downstream is keyed on the
    // original's sequence space:
    //
    //   * the jitter buffer's frame assembly (a frame's packets are
    //     grouped by RTP timestamp, and the *sequence* decides which
    //     packet of the frame this is);
    //   * the NACK bookkeeping 鈥?the retransmitted packet must be
    //     *erased* from the pending-NACK list, or the receiver will ask
    //     for it again, and again, and again.
    //
    // The retransmission is a *late* arrival, so it must NOT go through
    // `track_seq()`: the gap detector would see an old sequence number
    // and ignore it (correct), but the erasure of the pending NACK is
    // the part that matters and it is done explicitly below.

    // Erase the restored sequence number from the pending NACK list:
    // the packet has arrived, so it is no longer missing.
    s.erase_pending_nack(osn);

    // The frame's RTP timestamp and marker are the *original's*
    // (RFC 4588 搂4.1), so the payload lands in the frame it belongs to.
    s.handle_payload(original, packet.marker, packet.timestamp, osn, now_us);

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
    s.fua_au          = 0;
    s.fua_end_marker  = false;
    s.fu_h265_in_progress = false;
    s.fu_h265_buf.clear();
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

    // ---- The key-frame storm limiter (remote-desktop plan B14) ----------
    //
    // The limiter sits between "a key frame was asked for" and "the
    // encoder is armed".  A burst of PLIs therefore produces ONE key
    // frame, not N 鈥?and the counters say how many were granted, held
    // back and dropped.
    //
    // The limiter is built around the `FirCoalescer`, which is the
    // mechanism plan B14 says was "finally wired into the engine": the
    // coalescer owns the per-media-SSRC interval gate (RFC 5104
    // 搂4.3.1.1) and the limiter consults it.
    video_rtcp_feedback::FirCoalescer coalescer{0};
    KeyframeLimiter limiter{coalescer, KeyframeLimiterConfig{}};

    // ---- The incremental (dirty-rect) encode driver (plan B9) -----------
    //
    // The driver owns the controller (the arbiter) and does the three
    // things that have to happen together for the mechanism to be worth
    // anything: it asks the controller what to do with the frame's dirty
    // rectangles, it pushes the resulting request to the encoder, and it
    // builds the region-refresh message the peer needs.
    IncrementalEncodeDriver incremental;

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

    // ---- The RFC 4588 retransmission stream (the sender side) ----------
    //
    // With RTX negotiated, a NACK is answered by sending the original
    // packet's payload on the *retransmission* payload type, behind the
    // two-octet OSN header (RFC 4588 搂4.1).  Without it, the sender
    // re-sends the original packet as-is 鈥?the pre-RTX behaviour.
    //
    // The RTX stream has its own sequence space (RFC 4588 搂4): a
    // retransmission does NOT reuse the original's sequence number, so
    // the sender keeps a separate counter for it.
    SenderRtxCallback         rtx_cb;
    bool                      rtx_enabled = false;

    /// The next sequence number of the retransmission stream.
    std::uint16_t             rtx_next_seq = 0;

    /// The scratch buffer the OSN header and the original payload are
    /// assembled in.  Pre-sized to the MTU + the header, so answering a
    /// NACK performs no allocation.
    std::vector<std::uint8_t> rtx_scratch;

    // ---- Reusable packetization scratch (Phase 3 / C-class) ---------------
    //
    // The sender used to build a fresh set of vectors for every frame:
    // the NAL unit list, the "does this NAL need FU-A" flags, the FU-A
    // fragment groups, the STAP-A group, the STAP-A span list and the
    // STAP-A output buffer.  On a 30 fps video path that is a steady
    // stream of short-lived heap allocations 鈥?the last thing a real-time
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
        // The coalescer is per-media-SSRC (RFC 5104 搂4.3.1.1), so it
        // must know which stream it is guarding.
        coalescer = video_rtcp_feedback::FirCoalescer(cfg.ssrc);
        KeyframeLimiterConfig lim_cfg{};
        lim_cfg.media_ssrc = cfg.ssrc;
        limiter = KeyframeLimiter(coalescer, lim_cfg);
        rtx_cache.resize(cfg.retransmit_cache_packets);
        // Pre-size every retransmission slot to the MTU so caching a packet
        // is a memcpy into an existing buffer, not a fresh allocation.
        // Without this the RTX ring allocated one buffer per outgoing
        // packet 鈥?a per-frame allocation on the hot path.
        for (CachedPacket& slot : rtx_cache) {
            slot.payload.reserve(cfg.mtu);
        }
        // Reserve the packetization scratch vectors up-front.  Their
        // sizes depend on the *content* of the frame (how many NAL units
        // it has), so without a reservation the first frame that carries
        // more NAL units than any before it grows the vector 鈥?a
        // one-off allocation that shows up as a per-frame allocation in
        // the benchmark whenever the payload happens to contain a
        // start-code-like byte sequence.
        nalus.reserve(kMaxScratchNalus);
        group.reserve(kMaxScratchNalus);
        use_fua.reserve(kMaxScratchNalus);
        ap_group.reserve(kMaxScratchNalus);
        scratch.reserve(cfg.mtu);
        // The RTX scratch holds the two-octet OSN header plus a whole
        // original payload, so it is a little larger than the MTU.
        rtx_scratch.reserve(cfg.mtu + rtx::kHeaderSize);
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

    /** Fragment one oversized H.264 NAL unit into FU-A packets.
     *
     *  The marker goes on the fragment that carries the NAL unit's last
     *  payload byte, and only when @p is_last_nalu says this unit is the
     *  frame's last (RFC 6184 §5.1: the marker marks the frame, not the
     *  unit). */
    core::Result<void> emit_fua(core::ByteSpan n, std::uint32_t rtp_ts,
                                bool is_last_nalu) noexcept {
        const std::uint8_t nalu_type = n.empty() ? 0 : (n[0] & kNaluTypeMask);
        const std::uint8_t nri       = n.empty() ? 0 : ((n[0] & kNriMask) >> 5);
        const std::size_t  body_size = n.size() > 1 ? n.size() - 1 : 0;
        if (body_size == 0) {
            // Header-only NAL unit: nothing to fragment.
            emit(n, rtp_ts, is_last_nalu);
            return core::Result<void>::make_ok();
        }
        const std::size_t max_body_per_frag = cfg.mtu > 2 ? cfg.mtu - 2 : 0;
        if (max_body_per_frag == 0) {
            return core::Result<void>::fail(
                core::ErrorCode::InvalidArgument,
                "video_pipeline: the MTU is too small for FU-A");
        }
        std::size_t offset = 0;
        while (offset < body_size) {
            const std::size_t remaining = body_size - offset;
            const std::size_t this_len =
                remaining > max_body_per_frag ? max_body_per_frag : remaining;
            const bool start = (offset == 0);
            const bool end   = (offset + this_len == body_size);
            const core::ByteSpan body_slice{n.data() + 1 + offset, this_len};
            if (fua_buf.size() < cfg.mtu) fua_buf.resize(cfg.mtu);
            const std::size_t wrote = build_fu_a(
                static_cast<NaluType>(nalu_type),
                body_slice,
                start,
                end,
                core::MutableByteSpan{fua_buf.data(), fua_buf.size()});
            if (wrote == 0) {
                return core::Result<void>::fail(
                    core::ErrorCode::InternalError,
                    "video_pipeline: build_fu_a failed");
            }
            // Patch the NRI on the indicator byte (build_fu_a defaults to
            // NRI=0).  Indicator layout: F(1) NRI(2) Type(5) -- the type
            // must stay 28.
            fua_buf[0] = static_cast<std::uint8_t>(
                (fua_buf[0] & ~kNriMask) | ((nri & 0x03) << 5));
            emit(core::ByteSpan{fua_buf.data(), wrote}, rtp_ts,
                 /*marker=*/end && is_last_nalu);
            offset += this_len;
        }
        stats.fua_frames_packetized++;
        return core::Result<void>::make_ok();
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
        // ---- The marker bit (RFC 6184 搂5.1) --------------------------------
        //
        // The marker marks the LAST packet of a *frame* 鈥?that is, the
        // packet that carries the frame's LAST NAL unit.  It does **not**
        // mark the last packet of every NAL group.
        //
        // Getting this wrong is not cosmetic.  The jitter buffer groups
        // the packets of a frame by RTP timestamp and uses the marker bit
        // to decide that a frame is complete, so a marker on an
        // intermediate packet makes the receiver emit a *truncated* frame
        // and then discard everything that arrives after it.
        //
        // The case that bites is the classic NVENC output: an IDR frame
        // that carries `SPS | PPS | <oversized IDR slice>`.  The SPS and
        // the PPS are small, so they are aggregated into one STAP-A
        // packet, while the slice is fragmented into FU-A packets.  The
        // STAP-A is *not* the last packet of the frame 鈥?but a marker
        // there made the receiver emit "SPS + PPS" as if it were the
        // whole picture, and the IDR slice that followed was dropped
        // because its frame had already been played out.
        //
        // `last_nalu` is therefore the index of the frame's last NAL unit:
        // the marker goes on the packet that carries it.
        const std::size_t last_nalu = nalus.size() - 1;

        // Walk through nalus; for each, either emit FU-A fragments OR
        // try to bundle with neighbouring small NALUs into a STAP-A.
        //
        // The marker always goes on the packet that carries the frame's
        // LAST bytes, whether that is the last fragment of the last
        // slice or a trailing parameter set.
        std::size_t i = 0;
        while (i < nalus.size()) {
            if (use_fua[i]) {
                if (auto rc = emit_fua(nalus[i], rtp_ts, i == last_nalu); !rc) {
                    return rc;
                }
                ++i;
                continue;
            }

            // Try STAP-A aggregation starting at i: pack as many small
            // NALUs as fit within mtu - 1 (STAP-A header) - 2*N (lengths).
            std::size_t budget = cfg.mtu > 1 ? cfg.mtu - 1 : 0;
            group.clear();
            std::size_t j = i;
            while (j < nalus.size() && !use_fua[j]) {
                if (budget < 2 + nalus[j].size()) break;
                budget -= 2 + nalus[j].size();
                group.push_back(nalus[j]);
                ++j;
                // The next NAL unit is a FU-A'd slice: a parameter set
                // starts a new access unit and must reach the receiver
                // before the slice's first fragment, so the group stops
                // here and goes out as its own packet ahead of it.
                if (j < nalus.size() && use_fua[j]) break;
            }

            if (group.size() >= 2) {
                // Emit a STAP-A packet.
                if (group_out.size() < cfg.mtu) group_out.resize(cfg.mtu);
                const std::size_t wrote = build_stap_a(
                    group.data(), group.size(),
                    core::MutableByteSpan{group_out.data(), group_out.size()});
                // The group spans the NAL units [i, j) -- the marker goes
                // on the packet only when the group carries the frame's
                // LAST NAL unit (see the marker note above).
                const bool group_is_last = (j - 1 == last_nalu);
                if (wrote == 0) {
                    // STAP-A build failed (e.g. too big).  Fall back to
                    // single NALUs one by one, and hand any unit that
                    // does not fit a packet to the FU-A fragmenter
                    // instead of emitting it truncated.
                    for (std::size_t k = 0; k < group.size(); ++k) {
                        const std::size_t idx = i + k;
                        if (group[k].size() > cfg.mtu) {
                            if (auto rc = emit_fua(nalus[idx], rtp_ts,
                                                   idx == last_nalu); !rc) {
                                return rc;
                            }
                        } else {
                            emit(group[k], rtp_ts, /*marker=*/(idx == last_nalu));
                            stats.single_nal_frames_packetized++;
                        }
                    }
                } else {
                    emit(core::ByteSpan{group_out.data(), wrote}, rtp_ts,
                         /*marker=*/group_is_last);
                    stats.stapa_frames_packetized++;
                }
                i = j;
                continue;
            }

            // group.size() == 1: emit single NALU.
            if (group.size() == 1) {
                // The RFC 6184 single-NAL packet format carries one NAL
                // unit and one packet holds at most MTU bytes, so a unit
                // that does not fit is fragmented rather than emitted
                // whole (an oversized "single NAL" packet would be split
                // by the transport without a NAL boundary in sight).
                if (group[0].size() > cfg.mtu) {
                    if (auto rc = emit_fua(group[0], rtp_ts, i == last_nalu); !rc) {
                        return rc;
                    }
                    i = j;
                    continue;
                }
                emit(group[0], rtp_ts, /*marker=*/(i == last_nalu));
                stats.single_nal_frames_packetized++;
                i = j;
                continue;
            }

            // group empty -- shouldn't happen, but be defensive.
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
                // unit header (RFC 7798 搂1.1.4).
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
        // We don't override frame.is_keyframe 鈥?the caller is responsible
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
            // H.264 鈥?only the NAL unit header differs (two bytes).
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

void VideoSender::set_keyframe_limiter_config(
        KeyframeLimiterConfig cfg) noexcept {
    impl_->limiter.configure(cfg);
}

KeyframeLimiterConfig VideoSender::keyframe_limiter_config() const noexcept {
    return impl_->limiter.config();
}

KeyframeDecision VideoSender::request_keyframe(KeyframeReason reason,
                                                std::int64_t now_us,
                                                bool carries_parameter_sets) noexcept {
    auto& s = *impl_;
    const KeyframeDecision decision =
        s.limiter.request(reason, now_us, carries_parameter_sets);

    // Mirror the limiter's counters into the sender's stats, so a host
    // that only reads the sender's counters still sees the limiter
    // working.
    const KeyframeLimiterStats ls = s.limiter.stats();
    s.stats.keyframe_requests = ls.requests;
    s.stats.keyframes_granted = ls.granted;
    s.stats.keyframes_deferred = ls.deferred;
    s.stats.keyframes_rejected = ls.rejected;
    s.stats.keyframes_forced  = ls.forced_grants;

    if (decision == KeyframeDecision::kGranted) {
        // Arm the encoder-side marker.  The *encoder* produces the IDR;
        // the packetizer cannot.
        s.keyframe_pending = true;
        s.stats.force_keyframe_calls++;
    }
    return decision;
}

KeyframeLimiterStats VideoSender::keyframe_limiter_stats() const noexcept {
    return impl_->limiter.stats();
}

bool VideoSender::keyframe_request_pending() const noexcept {
    return impl_->limiter.pending();
}

std::uint32_t VideoSender::retry_pending_keyframe(std::int64_t now_us) noexcept {
    auto& s = *impl_;
    if (!s.limiter.pending()) return 0;

    // Re-offer the held request.  The limiter arbitrates again: if the
    // minimum interval still has not elapsed, the request stays held
    // and the next tick tries again 鈥?which is the whole point of
    // *holding* rather than dropping it.
    const KeyframeDecision decision =
        s.limiter.request(KeyframeReason::kPli, now_us);

    // Mirror the limiter's counters into the sender's stats, so a host
    // that only reads the sender's counters still sees the limiter
    // working.
    const KeyframeLimiterStats ls = s.limiter.stats();
    s.stats.keyframe_requests = ls.requests;
    s.stats.keyframes_granted = ls.granted;
    s.stats.keyframes_deferred = ls.deferred;
    s.stats.keyframes_rejected = ls.rejected;
    s.stats.keyframes_forced  = ls.forced_grants;

    if (decision != KeyframeDecision::kGranted) return 0;

    // The grant is published through the *same* path an inbound PLI
    // takes, so the engine has one place that arms the encoder.
    s.keyframe_pending = true;
    s.stats.force_keyframe_calls++;
    return 1;
}

// ---------------------------------------------------------------------------
// The incremental (dirty-rect) encode mechanism (remote-desktop plan B9)
// ---------------------------------------------------------------------------

void VideoSender::set_content_type(VideoContentType type) noexcept {
    impl_->incremental.controller().set_content_type(type);
}

VideoContentType VideoSender::content_type() const noexcept {
    return impl_->incremental.controller().content_type();
}

void VideoSender::set_incremental_encode_enabled(bool enabled) noexcept {
    impl_->incremental.controller().set_enabled(enabled);
}

bool VideoSender::incremental_encode_enabled() const noexcept {
    return impl_->incremental.controller().enabled();
}

IncrementalDecision VideoSender::offer_dirty_region(
        const std::vector<plugins::VideoRegion>& dirty,
        bool full_frame) noexcept {
    // The driver pushes the request to the encoder and builds the
    // region-refresh message; the controller counts the decision.
    return impl_->incremental.on_frame_dirty(dirty, full_frame);
}

core::ByteSpan VideoSender::take_region_refresh() noexcept {
    const core::ByteBuffer& buf = impl_->incremental.last_region_refresh();
    if (buf.empty()) return {};
    return core::ByteSpan{buf.data(), buf.size()};
}

IncrementalEncodeStats VideoSender::incremental_stats() const noexcept {
    return impl_->incremental.stats();
}

void VideoSender::set_incremental_encoder(plugins::IVideoEncoder* encoder) noexcept {
    impl_->incremental.set_encoder(encoder);
}

void VideoSender::set_region_refresh_ssrcs(std::uint32_t media_ssrc,
                                            std::uint32_t feedback_ssrc) noexcept {
    impl_->incremental.set_media_ssrc(media_ssrc);
    impl_->incremental.set_feedback_ssrc(feedback_ssrc);
}

bool VideoSender::retransmit(std::uint16_t seq) noexcept {
    auto& s = *impl_;
    const Impl::CachedPacket* p = s.find_cached(seq);
    if (!p) {
        s.stats.packets_retransmit_missed++;
        return false;
    }

    // ---- The RFC 4588 path ---------------------------------------------
    //
    // With RTX negotiated the retransmission travels on the
    // *retransmission* payload type, wrapped in the two-octet OSN
    // header (RFC 4588 搂4.1), so the receiver can tell it apart from
    // the original and restore the original's sequence number.
    //
    // The RTP header of the retransmission is the original's except for
    // the sequence number (RFC 4588 搂4.1) 鈥?so the *timestamp* and the
    // *marker bit* are the original's, and the payload is the OSN
    // header followed by the original's payload.
    if (s.rtx_enabled && s.rtx_cb) {
        const std::size_t needed =
            rtx::payload_size_for(core::ByteSpan{p->payload.data(),
                                                 p->payload.size()});
        if (s.rtx_scratch.size() < needed) s.rtx_scratch.resize(needed);
        const std::size_t wrote = rtx::build_payload(
            core::MutableByteSpan{s.rtx_scratch.data(), s.rtx_scratch.size()},
            p->seq,
            core::ByteSpan{p->payload.data(), p->payload.size()});
        if (wrote == 0) {
            s.stats.packets_retransmit_missed++;
            return false;
        }

        SenderRtxPacket out;
        out.payload       = core::ByteSpan{s.rtx_scratch.data(), wrote};
        out.rtp_timestamp = p->rtp_ts;
        out.marker        = p->marker;
        out.original_seq  = p->seq;
        // RFC 4588 搂4: the retransmission stream has its own sequence
        // number space, so the packet does not reuse the original's
        // number on the wire.
        out.rtx_seq       = s.rtx_next_seq;
        s.rtx_next_seq    = static_cast<std::uint16_t>(s.rtx_next_seq + 1);
        s.rtx_cb(out);

        s.stats.rtx_packets_emitted++;
        s.stats.packets_retransmitted++;
        return true;
    }

    // ---- The pre-RTX path -----------------------------------------------
    //
    // Re-emit with the ORIGINAL sequence number / timestamp / marker so
    // the receiver can splice the packet back into the frame it belongs
    // to.  RFC 4588 搂4.1 says this is *not* RTX 鈥?the packet keeps its
    // own payload type, so a receiver that multiplexes by payload type
    // cannot tell it from the original.
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

void VideoSender::set_rtx_packet_callback(SenderRtxCallback cb) noexcept {
    impl_->rtx_cb = std::move(cb);
}

void VideoSender::set_rtx_enabled(bool enabled) noexcept {
    impl_->rtx_enabled = enabled;
}

bool VideoSender::rtx_enabled() const noexcept {
    return impl_->rtx_enabled;
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

std::int64_t monotonic_now_us() noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

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
    return apply_feedback_rtcp(sender, rtcp, monotonic_now_us());
}

core::Result<int> apply_feedback_rtcp(VideoSender& sender,
                                      core::ByteSpan rtcp,
                                      std::int64_t now_us) noexcept {
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
            // The key-frame storm limiter (plan B14) arbitrates: a burst
            // of PLIs produces ONE key frame, not N.  The request is
            // still counted as an action 鈥?the feedback *was* handled 鈥?            // but only a granted request arms the encoder.
            (void)sender.request_keyframe(KeyframeReason::kPli, now_us);
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
            (void)sender.request_keyframe(KeyframeReason::kFir, now_us);
            ++actions;
        } else if (pt == fb::kAppPt && fb::is_region_refresh(packet)) {
            // A region-refresh message (plan B9) travels from the sender
            // to the peer, so the *receiving* side is what acts on it.
            // The video pipeline's sender has nothing to do with it 鈥?            // but it must not be mistaken for a protocol error.
        }
        // Other feedback kinds (REMB, transport-cc, 鈥? are ignored on
        // purpose 鈥?the video pipeline has no use for them.  The same
        // holds for the non-feedback RTCP packets (SR / RR / SDES) that
        // share a compound datagram with a NACK: they are skipped, not
        // treated as a protocol error.

        offset += packet_size;
    }

    return core::Result<int>::ok(actions);
}

} // namespace nimrtc::video_pipeline
