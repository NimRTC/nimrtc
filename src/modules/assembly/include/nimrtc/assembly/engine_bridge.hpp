/**
 * @file nimrtc/assembly/engine_bridge.hpp
 * @brief The Profile → engine bridge: `assembly::Profile` → `EngineConfig`.
 *
 * Architecture remediation plan D2 / Phase 4.2 ("要么接线，要么删除").
 * Before this header existed the `assembly` module was a complete
 * implementation with a test suite that **nothing in the product ever
 * called** — the worst state the plan calls out ("有完整实现 + 有测试 +
 * 引擎不加载").  The `demo-agent-gateway` example hand-copied four
 * fields out of a `Profile` into an `EngineConfig` and dropped the rest,
 * which is exactly the drift this header removes.
 *
 * ## Where this header lives, and why
 *
 * The bridge lives in `assembly/`, and the `nimrtc::assembly_engine`
 * target that carries it links `nimrtc::engine`.  That direction is
 * deliberate:
 *
 *   - `assembly` is the **scenario layer** — it describes *what* an
 *     engine instance should be.  The engine is the **runtime** — it *is*
 *     the engine instance.  Configuration sits above runtime, so the
 *     dependency arrow points `assembly → engine`.
 *   - Layout Invariant 4 (ADR-025) constrains what the **engine** may
 *     include: the engine never includes `assembly.hpp`.  This header
 *     therefore never appears in an engine translation unit, and the
 *     engine's public header stays free of every concrete module include.
 *   - The arrow is not a cycle: nothing the engine links (`rtp`, `sdp`,
 *     `ice`, `dtls`, `srtp`, `audio3a`, `transport`, `web_transport`, …)
 *     depends on `assembly`.
 *
 * The alternative — putting the bridge in `src/engine/` — would have
 * required adding `assembly` to the ADR-025 exception list, i.e. making
 * the *runtime* depend on the *configuration schema*.  That is the
 * inversion ADR-025 was written to prevent.
 *
 * ## Why a separate target
 *
 * `nimrtc::assembly` stays light: the Profile schema, the registry, the
 * Builder and the JSON loader — no engine dependency.  An embedded host
 * that only wants to *read* a Profile (to print it, to diff it, to ship
 * it to a remote agent) must not be forced to link the ICE / DTLS / SRTP
 * stack.  The engine-facing half lives in the INTERFACE target
 * `nimrtc::assembly_engine`, which a host links when it wants to drive
 * the engine from a Profile.
 *
 * ## The two-step contract
 *
 * A Profile carries **intent** ("this is a low-latency agent gateway");
 * an `EngineConfig` carries **plugin ids and numbers**.  The translation
 * is a pure function of the Profile, so it lives here as a free function:
 *
 * @code
 *   auto profile = assembly::Builder{}.load_profile("agent-gateway").build();
 *   engine::EngineConfig cfg = assembly::to_engine_config(profile);
 *   NimRTCEngine engine(cfg);
 * @endcode
 *
 * `to_engine_config()` **overwrites** the fields the Profile owns and
 * **leaves every other field at its `EngineConfig` default**.  That is
 * what makes it composable: a host can set `cfg.local_bind_address`
 * before or after the call, and the Profile will not stomp on it.
 *
 * ## Fields the Profile does NOT own
 *
 * Some `EngineConfig` knobs have no counterpart in the Profile schema and
 * are deliberately left alone:
 *
 *   - `transport_stack_name`  — the Selector derives the stack id from
 *     the strategy; the Profile's `transport.media.stack` is a
 *     *diagnostic* field, not an override (see `transport_strategy.hpp`).
 *   - `dtls_name`, `srtp_backend_name`, `sctp_socket_name`,
 *     `raw_udp_name` — crypto / transport backend selection is a
 *     deployment concern, not a scenario concern.
 *   - `net_feedback_name`, `xr_report_name`, `video_*_name` — the Profile
 *     schema (v1.1) has no field for them.
 *   - `audio_decoder_name`, `audio_use_inband_fec`, `audio_use_dtx`, … —
 *     the audio *receive* plane, which the Profile does not describe.
 *
 * ## The scheduler id caveat
 *
 * `Profile::scheduler_name` is `"default"` in the built-in constants and
 * `Profile::scheduler.impl` is `"weighted_fair"` for the live profile —
 * but the only scheduler plugin registered by the `sched` module is
 * `"strict_priority"`.  `to_engine_config()` resolves the id through
 * `resolve_scheduler_plugin_id()`, which maps the not-yet-implemented
 * ids onto the registered one, so a Profile never silently produces an
 * engine with *no* scheduler.
 */
#pragma once


// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13-plan follow-up.
#include <nimrtc/core/api_version.hpp>

#include <string>
#include <string_view>

#include <nimrtc/assembly/assembly.hpp>

// The transport-side adapter (TransportSection -> ProfileTransportInput).
// It supplies the transport module's include path, which this header
// needs for `nimrtc::ProfileTransportInput`.
#include <nimrtc/assembly/transport_bridge.hpp>

// The engine's public header.  `EngineConfig` is a plain aggregate of
// plugin ids and numbers — it has no engine implementation dependency,
// so including it here costs nothing at runtime.
#include <nimrtc/engine/engine.hpp>

namespace nimrtc::assembly {

// ---------------------------------------------------------------------------
// resolve_scheduler_plugin_id — the scheduler id the engine can resolve
// ---------------------------------------------------------------------------
//
// The `sched` module registers exactly one scheduler factory, under the
// id `"strict_priority"` (see `src/modules/sched/src/scheduler_plugin.cpp`).
// The assembly schema, however, names three strategies and the built-in
// profiles carry the legacy id `"default"`:
//
//   | Profile spelling            | Meaning                        |
//   |-----------------------------|--------------------------------|
//   | "strict_priority"           | the registered implementation  |
//   | "default"                   | legacy alias for the same     |
//   | "weighted_fair"             | reserved, not implemented yet  |
//   | "fixed_rate"                | reserved, not implemented yet  |
//
// Mapping the two reserved ids onto `"strict_priority"` keeps the
// scheduler *active* (the alternative — an unresolved id — makes the
// engine skip the scheduler entirely, which silently turns the Profile's
// priority weights into dead configuration).
//
// The substitution is a **fallback**, not a silent lie: when the id is
// remapped the caller is told via the returned `bool`, so a host can log
// it.  A Profile that names a third-party scheduler id is passed through
// **unchanged** — a user who registers their own `"my_scheduler"` must be
// able to select it.
//
// @param impl         The id from `Profile::scheduler.impl` (or the
//                     strategy-derived default).
// @param substituted  Set to true when the id was remapped.
// @return The id the engine should use.
NIMRTC_API_STABLE inline std::string
resolve_scheduler_plugin_id(std::string_view impl, bool* substituted = nullptr) {
    if (substituted) *substituted = false;

    if (impl.empty() || impl == "default" || impl == "strict_priority") {
        // "default" is the legacy spelling of "strict_priority".
        if (substituted && impl == "default") *substituted = true;
        return "strict_priority";
    }

    if (impl == "weighted_fair" || impl == "fixed_rate") {
        // Reserved ids: the strategies are modelled in the schema but the
        // implementations are not registered yet.  Fall back to the
        // registered implementation so the scheduler stays in the path.
        if (substituted) *substituted = true;
        return "strict_priority";
    }

    // A third-party id — pass it through untouched.
    return std::string{impl};
}

// ---------------------------------------------------------------------------
// The v1.2 video-scene vocabulary — Profile strings → engine values
// ---------------------------------------------------------------------------
//
// The Profile schema (v1.2) names four scene properties with *strings*:
//
//   video_codec_preference | video_resolution_floor
//   video_ladder_mode      | video_content_type
//
// and the engine wants *typed* values:
//
//   EngineConfig::video_codec_name              (a plugin id)
//   EngineConfig::video_resolution_floor_height (a pixel count)
//   EngineConfig::video_ladder_mode             (a VideoLadderMode enum)
//   EngineConfig::video_content_hint            (a ContentHint enum)
//
// These four functions are the single authority for the translation.  They
// live here, next to `to_engine_config()`, because a string→enum mapping
// that lives anywhere else is a mapping that can drift from the one the
// bridge uses — the failure mode this file exists to prevent.
//
// ## Why the mapping cannot be a table lookup
//
// The schema's enum lists the *vocabulary*; the engine's vocabulary is a
// *different* set of names:
//
//   - the ladder mode's names are the same ("quality" / "smoothness"),
//   - the resolution floor's names are picture heights ("720p"), which
//     the engine wants as an integer,
//   - the content type's names are scene kinds ("camera" / "screen"),
//     which the engine wants as a codec content hint, and
//   - the codec preference's names are plugin ids ("h264"), but the
//     engine's *default* id is also "h264" and an empty id means "no
//     video codec at all".
//
// A value the schema allows but the engine cannot express is **not** an
// error: it falls back to the engine default, and the reason is stated
// in the comment.  The schema is the validator; this file is the
// translator.

// Resolve a Profile ladder-mode name onto the engine's enum.
//
// The names are identical to the enum's ("quality" / "smoothness"), so
// this is a spelling check.  An unrecognised name falls back to
// `kQuality` — the engine's documented default (see
// `EngineConfig::video_ladder_mode`).
NIMRTC_API_STABLE inline plugins::VideoLadderMode
video_ladder_mode_of(std::string_view name) noexcept {
    if (name == "smoothness") return plugins::VideoLadderMode::kSmoothness;
    if (name == "quality")    return plugins::VideoLadderMode::kQuality;
    return plugins::VideoLadderMode::kQuality;
}

// Resolve a Profile resolution-floor name onto a picture height.
//
// The names are the conventional picture heights: "360p" → 360,
// "480p" → 480, "720p" → 720, "1080p" → 1080, "1440p" → 1440,
// "2160p" → 2160.
//
// An unrecognised name returns 0, which the engine reads as "no floor"
// (`EngineConfig::video_resolution_floor_height`).  That is the honest
// reading: a floor the engine cannot resolve is not a floor.
NIMRTC_API_STABLE inline std::uint32_t
video_resolution_floor_height_of(std::string_view name) noexcept {
    if (name == "360p")  return 360;
    if (name == "480p")  return 480;
    if (name == "720p")  return 720;
    if (name == "1080p") return 1080;
    if (name == "1440p") return 1440;
    if (name == "2160p") return 2160;
    return 0;
}

// Resolve a Profile content-type name onto the engine's content hint.
//
// ADR-022 is the rule this follows: a screen capture is **not** "a camera
// with a huge resolution", so the content type selects the screen-coding
// tools.  The engine expresses that as a codec content hint:
//
//   - "camera" → `kUnspecified` — the engine has no opinion, which is
//     the correct value for camera content;
//   - "screen" → `kText` — a remote desktop is dominated by text, and
//     "text" is the hint that asks the encoder to preserve it.
//
// An unrecognised name returns `kUnspecified` — the engine default.
NIMRTC_API_STABLE inline plugins::VideoEncoderParams::ContentHint
video_content_hint_of(std::string_view content_type) noexcept {
    if (content_type == "screen") {
        return plugins::VideoEncoderParams::ContentHint::kText;
    }
    return plugins::VideoEncoderParams::ContentHint::kUnspecified;
}

// ---------------------------------------------------------------------------
// to_engine_config — Profile → EngineConfig
// ---------------------------------------------------------------------------
//
// The single authority for "what does this Profile ask the engine to do".
//
// Every host that drives the engine from a Profile must go through this
// function, so two hosts can never disagree about the mapping.
//
// ## The field map
//
//   Profile::transport_name          → EngineConfig::transport_name
//   Profile::jb_name                 → EngineConfig::jb_name
//   Profile::audio3a_name            → EngineConfig::audio3a_name
//   Profile::codec_name              → EngineConfig::codec_name
//   Profile::bwe_name                → EngineConfig::bwe_name
//   Profile::scheduler_name          → (resolved — see below)
//   Profile::bwe.params              → EngineConfig::bwe_config
//   Profile::scheduler.params        → EngineConfig::scheduler_config
//   Profile::scheduler.enabled       → EngineConfig::scheduler_config.enabled
//   Profile::timeline_enabled        → EngineConfig::timeline_enabled
//   Profile::datachannel_enabled     → EngineConfig::datachannel_backend
//   Profile::audio_sample_rate_hz    → EngineConfig::pcm_sample_rate_hz
//   Profile::audio_channels          → EngineConfig::pcm_channels
//   Profile::audio_payload_type      → EngineConfig::audio_codec.payload_type
//   Profile::jitter_buffer.initial_delay_ms → EngineConfig::jb_initial_delay_ms
//   Profile::jitter_buffer.min_delay_ms     → EngineConfig::jb_min_delay_ms
//   Profile::jitter_buffer.max_delay_ms     → EngineConfig::jb_max_delay_ms
//   Profile::transport.strategy             → EngineConfig::transport_strategy_name
//   Profile::transport.media.max_p99_ms     → EngineConfig::transport_media_max_p99_ms
//   Profile::video_codec_preference  → EngineConfig::video_codec_name
//   Profile::video_resolution_floor  → EngineConfig::video_resolution_floor_height
//   Profile::video_ladder_mode       → EngineConfig::video_ladder_mode
//   Profile::video_content_type      → EngineConfig::video_content_hint
//   Profile::video_stream_weights    → EngineConfig::video_stream_weights
//
// ## The two overrides
//
// `Profile` carries the scheduler id in **two** places, and they can
// disagree:
//
//   - `Profile::scheduler_name` — the legacy top-level field.  Every
//     built-in profile sets it to `"default"`, which is *not* a
//     registered plugin id.
//   - `Profile::scheduler.impl` — the v1.1 schema spelling, auto-filled
//     from the strategy by the JSON loader.
//
// The nested field wins when it is set and differs from the legacy one,
// because that is the field the JSON schema documents and the field the
// loader resolves.  A profile that leaves the nested field empty keeps
// the top-level one.
//
// `Profile::bwe_name` / `Profile::bwe.impl` have the same "two places"
// shape, but the **precedence is inverted**: the top-level `bwe_name` is
// the on/off switch and the nested `bwe.impl` is the id.  The relay /
// transport profiles switch BWE off by clearing `bwe_name` while keeping
// `"impl": "aimd"` in the JSON (the block has to stay for round-trip
// stability), so preferring the nested id unconditionally would silently
// re-enable BWE on every relay profile.
//
// ## The `jb_name` vs `jitter_buffer.impl` pair
//
// `Profile::jb_name` is the plugin id the engine consumes
// (`EngineConfig::jb_name`); `Profile::jitter_buffer.impl` is the same
// thing under the v1.1 schema spelling.  The profiles that disable the
// jitter buffer set **both** to `""`, so the "nested wins" rule is
// safe here: it never turns a disabled JB back on.
//
// ## The disabled-subsystem guard
//
// The relay / transport profiles disable whole subsystems by clearing the
// corresponding plugin id.  When a subsystem is off, the Profile's
// *parameters* for it are placeholders, not intent:
//
//   - `kProfileRelay` has `audio_sample_rate_hz = 0`,
//     `audio_channels = 0`, `audio_payload_type = 0` — because there is
//     no audio codec.  Copying those onto the `EngineConfig` would give
//     the engine a 0 Hz PCM format and a PT-0 (PCMU) offer.
//   - `kProfileRelay` has `jitter_buffer.*_delay_ms = 0` — because there
//     is no jitter buffer.
//
// So the audio-format and jitter-delay copies are **guarded** by the
// corresponding plugin id being non-empty.  A disabled subsystem leaves
// the engine's defaults in place, which is the honest reading.
//
// @param profile  The scenario description.  Copied by value into the
//                 returned config — no lifetime constraint on the caller.
// @param cfg      Optional pre-populated config to overwrite in place.
//                 This is the composable form: the host sets the
//                 deployment-specific knobs (bind address, STUN server,
//                 DTLS backend) and then lets the Profile fill in the
//                 scenario knobs.
//
// @return The EngineConfig the engine should be opened with.
NIMRTC_API_STABLE inline engine::EngineConfig
to_engine_config(const Profile& profile, engine::EngineConfig cfg) {
    // ---- Plugin ids -------------------------------------------------------
    cfg.transport_name = profile.transport_name;
    cfg.jb_name        = profile.jb_name;
    cfg.audio3a_name   = profile.audio3a_name;
    cfg.codec_name     = profile.codec_name;

    // The BWE id has an asymmetry the scheduler id does not: the
    // **top-level** `bwe_name` is the on/off switch, while the nested
    // `bwe.impl` is the id.  The relay / transport profiles switch BWE
    // off by clearing `bwe_name` but keep `"impl": "aimd"` in the JSON
    // (the block has to stay for round-trip stability).  So:
    //
    //   - `bwe_name` empty  → BWE is off; keep it off.
    //   - `bwe_name` set    → the nested `bwe.impl` names the estimator.
    //
    // Preferring the nested id unconditionally would silently re-enable
    // BWE on every relay profile.
    if (!profile.bwe_name.empty()) {
        cfg.bwe_name = profile.bwe.impl.empty() ? profile.bwe_name
                                                 : profile.bwe.impl;
    } else {
        cfg.bwe_name.clear();
    }

    // The scheduler id needs the resolution step (see
    // `resolve_scheduler_plugin_id`): the Profile's raw spelling is not
    // necessarily a registered plugin id.
    bool substituted = false;
    cfg.scheduler_name =
        resolve_scheduler_plugin_id(resolve_scheduler_impl(profile.scheduler),
                                    &substituted);

    // ---- Plugin configs ---------------------------------------------------
    // `to_plugins_bwe_config()` is the assembly layer's own projection
    // onto the canonical plugin config — using it keeps the mapping in
    // one place even if the two shapes diverge later.
    cfg.bwe_config = to_plugins_bwe_config(profile.bwe);

    cfg.scheduler_config = to_plugins_scheduler_config(profile.scheduler);
    cfg.scheduler_config.enabled = profile.scheduler.enabled;

    // ---- Feature switches --------------------------------------------------
    cfg.timeline_enabled = profile.timeline_enabled;

    // The DataChannel plane is a *backend family* selector on the engine
    // side (`datachannel_backend`).  A Profile that disables the
    // DataChannel clears the backend; one that enables it keeps the
    // engine's default family ("web_transport").
    //
    // `datachannel_name` (the legacy per-factory id) is left alone — the
    // Profile schema has no field for it, and the engine's `""` default
    // means "use `datachannel_backend`".
    if (!profile.datachannel_enabled) {
        cfg.datachannel_backend.clear();
    }

    // ---- Audio format ------------------------------------------------------
    // Guarded: a profile with no audio codec carries placeholder audio
    // parameters (rate 0, channels 0, PT 0).  Copying them would give
    // the engine a 0 Hz PCM format and a PCMU offer for a stream that
    // does not exist.
    if (!profile.codec_name.empty()) {
        cfg.pcm_sample_rate_hz = profile.audio_sample_rate_hz;
        cfg.pcm_channels       = profile.audio_channels;
        cfg.audio_codec.payload_type = profile.audio_payload_type;
    }

    // ---- Jitter buffer -----------------------------------------------------
    // Guarded for the same reason: a profile with no jitter buffer carries
    // placeholder delays of 0.
    if (!profile.jb_name.empty()) {
        cfg.jb_initial_delay_ms = profile.jitter_buffer.initial_delay_ms;
        cfg.jb_min_delay_ms     = profile.jitter_buffer.min_delay_ms;
        cfg.jb_max_delay_ms     = profile.jitter_buffer.max_delay_ms;
    }

    // ---- Transport strategy -------------------------------------------------
    // An empty strategy means "the engine compile-time default"
    // ("browser-call") — the same convention the transport layer uses.
    //
    // The strategy id AND the per-instance media p99 budget are taken
    // from the transport-side mirror, which is produced by the single
    // adapter in `transport_bridge.hpp`.  Reading both from one place
    // means the mirror and the Profile can never disagree about which
    // field carries what — the failure mode that used to leave
    // `transport.media.max_p99_ms` parsed, serialised, JSON
    // round-tripped and then silently dropped.
    {
        const ProfileTransportInput in = to_transport_input(profile);
        cfg.transport_strategy_name    = std::string{in.strategy};
        cfg.transport_media_max_p99_ms = in.media_max_p99_ms;
    }

    // ---- Video scene (profile schema v1.2) --------------------------------
    //
    // The four v1.2 video-scene keys used to be parsed, serialised and
    // JSON-round-tripped — and then **silently dropped**, because nothing
    // carried them from the Profile to the `EngineConfig`.  That is the
    // exact failure mode this file was written to end: a Profile that
    // says "screen content, 720p floor, smoothness ladder" produced an
    // engine that knew none of it, so the remote-desktop scene ran with
    // the camera defaults (no content hint, no floor, quality ladder).
    //
    // The four copies are guarded by the *meaning* of the value, not by a
    // plugin id:
    //
    //   - `video_codec_preference` names the **codec plugin**.  An empty
    //     value means "no video codec" (the engine's own meaning of an
    //     empty `video_codec_name`), so an empty preference must not
    //     clobber a codec the host configured.  A non-empty preference
    //     *is* the plugin id — the vocabulary is the plugin ids.
    //   - `video_resolution_floor` is a picture height.  0 means "no
    //     floor", which is the engine's own default, so the copy is
    //     always safe.
    //   - `video_ladder_mode` and `video_content_type` are always
    //     copied: their defaults are the engine's defaults, so a
    //     Profile that does not mention them leaves the engine where it
    //     already was.
    if (!profile.video_codec_preference.empty()) {
        cfg.video_codec_name = profile.video_codec_preference;
    }
    cfg.video_resolution_floor_height =
        video_resolution_floor_height_of(profile.video_resolution_floor);
    cfg.video_ladder_mode = video_ladder_mode_of(profile.video_ladder_mode);
    cfg.video_content_hint = video_content_hint_of(profile.video_content_type);

    // The declared per-stream shares (remote-desktop plan D5) are copied
    // verbatim: the map *is* the declaration, and the engine applies it
    // to the default stream at open() and to an extra stream when that
    // stream is created.
    //
    // The copy is unconditional (an empty map is the engine's own "every
    // stream splits the pool evenly" default).  The bridge does **not**
    // second-guess a declaration: the engine owns the rules (it refuses
    // to mute the default stream, for instance), and a bridge that
    // duplicated those rules would be a second place for them to drift.
    cfg.video_stream_weights = profile.video_stream_weights;

    return cfg;
}

// ---------------------------------------------------------------------------
// to_engine_config — the convenience overload
// ---------------------------------------------------------------------------
//
// Returns a freshly default-constructed `EngineConfig` filled from the
// Profile.  Identical to `to_engine_config(profile, EngineConfig{})`.
//
// (The two-argument overload deliberately has **no** default argument —
// a default would make `to_engine_config(profile)` ambiguous between the
// two overloads.)
NIMRTC_API_STABLE inline engine::EngineConfig
to_engine_config(const Profile& profile) {
    return to_engine_config(profile, engine::EngineConfig{});
}

}  // namespace nimrtc::assembly
