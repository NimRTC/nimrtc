/**
 * @file src/engine/src/engine_audio_stream_state.hpp
 * @brief PRIVATE engine collaborator: the per-stream audio bookkeeping
 *        (plan Track S / S4).
 *
 * Architecture remediation plan §4.3 (D1 — engine split): the audio
 * stream's identity and its counters belong to one feature — "what the
 * audio stream is, and what happened to it" — so they live in one named
 * collaborator rather than as loose members of `Impl`.
 *
 * This header is PRIVATE: it is not installed and no public header
 * includes it (Layout Invariant 4).
 *
 * ## What this entry is, and what it is not
 *
 * The entry describes **the engine's audio stream** — the audio track
 * the session carries, in both directions.  That is the counterpart of
 * `VideoStreamStats`, which describes the engine's video stream.
 *
 * It is deliberately **not** a second per-SSRC inbound table: Track S's
 * **S2** row already published `Stats::inbound_rtp_streams`, which
 * carries every inbound source — audio included — keyed by SSRC.  A
 * per-SSRC audio table here would publish the same packets twice, and a
 * host that summed both lists would count every packet twice.
 *
 * What the entry adds on top of S2 is the part S2 cannot know: the
 * **outbound** half of the audio stream (its identity, its bitrate, what
 * the engine put on the wire) and the **pipeline's** figures for the
 * source the receive side is actually working on.
 *
 * ## Why a list at all
 *
 * The audio plane is **single-stream today**: the engine owns one audio
 * sender and one audio receiver, and the flat `Stats::audio_*` counters
 * describe that one stream unambiguously.
 *
 * That is an accident of the current topology, not a property of the
 * interface.  A second audio source — a second microphone, a programme
 * bus, a mixed call leg — would make every flat counter unattributable,
 * and redrawing the whole statistics surface then would break every
 * caller at once.
 *
 * So the *shape* is built now, while the plane is still single-stream:
 * the state carries a stream **id** and the projection publishes a
 * **list**.  When the second stream arrives, the list simply grows.
 *
 * ## The counters
 *
 * The split follows the audio pipeline's own split (spec B-13):
 *
 *   - the **engine** counts what it handed to the host (frames, samples,
 *     concealed frames, FEC recoveries) and what it put on the wire
 *     (packets, bytes);
 *   - the **receive pipeline** counts what happened on the wire (packets
 *     lost, decode errors, jitter, playout delay), because it is the
 *     only party that sees the source's sequence space.
 *
 * ## Threading
 *
 * The engine serialises its work on the caller's thread.  A host that
 * polls `stats()` from another thread reads a *copy* of the entries, so
 * no lock is needed for the counters themselves.
 */

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "nimrtc/engine/engine.hpp"

namespace nimrtc::engine::detail {

/** The id of the engine's own audio stream. */
inline constexpr std::string_view kDefaultAudioStreamId = "default";

/** One inbound audio source, as the engine accounts for it. */
struct InboundAudioSource {
    /** The source's SSRC. */
    std::uint32_t ssrc = 0;

    /** The payload type the source's packets arrived with (observed). */
    std::uint8_t payload_type = 0;

    /** The encoding the payload type maps to ("opus", "pcmu", ...). */
    std::string codec_name;

    /** The media clock rate the source declared, in Hz. */
    std::uint32_t clock_rate_hz = 0;

    /** The channel count the source declared. */
    std::uint8_t channels = 0;

    /** The RTP packets received from the source. */
    std::uint64_t packets_received = 0;

    /** The RTP bytes received from the source (header + payload). */
    std::uint64_t bytes_received = 0;
};

/**
 * @brief The audio plane's per-stream bookkeeping.
 *
 * The engine's own stream is the *default* one, and it is always
 * present — a host that never asked for a second stream still gets one
 * entry in the published list.
 */
struct AudioStreamState {
    // ---- The engine's own stream (the "default" one) ------------------
    //
    // The outbound half.  The engine is the only party that knows what
    // it put on the wire, so these are the engine's own counters.
    struct OutboundCounters {
        /** The RTP packets the stream emitted. */
        std::uint64_t packets_sent = 0;

        /** The RTP bytes the stream emitted (header + payload). */
        std::uint64_t bytes_sent = 0;

        /** The frames that could not be encoded. */
        std::uint64_t encode_errors = 0;

        /** The PCM samples the host handed to the engine. */
        std::uint64_t samples_in = 0;

        /** The decoded frames the engine handed to the host. */
        std::uint64_t frames_received = 0;

        /** The PCM samples the engine handed to the host. */
        std::uint64_t samples_received = 0;

        /** The frames that were produced by packet-loss concealment. */
        std::uint64_t frames_concealed = 0;

        /** The frames that were reconstructed from in-band FEC. */
        std::uint64_t frames_fec_recovered = 0;
    } def;

    /** The inbound sources, keyed by SSRC.
     *
     *  The inbound side is per-*source* rather than per-*stream* because
     *  a peer may send several audio sources (a main stream and a
     *  comfort-noise stream, a second microphone).  The receive pipeline
     *  reports one aggregate, so the engine keeps the per-source
     *  identity to attribute that aggregate to the right source. */
    std::map<std::uint32_t, InboundAudioSource> inbound;

    /** When the state was started (the engine's monotonic clock).
     *
     *  The per-stream bitrate is "bytes since the stream started,
     *  divided by the time since then", so the number is well defined
     *  from the first frame on. */
    std::int64_t started_us = 0;

    /** Mark the state as started (called at open()). */
    void mark_started(std::int64_t now_us) noexcept {
        if (started_us == 0) started_us = now_us;
    }

    /** The entry for the inbound source @p ssrc, created on first sight. */
    InboundAudioSource& source_for(std::uint32_t ssrc) {
        return inbound[ssrc];
    }

    /**
     * @brief Note an inbound packet that arrived on @p ssrc.
     *
     * @param ssrc          The source's SSRC.
     * @param payload_type  The payload type the packet arrived with.
     * @param payload_bytes The RTP payload's size in bytes.  The
     *                      header's size is added by the caller, which
     *                      is the only party that knows it.
     * @param codec_name    The encoding the payload type maps to.
     */
    void note_inbound_packet(std::uint32_t ssrc,
                             std::uint8_t payload_type,
                             std::size_t payload_bytes,
                             std::string_view codec_name) {
        if (ssrc == 0) return;
        InboundAudioSource& src = inbound[ssrc];
        if (src.ssrc == 0) {
            src.ssrc = ssrc;
            src.payload_type = payload_type;
            src.codec_name = std::string(codec_name);
        }
        // The payload type is *observed*: a peer may change it after a
        // re-INVITE (RFC 3264 §5.1), and the latest reading is the one
        // that describes the stream now.
        src.payload_type = payload_type;
        if (!codec_name.empty()) src.codec_name = std::string(codec_name);
        ++src.packets_received;
        src.bytes_received += payload_bytes;
    }
};

}  // namespace nimrtc::engine::detail
