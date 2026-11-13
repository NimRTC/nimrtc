# ADR-022: Screen content is a content type, not a camera

| | |
|---|---|
| Number | 022 |
| Status | **Accepted** |
| Date | 2026-11-14 |
| Phase | M1 (remote-desktop plan, RD-3) |
| Bound to | v1.3.0 |
| Related | `docs/plan/remote-desktop-plan.md` §0 (RD-3), Track A (A1, A2, A6, A7), Track B (B7, B9), Track C (C4, C5); `docs/adr/ADR-021-codec-bundling-posture.md`; `src/plugins/include/nimrtc/plugins/video_capture.hpp`; `src/plugins/include/nimrtc/plugins/video_source.hpp`; `src/modules/video_capture/`; `src/engine/src/engine_video_pump.cpp`; `src/modules/video_sdp_attributes/` |

## Context

Remote desktop is the first NimRTC use case whose *picture* is not a
camera. The distinction is not cosmetic: it changes what the encoder
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
noise around the cursor, the text smears, and the operator reads nothing.
It looks like "the codec is broken", not like "the encoder was tuned for
the wrong content".

### What the tree actually knows today

Being precise about the current state matters more here than in most
ADRs, because the temptation is to describe a design that does not yet
exist and then cite the ADR as if it did. Here is the honest inventory,
file by file.

**The capture seam already carries a content distinction — as a *device*
kind, not a *frame* property.**
`src/plugins/include/nimrtc/plugins/video_capture.hpp` declares:

```cpp
enum class NIMRTC_API_STABLE CaptureDeviceKind : std::uint8_t {
    kUnknown   = 0,
    kScreen    = 1,   ///< a display / monitor (or the whole virtual desktop)
    kWindow    = 2,   ///< an application window
    kCamera    = 3,   ///< a physical camera
};
```

and `CaptureDevice::kind` carries it, with
`capture_device_kind_name()` for logging. The Windows backends set
`kScreen` (`src/modules/video_capture/src/platform/windows/windows_capture.cpp`),
and so does the synthetic device
(`src/modules/video_capture/src/synthetic_capture.cpp`). This is the
*device* classification: "what was opened". It is **not** a statement
about the *content* of a frame, and — importantly — it does not travel
anywhere: nothing in `src/engine/` reads `CaptureDevice::kind`.

**The capture seam already carries dirty-rectangle metadata.**
`CaptureDirtyRects` is `{ bool full_frame; std::vector<CaptureRegion> rects; }`,
and `CaptureFrame::dirty` holds one per frame. `CaptureFormat::
has_dirty_rects` and `CaptureCapabilities::dirty_rects` are the two
precondition flags, and the engine checks the combination at `open()`
time in `detail::check_capture_capabilities()`
(`src/engine/src/video_render_path.hpp`). The DXGI backend fills the
rects natively; the WGC backend honestly reports `full_frame = true`
because it has no per-pixel diff.

**The video source seam carries no content information at all.**
`src/plugins/include/nimrtc/plugins/video_source.hpp` declares
`VideoSourceFrame` with pixels, plane pointers, strides, a GPU handle,
`capture_ts_us`, `frame_seq` and `rtp_timestamp`. There is **no** dirty
rect, **no** content hint, **no** colour description on it. The only
content-ish field in the whole header is `VideoSourcePattern::kCustom`,
which says "the pixels are plugin-specific", not "these are screen
pixels".

**The engine's capture pump drops the dirty-rect information.**
`src/engine/src/engine_video_pump.cpp` (`NimRTCEngine::
pump_video_capture()`) pops a `CaptureFrame` and translates it into a
`plugins::VideoSourceFrame`. The translation copies the dimensions, the
format, the timestamps, the frame sequence and the GPU handle — and it
does **not** copy `captured.dirty` into the source frame, because
`VideoSourceFrame` has nowhere to put it. The dirty rectangles therefore
**stop at the pump**. The pump does handle metadata-only frames
explicitly: a frame with `pixels == nullptr` is counted in
`vstate.video_capture_pumped_metadata` and returns early, because
"nothing changed on this desktop" is a *legitimate* frame, not an error.

**The SDP layer has no content-hint vocabulary.**
`src/modules/video_sdp_attributes/` is the single authority on what each
codec family's format parameters are and how they are spelled: it
parses and builds `a=fmtp:` bodies for H.264 (`H264Fmtp`), H.265
(`H265Fmtp`), VP8 (`Vp8Fmtp`), VP9 (`Vp9Fmtp`) and AV1 (`Av1Fmtp`),
through `parse_fmtp()` / `build_fmtp()`. There is **no** `ContentHint`
type anywhere in `src/`, no `a=content-hint` emission, and no
`x-google-content-hint` fmtp parameter. The engine builds the video
m-line in `src/engine/src/engine_signaling.cpp`, where the fmtp body
comes from `detail::default_fmtp_of(family, with_sprop)`
(`src/engine/src/video_codec_config.hpp`) and any attribute that has no
dedicated field goes into `sdp::MediaDescription::extra_attrs` — the
generic, ordered list that `sdp::Munger` renders as `a=<key>:<value>`.

**There is no ROI, and no colour description, on the video path.**
`CaptureConfig::region` is the *capture* region — the sub-rectangle of
the surface the device is asked to deliver. It is a property of the
device, checked against `CaptureCapabilities::region_selection`. It is
**not** the encoder's region-of-interest (plan item C4), which is a
rectangle the encoder codes at higher fidelity, and which does not exist
yet. Likewise there is no colour-range / colour-matrix type in the tree.

So: the tree knows *that* a device is a screen; it knows *what changed*
in each frame; and it then throws both facts away before the encoder
sees the picture. That is the gap this ADR closes.

## Decision

### Decision 1 — Screen content is a first-class content type

**A screen capture is not "a webcam pointed at a monitor".** The engine
must be able to tell the two apart, because the difference enables four
distinct things that a camera path does not have:

1. **Screen-content coding** — intra block copy, palette / text mode,
   higher QP on flat regions (plan item B7).
2. **Region-of-interest** — the operator's focus area coded at higher
   fidelity (plan item C4).
3. **Dirty-rectangle incremental update** — a static desktop costs
   nothing between changes (plan items A7 and B9).
4. **Cursor compositing** — the pointer is a separate layer on the
   platforms that do not composite it (plan item A6).

The content type is what makes all four *selectable*. Without it, the
encoder would have to guess from the picture, and a guess that is wrong
is a wrong picture rather than a wrong flag.

### Decision 2 — What the engine knows about screen content

The engine knows three things about a screen stream, and they are
distinct from the camera equivalents:

| | Camera content | Screen content |
|---|---|---|
| **Statistics** | continuous motion, soft edges, photographic detail, per-pixel noise | flat regions, hard edges, text, large areas of *no change* between frames |
| **Coding tools that pay off** | temporal prediction, deblocking tuned for film-grain-like noise | intra block copy, palette / text mode, ROI fidelity, higher QP on flat areas, dirty-rect incremental update |
| **Trade-offs** | a stale frame is visibly stale; frame rate is the signal | a stale frame is nearly free; text legibility is the signal; the frame rate can be low; resolution must not fall below legibility |

The trade-off column is the one that is easy to get wrong. Because a
screen tolerates staleness, the natural instinct is to lower the frame
rate to save bandwidth — but a screen does **not** tolerate resolution
loss, because the thing the operator is reading is the pixels. That
inversion (rate is cheap, resolution is expensive) is the substantive
difference between the two content types, and it is why the ladder for a
screen session needs a resolution *floor* (plan item D2) rather than a
frame-rate floor.

### Decision 3 — Where the information flows

Three carriers, three distinct jobs. **Two of the three exist today; the
third is a proposal.**

```
  capture device (IVideoCapture)
        │  CaptureDevice::kind            (kScreen / kWindow / kCamera)   [exists]
        │  CaptureFrame::dirty            (CaptureDirtyRects)            [exists]
        ▼
  capture module (src/modules/video_capture/)
        │  CaptureQueue::push(..., dirty)  — the queue carries the rects   [exists]
        ▼
  engine pump (src/engine/src/engine_video_pump.cpp)
        │  pop_frame() → VideoSourceFrame                                [exists]
        │  ... dirty rects are NOT copied into VideoSourceFrame         [gap]
        ▼
  encoder (plugins::IVideoCodec)
        │  content hint / ROI                                                [proposed]
        ▼
  SDP (src/modules/video_sdp_attributes/ + engine_signaling.cpp)
           a=content-hint:detail / x-google-content-hint=detail            [proposed]
```

**Carrier 1 — the device kind (`CaptureDeviceKind`).** This is the
*device* classification and it exists. Its problem is that it is
per-device and it stops at the plugin boundary: the engine resolves the
capture plug-in through the registry, opens it, and never asks what kind
of device it opened. The kind is available on the seam; nothing reads it.

**Carrier 2 — the dirty rectangles (`CaptureDirtyRects`).** This is the
*per-frame* change information and it exists end to end **inside the
capture layer**: the device fills it, `CaptureQueue::push()` takes it as
a parameter and stores it in the queue's `Frame`, and `CaptureQueue::
pop()` copies it into the outgoing `CaptureFrame`. The synthetic device
exercises it exactly — `SyntheticCapture::set_static_frame(true)` freezes
the picture and the device then reports `full_frame == false` with an
*empty* `rects` vector, which is the property a remote-desktop encoder
cares about most.

The gap is at the boundary between the capture layer and the video path:
`NimRTCEngine::pump_video_capture()` builds a `plugins::VideoSourceFrame`
and does not carry `captured.dirty` across, because
`plugins::VideoSourceFrame` has no field for it. **Proposal:** add a
dirty-rect / incremental-update field to the source frame, and let the
pump carry the device's rects across unchanged. The dirty-rect
information is already computed, already copied once (into the queue),
and then discarded — carrying it the last twenty metres costs one struct
copy and buys the "a static desktop produces near-zero outbound bytes"
property.

**Carrier 3 — the content hint.** This does **not** exist yet, in any
form. The proposal is:

- A `plugins::ContentHint` enum on the **video codec seam**
  (`src/plugins/include/nimrtc/plugins/video_codec.hpp`), with values
  mirroring WebRTC's `RTCContentHint`: `kUnspecified`, `kMotion`,
  `kDetail`, `kText`. It is the **wire** vocabulary: it is what goes into
  SDP and fmtp, so a WebRTC-compatible peer understands it unchanged.
- A **configuration** vocabulary at the profile level (profile schema
  v1.2, plan item K4) — a `video.content_type` key with a screen / camera
  / text choice — which is what a deployment declares without touching
  C++.

The mapping between them is a decision, not an identity:

| Configuration (proposed) | Wire hint (proposed) | Meaning |
|---|---|---|
| camera / motion | `kMotion` | A camera. Favours temporal prediction. |
| screen | `kDetail` | A whole screen: text, UI chrome, some motion. |
| text | `kText` | Pure text: a terminal, a scrolling document. |
| unspecified | `kUnspecified` | No statement — camera-style default. |

**Why screen maps to `detail` and not to `text`:** a remote-desktop
screen is *mostly* text but not *only* text — there are images, video
playback, window chrome and a moving cursor. `kText` would push the
encoder into a mode that assumes a static, uniform background, which is
wrong for a screen with a playing video. `kDetail` (fine detail) is the
honest statement. The *screen-specific* coding tools (intra block copy,
palette mode) are selected by the encoder from the content type, not
from the wire hint.

**Why not add a `kScreen` value to the wire hint:** the wire vocabulary
is WebRTC's. Adding a value WebRTC does not define means a peer that
does not know it sees an unknown attribute value and the `a=content-hint`
attribute would need a NimRTC-specific spelling. The cost of *not*
adding it is that a plain WebRTC peer learns "detail" rather than
"screen" — a hint it can still act on sensibly. The cost of adding it is
interop breakage on the one seam that must interoperate. The
configuration vocabulary carries the extra precision.

**Carrier 4 — the ROI.** Also **not** existing. The proposal is that the
encoder receives a rectangle in capture coordinates, scaled with the
frame, and codes it at higher fidelity. This is deliberately *not*
`CaptureConfig::region`: that is the region the device is asked to
capture (a device capability, gated on `CaptureCapabilities::
region_selection`), whereas the ROI is what the encoder is asked to
*favour* inside a frame the device already delivers.

### Decision 4 — Where the content hint would live on the wire

The `video_sdp_attributes` module is the single authority on codec
format parameters, and the engine already routes the fmtp body through
it (`detail::default_fmtp_of()` → `vsa::build_h264_fmtp()` and
friends). A content hint therefore belongs in **two** places, and both
already have a home:

- **A session-level / media-level attribute.** `a=content-hint:detail`
  is not a codec parameter; it is an attribute of the media section. The
  engine's video m-line builder already has a place for attributes that
  have no dedicated field: `sdp::MediaDescription::extra_attrs`, which
  `sdp::Munger` renders in order as `a=<key>:<value>`.
- **A codec fmtp parameter.** `x-google-content-hint` (and the
  `x-google-start-bitrate` companion) are H.264 fmtp parameters, so
  `H264Fmtp` gains a field and `build_h264_fmtp()` emits it. That keeps
  the module's "one authority for `a=fmtp:`" property intact.

**This is a proposal.** Neither exists today, and this ADR does not land
them — the landing items are B7 (screen-content coding, including the
negotiated `fmtp` carrying the content hint) and K4 (profile schema
v1.2). What this ADR fixes is the *shape*: the content type is a first
class value with a defined spelling on both the configuration side and
the wire side, and it is negotiated rather than assumed.

### Decision 5 — The engine's content-type knowledge is one value, carried explicitly

The engine does not *infer* the content type from the picture, and it does
not take it from the device by inspection. It is carried **explicitly**,
and the rule for where it comes from is:

- A session that declares a screen content type gets screen-content
  behaviour: the screen-content coding tools are selected, the ladder's
  resolution floor applies, and the content hint is negotiated.
- A session that declares a camera content type gets the camera defaults
  and no content hint.
- A session that declares nothing gets the camera defaults — the
  behaviour that is correct for a camera, which is also the behaviour
  that is merely *safe* for a screen.

**Why the engine must not infer it from the device kind.** The device
kind is the capture plug-in's business, and it is *not* the same
question. A window capture is screen content. A display capture is screen
content. A camera capture is camera content. But the engine's encoder
tuning is a property of the *stream*, not of the device that happened to
produce it — and a host may want a camera stream tuned as screen content
(a document camera), or a screen stream tuned as motion (a video playing
full-screen). Inferring one from the other couples the encoder's tuning
to the capture backend's taxonomy, which is exactly the coupling RD-10
warns against.

The device kind remains useful — it is the engine's *hint* about what the
content probably is, and it is the honest answer to "what did the user
open". What it is not is the engine's *decision*.

### Decision 6 — Screen content and the drop policy

The capture queue's overflow policy is already content-sensitive, and the
reasoning in `src/modules/video_capture/include/nimrtc/video_capture/
capture_queue.hpp` is the same reasoning as this ADR:

- **Screen content → drop the oldest.** The newest frame is the truth; a
  stale frame of a screen that has since changed is worth nothing, and
  sending it wastes the user's bandwidth. `CaptureDropPolicy::kDropOldest`
  is the default.
- **Camera → drop the newest.** A slightly stale frame beats a frame that
  arrives too late to be displayed.

That the drop policy is already content-sensitive is evidence that the
content distinction is real and already load-bearing in the tree — and it
is the precedent for the rest of the content-type work.

## Consequences

### What gets better

- **The four screen capabilities become selectable.** Screen-content
  coding, ROI, dirty-rect incremental update and cursor compositing all
  depend on the engine knowing what it is looking at. Today they are
  either impossible or accidental.
- **The dirty-rect information stops being thrown away.** It is computed
  by the device, copied into the queue, and discarded at the pump.
  Carrying it the last twenty metres is the difference between "a static
  desktop costs a full frame every tick" and "a static desktop costs
  nothing".
- **The SDP has a vocabulary for it.** A screen session and a camera
  session become distinguishable on the wire, so a peer that *can* act on
  the difference (a browser, a WebRTC SFU) does.
- **The ladder stops treating a screen like a camera.** A camera ladder
  that trades resolution for frame rate is right for a camera and wrong
  for a screen; the resolution floor is what makes a screen session
  usable.

### What gets worse, honestly

- **One more value to carry.** A content type is one more thing a profile
  declares, a config carries, and the encoder receives. It is one more
  thing that can be set wrong.
- **The proposal is not free.** The carrier-2 and carrier-3 changes above
  touch the plugin seams, which are `NIMRTC_API_STABLE` surfaces under
  ADR-015. Adding a field to a stable struct is additive (a MINOR bump);
  changing the *meaning* of an existing field would not be. The proposals
  here are additive.
- **Screen tuning is a per-codec affair.** Intra block copy is an AV1
  tool; the H.264 equivalent is a different set of knobs. "Screen
  content" therefore means "the screen tools *this* codec has", and the
  preference table for which codec to use for a screen stream (plan item
  B8) is configuration, not a branch.

### What this ADR does **not** decide

- **Which codec a screen session prefers** (plan item B8: AV1 > HEVC >
  H.264). That is a configuration table, not a content type.
- **How the dirty-rect incremental *encode* works** (plan item B9,
  including the region-invalidation signalling to the peer). This ADR
  fixes that the information reaches the encoder; B9 fixes what the
  encoder then does with it.
- **The ROI's exact semantics** (plan item C4). This ADR fixes that a
  ROI is not the capture region.
- **The colour description** (plan item C5, the screen-is-BT.709-full-
  range point). It is a real and separate seam, and it is worth noting
  here only because it is the second thing, after the content type, that
  is *not* camera-like about a screen.

## Alternatives considered

### A — A screen capture is a camera with a big resolution

Rejected. This is the status quo the RD-3 decision explicitly refuses,
and it fails in three separate places:

1. **The encoder tunes for the wrong thing.** A camera-tuned encoder
   spends its budget on motion vectors and noise; on a screen that budget
   is wasted, and the text smears. The operator sees "the codec is
   broken".
2. **The bandwidth is spent on the wrong thing.** A camera stream pays
   for every changed pixel every frame. A screen's changed pixels are a
   small fraction of the frame, most of the time. Treating the two alike
   means a remote session is an order of magnitude more expensive than it
   needs to be.
3. **The ladder's trade-off is inverted.** For a camera, resolution and
   frame rate are comparable; for a screen, resolution is the thing the
   user is reading and frame rate is nearly free. A ladder that trades
   resolution away to protect frame rate destroys the product.

Note what this alternative is *not*: it is not an argument against
reusing the same codec implementations. The codecs are the same; the
*tuning* is not.

### B — Infer the content type from the device kind

Rejected, and this is the closest alternative. `CaptureDeviceKind` is
already there and already says `kScreen`. Why not have the engine read it
and be done?

Three reasons:

1. **The device kind is the capture backend's taxonomy, not the
   engine's.** A host may want a camera stream tuned as screen content
   (a document camera) or a screen stream tuned as motion (a full-screen
   video). The device kind cannot express either.
2. **The device kind does not reach the engine.** It is set by the
   capture plug-in and read by nobody. Making the engine read it would
   couple the encoder's tuning to the capture plug-in's taxonomy — the
   coupling RD-10 argues against — and it would be a *silent* coupling,
   because a plug-in that forgot to set `kind` would silently get camera
   defaults.
3. **The explicit value is auditable.** A declared content type appears
   in the profile, in the config and (eventually) in the stats. An
   inferred one appears nowhere and cannot be diagnosed.

The device kind is kept, and it is genuinely useful: it is the honest
answer to "what did the user open", and it is the natural default for a
host that does not configure anything.

### C — A separate "screen mode" flag on the engine

Rejected. A `screen_mode: true` boolean on `EngineConfig` would be
simpler to implement, and it is exactly the shape RD-10 warns about: a
product-specific mode flag inside the engine. It also cannot express the
third content type (a text-only terminal is not a screen), and it has no
wire representation — a boolean cannot be negotiated.

The content *type* is the right shape because it is a small closed set
with a defined spelling on both sides of the wire.

### D — Keep screen content entirely out of the engine

Rejected. This is the "the codec plug-in can figure it out" option: let
the screen-capable encoder detect flat regions and text and switch
itself. It fails on the two capabilities that are *not* per-frame codec
tuning:

- **Dirty-rect incremental update** is a capture-layer and pipeline
  concern. The encoder cannot invent the knowledge of which pixels
  changed; only the device knows, and only the engine can carry it to
  the encoder.
- **The resolution floor and the ladder's trade-off** are rate-control
  concerns, and the rate controller is the engine's.

A codec that detects screen content on its own is a *useful heuristic*
and is not a substitute for a declared content type.

## References

- `docs/plan/remote-desktop-plan.md` — the source plan: §0 (RD-3, the
  decision this ADR records), Track A (A1 capture seam, A2 capture
  module, A6 cursor compositing, A7 dirty-rect metadata), Track B (B7
  screen-content coding, B9 dirty-rect incremental encode), Track C (C4
  ROI, C5 colour), Track K (K2, K4).
- `docs/adr/ADR-021-codec-bundling-posture.md` — the codec side of the
  same milestone: which software codecs may be vendored at all.
- `docs/adr/ADR-015-api-stability-policy.md` — the stability policy that
  governs the additive seam changes this ADR's proposals imply.
- `src/plugins/include/nimrtc/plugins/video_capture.hpp` — the capture
  seam: `CaptureDeviceKind`, `CaptureDirtyRects`, `CaptureRegion`,
  `CaptureCapabilities`, `CaptureDropPolicy`, `CaptureFrame`.
- `src/plugins/include/nimrtc/plugins/video_source.hpp` — the video
  source seam: `VideoSourceFrame` and the absence of any content field.
- `src/modules/video_capture/include/nimrtc/video_capture/capture_queue.hpp`
  — the bounded queue, its content-sensitive drop policy, and the
  dirty-rect plumbing.
- `src/modules/video_capture/include/nimrtc/video_capture/synthetic_capture.hpp`
  — the portable reference device and its static-frame dirty-rect
  behaviour.
- `src/engine/src/engine_video_pump.cpp` — the capture → engine pump and
  the metadata-only frame path.
- `src/modules/video_sdp_attributes/` — the authority on `a=fmtp:`
  bodies, where the codec-level content hint would be built.
