/**
 * @file src/engine/src/engine_scratch.hpp
 * @brief PRIVATE engine collaborator: the reusable send-path buffers.
 *
 * Architecture remediation plan §4.3 (D1 — engine split): these buffers
 * used to live as eleven loose members inside `NimRTCEngine::Impl`.
 *
 * This header is PRIVATE: it is not installed and no public header
 * includes it (Layout Invariant 4).
 *
 * ## Why they are grouped
 *
 * Every buffer here exists for the same reason: the steady-state send
 * path must not allocate.  Before Phase 3 the engine allocated a fresh
 * `std::vector` per media packet — one heap allocation per frame,
 * forever — and the fix was to give each path a reusable buffer whose
 * capacity grows monotonically.  Grouping them makes the invariant
 * visible: "the send path allocates nothing" is a property of *this
 * struct*, not of a scattering of members.
 *
 * ## Lifetime
 *
 * The buffers are read *before* the packet is copied into the
 * scheduler's pooled outbound buffer, so a scratch never escapes the
 * call that filled it.  The DTMF scratch is deliberately separate from
 * the audio scratch: a host may press a key from a different thread
 * than the one feeding `send_audio()`, and sharing one buffer would
 * introduce a data race that did not exist when every call allocated
 * its own packet.
 */
#pragma once

#include <cstdint>
#include <vector>

#include <nimrtc/rtp/rtcp.hpp>
#include <nimrtc/video_frame/frame.hpp>

namespace nimrtc::engine::detail {

/** The reusable send-path buffers. */
struct ScratchBuffers {
    // ---- send_audio scratch buffers (P0#4) ----------------------------
    // Reusable per-engine buffers for the audio capture→encode→RTP→SRTP
    // path.  Capacity grows monotonically across calls, avoiding the heap
    // allocations that a fresh `std::vector` per call would incur.
    //
    // `audio_codec_pkt` holds the raw codec (Opus) output for the in-flight
    // frame.  The outbound (SRTP-protected) packet is NOT kept here: it is
    // taken from the scheduler's packet pool and handed back to the pool
    // once the packet has been drained (P1 hardening, 2026-11-09).  Keeping
    // it in a member and MOVING it into the scheduler would leave the
    // member moved-from, so every packet would allocate a fresh buffer.
    std::vector<std::uint8_t> audio_codec_pkt;

    // Scratch buffer for the capture-side PCM.  The 3A stage processes audio
    // in place, but send_audio() receives the caller's buffer as `const
    // float*` — writing through a const_cast would be undefined behaviour and
    // would silently mutate the caller's data.  The engine therefore owns a
    // copy: the caller's buffer is read-only, the copy is what 3A mutates and
    // what the encoder consumes.
    std::vector<float> audio_pcm_buf;

    // ---- send_video scratch buffer (P0#4) -----------------------------
    // Reusable buffer for the encoded H.264 frame.  Default reserve size
    // matches the worst-case 64 KiB IDR; resize grows monotonically.
    std::vector<std::uint8_t> video_enc_buf;

    // ---- RTP serialisation scratch buffers (Phase 3 / C4) --------------
    //
    // `rtp::PacketBuilder::build()` allocates a fresh buffer per call.  On
    // the send path that is one heap allocation per media packet, which is
    // the dominant allocation cost of a steady 50 fps audio + 30 fps video
    // stream.  `build_into()` serialises into caller-owned storage instead,
    // so these two scratch buffers absorb the whole cost: they are reused
    // for every packet and only ever grow.
    std::vector<std::uint8_t> audio_rtp_scratch;
    std::vector<std::uint8_t> video_rtp_scratch;

    // ---- Outbound packet buffers (Phase 3) ----------------------------
    //
    // When the engine has a scheduler, the outbound packet buffer comes
    // from the scheduler's pool (`video_pkt_pooled`) and is handed back
    // with `enqueue_owned()`.  When there is NO scheduler (the "transport"
    // profile — the engine writes straight to ICE), the packet is built
    // in the engine's own scratch buffer and handed to the transport's
    // `send()`, which writes it out synchronously — the bytes do not
    // escape the call, so a reusable buffer is safe.
    //
    // Before this, the no-scheduler path allocated a fresh vector per
    // outbound packet: one heap allocation per frame, forever.
    std::vector<std::uint8_t> video_pkt_pooled;
    std::vector<std::uint8_t> video_pkt_scratch;
    std::vector<std::uint8_t> audio_pkt_pooled;
    std::vector<std::uint8_t> audio_pkt_scratch;

    /// Scratch for the DTMF (RFC 4733) packet.
    ///
    /// DTMF has its own scratch rather than sharing the audio one: a host
    /// may press a key from a different thread than the one feeding
    /// `send_audio()`, and sharing one buffer would introduce a data race
    /// that did not exist when every call allocated its own packet.
    std::vector<std::uint8_t> dtmf_rtp_scratch;

    /// Reusable destination for the inbound RTCP demultiplexer.
    ///
    /// `rtp::demux_rtcp()` used to return a fresh `std::vector` per
    /// inbound datagram.  The scratch holds the blocks of the datagram
    /// currently being processed; it is cleared and refilled on every
    /// call, so its capacity is the only thing that persists.
    std::vector<rtp::RtcpBlock> rtcp_blocks_scratch;

    /// Pool the staged video frames come from (Phase 3 / C1).
    ///
    /// `stage_source_frame()` used to allocate a fresh frame-sized block
    /// for every frame the host pushed through `send_video()`.  The pool
    /// recycles the storage instead, so the steady-state send path
    /// performs no allocation and no full-frame memset.
    video_frame::VideoFrameBufferPool video_frame_pool;

    // Reusable buffer for the outbound SRTCP packet (avoid a per-tick
    // allocation; SR is 1 Hz by default so this is not hot, but tick() is
    // called at the transport cadence).
    std::vector<std::uint8_t> sr_send_buf;
};

}  // namespace nimrtc::engine::detail
