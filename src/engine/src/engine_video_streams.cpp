/**
 * @file src/engine/src/engine_video_streams.cpp
 * @brief The multi-stream video plane (remote-desktop plan H1 / H2 / H4).
 *
 * ## What this file is
 *
 * The engine used to hold **exactly one** of each video plugin
 * (`video_source_ / video_sink_ / video_receiver_ / video_sender_ /
 * video_codec_`).  A remote-desktop product needs more than one video
 * stream: the screen, the camera, and often a low-resolution thumbnail
 * of the screen.
 *
 * Plan H1 asks for a "stream set keyed by a stable stream id (SSRC +
 * mid)"; H2 asks that "each stream owns its own encoder, sender,
 * receiver, decoder and sink, resolved through the registry by id"; H4
 * asks that "the stats snapshot returns a **list** of per-stream
 * entries".
 *
 * ## The design: a hybrid, additive refactor
 *
 * The refactor is **incremental and revertible** (the plan's R-3 risk
 * row):
 *
 *   - the **default stream** (id `"default"`) keeps using the engine's
 *     own plugin members.  Those members are read by ~160 call sites
 *     across six translation units; migrating them into the set in one
 *     step would be a large, risky diff.
 *   - **every additional stream** owns its own plugin instances, which
 *     the set resolves through the registry exactly like the default
 *     stream does.
 *
 * The set therefore *tracks* every stream (the default one included) so
 * that iteration, statistics and routing have a single view, while the
 * default stream's plugins stay where they were.
 *
 * ## What is *not* here yet
 *
 * **H3 (simulcast) is deferred.**  The model leaves the room for it —
 * see the `VideoStream` doc comment in `video_stream_set.hpp` — but the
 * engine does not implement it: a stream carries exactly **one**
 * encoding today.  The thumbnail case the plan mentions is served by an
 * **independent low-resolution capture**, i.e. a second stream, which is
 * what this set supports.
 *
 * This file is PRIVATE: it is not installed and no public header
 * includes it (Layout Invariant 4).
 *
 * @see video_stream_set.hpp — the per-stream state.
 * @see docs/plan/remote-desktop-plan.md §Track H.
 */
#include "nimrtc/engine/engine.hpp"

#include "engine_impl.hpp"
#include "engine_video_params.hpp"
#include "engine_video_staging.hpp"
#include "video_stream_set.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <nimrtc/core/log.hpp>
#include <nimrtc/engine/pal/engine_plugin_resolver.hpp>
#include <nimrtc/rtp/packet.hpp>

namespace nimrtc::engine {

namespace {

/** Size of the SRTP authentication tag (RFC 3711 §3.5 — the default
 *  AES_CM_128_HMAC_SHA1_80 profile appends a 10-byte tag).
 *
 *  Used as the capacity hint when acquiring an outbound packet buffer
 *  from the scheduler's pool, so the protected packet fits without a
 *  reallocation. */
constexpr std::size_t kSrtpAuthTagBytes = 10;

/** The FNV-1a hash of a string (64-bit).
 *
 *  The extra streams' SSRCs and payload types are derived from the
 *  stream id rather than handed out from a counter, so the *same* id
 *  always maps to the same wire identity — across a reconfiguration, and
 *  across two engines that were configured from the same profile.
 *
 *  The hash is not a cryptographic one: it only has to spread the ids
 *  over the SSRC space.  The collision check below turns a hash
 *  collision into a linear probe, so a collision can never produce two
 *  streams with the same identity. */
std::uint64_t fnv1a(std::string_view text) noexcept {
    std::uint64_t h = 1469598103934665603ull;
    for (const char c : text) {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        h *= 1099511628211ull;
    }
    return h;
}

/** The default stream's payload type — the *effective* one.
 *
 *  It is the negotiated number when the description exchange settled on
 *  one, and the locally resolved number otherwise. */
std::uint8_t default_payload_type(const detail::VideoState& vstate,
                                  std::uint8_t configured) noexcept {
    const std::uint8_t negotiated =
        vstate.negotiated_video_pt.load(std::memory_order_relaxed);
    if (negotiated != 0) return negotiated;
    if (vstate.video_payload_type != 0) return vstate.video_payload_type;
    return configured;
}

/** The average bitrate of a byte counter over a time window.
 *
 *  The per-stream bitrate is "bytes since the stream started, divided
 *  by the time since then".  It is well defined from the first frame
 *  on, and it needs no window buffer — which matters because the
 *  per-stream counters live in the stream itself, not in a shared
 *  statistics sink. */
std::uint32_t average_bitrate_bps(std::uint64_t bytes,
                                   std::int64_t elapsed_us) noexcept {
    if (elapsed_us <= 0) return 0;
    const std::uint64_t bits = bytes * 8ull;
    const std::uint64_t bps = bits / static_cast<std::uint64_t>(elapsed_us);
    return bps > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<std::uint32_t>(bps);
}

/** The average keyframe rate of a keyframe counter over a time window. */
std::uint32_t average_keyframe_rate_hz(std::uint64_t keyframes,
                                        std::int64_t elapsed_us) noexcept {
    if (elapsed_us <= 0) return 0;
    const std::uint64_t hz =
        keyframes * 1'000'000ull / static_cast<std::uint64_t>(elapsed_us);
    return hz > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<std::uint32_t>(hz);
}

}  // namespace

// ---------------------------------------------------------------------------
// The stream views
// ---------------------------------------------------------------------------

NimRTCEngine::StreamView NimRTCEngine::default_stream_view() const noexcept {
    StreamView v;
    v.id           = std::string(detail::kDefaultVideoStreamId);
    v.ssrc         = config_.video_sender_tuning.ssrc;
    v.payload_type = impl_ != nullptr
        ? default_payload_type(impl_->video_state,
                               config_.video_sender_tuning.payload_type)
        : config_.video_sender_tuning.payload_type;
    v.mid          = impl_ != nullptr ? impl_->session_state.base_video_mid
                                      : std::string();
    v.codec_name   = config_.video_codec_name;
    v.backend_id   = impl_ != nullptr
        ? std::string{video_codec_ ? video_codec_->backend_id() : "unknown"}
        : std::string("unknown");
    v.codec      = video_codec_.get();
    v.sender     = video_sender_.get();
    v.receiver   = video_receiver_.get();
    v.sink       = video_sink_.get();
    v.source     = video_source_.get();
    v.controller = video_bitrate_controller_.get();
    return v;
}

bool NimRTCEngine::stream_view(std::string_view stream_id,
                               StreamView& out) const noexcept {
    if (impl_ == nullptr) return false;

    if (stream_id == detail::kDefaultVideoStreamId) {
        out = default_stream_view();
        return true;
    }

    const detail::VideoStream* s = impl_->video_state.video_streams.find(stream_id);
    if (s == nullptr) return false;

    out.id           = s->id;
    out.ssrc         = s->ssrc;
    out.payload_type = s->payload_type;
    out.mid          = s->mid;
    out.codec_name   = s->codec_name;
    out.backend_id   = s->backend_id;
    out.codec        = s->codec.get();
    out.sender       = s->sender.get();
    out.receiver     = s->receiver.get();
    out.sink         = s->sink.get();
    out.source       = s->source.get();
    out.controller   = s->controller.get();
    return true;
}

// ---------------------------------------------------------------------------
// The RTP send path (the per-stream packet callback)
// ---------------------------------------------------------------------------

void NimRTCEngine::wire_stream_sender(detail::VideoStream* s) noexcept {
    if (s == nullptr || !s->sender) return;

    s->sender->set_packet_callback(
        [this, s](plugins::BufferView payload,
                  std::uint32_t rtp_ts,
                  std::uint16_t seq,
                  bool marker) {
            rtp::PacketBuilder pb;
            pb.set_ssrc(s->ssrc)
              .set_seq(seq)
              .set_timestamp(rtp_ts)
              .set_payload_type(s->payload_type)
              .set_marker(marker);

            // The abs-send-time extension (ADR-017) — the same seam the
            // default stream's callback stamps.
            if (config_.timeline_enabled) {
                const std::int64_t steady_us =
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        core::SteadyClock::now().time_since_epoch()).count();
                if (impl_->timeline.local_abs_send_time_id != 0) {
                    const std::uint32_t ast24 = rtp::encode_abs_send_time_24(
                        std::chrono::microseconds{steady_us},
                        impl_->timeline.monotonic_origin_ntp64);
                    pb.set_abs_send_time(rtp::AbsSendTime{ast24},
                                         impl_->timeline.local_abs_send_time_id);
                }
            }

            pb.set_payload(core::ByteSpan(payload.data(), payload.size()));

            const std::size_t built_size = pb.compute_size();
            if (s->rtp_scratch.size() < built_size) {
                s->rtp_scratch.resize(built_size);
            }
            const std::size_t built_written =
                pb.build_into(s->rtp_scratch.data(), s->rtp_scratch.size());
            if (built_written == 0) return;

            std::vector<std::uint8_t>& rtp_pkt = s->pkt_scratch;
            if (scheduler_) {
                rtp_pkt = scheduler_->acquire_packet_buffer(
                    built_written + kSrtpAuthTagBytes);
            }
            rtp_pkt.assign(s->rtp_scratch.begin(),
                           s->rtp_scratch.begin() +
                               static_cast<std::ptrdiff_t>(built_written));

            // ---- SRTP protect (per SSRC) ----------------------------------
            if (impl_->srtp_installed && impl_->srtp) {
                auto* sess = impl_->srtp->get_session(s->ssrc,
                                                       /*outgoing=*/true);
                if (!sess) return;
                core::ByteSpan span(rtp_pkt.data(), rtp_pkt.size());
                auto enc = sess->protect_rtp(span, s->ssrc, rtp_ts);
                if (!enc) return;
                rtp_pkt.assign(enc.value().data(),
                               enc.value().data() + enc.value().size());
            }

            // ---- The counters ---------------------------------------------
            ++s->packets_sent;
            s->bytes_sent += rtp_pkt.size();

            // ---- Enqueue ----------------------------------------------------
            plugins::Priority prio = plugins::Priority::kVideo;
            if (marker && s->keyframe_pending) {
                prio = plugins::Priority::kVideoKeyframe;
                s->keyframe_pending = false;
            }
            if (scheduler_) {
                scheduler_->enqueue_owned(prio, std::move(rtp_pkt),
                                          plugins::Addr{});
            } else if (ice_t_) {
                plugins::BufferView bv{rtp_pkt.data(), rtp_pkt.size()};
                ice_t_->send(bv, {});
            }
        });
}

// ---------------------------------------------------------------------------
// H1 — creating and destroying a stream
// ---------------------------------------------------------------------------

bool NimRTCEngine::add_video_stream(std::string_view stream_id,
                                    std::string_view codec_name) noexcept {
    if (impl_ == nullptr) return false;
    if (stream_id.empty()) return false;

    // The default stream already exists (it is the engine's own plugin
    // members), so it cannot be "added" — but it *can* be addressed by
    // its id, and the call is idempotent rather than an error.
    if (stream_id == detail::kDefaultVideoStreamId) {
        return true;
    }

    auto& set = impl_->video_state.video_streams;
    if (set.find(stream_id) != nullptr) return false;   // already there

    const std::string codec_id =
        codec_name.empty() ? config_.video_codec_name : std::string(codec_name);
    if (codec_id.empty()) return false;

    const auto* codec_factory = pal::resolve_video_codec(codec_id);
    if (codec_factory == nullptr) {
        core::log::Logger::instance().error(
            "engine[video]: no video codec plugin named '" + codec_id +
            "' — the stream was not created");
        return false;
    }

    // ---- The stream's own state ----------------------------------------
    auto stream = std::make_unique<detail::VideoStream>();
    stream->id           = std::string(stream_id);
    stream->codec_name   = codec_id;
    stream->codec_family = detail::codec_family_from_plugin_id(codec_id);
    stream->created_us   = detail::steady_now_us();

    // Remote-desktop plan D5: the profile may *declare* this stream's
    // share.  The declaration is the stream's starting weight, not a
    // lock — `set_video_stream_weight()` overrides it afterwards.
    //
    // A declaration for a stream the session never creates is inert (it
    // is not an error), and a stream the profile does not mention keeps
    // the default weight of 1 (an even split).
    {
        const auto declared = config_.video_stream_weights.find(
            std::string(stream_id));
        if (declared != config_.video_stream_weights.end()) {
            stream->weight = declared->second;
        }
    }

    // ---- The identity allocations ---------------------------------------
    //
    // The identity is derived from the *id* (a hash), not from a
    // counter: the same id always maps to the same SSRC / payload
    // type, so a reconfiguration does not renumber the streams and two
    // engines configured from the same profile agree on the wire
    // identity.  A hash collision is resolved by a linear probe, so two
    // streams can never share an identity.
    {
        const std::uint64_t h = fnv1a(stream_id);
        const std::uint32_t default_ssrc = config_.video_sender_tuning.ssrc;

        // ---- The SSRC ---------------------------------------------------
        std::uint32_t candidate =
            default_ssrc + 1u + static_cast<std::uint32_t>(h % 0xFFFFu);
        for (std::uint32_t probe = 0; probe < 0xFFFFu; ++probe) {
            if (candidate == 0) { ++candidate; continue; }   // SSRC 0 is reserved
            if (candidate != default_ssrc && !set.ssrc_in_use(candidate)) break;
            ++candidate;
        }
        stream->ssrc = candidate;

        // ---- The payload type -------------------------------------------
        //
        // The dynamic range is 96..127 (RFC 3551 §4.2).  The default
        // stream's payload type is outside it (H.264 is 102), so the
        // extra streams start above the *configured* number and are
        // clamped into the dynamic range.
        std::uint32_t pt = config_.video_sender_tuning.payload_type + 1u +
                           static_cast<std::uint32_t>((h >> 16) % 16u);
        if (pt < 96u) pt = 96u;
        if (pt > 127u) pt = 127u;
        for (std::uint32_t probe = 0; probe < 32u; ++probe) {
            const auto ptc = static_cast<std::uint8_t>(pt);
            if (ptc != default_payload_type(impl_->video_state,
                                            config_.video_sender_tuning.payload_type)
                && !set.payload_type_in_use(ptc)) {
                stream->payload_type = ptc;
                break;
            }
            pt = (pt >= 127u) ? 96u : (pt + 1u);
        }
        if (stream->payload_type == 0) {
            core::log::Logger::instance().error(
                "engine[video]: the dynamic payload-type range is "
                "exhausted — the stream was not created");
            return false;
        }
    }

    // ---- The stream's codec ---------------------------------------------
    {
        plugins::VideoCodecConfig cfg{};
        cfg.width  = config_.video_width;
        cfg.height = config_.video_height;
        cfg.fps    = config_.video_fps;
        cfg.bitrate_bps = config_.video_start_bitrate_bps > 0
                              ? config_.video_start_bitrate_bps
                              : 1'000'000;
        cfg.keyframe_interval = 60;
        cfg.pixel_format = plugins::VideoPixelFormat::kI420;
        cfg.codec = detail::codec_kind_of(stream->codec_family);
        cfg.payload_type = stream->payload_type;
        cfg.name = codec_id;

        plugins::IVideoCodec* codec = codec_factory->create(cfg);
        if (codec == nullptr) return false;
        if (codec->open() != plugins::kOk) {
            codec->close();
            delete codec;
            return false;
        }
        stream->codec.reset(codec);
        stream->codec_cfg = cfg;
        stream->backend_id = std::string{codec->backend_id()};
    }

    // ---- The stream's sender and receiver -------------------------------
    {
        const auto* sender_factory =
            pal::resolve_video_sender(config_.video_sender_name);
        if (sender_factory != nullptr) {
            plugins::VideoSenderConfig cfg{};
            cfg.codec = detail::codec_kind_of(stream->codec_family);
            cfg.ssrc = stream->ssrc;
            cfg.payload_type = stream->payload_type;
            cfg.mtu = config_.video_sender_tuning.mtu;
            cfg.initial_seq = config_.video_sender_tuning.initial_seq;
            cfg.packetization_mode =
                plugins::VideoPacketizationMode::kNonInterleaved;

            plugins::IVideoSender* sender = sender_factory->create(cfg);
            if (sender != nullptr && sender->open() == plugins::kOk) {
                stream->sender.reset(sender);
                stream->sender_cfg = cfg;
            } else if (sender != nullptr) {
                sender->close();
                delete sender;
            }
        }
    }
    {
        const auto* receiver_factory =
            pal::resolve_video_receiver(config_.video_receiver_name);
        if (receiver_factory != nullptr) {
            plugins::VideoReceiverConfig cfg{};
            cfg.codec = detail::codec_kind_of(stream->codec_family);
            cfg.ssrc = config_.video_receiver_tuning.ssrc;
            cfg.payload_type = stream->payload_type;
            cfg.expect_fu_a = config_.video_receiver_tuning.expect_fu_a;
            cfg.max_inflight_frames =
                config_.video_receiver_tuning.max_inflight_frames;
            cfg.max_jitter_buffer_ms =
                config_.video_receiver_tuning.max_jitter_buffer_ms;
            cfg.initial_jitter_buffer_ms =
                config_.video_receiver_tuning.initial_jitter_buffer_ms;
            cfg.emit_nacks = config_.video_receiver_tuning.emit_nacks;
            cfg.adaptive_playout =
                config_.video_receiver_tuning.adaptive_playout;

            plugins::IVideoReceiver* receiver = receiver_factory->create(cfg);
            if (receiver != nullptr && receiver->open() == plugins::kOk) {
                stream->receiver.reset(receiver);
                stream->receiver_cfg = cfg;
            } else if (receiver != nullptr) {
                receiver->close();
                delete receiver;
            }
        }
    }

    // ---- The stream's sink ----------------------------------------------
    {
        const auto* sink_factory =
            pal::resolve_video_sink(config_.video_sink_name);
        if (sink_factory != nullptr) {
            plugins::VideoSinkConfig cfg = resolve_sink_config();
            plugins::IVideoSink* sink = sink_factory->create(cfg);
            if (sink != nullptr && sink->open() == plugins::kOk) {
                stream->sink.reset(sink);
            } else if (sink != nullptr) {
                sink->close();
                delete sink;
            }
        }
    }

    // ---- The stream's source ---------------------------------------------
    {
        const auto* source_factory =
            pal::resolve_video_source(config_.video_source_name);
        if (source_factory != nullptr) {
            plugins::VideoSourceConfig cfg{};
            cfg.width  = config_.video_width;
            cfg.height = config_.video_height;
            cfg.fps    = config_.video_fps;
            cfg.ssrc   = stream->ssrc;

            plugins::IVideoSource* source = source_factory->create(cfg);
            if (source != nullptr && source->open() == plugins::kOk) {
                stream->source.reset(source);
            } else if (source != nullptr) {
                source->close();
                delete source;
            }
        }
    }

    // ---- The stream's rate controller (plan D1) ---------------------------
    if (!config_.video_bitrate_controller_name.empty()) {
        const auto* controller_factory =
            pal::resolve_video_bitrate_controller(
                config_.video_bitrate_controller_name);
        if (controller_factory != nullptr) {
            plugins::VideoBitrateControllerConfig bcfg{};
            bcfg.mode          = config_.video_ladder_mode;
            bcfg.source_width  = config_.video_width;
            bcfg.source_height = config_.video_height;
            bcfg.source_fps    = config_.video_fps;
            bcfg.min_height    = config_.video_resolution_floor_height;
            if (config_.video_start_bitrate_bps > 0) {
                bcfg.source_bitrate_bps = config_.video_start_bitrate_bps;
            }
            plugins::IVideoBitrateController* controller =
                controller_factory->create(bcfg);
            if (controller != nullptr) {
                stream->controller.reset(controller);
            }
        }
    }

    // ---- The stream's RTP send path --------------------------------------
    //
    // Every stream owns its own packetizer, so every stream needs its
    // own packet callback: the RTP header carries *this* stream's SSRC
    // and payload type, and the SRTP session is the one for that SSRC.
    // The default stream's callback lives in `engine_media.cpp` (it was
    // there first and it carries the pre-H1 behaviour verbatim); this
    // one is the same shape for the extra streams.
    wire_stream_sender(stream.get());

    core::log::Logger::instance().info(
        "engine[video]: the stream '" + stream->id + "' was created "
        "(ssrc=" + std::to_string(stream->ssrc) +
        ", payload type=" + std::to_string(stream->payload_type) +
        ", codec='" + stream->codec_name + "')");

    set.streams.push_back(std::move(stream));
    return true;
}

bool NimRTCEngine::remove_video_stream(std::string_view stream_id) noexcept {
    if (impl_ == nullptr) return false;

    // The default stream *is* the legacy single-stream path: removing it
    // would break every existing call site, so it is refused.
    if (stream_id == detail::kDefaultVideoStreamId) return false;

    auto& set = impl_->video_state.video_streams;
    for (auto it = set.streams.begin(); it != set.streams.end(); ++it) {
        if ((*it)->id != stream_id) continue;

        // The plugins are closed before they are released, so a device
        // that owns an OS handle (a capture device, a presentation
        // surface) gets a chance to clean up.  The sender goes first:
        // its packet callback captures the stream, so it must not fire
        // after the stream is gone.
        if ((*it)->source)     { (*it)->source->stop();    (*it)->source->close(); }
        if ((*it)->sender)     (*it)->sender->close();
        if ((*it)->sink)       (*it)->sink->close();
        if ((*it)->receiver)   (*it)->receiver->close();
        if ((*it)->codec)      (*it)->codec->close();
        if ((*it)->controller) (*it)->controller->close();

        core::log::Logger::instance().info(
            "engine[video]: the stream '" + (*it)->id + "' was removed");
        set.streams.erase(it);
        return true;
    }
    return false;
}

std::vector<std::string> NimRTCEngine::video_stream_ids() const noexcept {
    std::vector<std::string> out;
    if (impl_ == nullptr) return out;
    for (const auto& id : impl_->video_state.video_streams.ids()) {
        out.emplace_back(id);
    }
    return out;
}

// ---------------------------------------------------------------------------
// D5 — the per-stream bandwidth shares
// ---------------------------------------------------------------------------
//
// The weight is the stream's *claim* on the elastic pool: the pool (what
// the reserved audio did not take) is divided between the video streams
// in proportion to the weights.  The rule itself lives in
// `bandwidth_allocation.hpp` — the very same rule the audio path uses —
// so there is one split rule in the codebase, not two.

bool NimRTCEngine::set_video_stream_weight(std::string_view stream_id,
                                           std::uint32_t weight) noexcept {
    if (impl_ == nullptr) return false;

    auto& set = impl_->video_state.video_streams;

    // The default stream is not in `streams` (its plugin instances live
    // in the engine's own members), so it gets its own branch.
    if (stream_id == detail::kDefaultVideoStreamId) {
        // A weight of 0 would take the session's primary video path out
        // of the split entirely.  That is a foot-gun, not a feature: the
        // default stream is the session's video, and a host that wants
        // a stream to sit out must remove it, not mute it.
        if (weight == 0) return false;
        set.default_weight = weight;
        return true;
    }

    detail::VideoStream* stream = set.find(stream_id);
    if (stream == nullptr) return false;
    stream->weight = weight;
    return true;
}

std::uint32_t NimRTCEngine::video_stream_weight(
    std::string_view stream_id) const noexcept {
    if (impl_ == nullptr) return 0;
    const auto& set = impl_->video_state.video_streams;

    if (stream_id == detail::kDefaultVideoStreamId) return set.default_weight;

    const detail::VideoStream* stream = set.find(stream_id);
    return stream != nullptr ? stream->weight : 0u;
}

std::uint32_t NimRTCEngine::video_stream_share_bps(
    std::string_view stream_id) const noexcept {
    if (impl_ == nullptr) return 0;
    const auto& set = impl_->video_state.video_streams;

    if (stream_id == detail::kDefaultVideoStreamId) {
        return set.default_share_bps;
    }

    const detail::VideoStream* stream = set.find(stream_id);
    return stream != nullptr ? stream->share_bps : 0u;
}

void NimRTCEngine::shutdown_extra_video_streams() noexcept {
    if (impl_ == nullptr) return;
    auto& set = impl_->video_state.video_streams;
    for (auto& s : set.streams) {
        if (s->source)     { s->source->stop();    s->source->close(); }
        if (s->sender)     s->sender->close();
        if (s->sink)       s->sink->close();
        if (s->receiver)   s->receiver->close();
        if (s->codec)      s->codec->close();
        if (s->controller) s->controller->close();
    }
    set.streams.clear();
}

// ---------------------------------------------------------------------------
// H2 — replacing a stream's plugins
// ---------------------------------------------------------------------------

plugins::Status NimRTCEngine::set_video_codec(std::string_view stream_id,
                                              std::string_view codec_name) noexcept {
    if (impl_ == nullptr) return plugins::kErrNotReady;
    if (!is_open()) return plugins::kErrNotReady;

    StreamView view;
    if (!stream_view(stream_id, view)) return plugins::kErrInvalidParam;

    const std::string codec_id =
        codec_name.empty() ? config_.video_codec_name : std::string(codec_name);
    const auto* factory = pal::resolve_video_codec(codec_id);
    if (factory == nullptr) return plugins::kErrInvalidParam;

    plugins::VideoCodecConfig cfg{};
    cfg.width  = config_.video_width;
    cfg.height = config_.video_height;
    cfg.fps    = config_.video_fps;
    cfg.bitrate_bps = config_.video_start_bitrate_bps > 0
                          ? config_.video_start_bitrate_bps
                          : 1'000'000;
    cfg.keyframe_interval = 60;
    cfg.pixel_format = plugins::VideoPixelFormat::kI420;
    cfg.codec = detail::codec_kind_of(detail::codec_family_from_plugin_id(codec_id));
    cfg.payload_type = view.payload_type;
    cfg.name = codec_id;

    plugins::IVideoCodec* codec = factory->create(cfg);
    if (codec == nullptr) return plugins::kErrInternal;
    if (codec->open() != plugins::kOk) {
        codec->close();
        delete codec;
        return plugins::kErrInternal;
    }

    if (stream_id == detail::kDefaultVideoStreamId) {
        if (video_codec_) {
            video_codec_->close();
            video_codec_.reset();
        }
        video_codec_.reset(codec);
        // The codec's own `kind()` is authoritative (the plugin id is
        // only a lookup hint) — see `video_codec_config.hpp`.
        impl_->video_state.video_codec_family =
            detail::codec_family_from_codec_kind(codec->kind());
        if (impl_->video_state.video_codec_family ==
            detail::VideoCodecFamily::kUnknown) {
            impl_->video_state.video_codec_family =
                detail::codec_family_from_plugin_id(codec_id);
        }
        impl_->video_state.video_encoder_params_set = false;

        // The packetizer is codec-aware (the H.264 / H.265 paths
        // differ), so a family change means the default stream's sender
        // has to be rebuilt with the new kind.  Its SSRC and payload
        // type stay the same: the stream's identity on the wire does not
        // change when the host reconfigures the codec.
        if (video_sender_) {
            const auto* sender_factory =
                pal::resolve_video_sender(config_.video_sender_name);
            if (sender_factory != nullptr) {
                plugins::VideoSenderConfig scfg{};
                scfg.codec = detail::codec_kind_of(
                    impl_->video_state.video_codec_family);
                scfg.ssrc = config_.video_sender_tuning.ssrc;
                scfg.payload_type = view.payload_type;
                scfg.mtu = config_.video_sender_tuning.mtu;
                scfg.initial_seq = config_.video_sender_tuning.initial_seq;
                scfg.packetization_mode =
                    plugins::VideoPacketizationMode::kNonInterleaved;
                plugins::IVideoSender* sender = sender_factory->create(scfg);
                if (sender != nullptr && sender->open() == plugins::kOk) {
                    video_sender_->close();
                    video_sender_.reset(sender);
                    // The rebuilt packetizer needs the very same packet
                    // callback the original had -- otherwise the stream
                    // would go silent after a codec swap.
                    wire_default_sender();
                } else if (sender != nullptr) {
                    sender->close();
                    delete sender;
                }
            }
        }
        return plugins::kOk;
    }

    detail::VideoStream* s =
        impl_->video_state.video_streams.find(stream_id);
    if (s == nullptr) {
        codec->close();
        delete codec;
        return plugins::kErrInvalidParam;
    }
    if (s->codec) {
        s->codec->close();
        s->codec.reset();
    }
    s->codec.reset(codec);
    s->codec_name = codec_id;
    // The codec's own `kind()` is the authoritative source of the family
    // (the plugin id is only a lookup hint) — see `video_codec_config.hpp`.
    s->codec_family = detail::codec_family_from_codec_kind(codec->kind());
    if (s->codec_family == detail::VideoCodecFamily::kUnknown) {
        s->codec_family = detail::codec_family_from_plugin_id(codec_id);
    }
    s->codec_cfg = cfg;
    s->backend_id = std::string{codec->backend_id()};
    s->encoder_params_set = false;

    // The packetizer is codec-aware (the H.264 / H.265 paths differ), so
    // a family change means the stream's sender has to be rebuilt with
    // the new kind.  The SSRC and the payload type stay the same: the
    // stream's identity on the wire does not change when the host
    // reconfigures the codec.
    if (s->sender) {
        const auto* sender_factory =
            pal::resolve_video_sender(config_.video_sender_name);
        if (sender_factory != nullptr) {
            plugins::VideoSenderConfig scfg = s->sender_cfg;
            scfg.codec = detail::codec_kind_of(s->codec_family);
            plugins::IVideoSender* sender = sender_factory->create(scfg);
            if (sender != nullptr && sender->open() == plugins::kOk) {
                s->sender->close();
                s->sender.reset(sender);
                s->sender_cfg = scfg;
                wire_stream_sender(s);
            } else if (sender != nullptr) {
                sender->close();
                delete sender;
            }
        }
    }
    return plugins::kOk;
}

plugins::Status NimRTCEngine::set_video_source(std::string_view stream_id,
                                               std::string_view source_name) noexcept {
    if (impl_ == nullptr) return plugins::kErrNotReady;
    if (!is_open()) return plugins::kErrNotReady;

    StreamView view;
    if (!stream_view(stream_id, view)) return plugins::kErrInvalidParam;

    const auto* factory = pal::resolve_video_source(source_name);
    if (factory == nullptr) return plugins::kErrInvalidParam;

    plugins::VideoSourceConfig cfg{};
    cfg.width  = config_.video_width;
    cfg.height = config_.video_height;
    cfg.fps    = config_.video_fps;
    cfg.ssrc   = view.ssrc;

    plugins::IVideoSource* source = factory->create(cfg);
    if (source == nullptr) return plugins::kErrInternal;
    if (source->open() != plugins::kOk) {
        source->close();
        delete source;
        return plugins::kErrInternal;
    }

    if (stream_id == detail::kDefaultVideoStreamId) {
        if (video_source_) {
            video_source_->stop();
            video_source_->close();
            video_source_.reset();
        }
        video_source_.reset(source);
        return plugins::kOk;
    }

    detail::VideoStream* s =
        impl_->video_state.video_streams.find(stream_id);
    if (s == nullptr) {
        source->close();
        delete source;
        return plugins::kErrInvalidParam;
    }
    if (s->source) {
        s->source->stop();
        s->source->close();
        s->source.reset();
    }
    s->source.reset(source);
    return plugins::kOk;
}

plugins::Status NimRTCEngine::set_video_sink(std::string_view stream_id,
                                             std::string_view sink_name) noexcept {
    if (impl_ == nullptr) return plugins::kErrNotReady;
    if (!is_open()) return plugins::kErrNotReady;

    StreamView view;
    if (!stream_view(stream_id, view)) return plugins::kErrInvalidParam;

    const auto* factory = pal::resolve_video_sink(sink_name);
    if (factory == nullptr) return plugins::kErrInvalidParam;

    plugins::VideoSinkConfig cfg = resolve_sink_config();
    plugins::IVideoSink* sink = factory->create(cfg);
    if (sink == nullptr) return plugins::kErrInternal;
    if (sink->open() != plugins::kOk) {
        sink->close();
        delete sink;
        return plugins::kErrInternal;
    }

    if (stream_id == detail::kDefaultVideoStreamId) {
        if (video_sink_) {
            video_sink_->close();
            video_sink_.reset();
        }
        video_sink_.reset(sink);
        return plugins::kOk;
    }

    detail::VideoStream* s =
        impl_->video_state.video_streams.find(stream_id);
    if (s == nullptr) {
        sink->close();
        delete sink;
        return plugins::kErrInvalidParam;
    }
    if (s->sink) {
        s->sink->close();
        s->sink.reset();
    }
    s->sink.reset(sink);
    return plugins::kOk;
}

// ---------------------------------------------------------------------------
// The outbound path (H1 — `send_video(stream_id, frame)`)
// ---------------------------------------------------------------------------

plugins::Status NimRTCEngine::send_video(std::string_view stream_id,
                                          const plugins::VideoSourceFrame& frame) noexcept {
    if (!is_open()) return plugins::kErrNotReady;

    // The default stream is the legacy single-stream path: it is served
    // by `encode_and_send_video_frame()`, the very function the
    // pre-H1 `send_video(frame)` calls.  Delegating (rather than
    // re-implementing the path) is what guarantees the two are
    // byte-for-byte identical — the counters, the ref-frame record, the
    // staging, the pacing priority and the SRTP session all stay the
    // ones the existing tests assert.
    if (stream_id == detail::kDefaultVideoStreamId) {
        return encode_and_send_video_frame(frame);
    }

    StreamView view;
    if (!stream_view(stream_id, view)) return plugins::kErrInvalidParam;

    return encode_and_send_video_frame_for(frame, view);
}

plugins::Status NimRTCEngine::encode_and_send_video_frame_for(
        const plugins::VideoSourceFrame& frame,
        const StreamView& stream) noexcept {
    if (stream.codec == nullptr || stream.sender == nullptr) {
        return plugins::kErrNotReady;
    }

    detail::VideoStream* s =
        impl_->video_state.video_streams.find(stream.id);
    if (s == nullptr) return plugins::kErrInvalidParam;

    // ---- The parameter set (plan B12 / B13) -----------------------------
    //
    // The same rule the default stream follows: the parameters are
    // computed in ONE place and pushed only when they actually change.
    // A geometry change is a parameter-set change, so the encoder must
    // restart and emit the new parameter sets with the frame.
    {
        detail::VideoEncoderParamInputs in;
        in.width  = config_.video_width;
        in.height = config_.video_height;
        in.fps    = config_.video_fps;
        in.bitrate_bps = stream.codec->config().bitrate_bps;
        in.keyframe_interval = 60;
        in.pixel_format = stream.codec->config().pixel_format;
        in.content_hint = config_.video_content_hint;
        in.color = frame.color;
        in.roi = config_.video_roi;
        in.capture_width = frame.width;
        in.capture_height = frame.height;
        if (frame.width != 0 && frame.height != 0) {
            in.width  = frame.width;
            in.height = frame.height;
        }

        // The rate controller's level (plan D4) — the *stream's own*
        // controller, so two streams can sit on different rungs.
        if (stream.controller != nullptr) {
            const plugins::VideoLadderLevel level =
                stream.controller->current_level();
            if (level.bitrate_bps > 0) in.bitrate_bps = level.bitrate_bps;
            if (level.fps > 0) in.fps = level.fps;
        }

        const plugins::VideoEncoderParams wanted =
            detail::compute_video_encoder_params(in);

        const bool unchanged =
            s->encoder_params_set &&
            s->encoder_params.width  == wanted.width  &&
            s->encoder_params.height == wanted.height &&
            s->encoder_params.fps == wanted.fps &&
            s->encoder_params.bitrate_bps == wanted.bitrate_bps &&
            s->encoder_params.keyframe_interval == wanted.keyframe_interval &&
            s->encoder_params.pixel_format == wanted.pixel_format &&
            s->encoder_params.content_hint == wanted.content_hint;

        if (!unchanged) {
            (void)stream.codec->set_params(wanted);
            s->encoder_params = wanted;
            s->encoder_params_set = true;
        }
    }

    // ---- Stage the pixels ------------------------------------------------
    //
    // The plugin-level `VideoSourceFrame` is a *view* into the producer's
    // own storage; the codec seam wants a refcounted buffer that stays
    // alive across `encode()`.  The staging rule is the shared one (see
    // `engine_video_staging.hpp`) — the default stream and every extra
    // stream use the *same* colour-space rule, so they cannot drift.
    plugins::VideoFrame raw{};
    raw.info.capture_ts_us = frame.capture_ts_us;
    raw.info.frame_seq     = frame.frame_seq;
    raw.info.rtp_timestamp = frame.rtp_timestamp;

    detail::ConversionKind conv_kind = detail::ConversionKind::kNone;
    raw.buffer = detail::stage_source_frame(
        frame, stream.codec->config().pixel_format,
        impl_->scratch.video_frame_pool, &conv_kind);
    if (raw.buffer) {
        if (conv_kind == detail::ConversionKind::kConvert) {
            ++impl_->video_state.video_format_conversions;
        } else if (conv_kind == detail::ConversionKind::kCopy) {
            ++impl_->video_state.video_format_copies;
        }
    }

    // ---- Encode -----------------------------------------------------------
    //
    // The encoder's output buffer is the *stream's own* (the default
    // stream has its own in the engine's scratch), so two streams never
    // write into the same storage.  The *staged* pixel buffer, on the
    // other hand, comes from the engine's shared frame pool — that is
    // safe because the pool hands out a distinct refcounted buffer per
    // acquisition, and the buffer stays alive across `encode()`.
    constexpr std::size_t kInitialVideoEncCap = 64 * 1024;
    if (s->enc_buf.capacity() < kInitialVideoEncCap) {
        s->enc_buf.reserve(kInitialVideoEncCap);
    }
    s->enc_buf.resize(s->enc_buf.capacity());

    plugins::EncodedVideoFrame encoded{};
    plugins::Status enc_rc = stream.codec->encode(raw, s->enc_buf.data(),
                                                    s->enc_buf.size(), encoded);
    if (enc_rc != plugins::kOk) {
        // The codec refused the frame.  A software stub codec ignores
        // the pixels and encodes a synthetic frame; a hardware codec
        // needs them and reports the failure through this path.
        return plugins::kErrInternal;
    }

    if (encoded.is_keyframe) {
        s->keyframe_pending = true;
    }

    // ---- Packetize ---------------------------------------------------------
    const plugins::Status push_rc =
        stream.sender->push_frame(encoded, frame.rtp_timestamp);
    if (push_rc != plugins::kOk) return push_rc;

    // ---- The counters ------------------------------------------------------
    ++s->frames_sent;
    ++s->frames_encoded;
    if (encoded.is_keyframe) ++s->keyframes_sent;
    return plugins::kOk;
}

// ---------------------------------------------------------------------------
// H4 — the per-stream statistics
// ---------------------------------------------------------------------------

void NimRTCEngine::append_video_stream_stats(
        std::vector<VideoStreamStats>& out) const noexcept {
    if (impl_ == nullptr) return;

    const auto& set = impl_->video_state.video_streams;
    const std::int64_t now_us = detail::steady_now_us();
    const std::int64_t session_start_us =
        set.started_us != 0 ? set.started_us : now_us;
    const std::int64_t session_elapsed_us = now_us - session_start_us;

    // ---- The default stream ---------------------------------------------
    //
    // The flat `Stats::video_*` fields describe the default stream, so
    // the list's first entry carries the same numbers.  A host that
    // never touches the multi-stream API sees exactly what it saw
    // before.
    {
        VideoStreamStats s;
        s.stream_id    = std::string(detail::kDefaultVideoStreamId);
        s.ssrc         = config_.video_sender_tuning.ssrc;
        s.payload_type = default_payload_type(impl_->video_state,
                                              config_.video_sender_tuning.payload_type);
        s.mid          = impl_->session_state.base_video_mid;
        s.codec_name   = config_.video_codec_name;
        s.backend_id   = video_codec_ ? std::string{video_codec_->backend_id()}
                                       : std::string("unknown");
        s.ladder_level = video_bitrate_controller_
            ? video_bitrate_controller_->stats().level_index : 0u;
        s.bandwidth_share_bps = set.default_share_bps;
        s.bandwidth_weight    = set.default_weight;
        s.bitrate_bps = average_bitrate_bps(impl_->video_state.video_bytes_sent,
                                             session_elapsed_us);
        s.keyframe_rate_hz = average_keyframe_rate_hz(
            video_codec_ ? video_codec_->stats().keyframes_encoded : 0ull,
            session_elapsed_us);
        s.frames_sent    = impl_->video_state.video_frames_sent;
        s.frames_received = impl_->video_state.video_frames_received;
        s.frames_encoded  = impl_->video_state.video_frames_sent;
        s.packets_sent  = impl_->video_state.video_packets_sent;
        s.bytes_sent    = impl_->video_state.video_bytes_sent;
        s.keyframes_sent = video_codec_ ? video_codec_->stats().keyframes_encoded
                                         : 0ull;
        out.push_back(std::move(s));
    }

    // ---- The extra streams ------------------------------------------------
    for (const auto& stream : set.streams) {
        VideoStreamStats s;
        s.stream_id    = stream->id;
        s.ssrc         = stream->ssrc;
        s.payload_type = stream->payload_type;
        s.mid          = stream->mid;
        s.codec_name   = stream->codec_name;
        s.backend_id   = stream->backend_id;
        s.ladder_level = stream->controller
            ? stream->controller->stats().level_index : 0u;
        s.bandwidth_share_bps = stream->share_bps;
        s.bandwidth_weight    = stream->weight;
        const std::int64_t elapsed_us = now_us - stream->created_us;
        s.bitrate_bps = average_bitrate_bps(stream->bytes_sent, elapsed_us);
        s.keyframe_rate_hz =
            average_keyframe_rate_hz(stream->keyframes_sent, elapsed_us);
        s.frames_sent    = stream->frames_sent;
        s.frames_received = stream->frames_received;
        s.frames_encoded  = stream->frames_encoded;
        s.packets_sent  = stream->packets_sent;
        s.bytes_sent    = stream->bytes_sent;
        s.keyframes_sent = stream->keyframes_sent;
        out.push_back(std::move(s));
    }
}

}  // namespace nimrtc::engine
