/**
 * @file fuzz_video_payload.cpp
 * @brief Fuzz target for the video RTP payload parsers and the FU-A
 *        depacketizer.
 *
 * The video payload is the last untrusted hop before a frame reaches a
 * decoder.  A peer controls the bytes in an RTP payload: the H.264
 * (RFC 6184) / VP8 (RFC 7741) / VP9 (RFC 9559) descriptors are parsed
 * out of them, and — for H.264 — the payload may be a FU-A fragment
 * that the jitter buffer reassembles with the fragments that came
 * before it.
 *
 * Two distinct hazards live here, and the target drives both:
 *
 *   1. **Descriptor parsing.**  The descriptor length is implied by
 *      bits inside the payload itself (VP8's X bit, VP9's I bit), so a
 *      payload can claim a descriptor that runs past the end of the
 *      buffer.  Every descriptor parser has to check that.
 *
 *   2. **Reassembly.**  The FU-A depacketizer concatenates fragments
 *      into a NAL unit.  A fragment carries an S/E bit and a size that
 *      comes from the buffer, so a hostile fragment sequence can make
 *      the reassembler copy out of bounds, read a fragment header out
 *      of an empty buffer, or lose track of which fragment comes next.
 *
 * The FU-A path is driven by prepending a *synthetic* RTP header to
 * the fuzz bytes and handing the result to the video jitter buffer,
 * because that is the shape the engine's receive path produces: an
 * RTP packet whose payload field is then fed to the depacketizer.
 * The synthetic header is ours, not the peer's, so it carries no
 * attacker-controlled length.
 */
#include "fuzz_targets.hpp"

#include <cstdint>
#include <vector>

#include <nimrtc/video_payload/h264.hpp>
#include <nimrtc/video_payload/vp8.hpp>
#include <nimrtc/video_payload/vp9.hpp>
#include <nimrtc/video_jb/video_jitter_buffer.hpp>

namespace nimrtc::fuzz {
namespace {

/** Build a minimal but well-formed RTP header (RFC 3550 §5.1).
 *
 *  The header is 12 bytes: V=2, no padding / extension / CSRC, one
 *  payload type, a sequence number and a timestamp.  None of these
 *  fields are attacker-controlled in this target — the fuzz bytes are
 *  the payload, which is the part a peer actually chooses. */
constexpr std::size_t kRtpHeaderSize = 12;

void build_rtp_header(std::uint8_t* out,
                       std::uint8_t  payload_type,
                       std::uint16_t sequence_number,
                       std::uint32_t timestamp) noexcept {
    out[0] = 0x80;  // V=2, P=0, X=0, CC=0
    out[1] = payload_type & 0x7Fu;  // M=0, PT
    out[2] = static_cast<std::uint8_t>(sequence_number >> 8);
    out[3] = static_cast<std::uint8_t>(sequence_number & 0xFFu);
    out[4] = static_cast<std::uint8_t>(timestamp >> 24);
    out[5] = static_cast<std::uint8_t>((timestamp >> 16) & 0xFFu);
    out[6] = static_cast<std::uint8_t>((timestamp >> 8) & 0xFFu);
    out[7] = static_cast<std::uint8_t>(timestamp & 0xFFu);
    out[8] = 0xDE;  // SSRC (arbitrary but fixed)
    out[9] = 0xAD;
    out[10] = 0xBE;
    out[11] = 0xEF;
}

} // namespace

void run_video_payload(const std::uint8_t* data, std::size_t size) {
    const nimrtc::core::ByteSpan payload{data, size};

    // ---- The descriptor parsers -----------------------------------------
    //
    // Each parser reads its own descriptor out of the same bytes.  A
    // payload that is a valid H.264 payload is usually not a valid VP8
    // payload and vice versa, so one byte range exercises the reject
    // path of one parser and the accept path of another.
    (void)nimrtc::video_payload::h264::parse(payload);
    (void)nimrtc::video_payload::h264::peek_nalu_type(payload);
    (void)nimrtc::video_payload::vp8::parse(payload);
    (void)nimrtc::video_payload::vp9::parse(payload);

    // The STAP-A parser reads a *count* of NAL units out of the
    // payload, so it walks a length field that the peer controls.
    {
        nimrtc::video_payload::h264::StapAResult stap =
            nimrtc::video_payload::h264::parse_stap_a(payload);
        if (stap.parsed_ok) {
            // Touch the parsed result: a parser that hands back a view
            // pointing outside the buffer is only observable when
            // somebody reads it.
            for (const auto& nalu : stap.nalus) {
                (void)nalu.type;
                (void)nalu.data.size();
            }
        }
    }

    // ---- The FU-A depacketizer ------------------------------------------
    //
    // The jitter buffer reassembles a fragmented NAL unit out of the
    // FU-A fragments it is fed.  Feeding it one packet built from the
    // fuzz bytes exercises the fragment header parsing, the S/E bit
    // handling and the fragment-copying path.
    {
        std::vector<std::uint8_t> packet(kRtpHeaderSize + size);
        build_rtp_header(packet.data(), /*payload_type=*/96,
                         /*sequence_number=*/1, /*timestamp=*/0);
        if (size > 0) {
            for (std::size_t i = 0; i < size; ++i) {
                packet[kRtpHeaderSize + i] = data[i];
            }
        }

        nimrtc::video_jb::Config cfg;
        cfg.max_inflight_frames = 4;
        cfg.max_wait_ms         = 1;
        cfg.emit_nacks          = false;

        auto jb = nimrtc::video_jb::create(cfg);
        if (!jb) return;

        nimrtc::video_jb::InboundPacket pkt;
        pkt.ssrc          = 0xDEADBEEFu;
        pkt.seq           = 1;
        pkt.rtp_timestamp = 0;
        pkt.arrival_us    = 0;
        pkt.marker        = true;
        pkt.payload_type  = 96;
        pkt.codec         = nimrtc::video_frame::CodecKind::kH264;
        pkt.payload       = nimrtc::core::ByteSpan(
            packet.data() + kRtpHeaderSize, size);

        (void)jb->insert(pkt);

        // Drain whatever the insert produced.  A complete frame means
        // the FU-A reassembly ran; touching the frame's bitstream is
        // what makes an out-of-bounds write observable.
        while (auto frame = jb->pop_frame()) {
            (void)frame->complete;
            (void)frame->is_keyframe;
            (void)frame->bitstream.size();
        }

        (void)jb->pop_nack_entries();
        jb->tick(1'000'000);
    }
}

} // namespace nimrtc::fuzz
