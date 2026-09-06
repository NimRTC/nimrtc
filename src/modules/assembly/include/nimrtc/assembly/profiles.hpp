#pragma once

// =============================================================================
// nimrtc::assembly::profiles
// --------------------------------------------------------------------------------
// Pre-defined Profile constants for all built-in NimRTC scenarios.
//
// Each constant corresponds to one entry in the §2.6 Profile Table:
//   https://example.com/docs/zh/NimRTC-V2-技术文档.md#_profile-table
//
// These are static objects (link-time constants) that are automatically
// registered by ProfileRegistry::instance() at static-init time.
//
// Applications may also override or add profiles via Builder + JSON files
// (see profiles/ directory).
//
// Scenario summary:
//   call     — 1:1 bidirectional voice call (interactive)
//   live     — live broadcast / watch (server push, minimal latency)
//   transport — raw relay gateway: ICE transport only, no media processing
//   agent    — AI Agent SDK: DataChannel + timeline + low-latency JB
//   teleop   — robot teleoperation: strict priority scheduler + datachannel
//   sfu      — SFU forwarding: skips L2 media processing entirely
// =============================================================================

#include <nimrtc/assembly/assembly.hpp>

namespace nimrtc::assembly {

// ---------------------------------------------------------------------------
// kProfileCall — 1:1 bidirectional voice/video call
// ---------------------------------------------------------------------------
// Design notes:
//   - LowLatency jitter buffer: minimises round-trip delay.
//   - WebRTC 3A (AEC/ANR/AGC) enabled for clean audio.
//   - AIMD BWE: standard TCP-friendly congestion control.
//   - No timeline instrumentation: reduces overhead for voice-only paths.
//   - No DataChannel: not needed for standard VoIP call.
// ---------------------------------------------------------------------------
inline const Profile kProfileCall = Profile{
    .name                 = "call",
    .transport_name       = "ice",
    .sdp_name             = "webrtc",
    .rtp_name             = "webrtc",
    .jb_name              = "adaptive",
    .audio3a_name         = "webrtc",
    .codec_name           = "opus",
    .bwe_name             = "aimd",
    .scheduler_name       = "default",
    .timeline_enabled     = false,
    .datachannel_enabled  = false,
    .scheduler            = {
        .enabled  = true,
        .strategy = SchedulerConfig::Strategy::kStrictPriority,
        .control_weight     = 10,
        .audio_weight       = 8,
        .video_weight       = 4,
        .best_effort_weight = 1,
    },
    .bwe = {
        .impl               = "aimd",
        .initial_bitrate_bps = 1'000'000,   // 1 Mbps
        .max_bitrate_bps    = 10'000'000,   // 10 Mbps
    },
    .jitter_buffer = {
        .impl           = "adaptive",
        .mode           = JitterBufferConfig::Mode::kLowLatency,
        .initial_delay_ms = 40,
        .min_delay_ms   = 10,
        .max_delay_ms   = 200,
    },
    .audio_sample_rate_hz = 48'000,
    .audio_channels      = 1,
    .audio_payload_type  = 111,  // Opus
};

// ---------------------------------------------------------------------------
// kProfileLive — live broadcast / one-way streaming
// ---------------------------------------------------------------------------
// Design notes:
//   - Live-mode jitter buffer: higher depth than kLowLatency to absorb
//     burst packet re-ordering common in CDN/UDP paths.
//   - No 3A: sender side typically already processed; receiver may run its own.
//   - DataChannel disabled: broadcast is send-only.
//   - Timeline instrumentation disabled: not needed for CDN-style push.
// ---------------------------------------------------------------------------
inline const Profile kProfileLive = Profile{
    .name                 = "live",
    .transport_name       = "ice",
    .sdp_name             = "webrtc",
    .rtp_name             = "webrtc",
    .jb_name              = "adaptive",
    .audio3a_name         = "webrtc",
    .codec_name           = "opus",
    .bwe_name             = "aimd",
    .scheduler_name       = "default",
    .timeline_enabled     = false,
    .datachannel_enabled  = false,
    .scheduler            = {
        .enabled  = true,
        .strategy = SchedulerConfig::Strategy::kWeightedFair,
        .control_weight     = 10,
        .audio_weight       = 8,
        .video_weight       = 4,
        .best_effort_weight = 1,
    },
    .bwe = {
        .impl               = "aimd",
        .initial_bitrate_bps = 2'000'000,   // 2 Mbps (higher for video)
        .max_bitrate_bps    = 20'000'000,   // 20 Mbps
    },
    .jitter_buffer = {
        .impl           = "adaptive",
        .mode           = JitterBufferConfig::Mode::kLive,
        .initial_delay_ms = 80,             // more headroom than call
        .min_delay_ms   = 30,
        .max_delay_ms   = 500,
    },
    .audio_sample_rate_hz = 48'000,
    .audio_channels      = 1,
    .audio_payload_type  = 111,
};

// ---------------------------------------------------------------------------
// kProfileTransport — embedded ICE transport gateway (no media processing)
// ---------------------------------------------------------------------------
// Design notes:
//   - All media plugin names are empty strings: no JB, no 3A, no BWE.
//     This makes the engine act as a pure DTLS-SRTP passthrough.
//   - Suitable for: embedded gateways, protocol translators, test harnesses.
//   - Scheduler is disabled: no priority logic needed for raw relay.
// ---------------------------------------------------------------------------
inline const Profile kProfileTransport = Profile{
    .name                 = "transport",
    .transport_name       = "ice",
    .sdp_name             = "webrtc",
    .rtp_name             = "webrtc",
    .jb_name              = "",             // no jitter buffer
    .audio3a_name         = "",             // no 3A
    .codec_name           = "",             // no codec
    .bwe_name             = "",             // no bandwidth estimator
    .scheduler_name       = "default",
    .timeline_enabled     = false,
    .datachannel_enabled  = false,
    .scheduler            = {
        .enabled  = false,
        .strategy = SchedulerConfig::Strategy::kStrictPriority,
    },
    .bwe = {},  // all defaults overridden by empty impl string
    .jitter_buffer = {
        .impl           = "",
        .mode           = JitterBufferConfig::Mode::kLowLatency,
        .initial_delay_ms = 0,
        .min_delay_ms   = 0,
        .max_delay_ms   = 0,
    },
    .audio_sample_rate_hz = 48'000,
    .audio_channels      = 1,
    .audio_payload_type  = 111,
};

// ---------------------------------------------------------------------------
// kProfileAgent — AI Agent SDK (voice/text interaction)
// ---------------------------------------------------------------------------
// Design notes:
//   - LowLatency JB: interactive voice requires minimal buffering.
//   - DataChannel enabled: SDK uses it for control messages and state sync.
//   - Timeline enabled: per-frame timestamps are required for latency
//     observability in agent pipelines.
//   - WebRTC 3A enabled: clean audio input for ASR/STT models.
//   - AIMD BWE: standard TCP-friendly rate control.
// ---------------------------------------------------------------------------
inline const Profile kProfileAgent = Profile{
    .name                 = "agent",
    .transport_name       = "ice",
    .sdp_name             = "webrtc",
    .rtp_name             = "webrtc",
    .jb_name              = "adaptive",
    .audio3a_name         = "webrtc",
    .codec_name           = "opus",
    .bwe_name             = "aimd",
    .scheduler_name       = "default",
    .timeline_enabled     = true,           // required for agent latency observability
    .datachannel_enabled  = true,           // SDK control channel
    .scheduler            = {
        .enabled  = true,
        .strategy = SchedulerConfig::Strategy::kStrictPriority,
        .control_weight     = 10,
        .audio_weight       = 8,
        .video_weight       = 4,
        .best_effort_weight = 1,
    },
    .bwe = {
        .impl               = "aimd",
        .initial_bitrate_bps = 500'000,     // lower than call; agent may be low-bandwidth
        .max_bitrate_bps    = 5'000'000,
    },
    .jitter_buffer = {
        .impl           = "adaptive",
        .mode           = JitterBufferConfig::Mode::kLowLatency,
        .initial_delay_ms = 30,             // tighter than call for agent
        .min_delay_ms   = 5,
        .max_delay_ms   = 150,
    },
    .audio_sample_rate_hz = 48'000,
    .audio_channels      = 1,
    .audio_payload_type  = 111,
};

// ---------------------------------------------------------------------------
// kProfileTeleop — robot teleoperation
// ---------------------------------------------------------------------------
// Design notes:
//   - StrictPriority scheduler: control/emergency packets MUST preempt all else.
//   - No 3A: audio path may be disabled entirely for pure teleop scenarios.
//   - DataChannel enabled: robot state telemetry, sensor streams.
//   - Timeline enabled: frame timing is critical for closed-loop control.
//   - LowLatency JB: minimises motion-to-video delay.
// ---------------------------------------------------------------------------
inline const Profile kProfileTeleop = Profile{
    .name                 = "teleop",
    .transport_name       = "ice",
    .sdp_name             = "webrtc",
    .rtp_name             = "webrtc",
    .jb_name              = "adaptive",
    .audio3a_name         = "",             // 3A may be disabled in pure teleop
    .codec_name           = "opus",
    .bwe_name             = "aimd",
    .scheduler_name       = "default",
    .timeline_enabled     = true,           // required for closed-loop control
    .datachannel_enabled  = true,           // robot telemetry
    .scheduler            = {
        .enabled  = true,
        .strategy = SchedulerConfig::Strategy::kStrictPriority,
        .control_weight     = 20,           // control messages get higher weight
        .audio_weight       = 8,
        .video_weight       = 4,
        .best_effort_weight = 1,
    },
    .bwe = {
        .impl               = "aimd",
        .initial_bitrate_bps = 2'000'000,   // 2 Mbps (video + telemetry)
        .max_bitrate_bps    = 20'000'000,
    },
    .jitter_buffer = {
        .impl           = "adaptive",
        .mode           = JitterBufferConfig::Mode::kLowLatency,
        .initial_delay_ms = 20,             // lowest possible for teleop
        .min_delay_ms   = 5,
        .max_delay_ms   = 100,
    },
    .audio_sample_rate_hz = 48'000,
    .audio_channels      = 1,
    .audio_payload_type  = 111,
};

// ---------------------------------------------------------------------------
// kProfileSfu — server-side forwarding (SFU hop, no L2 processing)
// ---------------------------------------------------------------------------

// Design notes:
//   - All media processing plugins are disabled: SFU only forwards SRTP
//     packets without touching the media — no JB, no 3A, no BWE.
//   - No audio format needed: SFU is format-agnostic.
//   - Scheduler disabled: SFU operates at the transport layer.
//   - Suitable for: SFU middlebox, media recorder, protocol bridge.
// ---------------------------------------------------------------------------
inline const Profile kProfileSfu = Profile{
    .name                 = "sfu",
    .transport_name       = "ice",
    .sdp_name             = "webrtc",
    .rtp_name             = "webrtc",
    .jb_name              = "",             // no jitter buffer
    .audio3a_name         = "",             // no 3A
    .codec_name           = "",             // no codec
    .bwe_name             = "",             // no bandwidth estimator
    .scheduler_name       = "default",
    .timeline_enabled     = false,
    .datachannel_enabled  = false,
    .scheduler            = {
        .enabled  = false,
        .strategy = SchedulerConfig::Strategy::kStrictPriority,
    },
    .bwe = {},
    .jitter_buffer = {
        .impl           = "",
        .mode           = JitterBufferConfig::Mode::kLowLatency,
        .initial_delay_ms = 0,
        .min_delay_ms   = 0,
        .max_delay_ms   = 0,
    },
    .audio_sample_rate_hz = 0,              // not applicable for SFU
    .audio_channels      = 0,
    .audio_payload_type  = 0,
};

}  // namespace nimrtc::assembly
