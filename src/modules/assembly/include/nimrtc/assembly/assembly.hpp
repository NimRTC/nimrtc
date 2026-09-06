#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// =============================================================================
// nimrtc::assembly
// --------------------------------------------------------------------------------
// Scenario assembly: Profile struct, ProfileRegistry singleton, and Builder.
//
// Design (NimRTC-V2 技术文档 §2.6, §5):
//   - Profile bundles every plugin-choice and per-scenario switch needed to
//     configure one NimRTCEngine instance.
//   - ProfileRegistry is a global singleton that holds all known Profile
//     instances (both built-in C++ constants and JSON-loaded ones).
//   - Builder provides a fluent API: load a named profile, then override
//     individual fields before handing the final Profile to NimRTCEngine.
//
// Thread-safety:
//   - ProfileRegistry::instance() is Meyers' singleton — thread-safe.
//   - Registration (register_profile) is NOT thread-safe; call it at startup.
//   - Reader functions (find, all_names) are thread-safe.
// =============================================================================

namespace nimrtc::assembly {

// ---------------------------------------------------------------------------
// SchedulerConfig — packet-scheduling priority configuration
// ---------------------------------------------------------------------------
struct SchedulerConfig {
    /// Enable the priority scheduler. When false, packets are sent FIFO.
    bool enabled = true;

    /// Scheduling priority strategy.
    enum class Strategy {
        kStrictPriority,  ///< High-priority frames always preempt low-priority.
        kWeightedFair,    ///< Weighted fair queuing across priority bands.
        kFixedRate        ///< Fixed bandwidth reservation per priority band.
    };
    Strategy strategy = Strategy::kStrictPriority;

    /// Per-priority-band weight (WeightedFair mode only).
    std::uint8_t control_weight     = 10;  ///< RTCP / STUN / signaling
    std::uint8_t audio_weight       = 8;   ///< audio frames
    std::uint8_t video_weight       = 4;   ///< video frames
    std::uint8_t best_effort_weight = 1;   ///< background / bulk data
};

// ---------------------------------------------------------------------------
// BweConfig — bandwidth estimator settings
// ---------------------------------------------------------------------------
struct BweConfig {
    /// BWE implementation name. Known values: "aimd" (P1 default), "googcc" (P3).
    std::string impl = "aimd";

    /// Initial target bitrate in bits per second.
    std::uint32_t initial_bitrate_bps = 1'000'000;  // 1 Mbps

    /// Maximum bitrate ceiling; BWE will never suggest above this.
    std::uint32_t max_bitrate_bps = 10'000'000;  // 10 Mbps
};

// ---------------------------------------------------------------------------
// JitterBufferConfig — adaptive jitter buffer settings
// ---------------------------------------------------------------------------
struct JitterBufferConfig {
    /// JitterBuffer implementation name. Known values: "adaptive" (P1 default).
    std::string impl = "adaptive";

    /// Buffer operating mode — trades off latency vs. robustness.
    enum class Mode {
        kLowLatency,   ///< Minimum delay; best for interactive call/agent.
        kLive,         ///< Moderate delay; good for live streaming.
        kInteractive,  ///< Maximum robustness; for hostile networks.
    };
    Mode mode = Mode::kLowLatency;

    /// Initial buffer depth in milliseconds.
    int initial_delay_ms = 40;

    /// Minimum buffer depth in milliseconds (lower bound).
    int min_delay_ms = 10;

    /// Maximum buffer depth in milliseconds (upper bound).
    int max_delay_ms = 200;
};

// ---------------------------------------------------------------------------
// Profile — scenario assembly unit
// ---------------------------------------------------------------------------
//
// We use std::string (owning) rather than std::string_view (non-owning)
// so that Profile values can be safely serialised/deserialised via JSON
// (where the JSON parser creates temporary strings; with string_view
// those temporaries would dangle after the parser returns).  Performance
// impact is negligible — Profile is constructed once at engine setup,
// not on the hot path.
//
struct Profile {
    /// Human-readable unique identifier, e.g. "call", "live", "agent".
    std::string name;

    // ---- Plugin name bindings -----------------------------------------------
    /// Transport plugin name. Empty string means "no transport" (pure relay).
    std::string transport_name = "ice";

    /// SDP plugin name.
    std::string sdp_name = "webrtc";

    /// RTP plugin name.
    std::string rtp_name = "webrtc";

    /// JitterBuffer plugin name. Empty string means no JB (raw passthrough).
    std::string jb_name = "adaptive";

    /// Audio 3A plugin name. Empty string means no 3A (passthrough).
    std::string audio3a_name = "webrtc";

    /// Audio codec plugin name.
    std::string codec_name = "opus";

    /// Bandwidth estimator plugin name. Empty string means no BWE.
    std::string bwe_name = "aimd";

    /// Scheduler plugin name.
    std::string scheduler_name = "default";

    // ---- Scenario-level feature flags ----------------------------------------
    /// Enable per-frame timeline instrumentation (agent / teleop = true).
    bool timeline_enabled = false;

    /// Enable DataChannel for binary/control data (agent / teleop = true).
    bool datachannel_enabled = false;

    // ---- Sub-module configurations -------------------------------------------
    SchedulerConfig   scheduler;
    BweConfig         bwe;
    JitterBufferConfig jitter_buffer;

    // ---- Audio format --------------------------------------------------------
    /// Audio sample rate in Hz (default: 48 000 Hz).
    std::uint32_t audio_sample_rate_hz = 48'000;

    /// Audio channel count (1 = mono, 2 = stereo).
    std::uint8_t audio_channels = 1;

    /// RTP payload type for the audio codec (Opus default = 111).
    std::uint8_t audio_payload_type = 111;
};

// ---------------------------------------------------------------------------
// ProfileRegistry — global singleton for named profile lookups
// ---------------------------------------------------------------------------
class ProfileRegistry {
public:
    /// Returns the singleton instance.
    static ProfileRegistry& instance();

    /// Register (or overwrite) a profile under the given name.
    void register_profile(Profile p);

    /// Lookup a profile by name. Returns nullptr if not found.
    [[nodiscard]] const Profile* find(std::string_view name) const;

    /// Returns all registered profile names.
    [[nodiscard]] std::vector<std::string> all_names() const;

    // Prevent copying/moving.
    ProfileRegistry(const ProfileRegistry&)            = delete;
    ProfileRegistry& operator=(const ProfileRegistry&) = delete;

private:
    ProfileRegistry();

    std::map<std::string, Profile> profiles_;
};

// ---------------------------------------------------------------------------
// Builder — fluent Profile construction
// ---------------------------------------------------------------------------
//
// Usage:
//   auto profile = Builder{}
//       .load_profile("agent")
//       .override_transport("ice")
//       .override_bwe({ .initial_bitrate_bps = 500'000 })
//       .build();
//
class Builder {
public:
    /// Load a named profile from the registry (mutates internal state).
    /// Returns *this for chaining. Logs a warning if the name is not found.
    Builder& load_profile(std::string_view name);

    /// Override the transport plugin name.
    Builder& override_transport(std::string_view name);

    /// Override the bandwidth estimator configuration.
    Builder& override_bwe(BweConfig cfg);

    /// Override the scheduler configuration.
    Builder& override_scheduler(SchedulerConfig cfg);

    /// Override the jitter buffer configuration.
    Builder& override_jitter_buffer(JitterBufferConfig cfg);

    /// Enable or disable per-frame timeline instrumentation.
    Builder& override_timeline(bool enabled);

    /// Enable or disable DataChannel.
    Builder& override_datachannel(bool enabled);

    /// Finalise and return the Profile. The Builder remains valid for reuse.
    [[nodiscard]] Profile build() const;

private:
    Profile profile_;
};

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------

/// Load a Profile from a JSON file located at the given absolute path.
/// Throws nlohmann::json::parse_error on malformed JSON.
/// Empty/null plugin-name fields are converted to empty strings.
Profile profile_from_json_file(const std::string& file_path);

/// Serialise a Profile to a JSON string. Empty plugin names appear as "".
std::string profile_to_json_string(const Profile& p);

}  // namespace nimrtc::assembly
