/**
 * @file src/engine/src/engine_inbound_state.hpp
 * @brief PRIVATE engine collaborator: the per-source inbound RTP
 *        bookkeeping (plan Track S / S2).
 *
 * Architecture remediation plan §4.3 (D1 — engine split): the inbound
 * bookkeeping belongs to one feature — "what the peer sent us, per
 * source" — so it lives in one named collaborator rather than as loose
 * members of `Impl`.
 *
 * This header is PRIVATE: it is not installed and no public header
 * includes it (Layout Invariant 4).
 *
 * ## Why the state is per-SSRC
 *
 * The engine's flat `Stats` mixes every inbound stream into one set
 * of counters.  That is the right shape for a single peer stream and
 * the wrong one for anything else: a peer that sends **simulcast**
 * (RFC 5956 — several encodings of the same picture on different
 * SSRCs) puts three different qualities into one counter, and a host
 * that wants "how many packets arrived on SSRC X" — the question a
 * loss investigation actually asks — has no way to ask it.
 *
 * The SSRC is the only thing on the wire that identifies a *source*.
 * The payload type says how to *decode* a packet, not who sent it, and
 * a peer may reuse a payload type for several sources.
 *
 * ## The counters
 *
 * The packet and byte counts are the engine's own: the engine sees
 * every inbound packet on the RTP path, so it counts them where it
 * routes them.
 *
 * The *loss* figure is different: the loss of a source is a property
 * of its **sequence space**, and only the receive pipeline sees that
 * space (the engine does not track the video sequence numbers — the
 * jitter buffer and the assembler do).  So the loss counters come
 * from the pipeline's own snapshot rather than being re-derived
 * here.
 *
 * ## Threading
 *
 * The engine serialises its work on the caller's thread.  A host that
 * polls `stats()` from another thread reads a *copy* of the entries
 * (the projection is taken under the caller's own serialisation), so
 * no lock is needed for the counters themselves.
 */

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "nimrtc/engine/engine.hpp"

namespace nimrtc::engine::detail {

/** One inbound RTP source, as the engine accounts for it. */
struct InboundSource {
    /** The source's SSRC. */
    std::uint32_t ssrc = 0;

    /** The payload type the source's packets arrived with (observed). */
    std::uint8_t payload_type = 0;

    /** What the source carries: "video", "audio" or "unknown". */
    std::string kind = "unknown";

    /** The RTP packets received from this source. */
    std::uint64_t packets_received = 0;

    /** The RTP bytes received from this source (header + payload). */
    std::uint64_t bytes_received = 0;

    /** The NACK entries emitted for this source. */
    std::uint64_t nacks_sent = 0;

    /** The frames assembled from this source's packets. */
    std::uint64_t frames_received = 0;

    /** The frames that arrived but could not be assembled. */
    std::uint64_t frames_incomplete = 0;

    /** The source's loss figure, in packets.
     *
     *  Taken from the receive pipeline, which is the only party that
     *  sees the source's sequence space.  Zero when no pipeline has
     *  reported for this source yet. */
    std::uint64_t packets_lost = 0;

    /** The source's interarrival jitter, in milliseconds. */
    std::uint32_t jitter_ms = 0;

    /** The source's current playout delay, in milliseconds. */
    std::uint32_t playout_delay_ms = 0;
};

/** The inbound RTP bookkeeping: one entry per source. */
struct InboundState {
    /**
     * @brief Note a packet that arrived on @p ssrc.
     *
     * Creates the entry on first sight.  The counters are cumulative
     * for the life of the source.
     *
     * @param ssrc         The source's SSRC.
     * @param payload_type The payload type the packet arrived with.
     * @param payload_bytes The RTP payload's size in bytes.  The
     *                      header's size is added by the caller, which
     *                      is the only party that knows it.
     * @param kind         What the source carries ("video" / "audio").
     */
    void note_packet(std::uint32_t ssrc,
                     std::uint8_t payload_type,
                     std::size_t payload_bytes,
                     std::string_view kind) {
        if (ssrc == 0) return;
        InboundSource& src = sources_[ssrc];
        if (src.ssrc == 0) {
            src.ssrc = ssrc;
            src.payload_type = payload_type;
        }
        // The payload type is *observed*: a peer may change it after a
        // re-INVITE (RFC 3264 §5.1), and the latest reading is the
        // one that describes the stream now.
        src.payload_type = payload_type;
        if (kind != "unknown") src.kind = std::string(kind);
        ++src.packets_received;
        src.bytes_received += payload_bytes;
    }

    /** The entry for @p ssrc, or nullptr when the source is unknown. */
    InboundSource* find(std::uint32_t ssrc) {
        auto it = sources_.find(ssrc);
        return it == sources_.end() ? nullptr : &it->second;
    }

    /** The entry for @p ssrc, or nullptr when the source is unknown. */
    const InboundSource* find(std::uint32_t ssrc) const {
        auto it = sources_.find(ssrc);
        return it == sources_.end() ? nullptr : &it->second;
    }

    /** The source @p ssrc, created on first sight. */
    InboundSource& source_for(std::uint32_t ssrc) {
        return sources_[ssrc];
    }

    /**
     * @brief Note a frame assembled from @p ssrc's packets.
     *
     * The frame count is the engine's own: the receiver plugin tells
     * the engine that a frame arrived, and the engine knows which
     * source the stream belongs to.
     */
    void note_frame(std::uint32_t ssrc) {
        if (ssrc == 0) return;
        ++source_for(ssrc).frames_received;
    }

    /**
     * @brief Note the NACK entries emitted for @p ssrc.
     *
     * @param ssrc  The source the retransmission requests were about.
     * @param count How many sequence numbers were requested.
     */
    void note_nacks(std::uint32_t ssrc, std::size_t count) {
        if (ssrc == 0 || count == 0) return;
        source_for(ssrc).nacks_sent += count;
    }

    /** Every known source, in SSRC order. */
    const std::map<std::uint32_t, InboundSource>& sources() const noexcept {
        return sources_;
    }

private:
    // `std::map`, not `unordered_map`: the stats list is read far more
    // often than it is written, and a stable order (by SSRC) makes
    // the published list deterministic — a host that diffs two
    // readings must not see the entries swap places.
    std::map<std::uint32_t, InboundSource> sources_;
};

}  // namespace nimrtc::engine::detail
