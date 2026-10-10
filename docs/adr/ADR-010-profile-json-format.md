# ADR-010: JSON as first-class declarative Profile format

| | |
|---|---|
| Number | 010 |
| Status | **Accepted** |
| Date | 2026-09-14 |
| Phase | P1.1 |
| Bound to | v0.10.0 |
| Related | `src/modules/assembly/`, `profiles/*.json`, ADR-001 |

## Context

v0.9 留下一个开放问题：**assembly 以 C++ Builder 为第一形态，是否同时提供 JSON / TOML 声明式 Profile 加载**（便于按项目 / 场景下发配置，见 `architecture.md` §2.6）？本 ADR 收口该问题。

Today, the project already has:

- `profiles/agent.json`, `profiles/call.json`, `profiles/live.json`,
  `profiles/transport.json` — committed JSON Profile documents
- `src/modules/assembly/tests/` already round-trips JSON
  Profile payloads (`ProfileRegistry`, `Builder`, JSON round-trip)
- The CHANGELOG `[Unreleased]` documents "Copies JSON profiles into
  `${CMAKE_BINARY_DIR}/profiles/` at configure time so tests run from
  the build dir"

So JSON Profile loading is **already partially first-class**; what is
missing is:

1. An explicit statement that JSON is the canonical declarative Profile
   format (vs. C++ Builder being the only first-class option).
2. A schema versioning convention so future format changes don't break
   existing `profiles/*.json` files.
3. A documented relationship to the C++ Builder (which remains first-class
   for programmatic composition — JSON and Builder are not exclusive).

TOML was raised as a v0.9 alternative. There is no existing
TOML consumer in the codebase, and adding TOML now would double the
maintenance surface for a small, resource-constrained project.

## Decision

1. **JSON is the first-class declarative Profile format.** The four
   existing `profiles/*.json` files are pinned as **schema v1.0**.

2. **C++ Builder remains first-class** for programmatic composition.
   JSON ↔ Builder round-trip is lossless in v1.0. Either can be the
   entry point; the consumer code path is identical.

3. **TOML is not added.** Deferred until a real consumer asks for it
   (no consumer today). Reopen this ADR if TOML becomes necessary.

4. **Schema versioning follows SemVer:**
   - v1.0 (current): the four `profiles/*.json` files in the repo root
     define the canonical schema. Any future format-breaking change
     bumps the JSON schema major version and the consumer's MINOR
     version (so v0.11.x or v0.12.x can ship a schema v2.0 if needed).
   - Additive changes (new optional fields) ship in the current schema
     version with a PATCH bump.

5. **Schema documentation** lives at `profiles/schema/profile-v1.0.json`
   (JSON Schema draft 2020-12). The four canonical profiles are
   validated against this schema in CI as a non-blocking check
   (consistent with the existing vendor-check / pre-flight pattern).

6. **Loader surface** (to be implemented in v0.11.0 Profile 库) is
   intentionally **not** specified here — this ADR binds the **format
   choice**, not the loader API. Loader API will be its own ADR when
   the loader lands.

## Consequences

### Positive

- Reviewers / contributors see an explicit, written commitment to JSON
  as the canonical format. Future PRs that propose adding YAML / TOML /
  HCL must reopen this ADR rather than slipstream a new format.
- Schema versioning pinned in writing prevents accidental format drift.
- No code change required for v0.10.0 — this ADR documents an existing
  convention. All four canonical files are already schema v1.0 by
  construction.

### Negative / risks

- Pinning schema v1.0 means future breaking changes to the JSON layout
  require an ADR amendment. Acceptable: ADR-amendment-for-breaking-format
  is the same discipline as ADR-amendment-for-public-API.

### Neutral

- No new dependencies (JSON parser already vendored — nlohmann_json 3.11.3,
  per CHANGELOG v0.9.x).
- TOML remains an option if a real consumer appears; no commitment
  either way.

## Verification

| Check | Where | Required |
|---|---|---|
| `profiles/{agent,call,live,transport}.json` validate against `profiles/schema/profile-v1.0.json` | CI, non-blocking | pass (warning-level) |
| `src/modules/assembly/tests/` JSON round-trip tests still pass | ctest | green |
| No new TOML / YAML / HCL consumer in tree | `git grep -l "toml\\|yaml\\|hcl"` | zero matches |

## Schema v1.1 incremental fields (PROFILE-1 — v0.11.0 amendment)

The v0.11.0 Profile library (`docs/plan/v0.11-plan.md` §9 PROFILE-1)
introduces five new Profile variants (`sfu`, `agent-gateway`,
`sfu-agent`, `teleop`, `agent-low-latency`). The four v1.0 canonical
profiles (`agent`, `call`, `live`, `transport`) remain unchanged and
continue to validate against `profiles/schema/profile-v1.0.json`; they
are not bumped and require no field migration.

Five of the new profiles declare **additive** JSON fields that signal
scenario intent at the document level. These fields are
**ignored by the v1.0 loader** (`assembly::profile_from_json_file`)
and preserve full backward compatibility — a profile JSON containing
only v1.0 keys still parses correctly via the v1.0 / v1.1 loaders.

| Field | Type | First profile | Semantics |
|---|---|---|---|
| `sfu_enabled` | `bool` | `sfu.json`, `sfu-agent.json` | Marker — profile is a Selective Forwarding Unit (server-side forwarding hop, no L2 media). Loader-agnostic; runtime reads via `core::PluginRegistry::get_transport_stack(...)`. |
| `agent_gateway_enabled` | `bool` | `agent-gateway.json`, `sfu-agent.json` | Marker — profile exposes the PCM-tap-able agent gateway surface (`set_pre_process_tap` / `set_post_process_tap`, per `docs/adr/ADR-008-pcm-tap.md`). |
| `agent_low_latency` | `bool` | `agent-low-latency.json` | Marker — profile turns off 3A's AGC level estimation to avoid latency contribution; intended for ASR-bound pipelines where the model is the consumer. |
| `max_sessions_per_engine` | `unsigned int` | `sfu.json`, `sfu-agent.json` | Hint — upper bound on concurrent peer sessions one engine instance is expected to handle; consumed by the SFU engine wiring layer (not by the assembly loader). |
| `audio3a_name_explicit` | `string` (optional) | `sfu-agent.json` (value = `"webrtc_apm"`) | Convenience — when the profile carries an explicit `audio3a_name` override (vs the empty-string SFU default), this field mirrors it for tooling that wants to find the agent audio pipeline without re-parsing the canonical field. Optional; absence = not agent-side 3A. |

**Loader contract for v1.1**: `profile_from_json_file` continues to read
only the v1.0 keys (it knows nothing about the additive fields).
Profiles that need the markers at runtime do one of:
  (a) embed the marker into the C++ constant companion
      (`kProfileSfu`, `kProfileAgentGateway`, …) and access it via
      `ProfileRegistry::find(name)`;
  (b) read the JSON twice — once via `profile_from_json_file` for the
      canonical fields, once via a thin `nlohmann::json` parse for the
      additive marker block (caller code only, no engine wiring
      change).

No loader API is added in v0.11.0. The schema change is purely
documentary — it pins what the JSON author is allowed to express, not
how the engine consumes it.

**Migration / version notes**:
  - A v1.0 loader happily parses a v1.1 JSON (additive fields are
    ignored).
  - A v1.1 parser reading a v1.0 JSON behaves identically to v1.0
    (no v1.1 marker is present, so no marker-driven code path is
    triggered).
  - Bumping to **schema v2.0** requires renaming or removing any v1.0
    field, not just adding markers — that falls outside this
    amendment and would be its own ADR.

**CI guard**: `tests/.../test_profile_json`-style round-trip tests
verify (i) all four v1.0 canonical profiles still parse, (ii) all five
v1.1 profiles parse, and (iii) the parsed keys intersected with the C++
constant companion match field-by-field.

## Out of scope (this ADR)

- The C++ Builder API (unchanged).
- Loader API for runtime Profile composition (v0.11.0 work, own ADR).
- Profile 库 官方化 (5 个 Profile: sfu / transport / agent / agent-gateway /
  sfu-agent) — v0.11.0.

## References

- `docs/zh/architecture.md` §2.6 (Profile 概念), §13 P2 row
  (Profile 库官方化 deferred to v0.11.0)
- `src/modules/assembly/` (existing module)
- `profiles/*.json` (canonical v1.0 profiles)
- CHANGELOG `[Unreleased]` (records the JSON round-trip work)
