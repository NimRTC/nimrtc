# ADR-022: Screen content is a content type, not a camera

| | |
|---|---|
| Number | 022 |
| Status | **Accepted** |
| Date | 2026-11-13 |
| Phase | M1 (remote-desktop plan, RD-3) |
| Bound to | v1.3.0 |
| Related | `src/plugins/include/nimrtc/plugins/video_codec.hpp`, `src/plugins/include/nimrtc/plugins/video_capture.hpp`, `docs/adr/ADR-021-codec-bundling-posture.md` |

## Context

Remote desktop is the first NimRTC use case whose *picture* is not a
camera. The distinction is not cosmetic — it changes what the encoder
should spend bitrate on, what the SDP should say, what the capture seam
should report, and what the resolution ladder should do when bandwidth
runs short.

A screen capture and a camera pointed at a monitor look similar and
encode *completely* differently:

| Property | Camera | Screen |
|---|---|---|
| Motion | continuous, everywhere | sparse, localised |
| Edges | soft | hard |
| Content | photographic | flat regions + text |
| Bitrate sensitivity | face quality | text legibility |
| What degrades first | sharpness on the face | text becoming unreadable |
| Frame rate value | high (motion is the signal) | low (30 fps is plenty) |
| Staleness tolerance | low | **high** — a stale text screen is fine |

The failure mode of ignoring this is specific and ugly: a camera-style
encoder at a screen's bitrate spends its budget on motion vectors and
noise around the cursor, the text smears, and the operator reads
nothing. It looks like "the codec is broken", not like "the encoder was
tuned for the wrong content".

Three seams already exist in the tree, each carrying *part* of this
knowledge, and they were added for different reasons at different times:

- `plugins::ContentHint` (`video_codec.hpp`) — `kUnspecified / kMotion
  / kDetail / kText`, mirroring WebRTC's `RTCContentHint`. It rides the
  SDP (`a=content-hint:`) and the H.264 `x-google-content-hint` fmtp.
- `plugins::CaptureDirtyRects` (`video_capture.hpp`) — per-frame changed
  regions plus a `full_frame` flag.
- `plugins::ColorRange` / `plugins::ColorMatrix` — the colour
  description (C5).

What was missing is a **decision**: what the engine knows about screen
content, where the information flows, and — most importantly — *why a
screen capture is not a camera*. This ADR states that, so the individual
items (B7, A7, C5, D2) implement one coherent design instead of three
independent half-measures.

## Decision

### 1. Screen content is a first-class content type

The engine models "what the encoder is looking at" as a **content type**
carried on the video path. There are two spellings of it in the tree and
they are related, not competing:

- `plugins::ContentHint` — the **wire-level** vocabulary. It is what
  goes into SDP and fmtp, so a WebRTC-compatible peer understands it
  unchanged. Its values mirror `RTCContentHint`.
- `assembly::VideoContentType` — the **configuration** vocabulary. It is
  what a profile declares (`video.content_type` in profile schema v1.2),
  so a deployment can say "this session is a screen session" without
  touching C++.

The configuration vocabulary maps onto the wire vocabulary:

| `assembly::VideoContentType` | `plugins::ContentHint` | Meaning |
|---|---|---|
| `kMotion` | `kMotion` | A camera. Favours temporal prediction. |
| `kScreen` | `kDetail` | A whole screen: text, UI chrome, some motion. |
| `kText` | `kText` | Pure text / a terminal / a scrolling document. |
| `kUnspecified` | `kUnspecified` | No statement — camera-style default. |

**Why `kScreen` maps to `kDetail` and not to `kText`:** a remote-desktop
screen is *mostly* text but not *only* text — there are images,
video playback, window chrome and a moving cursor. `kText` would push
the encoder into a mode that assumes a static, uniform background, which
is wrong for a screen with a playing video. `kDetail` (fine detail) is
the honest statement, and the *screen-specific* coding tools (intra
block copy, palette mode) are selected by the encoder from the `kScreen`
content type at the profile level, not from the wire hint.

**Why not add a `kScreen` value to `plugins::ContentHint`:** the wire
vocabulary is WebRTC's. Adding a value WebRTC does not define means a
peer that does not know it sees an unknown attribute value, and the
`a=content-hint:` attribute would need a NimRTC-specific spelling. The
cost of *not* adding it is that a plain WebRTC peer learns "detail"
rather than "screen" — which is a hint it can still act on sensibly. The
cost of adding it is interop breakage on the one seam that must
interoperate. The configuration vocabulary carries the extra precision.

### 2. The information flow

```
   profile (JSON)                     configuration
   video.content_type = "screen"
            │
            ▼
   assembly::VideoSection ──▶ engine config (video_content_hint)
            │                          │
            │                          ▼
            │                    plugins::ContentHint ──▶ encoder tuning
            │                          │
            │                          └──▶ SDP: a=content-hint:detail
            │                               fmtp: x-google-content-hint=detail
            ▼
   capture seam ──▶ plugins::CaptureDirtyRects { full_frame, rects[] }
            │
            └──▶ encoder: skip the unchanged region (Track B, B9)
```

Four distinct carriers, four distinct jobs:

1. **`assembly::VideoSection.content_type`** — *what kind of session this
   is*. Set once, from the profile. Drives the encoder's tuning mode,
   the ladder mode, and the content hint that goes on the wire.
2. **`plugins::ContentHint`** — *the wire statement*. Carried in SDP so
   the receiver knows what to expect and a WebRTC peer can act on it.
3. **`plugins::CaptureDirtyRects`** — *what changed this frame*. Per
   frame, from the capture seam. This is the mechanism that lets a
   static screen cost almost nothing.
4. **`plugins::ColorRange` / `plugins::ColorMatrix`** — *how the colours
   are coded*. Carried end to end (C5). A desktop compositor produces
   **full-range BT.709**; a camera produces **limited-range BT.601**.

### 3. The dirty-rect / incremental metadata contract

`plugins::CaptureDirtyRects` is the seam that makes a static screen
cheap. Its contract:

- `full_frame == true` means "everything changed, ignore the rects". A
  backend that cannot report incremental changes sets this to `true`
  for every frame. It is the **honest default**, not a failure.
- `full_frame == false` with an **empty** `rects` vector means "nothing
  changed since the last frame" — the frame carries no new information
  and the encoder may skip it entirely.
- `full_frame == false` with a non-empty `rects` vector means "these
  regions changed"; everything outside them is still valid.

The **bounding box** vs. **rect list** distinction is deliberate:

- **Windows DXGI Desktop Duplication** reports true per-region dirty
  rectangles natively (`GetFrameDirtyRects`) — several disjoint regions
  at once, which is exactly what a multi-window desktop update looks
  like. A single bounding box would then be "the whole screen" and
  carry no information.
- **Linux (X11 / PipeWire portal) and macOS (ScreenCaptureKit)** have
  no native equivalent, so they report a **conservative bounding box**
  computed by a portable pixel diff (`DirtyRectDetector`,
  `src/modules/video_capture/`), or `full_frame` when the diff is not
  available.

The capture backend's `has_dirty_rects` capability flag tells the engine
which of the two it is getting, so the engine never has to guess.

### 4. Encoder behaviour for screen content

When the content type is `kScreen`, the encoder is expected to:

- Prefer **intra block copy** / palette-style prediction over motion
  estimation for flat regions (AV1's screen mode, `svt-av1 --scm`).
- Spend bitrate on **edge sharpness**, not on motion vector accuracy.
- Treat a **higher frame rate as worthless** and a **higher resolution as
  valuable**. This is why the ladder mode for screen content is
  `quality` (the plan's D2): when bandwidth runs short, drop the frame
  rate, keep the resolution. The opposite of the camera policy.
- **Not** treat staleness as an error. A screen that updates at 5 fps
  because nothing on it is moving is correct behaviour, not a dropped
  frame.

### 5. What the engine does *not* do

- It does **not** special-case screens in the engine's core paths. The
  content type reaches the encoder as configuration; the encoder decides
  what to do with it. An engine branch on "is this a screen" would
  duplicate the encoder's policy and make the two drift.
- It does **not** assume a screen is always full-range BT.709. The
  colour description is carried, not assumed — a remote machine running
  a video player over the desktop still produces a composited
  full-range surface, but a *raw* capture API might not.
- It does **not** implement ROI / region-of-interest encoding in this
  milestone. The dirty-rect metadata is the input ROI would be built
  from; the ROI encoding itself is a later item (B9).

## Consequences

### Positive

- One coherent design behind B7, A7, C5 and D2 instead of three
  half-measures.
- The `kScreen` → `kDetail` mapping keeps SDP WebRTC-compatible while
  the configuration vocabulary stays precise. A peer that does not
  understand "screen" still gets a useful hint.
- The dirty-rect contract distinguishes "nothing changed" from "the
  backend cannot tell" — so the engine can prune frames on the former
  and must not on the latter.
- The bounding-box vs. rect-list split matches what each platform
  actually provides, so no backend is asked to lie.

### Negative / risks

- **Two vocabularies** for one concept is a permanent cost. The mapping
  table in §1 is the contract; a future `plugins::ContentHint` change
  has to update it.
- A peer that ignores `a=content-hint:` gets a camera-tuned decode of a
  screen. Decoding is far less sensitive than encoding, so the risk is
  cosmetic, but it is real.
- The pixel-diff dirty-rect path on Linux/macOS costs CPU every frame.
  The detector samples with a configurable stride (default 4 pixels) and
  is a portable scalar loop; it is a candidate for SIMD later, but that
  is not a correctness dependency.

### Neutral

- The `assembly::VideoContentType` enum is **additive**: a v1.1 profile
  without a `video` block parses to `kUnspecified` and behaves exactly
  as before.
- The `full_frame` flag keeps every backend honest: a backend that
  cannot do incremental reporting is not broken, it is just not
  incremental.

## Verification

| Check | Where | Required |
|---|---|---|
| The `ContentHint` ↔ `VideoContentType` mapping is total and the profile's `content_type` reaches the encoder config | `src/engine/tests/` | pass |
| A profile declaring `content_type = "screen"` emits `a=content-hint:` in the SDP | `src/modules/sdp/tests/` | pass |
| The dirty-rect contract: `full_frame == true` never prunes; `full_frame == false` + empty rects prunes | `src/modules/video_capture/tests/` | pass |
| `DirtyRectDetector` reports a tight bounding box for a known changed region and `full_frame` for an identical frame | `src/modules/video_capture/tests/` | pass |
| The colour description round-trips capture → encoder → renderer | `src/modules/*/tests/` | pass |

## Out of scope

- ROI / region-of-interest encoding (B9).
- The resolution-ladder implementation (D2) — this ADR only states that
  screen content's ladder mode is `quality`.
- Per-content-type encoder tuning tables (the encoders' business).
- `svt-av1` screen mode itself (B11 / ADR-021).

## References

- `docs/plan/remote-desktop-plan.md` — RD-3, B7, A7, C5, D2, B9.
- `src/plugins/include/nimrtc/plugins/video_codec.hpp` — `ContentHint`,
  `ColorRange`, `ColorMatrix`.
- `src/plugins/include/nimrtc/plugins/video_capture.hpp` —
  `CaptureDirtyRects`.
- `src/modules/video_capture/include/nimrtc/video_capture/dirty_rect_diff.hpp`
  — the portable pixel-diff detector.
- `docs/adr/ADR-021-codec-bundling-posture.md` — the companion ADR on
  which codecs are in the tree.
