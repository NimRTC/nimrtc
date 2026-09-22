# Authoring a Profile

> **Audience**: integrators adding a new assembly profile to
> NimRTC — either alongside the built-in `call` / `live` / `teleop`
> / `agent-gateway` / etc., or as a custom build of their own.
>
> **Pre-requisites**: familiar with `docs/profiles.md`, ADR-010,
> and the JSON schema referenced therein.

---

## 1. What a Profile is (and isn't)

A `Profile` is a **compile-time, JSON-serialisable** description
of how `nimrtc::assembly::Builder` should wire the engine. Profiles
describe capabilities and plugin ids, not runtime state — they
have no `tick()`, no callbacks, no threads.

A Profile is **not**:

- a process state machine (use the engine lifecycle for that);
- a build-time decision (use CMake options / PAL Slice 1–8 instead
  — `Profiles` compile-time == linker-time == JSON parse-time);
- a transport policy (DataChannel QoS is set per channel at
  `create_data_channel(label)` time, not at profile level).

If you find yourself reaching for a knob that the `Profile` struct
doesn't have, ask "is this per-engine configuration or per-profile
morphology?" — if the former, it belongs in `EngineConfig`; if the
latter, see below.

---

## 2. Anatomy of a Profile

```jsonc
{
  "name": "<profile-id>",            // required, stable, lower_snake_case
  "transport_name": "<plugin-id>",
  "sdp_name":       "<plugin-id>",
  "rtp_name":       "<plugin-id>",
  "jb_name":        "<plugin-id>",
  "audio3a_name":   "<plugin-id>",   // "" disables 3A
  "codec_name":     "<plugin-id>",
  "bwe_name":       "<plugin-id>",
  "scheduler_name": "<plugin-id>",

  "timeline_enabled":     true|false,
  "datachannel_enabled":  true|false,
  "audio_sample_rate_hz": 48000,
  "audio_channels":       1,
  "audio_payload_type":   111,        // RFC 7587 opus

  "bwe": {
    "impl": "<plugin-id>",
    "initial_bitrate_bps": <int>,
    "max_bitrate_bps":     <int>,
    // ... additional parameters depend on the plugin ...
  },
  "jitter_buffer": {
    "impl": "<plugin-id>",
    "mode": "low_latency" | "live",
    "initial_delay_ms": <int>,
    "min_delay_ms":     <int>,
    "max_delay_ms":     <int>
  },
  "scheduler": {
    "enabled":         true|false,
    "strategy":        "strict_priority",
    "control_weight":     <int>,
    "audio_weight":       <int>,
    "video_weight":       <int>,
    "best_effort_weight": <int>
  },

  // ---- Schema v1.1 markers (loader-agnostic; v0.11.0+) ----
  "sfu_enabled": true|false,
  "agent_gateway_enabled": true|false,
  "agent_low_latency": true|false,
  "max_sessions_per_engine": <int>,
  "audio3a_name_explicit": true
}
```

Every key in the JSON corresponds to a field in `Profile`. The
C++ `Builder` enforces schema + invariants at parse time.

---

## 3. The two-file pattern: JSON + C++ constant

> Two-file because the [JSON ↔ C++ invariant](profiles.md) is
> enforced by a unit test that fails the build if they drift.

For each Profile, you ship **two** artefacts:

1. **`profiles/<name>.json`** — the canonical, reviewable,
   diff-friendly source.
2. **`kProfile<Name>`** constant in
   `src/modules/assembly/include/nimrtc/assembly/profiles.hpp`.

These are version-controlled together and registered via
`ProfileRegistry::instance()` so the engine sees both forms.

### 3.1 Add the JSON file

Copy an existing profile (e.g. `profiles/teleop.json`) and edit:

```bash
cp profiles/teleop.json profiles/<my-profile>.json
$EDITOR profiles/<my-profile>.json
```

Set `"name": "<my-profile>"` first. Pick plugin ids **before**
you publish — they must already be registered in
`core::PluginRegistry::register_*` (built-in or your own
implementation). Unknown ids cause `Builder::load_profile()` to
return errors that surface via `on_error_`.

Update the header comment block with:

- **Use case** in one sentence.
- **Design rationale** for each non-default knob.
- **Override tips** for downstream integrations.
- **Schema version** if you introduce any v1.x additive fields.

### 3.2 Add the C++ constant

In `profiles.hpp`, append:

```cpp
inline const Profile kProfile<Name> = Profile{
    .name = "<my-profile>",
    .transport_name = "ice",
    // ... match the JSON exactly ...
};
```

The design-rationale goes in a comment block matching the JSON's
header comment.

### 3.3 Register

In `src/modules/assembly/src/assembly.cpp`, add to the
`ProfileRegistry::ProfileRegistry()` ctor:

```cpp
profiles_.emplace("my-profile", &kProfile<Name>);
```

### 3.4 Add the invariant test

In `tests/test_assembly.cpp` (or equivalent), add:

```cpp
TEST(Assembly, JsonFileMatchesBuiltinConstant_MyProfile) {
    auto builtin = nimrtc::assembly::kProfile<Name>;
    auto json    = nimrtc::assembly::profile_from_json_file(
        "profiles/<my-profile>.json");
    ASSERT_EQ(json, builtin);
}
```

Drift (default values changed in one place but not the other) fails
the build.

---

## 4. Which fields are mandatory?

| Field | Mandatory? | Notes |
|---|---|---|
| `name` | yes | Stable id; matches the file name. |
| `transport_name` | yes | Default = `"ice"`. |
| `sdp_name`, `rtp_name`, `jb_name` | yes | Defaults match the call profile. |
| `audio3a_name` | yes | `""` disables 3A (raw PCM passthrough). |
| `codec_name` | yes | `""` falls back to first statically-linked codec. |
| `bwe_name`, `scheduler_name` | yes | Defaults = `"aimd"` / `"default"`. |
| `timeline_enabled` | yes | Default = `false` for non-Agent profiles. |
| `datachannel_enabled` | yes | Default = `false` for non-Agent / non-SFU profiles. |
| `audio_sample_rate_hz` | yes | Typically 48000 (Opus) or 8000 (PCMU/PCMA). |
| `audio_channels` | yes | 1 for telephony, 2 for stereo music. |
| `audio_payload_type` | yes | 111 for Opus, 0 for PCMU, 8 for PCMA. |
| `bwe` | yes | At minimum `impl` + `initial_bitrate_bps`. |
| `jitter_buffer` | yes | At minimum `impl` + `mode` + `initial_delay_ms`. |
| `scheduler` | yes | `enabled` + `strategy` + weights. |
| v1.1 markers (`sfu_enabled`, `agent_gateway_enabled`, …) | no | Loader-agnostic; populated only by profiles that consume them. |

If a field is missing in JSON, the Builder errors out at load —
there is no "quiet default fill" beyond the dotted-line keys.

---

## 5. Worked example: adding a `cloudgame` profile

A user wants a profile that targets cloud rendering:

- aggressive low-latency (15 ms JB initial);
- high BWE initial bitrate (5 Mbps);
- DataChannel for input events (mouse / keyboard);
- 3A required (microphone-driven voice chat);
- timeline required.

### 5.1 `profiles/cloudgame.json`

```jsonc
{
  // ==========================================================================
  // profiles/cloudgame.json — cloud rendering / cloud gaming profile
  // ==========================================================================
  //
  // Use case: remote rendering pipeline that streams GPU-produced frames
  // back to a thin client. The thin client renders locally and sends
  // input events back over DataChannel. Optionally carries a voice
  // side-channel.
  //
  // Design rationale (v0.11.0 PROFILE-1 family extension; not in v1.0
  // schema, lands in v1.0+ once schema v1.2 is locked):
  //   - jitter_buffer.initial_delay_ms = 15: tightest in the schema,
  //     justified by the closed-loop nature (frame → input → frame
  //     round-trip dominates perceived latency).
  //   - bwe.initial_bitrate_bps = 5 Mbps / cap 50 Mbps: matches modern
  //     1080p60 compressed video bitrate envelopes.
  //   - datachannel_enabled = true with control_weight = 30 (3x call): input
  //     events MUST preempt everything else.
  //   - timeline_enabled = true: per docs/zh/architecture.md §8.4 ref_frame
  //     binding is essential for synchronising client inputs with remote
  //     frame timing.
  // ==========================================================================

  "name": "cloudgame",
  "transport_name": "ice",
  "sdp_name":       "webrtc",
  "rtp_name":       "webrtc",
  "jb_name":        "adaptive",
  "audio3a_name":   "webrtc_apm",
  "codec_name":     "opus",
  "bwe_name":       "aimd",
  "scheduler_name": "default",

  "timeline_enabled":    true,
  "datachannel_enabled": true,

  "audio_sample_rate_hz": 48000,
  "audio_channels":       1,
  "audio_payload_type":   111,

  "bwe": {
    "impl": "aimd",
    "initial_bitrate_bps": 5000000,
    "max_bitrate_bps":     50000000
  },
  "jitter_buffer": {
    "impl":             "adaptive",
    "mode":             "low_latency",
    "initial_delay_ms": 15,
    "min_delay_ms":     5,
    "max_delay_ms":     100
  },
  "scheduler": {
    "enabled":            true,
    "strategy":           "strict_priority",
    "control_weight":     30,
    "audio_weight":       8,
    "video_weight":       4,
    "best_effort_weight": 1
  }
}
```

### 5.2 `profiles.hpp` constant

```cpp
inline const Profile kProfileCloudgame = Profile{
    .name                 = "cloudgame",
    .transport_name       = "ice",
    .sdp_name             = "webrtc",
    .rtp_name             = "webrtc",
    .jb_name              = "adaptive",
    .audio3a_name         = "webrtc_apm",
    .codec_name           = "opus",
    .bwe_name             = "aimd",
    .scheduler_name       = "default",
    .timeline_enabled     = true,
    .datachannel_enabled  = true,
    .scheduler            = {
        .enabled  = true,
        .strategy = SchedulerConfig::Strategy::kStrictPriority,
        .control_weight     = 30,
        .audio_weight       = 8,
        .video_weight       = 4,
        .best_effort_weight = 1,
    },
    .bwe = {
        .impl   = "aimd",
        .params = {
            .initial_bitrate_bps = 5'000'000,
            .max_bitrate_bps     = 50'000'000,
        },
    },
    .jitter_buffer = {
        .impl             = "adaptive",
        .mode             = JitterBufferConfig::Mode::kLowLatency,
        .initial_delay_ms = 15,
        .min_delay_ms     = 5,
        .max_delay_ms     = 100,
    },
    .audio_sample_rate_hz = 48'000,
    .audio_channels       = 1,
    .audio_payload_type   = 111,
};
```

### 5.3 Register in `assembly.cpp`

```cpp
ProfileRegistry::ProfileRegistry() {
    // ... existing emplace calls ...
    profiles_.emplace("cloudgame", &kProfileCloudgame);
}
```

### 5.4 Add the invariant test

```cpp
TEST(Assembly, JsonFileMatchesBuiltinConstant_Cloudgame) {
    auto builtin = nimrtc::assembly::kProfileCloudgame;
    auto json    = nimrtc::assembly::profile_from_json_file(
        "profiles/cloudgame.json");
    ASSERT_EQ(json, builtin);
}

TEST(Assembly, ProfileCloudgame_DistinguishingFeature) {
    auto profile = nimrtc::assembly::kProfileCloudgame;
    EXPECT_EQ(profile.scheduler.control_weight, 30u);  // 3× call's 10
    EXPECT_EQ(profile.jitter_buffer.initial_delay_ms, 15);
    EXPECT_TRUE(profile.timeline_enabled);
    EXPECT_TRUE(profile.datachannel_enabled);
}
```

---

## 6. Loader-agnostic v1.1 markers (advanced)

ADR-010 §"Schema v1.1 incremental fields" allows profiles to add
markers the **C++ loader ignores** but downstream tools honour:

| Marker | Type | Use case |
|---|---|---|
| `sfu_enabled` | bool | Mark a profile as an SFU; SFU-aware tooling groups them. |
| `agent_gateway_enabled` | bool | Mark a profile as agent-gateway for Agent-platform integrations. |
| `agent_low_latency` | bool | Mark a profile as the tightest-latency Agent variant. |
| `max_sessions_per_engine` | int | Cap concurrent sessions in this profile (helper for cloud deploys). |
| `audio3a_name_explicit` | bool | Indicates the profile intends to override `audio3a_name` (used by infra tooling to spot PCMU-coded-but-with-3A profiles). |

If your profile introduces a new marker, you **must** amend ADR-010
§"Schema v1.x incremental fields" with:

- the field name;
- the type;
- the additive contract (loader-agnostic semantics);
- at least one consumer (tooling or C++ pipeline) that actually uses
  the marker.

Otherwise reviewers will flag it as "speculative".

---

## 7. Common pitfalls

- **Forgetting the invariant test**. The unit test catches drift,
  but only if it exists. If you can't write the test, your profile
  isn't yet ready to merge — unblock by filing a follow-up issue.
- **Overriding `audio_sample_rate_hz`** to something exotic (e.g.
  22050). Opus supports it but you'll need to also tune
  `EngineConfig::pcm_sample_rate_hz` and the codec's
  `clock_rate`. Stick to 8000 / 16000 / 48000 unless there's a hard
  reason.
- **Setting `control_weight` so high that audio starves**.
  `control_weight = 1000` sounds like it guarantees control
  delivery but it pauses audio for the entire control frame
  burst. Profile designers should measure under load. v0.12.0
  ships good defaults; don't go above 30 without a benchmark.
- **Loading a profile that requires a plugin the host binary
  doesn't have**. The Builder emits a clear error but if you
  accept profiles from JSON files at runtime (e.g. an SFU reading
  user-supplied profiles), call `core::register_all_default_plugins()`
  first AND validate the plugin ids before instantiation.

---

## 8. Versioning

Adding a new profile is **non-breaking**: the engine treats
profiles as registry entries. Existing integrations that ignore the
new profile see no change.

Renaming / removing a profile is **breaking**: any downstream code
that builds `Builder{}.load_profile("foo").build()` will fail.
Profile names are part of the public API at this level, even
pre-1.0.

---

## 9. See also

- `docs/profiles.md` — single-page index of all current profiles.
- `docs/adr/ADR-010-profile-json-format.md` — JSON schema
  definition.
- `src/modules/assembly/include/nimrtc/assembly/profiles.hpp` —
  built-in `kProfile<Name>` constants.
- `src/modules/assembly/include/nimrtc/assembly/assembly.hpp` —
  `Builder` / `ProfileRegistry`.
- `profiles/*.json` — JSON sources.
- `tests/test_assembly.cpp` — `JsonFileMatchesBuiltinConstant_*`.
- `examples/demo-agent-gateway/` — example of consuming a profile.
