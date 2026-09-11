// =============================================================================
// assembly.cpp — ProfileRegistry, Builder, and JSON helpers implementation
// =============================================================================

#include <fstream>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include <nimrtc/assembly/assembly.hpp>
#include <nimrtc/assembly/profiles.hpp>

// Convenience alias so we don't qualify every nlohmann:: call.
using json = nlohmann::json;

namespace nimrtc::assembly {

// ============================================================================
// ProfileRegistry
// ============================================================================

ProfileRegistry::ProfileRegistry() {
    // Static-init: register all built-in profiles at singleton construction.
    // These objects are defined in profiles.hpp and are guaranteed to be
    // initialised before this constructor runs (order is undefined between
    // translation units, but all are guaranteed to be constructed before
    // first use of instance() due to the "static local" idiom).
    profiles_.emplace(std::string{kProfileCall.name},     kProfileCall);
    profiles_.emplace(std::string{kProfileLive.name},      kProfileLive);
    profiles_.emplace(std::string{kProfileTransport.name}, kProfileTransport);
    profiles_.emplace(std::string{kProfileAgent.name},     kProfileAgent);
    profiles_.emplace(std::string{kProfileTeleop.name},   kProfileTeleop);
    profiles_.emplace(std::string{kProfileSfu.name},      kProfileSfu);
}

ProfileRegistry& ProfileRegistry::instance() {
    // Meyers' singleton — thread-safe in C++11+.
    static ProfileRegistry inst;
    return inst;
}

void ProfileRegistry::register_profile(Profile p) {
    // insert_or_assign is the C++17 standard map equivalent of unordered_map's
    // C++20 emplace_or_assign. MSVC's STL does not provide emplace_or_assign
    // for std::map (it only ships the unordered_map variant), so we use
    // insert_or_assign which has identical semantics.
    profiles_.insert_or_assign(std::string{p.name}, std::move(p));
}

const Profile* ProfileRegistry::find(std::string_view name) const {
    auto it = profiles_.find(std::string{name});
    if (it != profiles_.end()) {
        return &(it->second);
    }
    return nullptr;
}

std::vector<std::string> ProfileRegistry::all_names() const {
    std::vector<std::string> names;
    names.reserve(profiles_.size());
    for (const auto& [key, _] : profiles_) {
        names.push_back(key);
    }
    return names;
}

// ============================================================================
// Builder
// ============================================================================

Builder& Builder::load_profile(std::string_view name) {
    const ProfileRegistry& reg = ProfileRegistry::instance();
    const Profile* p = reg.find(name);
    if (p != nullptr) {
        profile_ = *p;  // copy the found profile
    }
    // If not found, profile_ is left unchanged (allows building from scratch).
    return *this;
}

Builder& Builder::override_transport(std::string_view name) {
    profile_.transport_name = name;
    return *this;
}

Builder& Builder::override_bwe(BweConfig cfg) {
    profile_.bwe = std::move(cfg);
    return *this;
}

Builder& Builder::override_scheduler(SchedulerConfig cfg) {
    profile_.scheduler = std::move(cfg);
    return *this;
}

Builder& Builder::override_jitter_buffer(JitterBufferConfig cfg) {
    profile_.jitter_buffer = std::move(cfg);
    return *this;
}

Builder& Builder::override_timeline(bool enabled) {
    profile_.timeline_enabled = enabled;
    return *this;
}

Builder& Builder::override_datachannel(bool enabled) {
    profile_.datachannel_enabled = enabled;
    return *this;
}

Profile Builder::build() const {
    return profile_;
}

// ============================================================================
// JSON helpers
// ============================================================================

// Helper: convert a JSON string to a std::string (handles null).
static std::string json_to_string(const json& j) {
    if (j.is_null()) return {};
    return j.get<std::string>();
}

// Helper: convert a JitterBufferConfig::Mode enum to/from string.
static const char* jitter_buffer_mode_to_string(JitterBufferConfig::Mode mode) {
    switch (mode) {
        case JitterBufferConfig::Mode::kLowLatency:   return "low_latency";
        case JitterBufferConfig::Mode::kLive:         return "live";
        case JitterBufferConfig::Mode::kInteractive:  return "interactive";
    }
    return "low_latency";
}

static JitterBufferConfig::Mode string_to_jitter_buffer_mode(std::string_view s) {
    if (s == "live")        return JitterBufferConfig::Mode::kLive;
    if (s == "interactive") return JitterBufferConfig::Mode::kInteractive;
    return JitterBufferConfig::Mode::kLowLatency;  // default
}

// Helper: convert a SchedulerConfig::Strategy enum to/from string.
static const char* scheduler_strategy_to_string(SchedulerConfig::Strategy s) {
    switch (s) {
        case SchedulerConfig::Strategy::kStrictPriority: return "strict_priority";
        case SchedulerConfig::Strategy::kWeightedFair:   return "weighted_fair";
        case SchedulerConfig::Strategy::kFixedRate:      return "fixed_rate";
    }
    return "strict_priority";
}

static SchedulerConfig::Strategy string_to_scheduler_strategy(std::string_view s) {
    if (s == "weighted_fair") return SchedulerConfig::Strategy::kWeightedFair;
    if (s == "fixed_rate")     return SchedulerConfig::Strategy::kFixedRate;
    return SchedulerConfig::Strategy::kStrictPriority;
}

// ---------------------------------------------------------------------------
// profile_from_json_file
// ---------------------------------------------------------------------------
Profile profile_from_json_file(const std::string& file_path) {
    std::ifstream file{file_path};
    if (!file) {
        throw std::runtime_error("assembly: cannot open profile JSON file: " + file_path);
    }

    json root;
    try {
        // nlohmann_json's stream-operator `file >> root` does not honour
        // ignore_comments, so we go straight through json::parse() with
        // ignore_comments=true. This allows profile JSON files to contain
        // `//` and `/* */` comments — convenient for documenting fields
        // directly in production config files.
        json::parse(file, /*cb=*/nullptr, /*allow_exceptions=*/true,
                    /*ignore_comments=*/true).swap(root);
    } catch (const json::parse_error& e) {
        throw std::runtime_error("assembly: JSON parse error in " + file_path + ": " + e.what());
    }

    Profile p;

    // -- name (required) -------------------------------------------------------
    p.name = json_to_string(root.value("name", ""));

    // -- plugin name bindings --------------------------------------------------
    p.transport_name  = json_to_string(root.value("transport_name",  "ice"));
    p.sdp_name        = json_to_string(root.value("sdp_name",         "webrtc"));
    p.rtp_name        = json_to_string(root.value("rtp_name",         "webrtc"));
    p.jb_name         = json_to_string(root.value("jb_name",          "adaptive"));
    p.audio3a_name    = json_to_string(root.value("audio3a_name",      "webrtc_apm"));
    p.codec_name      = json_to_string(root.value("codec_name",        "opus"));
    p.bwe_name        = json_to_string(root.value("bwe_name",          "aimd"));
    p.scheduler_name  = json_to_string(root.value("scheduler_name",    "default"));

    // -- feature flags ---------------------------------------------------------
    p.timeline_enabled    = root.value("timeline_enabled",    false);
    p.datachannel_enabled = root.value("datachannel_enabled", false);

    // -- audio format ----------------------------------------------------------
    p.audio_sample_rate_hz = root.value("audio_sample_rate_hz", 48'000u);
    p.audio_channels       = static_cast<std::uint8_t>(root.value("audio_channels", 1));
    p.audio_payload_type  = static_cast<std::uint8_t>(root.value("audio_payload_type", 111));

    // -- BWE config ------------------------------------------------------------
    //
    // JSON schema (kept stable for backward compatibility):
    //   "bwe": {
    //       "impl":                "<plugin id, e.g. aimd>",
    //       "initial_bitrate_bps": <uint32>,   // -> params.initial_bitrate_bps
    //       "max_bitrate_bps":     <uint32>    // -> params.max_bitrate_bps
    //   }
    //
    // All other `plugins::BweConfig` fields fall back to their default
    // values; future schema extension may add `params: { ... }` as a
    // nested object to surface the full canonical config.
    if (root.contains("bwe") && root["bwe"].is_object()) {
        const auto& bwe = root["bwe"];
        p.bwe.impl = json_to_string(bwe.value("impl", "aimd"));
        p.bwe.params.initial_bitrate_bps =
            bwe.value("initial_bitrate_bps", 1'000'000u);
        p.bwe.params.max_bitrate_bps =
            bwe.value("max_bitrate_bps", 10'000'000u);
    }

    // -- jitter buffer config --------------------------------------------------
    if (root.contains("jitter_buffer") && root["jitter_buffer"].is_object()) {
        const auto& jb = root["jitter_buffer"];
        p.jitter_buffer.impl            = json_to_string(jb.value("impl", "adaptive"));
        p.jitter_buffer.mode            = string_to_jitter_buffer_mode(
            json_to_string(jb.value("mode", "low_latency")));
        p.jitter_buffer.initial_delay_ms = jb.value("initial_delay_ms", 40);
        p.jitter_buffer.min_delay_ms     = jb.value("min_delay_ms",     10);
        p.jitter_buffer.max_delay_ms     = jb.value("max_delay_ms",    200);
    }

    // -- scheduler config -------------------------------------------------------
    if (root.contains("scheduler") && root["scheduler"].is_object()) {
        const auto& sc = root["scheduler"];
        p.scheduler.enabled = sc.value("enabled", true);
        p.scheduler.strategy = string_to_scheduler_strategy(
            json_to_string(sc.value("strategy", "strict_priority")));
        p.scheduler.control_weight     = static_cast<std::uint8_t>(sc.value("control_weight", 10));
        p.scheduler.audio_weight       = static_cast<std::uint8_t>(sc.value("audio_weight", 8));
        p.scheduler.video_weight       = static_cast<std::uint8_t>(sc.value("video_weight", 4));
        p.scheduler.best_effort_weight = static_cast<std::uint8_t>(sc.value("best_effort_weight", 1));

        // `impl` is the plugin id resolved through PluginRegistry. Older
        // profile JSON files predate this field — auto-fill from the
        // Strategy so they continue to round-trip.
        const std::string impl_explicit = json_to_string(sc.value("impl", ""));
        p.scheduler.impl = impl_explicit.empty()
            ? std::string{default_impl_for(p.scheduler.strategy)}
            : impl_explicit;

        // `params` is the plugins::SchedulerConfig (avg packet size,
        // queue depth, keyframe protection). Optional — the default
        // constructed struct is fine for typical usage.
        if (sc.contains("params") && sc["params"].is_object()) {
            const auto& ps = sc["params"];
            p.scheduler.params.avg_packet_size_bytes =
                ps.value("avg_packet_size_bytes", 1200u);
            p.scheduler.params.max_queue_depth =
                ps.value("max_queue_depth",     8192u);
            p.scheduler.params.protect_keyframes =
                ps.value("protect_keyframes",   true);
        }
    }

    return p;
}

// ---------------------------------------------------------------------------
// profile_to_json_string
// ---------------------------------------------------------------------------
std::string profile_to_json_string(const Profile& p) {
    json root;

    root["name"]                 = std::string{p.name};
    root["transport_name"]       = std::string{p.transport_name};
    root["sdp_name"]             = std::string{p.sdp_name};
    root["rtp_name"]             = std::string{p.rtp_name};
    root["jb_name"]              = std::string{p.jb_name};
    root["audio3a_name"]         = std::string{p.audio3a_name};
    root["codec_name"]           = std::string{p.codec_name};
    root["bwe_name"]             = std::string{p.bwe_name};
    root["scheduler_name"]       = std::string{p.scheduler_name};

    root["timeline_enabled"]     = p.timeline_enabled;
    root["datachannel_enabled"]  = p.datachannel_enabled;

    root["audio_sample_rate_hz"] = p.audio_sample_rate_hz;
    root["audio_channels"]       = p.audio_channels;
    root["audio_payload_type"]  = p.audio_payload_type;

    // BWE sub-object — keep the legacy top-level schema so existing profile
    // JSONs continue to round-trip. We serialise `params.initial_bitrate_bps`
    // / `params.max_bitrate_bps` at the top level (the canonical
    // `plugins::BweConfig` lives at `params`, but the on-disk layout is
    // intentionally flat for backward compatibility).
    root["bwe"] = json::object();
    root["bwe"]["impl"]                 = std::string{p.bwe.impl};
    root["bwe"]["initial_bitrate_bps"] = p.bwe.params.initial_bitrate_bps;
    root["bwe"]["max_bitrate_bps"]     = p.bwe.params.max_bitrate_bps;

    // JitterBuffer sub-object
    root["jitter_buffer"] = json::object();
    root["jitter_buffer"]["impl"]           = std::string{p.jitter_buffer.impl};
    root["jitter_buffer"]["mode"]           = jitter_buffer_mode_to_string(p.jitter_buffer.mode);
    root["jitter_buffer"]["initial_delay_ms"] = p.jitter_buffer.initial_delay_ms;
    root["jitter_buffer"]["min_delay_ms"]   = p.jitter_buffer.min_delay_ms;
    root["jitter_buffer"]["max_delay_ms"]   = p.jitter_buffer.max_delay_ms;

    // Scheduler sub-object
    root["scheduler"] = json::object();
    root["scheduler"]["enabled"]             = p.scheduler.enabled;
    root["scheduler"]["strategy"]            = scheduler_strategy_to_string(p.scheduler.strategy);
    root["scheduler"]["control_weight"]     = p.scheduler.control_weight;
    root["scheduler"]["audio_weight"]       = p.scheduler.audio_weight;
    root["scheduler"]["video_weight"]        = p.scheduler.video_weight;
    root["scheduler"]["best_effort_weight"]  = p.scheduler.best_effort_weight;

    // Plugin binding (new in P1 — absent on legacy profiles).
    // We emit `impl` as the resolved value (after default-filling), so
    // the round-trip is byte-identical even when the source JSON omits
    // `impl`. The legacy fields above remain first-class for backwards
    // compatibility.
    root["scheduler"]["impl"] = std::string{resolve_scheduler_impl(p.scheduler)};

    // Runtime parameters (plugins::SchedulerConfig). Always emitted so
    // a downstream consumer that only reads `impl + params` doesn't have
    // to special-case missing fields.
    root["scheduler"]["params"] = json::object();
    root["scheduler"]["params"]["avg_packet_size_bytes"] =
        p.scheduler.params.avg_packet_size_bytes;
    root["scheduler"]["params"]["max_queue_depth"] =
        p.scheduler.params.max_queue_depth;
    root["scheduler"]["params"]["protect_keyframes"] =
        p.scheduler.params.protect_keyframes;

    return root.dump(/*indent=*/2);
}

}  // namespace nimrtc::assembly
