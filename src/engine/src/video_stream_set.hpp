/**
 * @file src/engine/src/video_stream_set.hpp
 * @brief PRIVATE engine collaborator: the video **stream set**
 *        (remote-desktop plan H1 / H2).
 *
 * This header is PRIVATE: it is not installed and no public header
 * includes it (Layout Invariant 4).
 *
 * ## Why a stream set
 *
 * The engine used to hold **exactly one** of each video plugin
 * (`video_source_ / video_sink_ / video_receiver_ / video_sender_ /
 * video_codec_`) plus a single `video_sender_tuning.ssrc`.  A
 * remote-desktop product needs more than one video stream: the screen,
 * the camera, and often a low-resolution thumbnail of the screen.
 *
 * The plan's H1 row asks for a "stream set keyed by a stable stream id
 * (SSRC + mid)", and H2 asks that "each stream owns its own encoder,
 * sender, receiver, decoder and sink, resolved through the registry by
 * id".
 *
 * ## The design: a hybrid, additive refactor
 *
 * The refactor is **incremental and revertible** (the plan's R-3 risk
 * row): the public API change is additive, so the single-stream call
 * sites keep working.
 *
 *   - the **default stream** (id `"default"`) keeps using the engine's
 *     own plugin members (`video_codec_`, `video_sender_`, ...).  Those
 *     members are read by ~160 call sites across six translation
 *     units; migrating them into the set in one step would be a large,
 *     risky diff that touches files other work is also editing.
 *   - **every additional stream** owns its own plugin instances, which
 *     the set resolves through the registry exactly like the default
 *     stream does.
 *
 * The set therefore *tracks* every stream (the default one included) so
 * that iteration, statistics and routing have a single view, while the
 * default stream's plugins stay where they were.
 *
 * ## The stream id
 *
 * `VideoStreamId` is a `std::string` — a **stable, human-readable**
 * identifier ("default", "screen", "camera", "thumbnail").
 *
 * It has to be stable across a reconfiguration (the whole point of the
 * plan's "keyed by a stable stream id"), and it has to survive a
 * process restart for a host that persists its configuration.  An
 * integer index would satisfy neither: removing a stream would renumber
 * the others, and the id would carry no meaning to a host that reads it
 * back from a configuration file.  The id is mapped to an **SSRC** (the
 * thing the wire identifies the stream by) and to a **mid** (the SDP
 * m-line that carries it) when the stream is created.
 *
 * ## Simulcast (H3) — deferred, but the room is there
 *
 * A stream may later carry **several encodings** of the same source at
 * different resolutions (the simulcast case).  The model leaves room
 * for it: the per-stream state below is the *source* state (the id, the
 * SSRC, the mid), and the encoding list of a stream is a list of
 * encoders, not a single encoder.  When H3 is implemented, the extra
 * encodings join the list and the stream's SSRC becomes the *first*
 * encoding's SSRC (the primary one the peer picks by default).
 *
 * Until then, a stream carries exactly one encoding, and the plan's
 * "thumbnail" case is served by an **independent low-resolution
 * capture** — i.e. a second stream — which is exactly what this set
 * supports today.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/plugins/video_bitrate_controller.hpp>
#include <nimrtc/plugins/video_codec.hpp>
#include <nimrtc/plugins/video_pipeline.hpp>
#include <nimrtc/plugins/video_sink.hpp>
#include <nimrtc/plugins/video_source.hpp>

#include "video_codec_config.hpp"

namespace nimrtc::engine::detail {

/** The stable identifier of a video stream.
 *
 *  A `std::string`, not an integer: the id must survive a
 *  reconfiguration (see the file header), must be readable back from a
 *  host's configuration file, and must mean something to a human
 *  looking at a log line. */
using VideoStreamId = std::string;

/** The id of the session's default (single) video stream.
 *
 *  The legacy single-stream path — `send_video(frame)` and the flat
 *  `Stats::video_*` fields — is this stream.  The overloads
 *  `send_video(stream_id, frame)` etc. accept it too, so a host can
 *  move to the multi-stream API without changing behaviour. */
inline constexpr std::string_view kDefaultVideoStreamId = "default";

/** One video stream's own state.
 *
 *  Everything that used to be a *global* video plugin slot is a member
 *  here — per stream.  The default stream's plugin instances are not
 *  owned here (they live in the engine's own members, which the set
 *  borrows); every other stream owns its instances. */
struct VideoStream {
    // ---- Identity -------------------------------------------------------

    /** The stable stream id ("default", "screen", "camera", ...). */
    VideoStreamId id;

    /** The RTP SSRC of the stream's (primary) encoding.
     *
     *  The SSRC is allocated when the stream is created and never
     *  changes for the lifetime of the stream: the peer identifies the
     *  stream on the wire by it. */
    std::uint32_t ssrc = 0;

    /** The RTP payload type of the stream's encoding.
     *
     *  Allocated from the dynamic range so that two streams of the same
     *  codec family can coexist. */
    std::uint8_t payload_type = 0;

    /** The SDP mid of the m-line that carries the stream.
     *
     *  Empty until the stream is signalled: the multi-stream SDP
     *  (a second video m-line) is a follow-up increment — see the file
     *  header. */
    std::string mid;

    /** The plugin id of the stream's codec ("h264", "av1", ...). */
    std::string codec_name;

    /** The codec family the stream's codec reports. */
    VideoCodecFamily codec_family = VideoCodecFamily::kUnknown;

    /** The identifier of the backend the stream's codec runs on
     *  ("nvenc", "openh264", "stub_h264", ...). */
    std::string backend_id;

    // ---- The stream's own plugin instances -----------------------------

    /** The stream's encoder / decoder (both directions). */
    std::unique_ptr<plugins::IVideoCodec> codec;

    /** The stream's RTP packetizer. */
    std::unique_ptr<plugins::IVideoSender> sender;

    /** The stream's RTP depacketizer + jitter buffer. */
    std::unique_ptr<plugins::IVideoReceiver> receiver;

    /** The stream's decoded-frame consumer. */
    std::unique_ptr<plugins::IVideoSink> sink;

    /** The stream's frame producer (the host may drive it directly). */
    std::unique_ptr<plugins::IVideoSource> source;

    /** The stream's rate controller (the ladder). */
    std::unique_ptr<plugins::IVideoBitrateController> controller;

    // ---- The stream's configurations ------------------------------------

    /** The configuration the stream's codec was created with. */
    plugins::VideoCodecConfig codec_cfg{};

    /** The configuration the stream's sender was created with. */
    plugins::VideoSenderConfig sender_cfg{};

    /** The configuration the stream's receiver was created with. */
    plugins::VideoReceiverConfig receiver_cfg{};

    /** The parameter set currently in force for the stream's encoder. */
    plugins::VideoEncoderParams encoder_params{};

    /** True once a parameter set has been pushed to the stream's codec. */
    bool encoder_params_set = false;

    /** True when the next encoded frame of this stream is a keyframe. */
    bool keyframe_pending = false;

    // ---- The ladder ------------------------------------------------------

    /** The rung of the ladder the stream's controller currently wants. */
    std::uint32_t ladder_level = 0;

    /** The bitrate the stream's share of the session budget allows. */
    std::uint32_t share_bps = 0;

    /** The stream's weight in the bandwidth split (plan D5).
     *
     *  The elastic pool left after the reserved audio is divided
     *  between the video streams in proportion to these weights.  The
     *  default is 1, so an unconfigured session splits the pool evenly.
     *
     *  A weight of 0 means "this stream takes nothing from the pool" —
     *  it is neither starved nor does it dilute the other streams'
     *  shares (the rule ignores a zero weight entirely). */
    std::uint32_t weight = 1;

    // ---- The per-stream counters ----------------------------------------

    /** Frames the host pushed through this stream. */
    std::uint64_t frames_sent = 0;

    /** Frames this stream received (assembled by its receiver). */
    std::uint64_t frames_received = 0;

    /** RTP packets this stream emitted. */
    std::uint64_t packets_sent = 0;

    /** RTP bytes this stream emitted (header + payload). */
    std::uint64_t bytes_sent = 0;

    /** Keyframes this stream emitted. */
    std::uint64_t keyframes_sent = 0;

    /** Frames this stream's encoder produced. */
    std::uint64_t frames_encoded = 0;

    // ---- The stream's scratch buffers -----------------------------------

    /** The reusable buffer the stream's encoder writes into. */
    std::vector<std::uint8_t> enc_buf;

    /** The reusable buffer the stream's RTP header is serialised into. */
    std::vector<std::uint8_t> rtp_scratch;

    /** The reusable buffer the stream's outbound packet is built in
     *  (the no-scheduler path). */
    std::vector<std::uint8_t> pkt_scratch;

    // ---- Bookkeeping -----------------------------------------------------

    /** When the stream was created (the engine's monotonic clock). */
    std::int64_t created_us = 0;
};

/** The engine's set of video streams.
 *
 *  The set owns every stream **except** the default one, whose plugin
 *  instances live in the engine's own members (see the file header for
 *  why).  The default stream is still *tracked* here, so that the
 *  iteration, the statistics and the inbound routing have a single
 *  view. */
struct VideoStreamSet {
    /** The extra streams (everything except the default one). */
    std::vector<std::unique_ptr<VideoStream>> streams;

    /** The offset of the last allocated SSRC.
     *
     *  The extra streams start above the *configured* SSRC (which
     *  belongs to the default stream), so two streams never share one.
     *
     *  The counter is a member of the set rather than a static: a
     *  static would be global mutable state (L6) shared by every engine
     *  in the process, and two engines would then hand out the same
     *  SSRC. */
    std::uint32_t next_ssrc_offset = 0;

    /** The offset of the last allocated payload type (see above). */
    std::uint8_t next_payload_type_offset = 0;

    /** When the set was started (the engine's monotonic clock).
     *
     *  The per-stream rates are measured against it: a rate is "bytes
     *  since the stream started, divided by the time since then", so
     *  the number is well defined from the first frame on. */
    std::int64_t started_us = 0;

    /** The **default** stream's share of the session budget, in bps.
     *
     *  The default stream's plugin instances live in the engine's own
     *  members (see the file header), so its share is kept here rather
     *  than in a `VideoStream`.  The pacing split writes it, and the
     *  rate controller reads it: the controller is handed the stream's
     *  *share* of the estimate, not the whole session estimate (that is
     *  what `IVideoBitrateController::on_bandwidth_estimate()`'
     *  contract says — "the engine has already applied the per-stream
     *  share rule before calling this").
     *
     *  Zero means "not computed yet" (pacing off, or no estimate). */
    std::uint32_t default_share_bps = 0;

    /** The **default** stream's weight in the bandwidth split (plan D5).
     *
     *  The default stream's plugin instances live in the engine's own
     *  members, so its weight lives here for the same reason its share
     *  does.  The default is 1 (an even split), matching the extra
     *  streams' default weight. */
    std::uint32_t default_weight = 1;

    /** Mark the set as started (called at open()). */
    void mark_started(std::int64_t now_us) noexcept {
        if (started_us == 0) started_us = now_us;
    }

    /** Find an extra stream by id (nullptr when there is none). */
    [[nodiscard]] VideoStream* find(std::string_view id) noexcept {
        for (auto& s : streams) {
            if (s->id == id) return s.get();
        }
        return nullptr;
    }

    /** Find an extra stream by id (const overload). */
    [[nodiscard]] const VideoStream* find(std::string_view id) const noexcept {
        for (const auto& s : streams) {
            if (s->id == id) return s.get();
        }
        return nullptr;
    }

    /** Find an extra stream by its RTP payload type. */
    [[nodiscard]] VideoStream* find_by_payload_type(
        std::uint8_t payload_type) noexcept {
        for (auto& s : streams) {
            if (s->payload_type == payload_type) return s.get();
        }
        return nullptr;
    }

    /** True iff the given SSRC is already taken by a stream. */
    [[nodiscard]] bool ssrc_in_use(std::uint32_t ssrc) const noexcept {
        for (const auto& s : streams) {
            if (s->ssrc == ssrc) return true;
        }
        return false;
    }

    /** True iff the given payload type is already taken by a stream. */
    [[nodiscard]] bool payload_type_in_use(std::uint8_t payload_type) const noexcept {
        for (const auto& s : streams) {
            if (s->payload_type == payload_type) return true;
        }
        return false;
    }

    /** The ids of every active stream — the default one first. */
    [[nodiscard]] std::vector<VideoStreamId> ids() const {
        std::vector<VideoStreamId> out;
        out.reserve(streams.size() + 1);
        out.emplace_back(kDefaultVideoStreamId);
        for (const auto& s : streams) out.push_back(s->id);
        return out;
    }
};

}  // namespace nimrtc::engine::detail
