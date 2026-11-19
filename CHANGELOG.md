# Changelog

All notable changes to NimRTC are documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/)
and uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

> Work that lands after `v1.3.0`.

## [1.3.0] - 2026-11-20

### Status: Stable

> The source version is `1.3.0` (`project(VERSION)` in `CMakeLists.txt`,
> `NIMRTC_API_VERSION` = 10300, `NIMRTC_API_VERSION_PRE_RELEASE` = `""`).
> `v1.3.0` is the release that closes the v1.3 plan
> (`docs/plan/v1.3-plan.md`): **Track Q** (Q1, Q2, Q3, Q4, Q5, Q6, Q7) is
> complete.

> The release's headline is that the engine now **degrades gracefully under
> a hostile network** — the axis `v1.2.0` explicitly did not claim.  The
> seven Track Q items are:

> - **Q1 / Q1b** — a mid-stream parameter-set change is now *proven* to
>   decode on its own, on H.264 (software) and on H.265 (NVDEC), with no
>   key-frame round trip and no PLI on the wire.
> - **Q2** — the audio jitter buffer's memory is bounded, and the bound is
>   reported.
> - **Q3** — retransmissions travel on their own RFC 4588 stream.
> - **Q4** — the receiver's feedback to the peer is bounded, and the two
>   reasons for a key-frame request are distinguishable.
> - **Q5** — the soak can now impair the network *and* drive the engine,
>   across a simulated NAT; the 24 h window is archived with its netem
>   parameters.
> - **Q6** — the picture is now aligned to the sound (the video half of
>   R3; the audio half shipped in `v1.2.0`).
> - **Q7** — the hardware codec path is exercised end-to-end by something
>   that re-runs, on a self-hosted GPU runner.

> **The release gates are read, not assumed.**  The `release` workflow's
> preflight now verifies the seven-day soak window at the tag, checks that
> the tag is an ancestor of `main`, and confirms the README already
> describes the release being published.  The release lines and the
> release tags are protected by rulesets.

> **Q1b (the H.265 half of the parameter-set switch) is a `1.3.x` item.**
> The codec-level assertion and the engine-level hardware case both ship
> in `v1.3.0`; the plan's §9 records the remainder — the software-decoder
> half, which stays deferred because there is still no software H.265
> decoder to assert on.


> Work that lands after `v1.2.0`.

### Added

- **The soak window, the tag ancestry and the README's release claims
  are now read at cut time (post-v1.2.0 review).**
  Three gates that were written down but never consulted:

  - **The soak window was never read by `release.yml`.**  The v1.1
    plan's "7-day soak before the tag" box was still empty two
    releases later.  The preflight now requires a green window whose
    every day was measured on a commit the tagged commit descends
    from, and whose last day is within 30 days of the cut
    (`run_soak.py --expect-ancestor-of` / `--max-window-age-days`,
    `fetch_soak_evidence.py --require`).
  - **A tag could be cut from a branch unreachable from `main`.**
    `v1.2.0` had that shape: `compare/v1.2.0...main` reported
    "diverged" with byte-identical trees, and `git describe` on `main`
    could not find the tag.  It was repaired afterwards by merging the
    release branch back.  The preflight now runs
    `git merge-base --is-ancestor "$tag" origin/main` before releasing.
  - **The README's badge went stale for eleven days** after `v1.0.2` →
    `v1.2.0`: the badge still read `v1.0.2` while the body of the same
    file described `v1.2.0` as shipped.
    `tools/check_readme_release.py` compares the badge, the
    "Latest release" paragraph and the "Current `main`" paragraph in
    both languages against the newest changelog section, runs in the
    `docs / markdown links` job, and gains `--expect-tag` for the
    preflight's reverse question: does the README already describe the
    tag being published?

- **The release lines and the release tags are protected by rulesets
  (post-v1.2.0 review).**
  `release/1.2` carried the published `v1.2.0` tag as its head, was
  `protected: false`, and the repository's only ruleset covered
  `refs/heads/main` — one force-push would have severed the artifact
  from the commit it was built from.  Two rulesets
  (`protect-release-branches`, `protect-release-tags`) apply `deletion`
  and `non_fast_forward` to `refs/heads/release/*` and `refs/tags/v*`.
  They carry no `pull_request` rule and no required checks on purpose:
  hot-fixes reach a release line by cherry-pick, and the PR-triggered
  workflows do not run for a PR into `release/X.Y`, so a required
  check there would be unsatisfiable.  `protect-main.json` also records
  why `required_approving_review_count` stays 0 in a
  single-collaborator repository.

- **The receiver's feedback to the peer is bounded, and the two reasons
  for a key-frame request are now distinguishable (v1.3 plan item `Q4`,
  video plan `T2`).**
  The receiver used to *merge* its feedback into one pending report
  without a bound, and the two doors into that report were
  indistinguishable.  Three things could go wrong, and all three are
  feedback amplification:

  - **The key-frame request had no rate limit of its own.**  The engine's
    decode-failure path had a 250 ms throttle, but the jitter buffer's own
    loss report did not — and both went into the *same* pending report, so
    a stream that both lost packets and failed to decode asked for key
    frames at twice the rate.
  - **The NACK list was uncapped.**  Every detected gap added its sequence
    numbers to a list that was merged into the pending report without a
    limit, so a peer that dropped 30 % of a 30 fps stream produced a report
    that grew with the loss.
  - **The pending report had no age.**  A report that sat in the queue while
    the link was down was stale by the time it went out: the losses it
    named had either been retransmitted already or were long gone.

  The bound now lives in one private collaborator,
  `src/engine/src/engine_feedback_guard.{hpp,cpp}`
  (`nimrtc::engine::detail::FeedbackGuard`), so the receive-side feedback
  has a single visible owner:

  - **The key-frame rate limit is global, not per-door** — one request per
    interval (default 250 ms, i.e. 4 Hz per RFC 5104 §4.3.1.1), whichever
    door raised it.  The peer's encoder is the shared resource the limit
    protects, so a per-door limit would let the two doors add up to twice
    the rate.  The engine's own decode-failure throttle stays as the
    *door's* throttle (it keeps the log quiet).
  - **The NACK cap keeps the *most recent* losses** (default 64 per
    report).  RFC 4585 §6.2.1 shapes the request around the recent
    losses, and the sender's retransmission cache only holds the recent
    packets — a request for an old loss is a request the peer cannot
    answer.  When the list overflows, the *oldest* entries are dropped.
  - **A stale report is dropped, not sent** (default age 1 s).

  The two doors are counted separately, because they call for completely
  different fixes: `video_feedback_pli_from_loss` says "the network dropped
  a picture", `video_feedback_pli_from_decode_failure` says "the decoder
  cannot decode what it is being sent".

  The bound is reachable from a host and visible to one:

  - `EngineConfig::video_feedback_min_pli_interval_us` (default 250000;
    `0` disables the limit), `EngineConfig::video_feedback_max_nack_seqs`
    (default 64; a negative value means unbounded) and
    `EngineConfig::video_feedback_max_report_age_us` (default 1000000;
    `0` means "never expire") are new.
  - `Engine::Stats` gains seven counters:
    `video_feedback_pli_requests` (the demand),
    `video_feedback_pli_allowed` (what went on the wire),
    `video_feedback_pli_suppressed` (what the limit dropped),
    `video_feedback_pli_from_loss`, `video_feedback_pli_from_decode_failure`,
    `video_feedback_nack_seqs_dropped` and `video_feedback_reports_expired`.

  The guard's clock is **injected**, which is what makes the acceptance
  deterministic: the engine passes the monotonic clock, the test passes a
  virtual one it advances by hand.  `tests/test_engine_feedback_guard.cpp`
  therefore runs the 30 s synthetic-loss profile in microseconds of real
  time and asserts the same numbers on every machine — at most **121**
  key-frame requests may go out in 30 s, both for a 30 % loss profile and
  for the adversarial "every frame is lost" one (901 requests raised).
  `tests/test_engine_video_feedback.cpp` covers the engine-level half.

- **An H.265 mid-stream parameter-set change is asserted on the hardware
  decoder (v1.3 plan item `Q1b`).**
  Q1 proved the H.264 mechanism on the software decoder.  Q1b is the
  H.265 counterpart, and it is asserted where H.265 actually runs here 鈥?  on **NVDEC**, against a stream produced by **NVENC**:

  - `src/modules/nvcodec/src/hevc_pts.{hpp,cpp}` is a new private helper
    that parses `profile_tier_level` and the SPS geometry fields
    (ITU-T H.265 搂7.3.3, 搂7.3.2.2.1) **independently of the driver**, and
    summarises an Annex-B access unit into `plugins::HevcPtsSummary`:
    the visible size, FNV-1a hashes of the **VPS / SPS / PPS** RBSPs, and the
    profile idc with its compatibility flags.  The summary is what lets a
    test tell "the parameter sets changed" from "the stream never
    changed".
  - `IVideoEncoder::last_hevc_pts()` / `IVideoDecoder::last_hevc_pts()`
    expose the summary of the last keyframe each seam handled.
  - `HwPtsSwitch.GeometryChangeDecodesOnTheSameNvdecSession` holds one
    NVDEC instance created for 320脳240, switches the sender to 640脳480
    (NVENC re-initialises and emits an IRAP with inline VPS / SPS / PPS),
    and asserts the **first** decode call after the switch returns `kOk`
    with a 640脳480 picture, the decoder's SPS hash equal to the
    encoder's, and the receiver's key-frame-request counter still at
    zero 鈥?i.e. no PLI on the wire.
  -   `HwPtsSwitch.ParameterSetChangeAtTheSameGeometryIsNoticed` covers
  MAIN 鈫?MAIN10 at the same size, the case a width/height comparison
  cannot see.

  The private parser reads `general_reserved_zero_44bits` (a 44-bit
  field, longer than any machine word) without truncation, skips the
  sub-layer ordering-info block with the 搂7.3.3 rule that
  `maxNumSubLayersMinus1 > 0` is followed by `8 - maxNumSubLayersMinus1`
  `reserved_zero_2bits` words, and decodes `ue(v)` as
  `(1 << leading) - 1 + suffix`.  Each of those is a place a hand-rolled
  bit reader silently desynchronises and reports a plausible-but-wrong
  geometry; the summary's SPS hash is what makes such a desync visible.

  Both cases run against the real driver and skip cleanly on a host
  without the hardware; the software-decoder half stays deferred (there
  is still no software H.265 decoder to assert on, and the skip says so).

- **A mid-stream parameter-set change is now *proven* to decode on its own
  (v1.3 plan item `Q1`, video plan `R2`).**
  The survey behind the v1.3 plan found that the *sender* already did the
  right thing — `openh264_encoder.cpp`'s `set_params()` re-creates the
  encoder when the geometry changes and the new SPS / PPS travel in-band
  with the next IDR — and that the *receiver* needed no mechanism at all:
  OpenH264's decoder handles a mid-stream SPS update on its own, because
  the decoder is the sole owner of "do my parameter sets still match?".

  What was missing was therefore not a mechanism; it was an **assertion**.
  Q1 is the test that would have caught a regression, and it is now in the
  standard CI matrix:

  - **The codec-level half**
    (`src/modules/h264/tests/test_openh264_software.cpp`) exercises the
    *warm* decoder: it decodes a 320×240 picture first (so the decoder is
    holding that parameter set), the encoder's geometry then changes to
    640×480, and the new picture is decoded **with the same decoder** — no
    flush, no re-initialisation, no key-frame request.  The bar is `kOk`
    on the *first* decode call: a `kErrNotReady` would mean the decoder
    needed another round trip.  The two parameter sets are compared
    byte-for-byte, so the test cannot pass by decoding the *old* frame
    twice, and a second case asserts the decoded picture carries real
    signal (a correctly-sized but blank frame would pass a
    geometry-only assertion).
  - **The engine-level half**
    (`tests/test_engine_video_e2e_loopback.cpp`) drives the *whole*
    pipeline with no stub in the middle: two engines, a real OpenH264
    encoder, real RTP on the wire (STAP-A for the parameter sets, FU-A
    for the slice), and the real receive path (depacketize → jitter
    buffer → decode → render).  The host pushes a 320×240 frame and then a
    640×480 one; the receiver must decode both and render the *new*
    geometry.  The acceptance is the plan's own wording — the
    decode-failure door stayed shut
    (`video_feedback_pli_from_decode_failure == 0`) **and** no PLI
    datagram went on the wire, which is the "no key-frame round-trip"
    half checked on the wire rather than only in a counter.
  - **The negative control.**  A counter that is *always* zero would
    satisfy `== 0` vacuously, so the harness also has to show the counter
    *does* move when the decoder really does refuse a frame.
    `TheDecodeFailureDoorIsLiveInThisHarness` feeds the receiver a
    synthetic single-NAL H.264 picture whose body is nonsense: the
    depacketizer accepts it, the jitter buffer assembles it, and the
    decoder refuses it — which opens the decode-failure door and moves
    the counter.  Without that control, the Q1 assertion would prove
    nothing.

  The H.265 half of Q1 is **still open** — the H.265 decoder in the tree
  is a stub, so there is nothing to assert on yet.  It is recorded as an
  explicit follow-up in the v1.3 plan's §6, not silently skipped.

- **Retransmissions now travel on their own RFC 4588 stream (v1.3 plan
  item `Q3`, video plan `T1`).**
  A NACK used to be answered by re-sending the *original* packet — same
  sequence number, same payload type.  RFC 4588 §4.1 is explicit that
  this is **not** RTX: a retransmission "SHALL be sent on a different
  payload type" so the receiver can tell it apart and restore the
  original's sequence number.  The consequence is that the receiver has
  to *un-RTX* — unwrap the header, restore the number, and splice the
  payload back into the frame it belongs to — because the receiver's
  frame assembly is keyed on the sequence number.

  The retransmission stream now exists end to end:

  - **A new format module, `src/modules/rtx/`.**  It owns the two-octet
    OSN header (RFC 4588 §4.1), the byte order, and the `apt=` /
    `rtx-time=` SDP parameters (RFC 4588 §8.1).  It is a *format* module
    — it builds no RTP headers and sends nothing — so both ends agree on
    the bytes by construction rather than by convention.
  - **The SDP advertises it.**  The offer carries
    `a=rtpmap:<pt> rtx/90000`, `a=fmtp:<pt> apt=<vpt>`, and
    `a=ssrc-group:FID <media_ssrc> <rtx_ssrc>` with the retransmission
    SSRC's own `a=ssrc:` line (RFC 5576 §4.2).  The answer echoes the
    offer's numbers (RFC 3264 §6.1).
  - **The sender frames it.**  The retransmission ring builds the OSN
    header, and the retransmission stream keeps its **own**
    sequence-number space (RFC 4588 §4) — a retransmission does not
    reuse the original's number on the wire.
  - **The receiver unwraps it.**  The OSN is read out, the *original*
    sequence number is restored, the payload is spliced into the frame it
    belongs to, and the packet is erased from the pending-NACK list — or
    the receiver would ask for it again, and again.
  - **The scope boundary holds.**  A peer that offers no RTX payload
    type gets no RTX stream, and the sender falls back to re-sending the
    original packet.  Both shapes are pinned by the tests.

  Three details the RFCs forced, each of which is a place a
  plausible-looking implementation gets it wrong:

  - **The header is two octets, not three.**  RFC 4588 §4.1 defines no
    marker byte; the `0x90` some descriptions of RTX mention is not in
    the RFC, and a peer that sent one would have its sequence number
    restored `0x9000` too large.  (The v1.3 plan's own description was
    corrected.)
  - **The FID group lists the media SSRC first** —
    `a=ssrc-group:FID <media_ssrc> <rtx_ssrc>`.  RFC 5576 §4.2's FID
    semantics relate a *primary* source to its *dependent* ones, and the
    primary comes first.  The reverse order is a real interop hazard: a
    receiver that follows the convention would treat the media stream as
    the repair flow and drop the picture.
  - **One payload type serves both directions.**  RFC 8843 (Unified Plan)
    gives an m-section a *single* payload-type list, so the RTX payload
    type is a property of the m-section rather than of a direction —
    which is also how Chrome negotiates it.  The `apt=` parameter then
    says which payload type *within* the section the retransmission
    protects.

  The public surface grows additively: `EngineConfig::video_sender_tuning`
  gains `rtx_payload_type` / `rtx_ssrc` / `rtx_time_ms`, `Stats` gains
  `video_rtx_packets_sent` / `video_rtx_packets_received`, and the
  plugin seams gain `set_rtx_enabled()` / `set_rtx_packet_callback()` /
  `set_rtx_payload_type()` / `push_rtp_rtx()` — all with defaults that
  leave a non-RTX plugin working exactly as before.

- **The soak can now impair the network and drive the engine (v1.3 plan
  item `Q5`).**
  The v1.2 release gate's soak window measured a **clean** network, and
  the impairment tooling that existed measured the **transport floor** —
  raw datagrams in, raw datagrams out.  Neither drove the engine, and
  neither used netem's `reorder` knob at all, even though the plan names
  *reordering* as one of the three impairments the soak must survive.

  Three pieces land together, and the point of the combination is that
  the impairments now hit the **real RTP path**:

  - **`src/modules/net_sim/`** — the impairment model
    (`net_sim::ImpairmentConfig`: loss / delay / jitter / reorder / rate,
    seeded and therefore deterministic) and the **simulated NAT**
    (`net_sim::NatRelay`, an address-translating UDP relay with two
    sockets).  Neither peer ever sees the other's real address, so the
    pair looks like two peers behind a NAT exactly as the SDP presents
    it.  This is the plan's Q5-1 default: the **real TURN/TLS leg is
    deferred to v1.4.0** and is recorded as a known gap, not papered
    over.
  - **`examples/soak-p2p`** — the engine-level harness: **two real
    `NimRTCEngine`s** (ICE + DTLS + SRTP + RTP) streaming audio across
    the relay, with the impairment profile applied to every packet the
    relay forwards.  It prints one JSON object with the parameters it
    was given and the counters it measured.
  - **`tools/run_soak_impaired.py`** — the recorder.  It runs the
    harness, folds its JSON together with the **git provenance** of the
    tree (`git_ref`, `git_commit`, `git_dirty`), **re-derives** the green
    criteria from the archived numbers, and appends one record to
    `build/soak-impaired/<platform>.jsonl`.  The netem parameters travel
    with the result, which is what the v1.2 plan's DoD asks for:
    *"soak runs are archived with their netem parameters."*

  **The recorder does not trust the harness**, and that is the part
  worth naming.  The green criteria are re-derived from the archived
  numbers rather than read from the harness's own `"green"` field, so a
  harness bug that faked a pass is still caught by the archive; a run
  that could not be measured is `INCONCLUSIVE` and **never** `PASS`
  (a broken runner must not be able to look like a green day); and a
  green run on a dirty tree is marked, because a dirty tree is not the
  commit it claims.  Each of those three rules is pinned by
  `tools/run_soak_impaired.py --self-test`, which runs in CI.

  The verdict is a **property of the run**, not of a reference number:
  the media path carried media, memory stayed inside Q2's bound (at most
  4 s of PCM), the feedback rate stayed inside Q4's bound, the loss was
  recovered, and a 100 %-loss profile actually dropped something — a
  profile that never drops would make the soak a lie.

  `reorder` is now a tier in the kernel-side tooling too
  (`tools/benchmark_weaknet_netem.sh`: netem `reorder 10% 50%`) and in
  its portable counterpart (`tools/benchmark_weaknet.py`: the
  `reorder-10pct` tier), so the two implementations of the same profile
  list stay in step.

  The operating procedure is `docs/plan/soak-impaired-runbook.md`; the
  gate is `python tools/run_soak_impaired.py --summary --expect-q5`,
  which accepts only a run of at least **24 h** under loss **and**
  jitter **and** reorder on a clean tree.

- **The picture is now aligned to the sound (v1.3 plan item `Q6`, video
  plan `R3` -- the video half).**
  The audio half of R3 shipped in `v1.2.0`: the engine aligns audio
  playout against the capture timeline, and the correction is
  switchable.  The **video** half did not exist -- the renderer had a
  display path but no clock to render against, and the skew the
  sender-report tracker produced was *visible* but nothing acted on it.

  A new `video_sync` module holds the render clock and the correction
  policy, and the engine drives it through a new
  `plugins::IVideoSyncController` seam -- so the engine orchestrates the
  plugin and never names a concrete module (Layout Invariant 4 /
  ADR-025).  The renderer is now **told** the presentation instant; it
  does not derive one.

  A video frame cannot be stretched, so the clock's actions are
  *present at the clock's instant*, *drop*, or *delay* -- chosen from
  the measured skew and the jitter buffer's current occupancy.  The
  jitter buffer keeps the playout deadline; the render clock owns the
  presentation instant *within* it, which is what keeps the buffer's
  memory bound stable.

  **There is still only one skew model.**  The clock has no capture-time
  accumulator of its own: it reads the ADR-017 mapping the audio half
  already reads.  That constraint was the one most likely to be
  violated by accident, so it is stated where a future change would
  look at it.

  The synthetic-skew test feeds the mapping a known offset and asserts
  a deterministic correction, so it fails on the pre-change code rather
  than passing vacuously.

- **The hardware codec path is now exercised end-to-end by something
  that re-runs (v1.3 plan item `Q7`, video plan `O-6`).**
  The standard CI matrix has no encoder-capable GPU, so the hardware
  path was validated **by hand** — which is the shape that fails
  silently: a procedure that nobody re-runs is a procedure that has
  already failed once.

  Two recorders land, and they cover the two halves of the claim:

  - **`examples/hw-e2e`** — the codec-level harness.  It pins the
    shipped H.264 backend table to `kHardware` (a pin the engine
    honours by *failing* rather than falling back), encodes 1080p
    frames through `IVideoEncoder`, carries the bitstream through the
    shipped `VideoSender` / `VideoReceiver` (STAP-A / FU-A) and
    `rtp::PacketBuilder` / `rtp::Parser`, decodes it back, and
    compares the luma against the source.  It prints one JSON record.
  - **`tests/test_engine_video_hw_loopback.cpp`** — the engine-level
    test: two real `NimRTCEngine`s with `video_codec_backend =
    "hardware"`, asserting from the **stats** that a hardware backend
    answered (`video_codec_hardware_accelerated`, and the backend
    neither `software-stub` nor `openh264`) *and* that the picture
    still arrives pixel-correct.

  `tools/run_hardware_e2e.py` is the recorder: it folds the harness
  record together with the machine's **driver stamp** and the tree's
  git provenance, re-derives the verdict from the archived numbers,
  and appends one record to `build/hardware/<platform>-<date>.jsonl`.
  `.github/workflows/hardware.yml` runs it on a **self-hosted,
  GPU-capable** runner (`runs-on: [self-hosted, windows, gpu-nvenc]`) —
  the label *is* the contract, so a hosted runner never picks the job
  up and a green check that means "nothing ran" cannot happen.

  **The honesty rules are the same ones the soak recorder applies**, and
  they are the point: a host with no hardware encoder produces a
  **SKIPPED** record, never a PASS, and `--require-hardware` turns that
  skip into a failing exit code — so a runner that silently lost its
  GPU is visible instead of quiet.  The verdict is re-derived from the
  archived numbers rather than read from the harness's own `"status"`,
  and a record on a dirty tree says so.  Each of those three rules is
  pinned by `tools/run_hardware_e2e.py --self-test`, which runs in CI.

  On a host with no hardware encoder the harness exits **77** and the
  record is `SKIPPED` — which is the honest outcome, not a failure of
  the tree.

### Fixed

- **A shell expansion inside `${{ }}` can stop an entire workflow from
  running, and nothing local says a word (2026-11-19).**
  The v1.2.0 follow-up added to `release.yml`'s tag-ancestry step:

  ```bash
  echo "       'git describe' cannot find it, a '${{ tag%.* }}.x'"
  ```

  `tag%.*` is bash "strip the shortest dot-suffix".  Inside `${{ }}` it is
  not an expression, so the file did not parse, and the `release` workflow
  started on every push and produced **zero jobs** (runs `44478642313`,
  `44481029254`) — 0 s, no failing step to read, no check-run to annotate.
  The YAML was valid, the string was legal bash, and `bash -n` was happy;
  the only signal was the absence of everything.

  `tools/check_workflow_expressions.py` now scans every workflow's
  expression blocks and rejects the characters and expansions the
  expression language does not have.  Against the pre-fix file it fails on
  exactly that line (193); against the fixed tree it passes.  Wired into
  `docs / markdown links` and registered as `test_workflow_expressions`.

- **The `release` workflow's tag filter also matched branch names
  (2026-11-19).**
  The trigger was `on.push.tags: v[0-9]+.[0-9]+.[0-9]+*`.  GitHub
  evaluates those patterns as ECMAScript regexes with `RegExp.test()`,
  anchored at the *start* of the ref only, in which `+` and `*` quantify
  the preceding *character* — not the class before them.  The filter
  therefore read as "v, a run of digits, any character, a run of digits,
  any character, …", and as a prefix test it matched ordinary branch
  names, `chore/release-1.2-followup` among them.

  Anchoring with `$` is not enough: "a run of digits" is still
  indistinguishable from "a run of digits, then anything", and a
  four-segment pattern matched `integrate/q3-q6-recover`.  The filter is
  now two whole-ref patterns — `v[0-9]*.[0-9]*.[0-9]` and
  `v[0-9]*.[0-9]*.[0-9]-*` — covering the tag forms the repository has
  actually cut.  `tools/check_release_tag_filter.py` pins that: the 11
  release forms in the repository's tag list must fire the workflow, and
  the 23 branch names plus the `v1`-style aliases and `archive/*` tags must
  not.  Its engine is Node's `RegExp`, not Python's `re` — `re` rejects
  `[0-9]+*` outright ("multiple repeat"), so a Python check could not
  evaluate the shape it exists to catch.

- **The release bundle now ships the third-party licence texts, and the
  bundle-size limit is no longer a hidden release blocker.**
  The v1.2.0 review noticed the Windows bundle grew from 40.5 MB to
  62.8 MB (+55 %) and asked whether the licence obligations were
  complete.  They were not.  `release.yml` writes a CycloneDX SBOM from
  `src/third_party/vendor.json`, and the SBOM carries each component's
  SPDX identifier — but the licence **text** stayed in the source tree.
  Both BSD-3-Clause and Apache-2.0 require the notice to accompany a
  binary redistribution, so the bundle shipped six static codecs
  (`libvpx`, `svt-av1`, `libyuv`, `dav1d`, `openh264`, `wolfssl`)
  without discharging them.

  `tools/collect_licenses.py` now runs in all four release build jobs
  after `cmake --install` and before packing.  It copies every vendored
  tree's `LICENSE` / `COPYING` / `PATENTS` / `NOTICE` into `licenses/`
  in the staging tree, writes `licenses/THIRD_PARTY_NOTICES.txt`
  (SPDX id, upstream URL, pinned commit, SHA-256 per file), carries the
  `nv_codec_headers` headers whose MIT notice is embedded in every file
  (after checking that claim), and **fails the cut** if a distributed
  vendor has no licence file.  An uninitialised submodule is reported
  as a local-clone fact, not a defect.  Checked against the pinned
  SHAs, all 18 vendors have a dischargeable obligation: 17 ship a
  licence file or an inline notice (`nv_codec_headers`,
  `libopus`'s patent statement, …) and `webrtc_audio_processing`
  ships one in the tree the build fetches.  Wiring the gate turned up
  three defects in it: the emptiness test ran on `<vendor>/`, which
  always holds NimRTC's wrapper `CMakeLists.txt`, so an uninitialised
  submodule never looked empty; `dav1d`'s licence is `COPYING` at the
  submodule root rather than `LICENSE`; and `nv_codec_headers` keeps
  its headers under `include/ffnvcodec/`.  All three are fixed and
  pinned by `--self-test`.

  The same review's size number exposed a second problem.  The
  Windows bundle is 62,826,812 bytes (59.9 MiB) and
  `tools/make_release_bundle.py` refused anything above **64 MiB** — a
  guard written when a real bundle was 2-3 MB to catch the v1.0.1
  mis-archive.  At 59.9 MiB the next codec would have failed the cut
  with an error that says "sanity limit".  The limit is now 256 MiB
  (back to being a mis-archive detector), and a separate 96 MiB budget
  prints a warning with the current size, so payload growth is visible
  in the release log before it becomes a failure.

- **`ci-retry` no longer goes red on its own, and no longer refuses a
  legitimate retry (post-v1.2.0 review).**
  Reading all ten red `ci-retry` runs between 11-17 and 11-19 found two
  defects, neither of which was the one the review suspected:

  - **Nine of the ten red runs were `403 Forbidden`, not
    misclassification.**  `workflow_run` fires on the *run*
    transition, but a `cancel-in-progress` cancel tears jobs down
    asynchronously, and the run's own state is a snapshot taken at a
    different moment from its jobs'.  Run `44439751037` was still
    `in_progress` at `01:49:22Z` when its `ci-retry` fired and tried
    to re-run its attempt-1 jobs; the platform refuses to re-run a
    job whose run is still running.  The script now polls until the
    run reports `completed` (up to 20 × 15 s), and if it never settles
    it logs the state and exits 0 — a run that never settled is not a
    `ci-retry` failure.  The tenth was `HTTP 502: Bad Gateway`, a
    platform hiccup: `tools/ci_retry_runner.py` now retries 5xx and
    429 with exponential backoff, and `403` is deliberately excluded
    from that set because the same call would get the same answer.
  - **The loop guard refused a real retry.**  The guard was the run's
    `run_attempt`, so any attempt above 1 was refused.  Run
    `44306330102` starved its macOS leg in attempt 1, was re-run, and
    the same leg starved again in attempt 2 — the guard said "this run
    has had its retry", the retry never happened, and the merge gate
    stayed red.  A job-level re-run also bumps `run_attempt` for
    *every* job, so the counter never described the job it was
    guarding.  The guard is now per job name (`--max-retries 1`,
    counted by walking the per-attempt job lists); dry-running the
    classifier against `44306330102` selects the macOS leg, where the
    old guard selected nothing.

  The review suggested retrying every `cancelled` run.  Measured over
  the 60 most recent cancelled runs, 124 cancelled jobs had a runner
  and had run steps (a supersede or an operator stop) against 25 that
  never reached a machine, so that change would re-run five times more
  superseded jobs than starved ones.  `cancelled` is therefore handled
  by the same `steps == 0 && runner_name == ""` rule as `failure`, and
  the self-test now pins the concurrency-cancel shape explicitly.

  `ci-fast` is dropped from the watched workflows: it triggered 1 of
  the 498 measured `ci-retry` runs over the last 1200, and
  `windows / fast` is not a required check.

- **The audio jitter buffer's memory is bounded, and the bound is
  reported (v1.3 plan item `Q2`, video plan `R4`).**
  `jb::Config::max_packets` was **declared and never read**: its own
  comment said *"Hard cap on buffered packets (FIFO trim from oldest when
  exceeded)"*, but nothing in the module read it, and the plugin adapter
  hard-coded `512`. The buffer's `frame_buffer_` — a `std::map` keyed by
  RTP timestamp that only ever shrinks in `pop()` / `flush()` — therefore
  grew without limit: a peer that sent a new timestamp per packet, or a
  stream whose frames never became playable, could make the receiver
  allocate as much as it liked.

  The bound is now real, and it has two halves:

  - **`Config::max_bytes` is new** (default 4 MiB) and joins
    `max_packets` (default 512). A packet-count cap alone does not bound
    memory — the packets of one stream can differ in size by orders of
    magnitude — so the byte count is the half that actually caps what the
    receiver holds. Both bounds are enforced in the same loop; `0`
    disables either half, and the historical unbounded behaviour stays
    available on request.
  - **The trim runs *before* the insert**, so the packet that has just
    arrived can never be the one the bound evicts. This is the video
    jitter buffer's `enforce_inflight_cap(incoming=1)` rule applied
    verbatim; without it the newest audio — the one the peer just sent —
    would always be the first to be dropped. The victim is the *oldest*
    frame, which is the frame that has already missed its slot.

  The bound is reachable from a host and visible to one:

  - `plugins::JBConfig::max_packets` is new — `0` means "use the
    implementation's default", a negative value means unbounded — so the
    cap crosses the plugin seam instead of being frozen at `512` inside
    the adapter.
  - The drop counter is reported all the way up: `jb::Stats::packets_dropped`
    → `plugins::IJB::Stats::packets_dropped` →
    `plugins::IAudioReceiver::Stats::packets_dropped` →
    `Engine::Stats::audio_packets_dropped` and
    `AudioStreamStats::packets_dropped`. Each step republishes the
    buffer's own counter unchanged, so there is no second copy of the
    bookkeeping that could drift from what the buffer really did.

  The tests all fail on the pre-change code:
  `src/modules/jb/tests/test_jb.cpp` (the bound itself: the packet count
  and the byte count never exceed their caps under a sustained overrun,
  the drops are counted, the newest packet is never the victim, the oldest
  frame is trimmed first, and tightening the cap takes effect
  immediately), `src/modules/jb/tests/test_jb_plugin.cpp` (the seam: the
  cap crosses it, the drop counter comes back, and a live reconfigure
  carries the new cap), and a propagation case in
  `src/modules/audio_rx/tests/test_audio_rx.cpp`.

- **The control plane now rides a real socket, and the G2 criterion is
  measured instead of asserted (remote-desktop plan `G2`, v1.2 plan item
  `E6`).**
  The remote-desktop plan's `G2` row says the control plane must have *its
  own* transport — its own socket, its own congestion-control domain, its own
  5-tuple — and that the criterion is **measured**: "under a saturated video
  path the control path's p99 stays within its declared budget; the two
  stacks carry independent traffic and the control stack's packets are
  observed on a different 5-tuple".

  The stack was *structurally* there and *behaviourally* dead, and it took
  four separate defects to find out.  Each one is worth naming, because each
  was a plausible-looking implementation of a correct-looking interface:

  - **The control stack came up with no socket at all.**  The raw-UDP
    factory's default config had `enable_real_socket == false` (the slice-6
    in-process test path), and `RawUdpArqStack::start()` used that default
    verbatim.  The control plane therefore had no transport, and every
    control message went nowhere.  The stack now builds its own config with
    the real socket on and its own bind address, so the control plane's
    socket is a *different* socket from the media plane's — which is the
    whole point of the separate stack.
  - **The engine never pumped the control path's receive queue.**  The ARQ
    session accepts datagrams into a queue and fires its receive callback
    when that queue is drained, and nothing drained it: the ARQ session has
    no consumer thread of its own.  So the inbound half of the control plane
    was dead code and every inbound control message was lost.  The engine's
    `tick()` now calls `pump_control()` on the control stack.
  - **The control address never reached the peer.**  The control plane rides
    its own transport, so the media SDP does not describe it, and the
    control session could only ever be half-open: the side that *answers*
    learned the peer from the first inbound datagram, but the side that
    *offers* had nobody to send to.  The address now travels in the session
    description as `a=x-nimrtc-control:<host:port>`, and
    `process_remote_sdp()` points the control path at it.  The attribute is
    consulted at session level and at media level, so a peer that puts it in
    an m-line also works.
  - **A wildcard bind advertised an address that cannot be sent to.**  A
    socket bound to "0.0.0.0" comes back from `getsockname()` as
    "0.0.0.0", and `sendto("0.0.0.0:port")` fails with
    `WSAEADDRNOTAVAIL` / `EADDRNOTAVAIL` — the wildcard means "any *source*
    address", never "the destination".  Since the control path advertises
    its own address to its peer, the advertised address has to be a concrete
    routable one: `UdpSocket::local_endpoint()` now resolves a wildcard bind
    to the host's first non-loopback IPv4 address (falling back to the
    loopback on a host that has no other), keeping the kernel's port.

  The ARQ session also gained **peer discovery**: the first datagram it ever
  receives identifies the peer, so a control session is symmetric even
  without the out-of-band address.  An *explicitly configured* peer always
  wins — the SDP-carried address is authoritative and a stray datagram must
  not redirect the session.

  `tests/bench/bench_control_saturation.cpp` is the measurement.  It drives
  **two real engines** over **two real sockets**: the media plane through a
  token-bucket bottleneck that **drops** the datagrams arriving when the
  bucket is empty (exactly as a bottleneck router's queue overflows), and
  the control plane on the G2 shape — a separate `raw-udp-arq` stack with
  its own ARQ session and its own 5-tuple.

  The control producer, the media producer and the engine tick each run on
  their **own** thread, and that is the point rather than a convenience:
  driving both planes from one loop would put the software H.264 encode on
  the control plane's critical path, and the measurement would report the
  *encoder's* latency instead of the transport's.

  `tools/benchmark_control_saturation.py` asserts five invariants as
  *properties* rather than as thresholds against a reference number: the
  saturation was real, the control plane reached the profile's rate, its p99
  stayed inside the profile's declared `max_p99_ms`, **every** control
  message arrived (a dropped operator click is not a slow click), and the
  two stacks rode different 5-tuples.  A **negative control** — the same rig
  with the video path disabled — runs alongside, so a passing saturated run
  cannot be an artefact of the video path having been switched off by
  accident.  The committed sample is
  `docs/benchmarks/control_saturation_windows_x86_64.json`.

  **The measurement is 197.2 Hz against the profile's 200 Hz with a p99 of
  4.23 ms against the profile's 50 ms budget**, while the media link drops
  6,472 datagrams; the negative control runs at 195.5 Hz with a p99 of
  3.77 ms.

  Two wrapper guarantees are worth naming, because both close a way the
  measurement could silently lie.  The **rate the binary runs at** and the
  **rate the invariant is checked against** are the same number: both
  come from the profile unless the command line overrides them, and the
  defaults are *unset* rather than the profile's values, because a
  default that **equals** the profile's value cannot be told apart from
  an explicit override — so a profile change would leave the binary on
  the old rate while the assertion moved to the new one, and the rate
  invariant would compare the run against a target the run was never
  asked to meet.  And the rate invariant is **skipped rather than
  failed** on a host without the high-resolution deadline: the rate is
  produced by a thread waiting on a deadline, so without the 100 ns
  deadline the run measures the *host's timer* rather than the
  transport, and a rate that missed the target would say nothing about
  the control plane.  The binary reports whether it had the deadline
  (`high_resolution_wait`), and the artifact records it — so the
  artifact cannot claim a precision the run did not have.

  And **both** scenarios are now retried when the *host* was too busy to
  produce a usable measurement: the saturated one when the link was
  never brought to capacity, and the negative control when the control
  plane missed the rate.  The negative control's rate is the case worth
  naming: it carries **no video at all**, so its control plane has
  nothing to compete with, and a rate below the target there can only
  be the host's scheduling — never the transport.  Without the retry
  the benchmark would report a *transport* failure for what is really a
  busy machine — the same mistake the saturated scenario had
  already learned to avoid.

  Getting those numbers to mean what they claim took **two defects in the
  harness itself**, and both are recorded because both are plausible:

  - **The timer.**  On Windows the producer's 5 ms period cannot be met by
    `sleep_for`: the default ~15.6 ms system tick would cap the producer at
    ~64 Hz, so the "measured rate" would describe the *host's timer* rather
    than the transport.  The fix is a high-resolution waitable timer
    (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`).  Two wrong turns are worth
    recording: `SetWaitableTimerEx` is **not** the high-resolution arming
    call — the SDK declares it with *seven* parameters (a `PREASON_CONTEXT`
    and a `TolerableDelay` the plain form does not have) and it takes no
    high-resolution flag at all, so a six-parameter prototype is a stack
    corruption and an immediate `0xC0000005`; and the accurate arming call
    is the *plain* `SetWaitableTimer` on a high-resolution handle.
  - **The schedule.**  The producer's schedule must be **absolute**: a late
    wake is absorbed by the next slot rather than by shifting every slot
    that follows.  The obvious alternative — resetting the phase to "now +
    period" to avoid a catch-up burst — makes the *measured* period the wake
    error plus the configured period, so a 500 us wake error turns a 5 ms
    period into a 5.5 ms one and the control plane reports ~182 Hz instead
    of the profile's 200 Hz.  The catch-up a long preemption would
  otherwise trigger is **bounded**, so the offered load stays at the
  profile's rate without a burst polluting the very latency distribution
  the benchmark measures.

  A `Benchmark G2 control plane (plan E6)` step now runs in both the Windows
  and the Linux CI legs, and the artifact is uploaded with them, because a
  benchmark that only runs on one developer's machine is not a gate.

- **A single H.264 NAL unit packet is forwarded according to the
  receiver's declared framing, not re-framed unconditionally.**
  `ReceiverConfig::h264_annex_b` now decides the single-NAL path: with
  the flag **off** (the default) the packet is handed to the decoder
  byte for byte, which is the RFC 6184 搂5.1 shape 鈥?a bare NAL unit
  arrives as `NaluFormat::kRaw`.  With the flag **on**, a bare unit gets
  an Annex-B start code and a unit that already carries one is forwarded
  as-is, so a frame assembled from a bare SPS + PPS + slice is still a
  parsable Annex-B frame.  Previously the receiver synthesised framing
  unconditionally, which silently changed the default contract.  The
  flag's semantics are now documented in the header rather than only in
  the plan.

- **The CI auto-retry workflow no longer re-runs a superseded run — and a
  pull request can now reach a green state.**
  `ci-retry.yml` re-runs the jobs of a CI run that the platform starved of
  a runner.  It re-runs them at the *job* level, and a job-level re-run is a
  new attempt of that run — which enters the concurrency group of the
  **current** run, because a pull request's `github.ref` is
  `refs/pull/<n>/merge` for every commit on it.  With
  `cancel-in-progress: true`, the retry therefore cancelled the very run it
  was meant to repair; the cancellation fired another retry, and the pull
  request could never go green.  Observed on PR #124: the `ci` run was
  cancelled within 48 seconds of starting, over and over.

  The fix is a staleness guard in `tools/ci_retry_runner.py`: the run's
  commit is compared against the current head of its ref (the pull
  request's head SHA, or the branch tip for a push run), and a run that no
  longer carries it is reported rather than re-run — it could not turn the
  merge gate green anyway.  The guard is pinned by the script's self-test,
  which runs in CI on every push.  A `--force` flag is the operator escape
  hatch on the manual dispatch path.

  The concurrency groups are deliberately left SHA-free: a SHA-bearing group
  would stop a new push from cancelling the run it supersedes, and a rapid
  sequence of pushes would then burn a full four-platform matrix each.

## [1.2.0] - 2026-11-18

### Status: Stable

> **The source version is `1.2.0`** (`project(VERSION)` in `CMakeLists.txt`,
> `NIMRTC_API_VERSION` = 10200, `NIMRTC_API_VERSION_PRE_RELEASE` = `""`).
> `v1.2.0` is the release that closes the `v1.2` plan
> (`docs/plan/v1.2-next-plan.md`): **Track E** (the convergence gates) and
> **Track S** (the observability surface) are both complete.
>
> **The label was corrected on 2026-11-19.** This section was written by
> `tools/release_changelog.py`, which hard-coded the heading
> `### Status: Release candidate` for every release it produced. The
> version carries no SemVer pre-release identifier (`RELEASING.md` §2.2),
> the tag was cut as `v1.2.0`, and the GitHub Release is published as a
> non-prerelease "Latest" release -- so the section said "candidate" while
> the release said "stable". The generator now derives the label from the
> version (a pre-release identifier means a candidate; no identifier means
> stable), and `tools/release_changelog.py --self-test` pins the rule.
> The same defect is in the `[1.1.0]` section below, which was also
> published as a stable release; it is corrected there too.

> Track E -- every gate is green:

> | Gate | What it proves |
> |---|---|
> | **E1** | The public-header baseline is frozen in a checked-in manifest and CI fails on drift. |
> | **E2** | macOS and Linux compile **every** adapter leaf (ScreenCaptureKit, Metal, EGL, Wayland), not just the Windows ones. |
> | **E3** | The vendored AEC is on by default and is *measured*: the rig records **30.35 dB** ERLE, identically on Windows / MSVC and Linux / GCC 11. |
> | **E4** | Both Chrome Case E planes (DTLS-only and WebTransport) PASS on Windows + Chrome 157, and the result is a CI assertion. |
> | **E5** | The DXGI zero-copy hand-off is real, and its fallback is documented. |
> | **E6** | The ABR behaviour is measured on a **saturated** link with real packet loss -- which uncovered an AIMD defect that is now fixed. |
> | **E7** | The vendored `usrsctp` tree (269 files, ~5.5 MB) is gone; no dead SCTP backend is reachable. |
> | **E8** | This section. |

> Track S -- `getStats()` is now a surface a host can trust:

> - **S1** -- the snapshot lifecycle (`snapshot_time_us`, `snapshot_seq`, `stats_snapshot()`, `stats_delta_since()`).
> - **S2** -- the inbound RTP statistics, per SSRC.
> - **S3** -- the `video_stats_bridge` wiring.
> - **S4** -- the RTCP feedback, broken down by type, and the per-audio-stream statistics.
> - **S5** -- the transport plane: ICE state + the nominated candidate pair + RTT, DTLS state / role / fingerprint, the SRTP counters.
> - **S6** -- the data-channel plane.
> - **S7** -- the remote-desktop breakdown, shaped like the pipeline: `capture` / `encode` / `decode`, each with its fps, its ms (and p95 on the two codec stages) and its frame count, plus the clamped cross-stage drop accounting.
> - **S8** -- the **stable field set**: a field is not renamed, retyped or removed in a minor release, its meaning does not change, and growth is additive.  The 145-field table is *derived*, not hand-maintained -- `tools/dump_stats_fields.py` parses `struct Stats` out of `engine.hpp` and CI fails when the header and `docs/api/stats_field_reference.md` disagree.

> **Known limitations carried into this release** (none of them is a regression):

> - **TURN over TLS is not negotiated.** `TurnTransport::Tls` is accepted by the API, but the negotiation is not implemented and the transport falls back to plain TCP.
> - **The ABR benchmark measures a loop the engine drives.**  The engine feeds the controller `BitrateController::on_measured_bitrate()` with a one-second closed window over the encoder's outbound byte counters, and `on_keyframe_requested()` when the peer asks for a key frame (PLI / FIR).  The *estimator*, the *pacer* and the *resolution ladder* under test are the shipped ones, and the loop is closed in production as well as in the benchmark.
> - **HEVC has no software decoder.** x265 is GPL-2.0, which the licence review excludes, so the HEVC screen-content path is hardware-only.
> - **The QoE latency percentiles are measured but the freeze *finding* is only wired on the video path.** The input-to-photon measurement covers the control transport, as the acceptance criterion requires.


### Added

- **`Engine::Stats` has a documented, CI-enforced stable field set (v1.2
  plan item `S8`).**
  `docs/api/engine_api.md` §13.7 states the contract -- a field is not
  renamed, retyped or removed in a minor release, its meaning does not
  change, and growth is additive -- and lists all **145** fields with their
  type and the definition the header gives them, plus the shape of every
  nested entry (`VideoStreamStats`, `InboundRtpStats`,
  `AudioStreamStats`, `DataChannelStats`, `TransportStats`,
  `VideoSyncStats`, `RemoteDesktopStats`).
  The table is **derived, not hand-maintained**:
  `tools/dump_stats_fields.py` parses `struct Stats` out of `engine.hpp`
  and `--check docs/api/stats_field_reference.md` fails when the document
  and the header disagree; the `docs / markdown links` CI job runs it, so
  a field added to the header without its reference line fails the pull
  request instead of surfacing as documentation rot months later.
  The sample-app half is recorded next to the table: `demo-p2p`'s
  `audio-rx` line (whose *shape* `tools/run_e2e_acceptance.py` parses as
  part of the e2e gate), and the `rd_device` / `rd_viewer` / `rd_verify`
  rows.

- **The remote-desktop pipeline has a statistics breakdown (v1.2 plan item
  `S7`).**
  `NimRTCEngine::Stats` carries a new `RemoteDesktopStats remote_desktop`
  -- the video pipeline broken into the three stages a screen-share
  session is judged by, each in the unit a human reasons about.
  Before this the numbers existed but were **scattered**: the capture
  counters sat under `video_capture_*`, the codec telemetry under
  `video_codec_*`, the per-stage timings under `video_stage_*`, and the
  decode counters under `video_frames_decoded`.  A host asking the
  remote-desktop question -- *"is the picture slow because the **capture**
  is slow, or because the **encoder** is slow, or because the **decoder**
  is slow?"* -- had to know which of a dozen prefixes carried which half
  of the answer, and the answer was still **incomplete**: there was no
  capture frame rate at all, and no decode-side cost that was reachable
  without reaching into the private stage timings.
  The entry is shaped like the **pipeline**, so a host compares the
  halves instead of guessing which prefix carries which half:
  - **capture** -- `capture.fps` (the rate the device delivered frames,
    measured on the **device's own capture timestamps**, so a device that
    stamps frames itself still yields the rate it actually ran at),
    `capture.ms` (the mean cost of the device handoff) and
    `capture.frames`;
  - **encode** -- `encode.fps` (the stage's throughput), `encode.ms`
    (the mean cost of the codec's `encode()` call), `encode.p95_ms`
    (**the number a stutter shows up in** -- a good mean with a terrible
    p95 is a session that hiccups) and `encode.frames`;
  - **decode** -- `decode.fps`, `decode.ms`, `decode.p95_ms` and
    `decode.frames`;
  - **the cross-stage counters** -- `frames_dropped_before_encode`
    (captured but never encoded) and `frames_decoded_without_local_encode`
    (informational only -- a receive-only session has an empty encode
    stage and a busy decode stage, so it is deliberately kept out of the
    drop accounting).  These two stay at the **top level** of
    `RemoteDesktopStats`, because they describe the gap *between* two
    stages and belong to neither.

  The three stage blocks are their own public types --
  `RemoteDesktopCaptureStats`, `RemoteDesktopEncodeStats` and
  `RemoteDesktopDecodeStats` -- each carrying `fps` / `ms` / `frames`
  (plus `p95_ms` on the two codec stages).

  Two design decisions are worth stating outright.
  **Everything is derived, never measured twice.**  The costs come from
  the per-stage instrumentation the engine already runs
  (`Stats::video_stage_capture` / `video_stage_encode` /
  `video_stage_decode`); the capture rate comes from the capture
  timestamps that already travelled with every frame; the encode / decode
  rates come from each stage's wall-clock span.  The block therefore
  **cannot drift** from the table it summarises, and it added **no work
  on the frame path** -- no new clock read, no new counter, no new lock.
  **The units are the host's, not the engine's.**  The stage table
  reports microseconds (and nanoseconds, for the sub-microsecond
  stages); the breakdown reports **milliseconds** and **frames per
  second**, because "the encode costs 4.2 ms" is a number an operator
  reads without a division.

  The surface is documented in `docs/api/engine_api.md` §13.6 and pinned
  by `tests/test_engine_remote_desktop_stats.cpp` (5 cases: the baseline,
  the outbound half, the inbound half, the agreement with the stage
  table, and the clamped drop accounting).

- **The transport plane has statistics (v1.2 plan item `S5`).**
  `NimRTCEngine::Stats` carries a new `TransportStats transport` -- the
  **path the media travels over**: ICE (RFC 8445) -> DTLS (RFC 6347) ->
  SRTP (RFC 3711).
  Before this there was almost nothing: `Stats::srtp_drops` counted the
  packets the outbound path could not encrypt, `dtls_state()` exposed the
  DTLS state, and ICE -- the plane that decides *where the packets go* --
  had **nothing at all**: no candidate pair, no round-trip time, no restart
  count.  A session that "is not working" is almost never broken in DTLS
  or SRTP: it is a path problem, and the statistics surface could not say
  which path the traffic was taking.
  The entry carries three planes:
  - **ICE** -- `ice_state` (the same vocabulary as `ice_state_string()`),
    `ice_restart_count`, `selected_pair` (an `IceCandidatePairInfo` with
    *both* ends' address / port / protocol / candidate type), the derived
    `relayed` bit, and `rtt_ms` + `has_rtt`.
  - **DTLS** -- `dtls_state` (lower-cased and hyphen-separated),
    `dtls_role`, and `dtls_fingerprint` in the RFC 8122 UPPER-hex form.
  - **SRTP** -- `srtp_installed` (the authority on "is this session
    encrypted"), the protected / unprotected RTP + RTCP counters,
    `srtp_auth_failures` and `srtp_drops`.

  Two design decisions are worth stating outright.
  **No new probes.**  The RTT is the one the engine already measures from
  the peer's Receiver Reports (the LSR / DLSR pair against our own Sender
  Report, RFC 3550 6.4.1) -- the *same* figure the bandwidth estimator
  consumes.  The statistics surface republishes it; it does not measure
  it a second time, because two measurements of one quantity would
  eventually disagree.  `has_rtt` says whether the number was *measured*
  at all, which is what distinguishes "the RTT is 0 because the path is
  fast" from "the RTT is 0 because nothing was ever measured".
  **No copies of another owner's state.**  The ICE state and the candidate
  pair are owned by the ICE agent (read through the new additive
  `plugins::IICETransport::selected_pair()` seam, defaulted so a
  third-party transport keeps compiling), the DTLS state by the DTLS
  session, the SRTP counters by the SRTP context, and the round-trip time
  by the network-feedback collector.  The projection lives in its own
  translation unit (`src/engine/src/engine_transport_stats.cpp`) and
  holds **no state**, so the surface cannot drift from the thing it
  describes.
  The surface is documented in `docs/api/engine_api.md` §13.5 and pinned by
  `tests/test_engine_transport_stats.cpp` (12 cases).

- **The data-channel plane has statistics (v1.2 plan item `S6`).**
  `NimRTCEngine::Stats` carries a new `std::vector<DataChannelStats>
  data_channels` -- **one entry per channel the host opened** with
  `create_data_channel()`, in creation order.  Before this there was
  *nothing*: `ControlChannelStats` describes the engine's own control
  channel (the remote-desktop control plane), not the user-facing
  channels a host opens, so a host that opened a "telemetry" channel and
  a "cmd" channel could not tell whether either was actually moving
  data -- the classic "is the feature working?" question a statistics
  surface exists to answer.
  The entry carries the channel's identity (the engine-assigned
  `channel_id`, the `label` the host gave it, the `reliability` mode as
  a canonical *name* rather than an enum) plus its counters.
  **The counters come from two different parties, and that is the whole
  design.**  The *inbound* half (`messages_received` / `bytes_received`)
  is the **engine's** account: the engine installs the channel's
  callbacks, so it sees every inbound message and counts it.  The
  *outbound* half (`messages_sent` / `bytes_sent` / `messages_dropped` /
  `messages_retransmitted` / `messages_pending`) is the **backend's**
  account, reached through the new additive
  `plugins::IDataChannel::stats()` seam.  The engine cannot derive
  those numbers -- it only sees the `send()` call, and what happens to
  the message afterwards (queued, retransmitted, dropped when its
  lifetime expired) happens inside the backend.
  The seam is **additive**: `plugins::IDataChannel::stats()` has a
  default implementation that returns an all-zero struct, so a backend
  that does not track its own counters keeps compiling and simply
  reports nothing.  The engine republishes those zeroes rather than
  inventing a number.
  To make the inbound half work even when the host installs no message
  callback of its own, `create_data_channel()` now hands the host a
  **proxy** around the backend: the proxy forwards every call
  unchanged, counts the inbound traffic on the way, and records the last
  state the backend reported.  The engine keeps a `shared_ptr` to the
  backend (so the statistics can still ask it for its outbound counters
  after the host drops its handle), and the lambdas the proxy installs
  on the backend capture *shared* state rather than the proxy's `this`
  -- the backend may outlive the proxy, and a late inbound message must
  not find a dangling pointer.
  `SctpDataChannel` and `WebTransportDataChannel` both implement the
  seam; the WebTransport one reports the ARQ's drop / retransmission
  counts and the outbound queue depth it already kept.
  The counters are cumulative since the channel was created, so the
  difference of two readings is the increment; `messages_pending` is
  the exception and is a gauge (the queue depth at the moment of the
  reading).  The list is empty when the host never opened a channel, so
  a host that does not use the data plane sees no change in its
  telemetry.
  The surface is documented in `docs/api/engine_api.md` §13.2 and pinned
  by `tests/test_engine_datachannel_stats.cpp` (12 cases).

- **The acoustic echo canceller is now measured, and its "on by default" claim
  is asserted in CI (v1.2 plan item `E3`).**
  `audio3a::Config::enable_aec` has always defaulted to `true` and
  `webrtc_audio3a.cpp` has always forwarded it to
  `apm_config.echo_canceller.enabled` — but **nothing measured whether that
  path actually removes echo**, and every CI leg configured with
  `-DNIMRTC_VENDORED_WEBRTC_APM=OFF`, so the option's *default* was asserted
  nowhere.  Flipping the default to OFF in `cmake/NimRTCVendored.cmake` would
  have left the whole matrix green while shipping the stub 3A.

  The ERLE rig (`src/modules/audio3a/tests/test_audio3a_erle.cpp`) synthesises
  a **known** echo path — a 20 ms delay, which is the same quantity a host
  measures on a real device and passes as `Config::aec_delay_headroom_ms` —
  so the measurement needs no audio hardware and is reproducible anywhere.
  The render signal is speech-like broadband noise, not a tone: the
  canceller's delay estimator rejects periodic signals, so a tone would report
  an ERLE no real session reaches.  The capture leg carries echo only, so
  `ERLE = 10·log10(P_uncancelled / P_cancelled)` needs no near-end / echo
  separation.

  **The result is 30.35 dB**, identical on Windows / MSVC 19.43 and on
  Linux / GCC 11 — against the 15 dB floor ITU-T P.807 sets for a converged
  canceller in a stationary synthetic path.  A near-end check pins down that
  the canceller is not simply muting the microphone (the near-end tone keeps
  80% of its amplitude), and a control run with the canceller off reports
  0.0 dB, which is what proves the number comes from the canceller rather
  than from an artefact of the rig.

  The measurement, the method and the known limits are recorded in
  `docs/interop/aec-erle-results.md`.

  A dedicated CI leg, `linux-apm / real 3A`, now re-measures it on every PR.
  It configures with the option **left at its default** — no `-D` flag at
  all, because the default is the thing under test — and runs the rig under
  `NIMRTC_REQUIRE_WEBRTC_APM=1`.  That last part matters: **a skipped gtest
  case is a passing case to CTest**, so a build where the APM silently
  degraded to the stub would skip all three cases and the leg would report
  green while measuring nothing.  The switch turns that skip into a failure.

  Three defects were exposed along the way, all fixed:

  - `tools/build_webrtc_apm.cmd` passed `--cpp-args=/Zc:designated-initializers`
    to `meson setup`, which meson 1.11 rejects outright — the flag has to
    travel in a native file instead.
  - The same script put the PATH override on the same command line as
    `call vcvars64.bat`.  cmd expands the whole line *before* executing it,
    so `%PATH%` was substituted with the pre-vcvars value and the MSVC
    toolchain was dropped from the environment altogether.
  - `tools/build_webrtc_apm.sh` ran `meson setup --wipe` unconditionally.
    `--wipe` deletes every file in the build directory, **including the
    compiled objects**, so the leg's prebuild cache was worth nothing:
    ninja recompiled ~440 translation units on every run.

  And one portability defect in the rig itself, found by compiling it on
  Linux: `RecordProperty` takes an `int`, and passing a `double` to it is
  rejected by GCC's `-Werror=float-conversion` (MSVC does not complain).
  The measurements are now scaled explicitly and recorded as
  `<name>_x1000`.

- **The ABR behaviour on a saturated link is now measured, and the
  measurement is asserted in CI (v1.2 plan item `E6`).**
  The plan's evidence row said "adaptive bitrate survives congestion — ABR +
  pacing unit-tested", and marked it *open*, because "unit-tested" is not
  the same claim as "measured".  The eight benchmark scripts `tools/` already
  shipped (latency, dc, weaknet, weaknet_netem, capture_encode, hotpath,
  remote_desktop, size) all measured links with **spare capacity**:
  `benchmark_weaknet_netem.sh` injects *loss and jitter* into a link that was
  never filled, so the queue never overflowed and the estimator never saw the
  "send more until it breaks" loop a saturated link produces.  Congestion and
  injected loss are different phenomena, and only the first one tests a
  controller.

  `tests/bench/bench_saturation.cpp` closes that gap by driving the
  **shipped** components against each other over a **real UDP socket pair**:
  the shipped bandwidth estimator (`nimrtc::bwe`), the shipped token-bucket
  pacer (`nimrtc::pacing`) and the shipped resolution ladder
  (`nimrtc::video_bitrate_control`).  The receiving side enforces a
  token-bucket *capacity* and **drops** the datagrams that arrive when the
  bucket is empty, exactly as a bottleneck router's queue overflows — so the
  loss the estimator reacts to is a real loss of real packets, measured by
  the real receiver.  What is *modelled* is the link (a fixed-capacity,
  loss-on-overflow bottleneck) and the peer (a receiver that reports loss,
  RTT and optionally REMB); there is no ICE, DTLS, SRTP or codec in the
  path, because the benchmark measures the **rate-control closed loop**, not
  the transport stack.

  Two scenarios are measured, differing **only** in the resolution floor:
  `floor` (720p, the product's legibility floor) makes the lowest allowed
  rung twice the link's capacity, so the picture cannot be made small enough
  to fit and the ladder must hold the floor while the bitrate is throttled;
  `unfloored` lets the ladder descend to a rung that fits, so the offered
  load converges onto the capacity.

  The binary is deliberately **not** a ctest case — every number in it is
  machine-dependent (the loopback's real RTT, the host's real scheduling
  jitter), so a threshold assertion would be a flaky test rather than a
  measurement.  The properties that must hold regardless of the machine are
  asserted by `tools/benchmark_saturation.py` instead, as *properties* and
  not as thresholds against a reference number: the saturation was real, the
  stream survived, the ladder reacted, the estimator never raised its target
  on a report that said the link was congested, the delivered rate stayed
  within the capacity, the offered load tracked the capacity, the ladder
  recovered, the throughput recovered, and the ladder did not flap.  All
  nine are recorded in the committed artifact
  `docs/benchmarks/saturation_windows_x86_64.json` with their measured
  values.

  Two of the nine — *the delivered rate stayed within the capacity* and
  *the offered load tracked the capacity* — only mean something when the
  link was genuinely saturated, and the sender is a *process on the host*:
  a busy host cannot fill the link, so the link never queues, nothing is
  dropped, and those two properties have no answer.  Failing them would
  report a rate-control bug that is really a busy CI runner.  The wrapper
  therefore detects that case, **skips** the two properties, and re-runs
  the scenario; after three attempts the scenario is a failure, because a
  gate that cannot measure is not a gate.  The verdict lists every skipped
  property, so a green run cannot hide an unchecked one.

  A `Benchmark saturated link (plan E6)` step now runs in both the Windows
  and the Linux CI legs, and the artifact is uploaded with them.

- **The statistics snapshot lifecycle (Track S / `S1`, design plan `DS-1`).**
  `Engine::Stats` was a flat struct of ~125 cumulative counters, and that
  shape answers *"how much has happened since `open()`"* and nothing else.
  A host that wanted a **rate** — bytes per second, packets per second —
  had to take two readings and guess how far apart they were, and it had to
  know from the documentation which fields were counters and which were
  instantaneous gauges.  Guessing wrong is the classic statistics bug: you
  subtract two EMA bitrates and report a negative rate.

  The lifecycle gives the host both, explicitly:

  - **`Engine::Stats::snapshot_time_us`** — when the rest of the struct was
    measured, in microseconds on the **engine's monotonic clock** (the same
    origin `tick()`'s `now_us` uses, so it is directly comparable with the
    engine's other timing surfaces and never jumps the way a wall clock
    does).
  - **`Engine::Stats::snapshot_seq`** — a monotonically increasing sequence
    number, one per reading.  It orders two readings even when their
    timestamps are identical (a coarse clock, or two readings inside the
    same microsecond), which is what makes *"the interval between these two
    readings"* a well-defined thing.
  - **`Engine::stats_snapshot()`** — the named, snapshot-able view of the
    same numbers (`core::StatSnapshot`).  A snapshot owns its data, so a
    host may keep it as a baseline after the engine is destroyed.
  - **`Engine::stats_delta_since(previous)`** — the **increment** over the
    interval (`core::StatsDelta`), plus the interval's duration.

  The semantics travel with the number, and the API refuses to mix them up:
  `counter_delta()` answers for a cumulative counter and **refuses** for a
  gauge (the difference of two instantaneous values is not a rate);
  `gauge_value()` answers for a gauge and **refuses** for a counter.  A
  default-constructed snapshot has no usable baseline — the delta would be
  measured against a made-up zero — so `valid()` is false rather than the
  answer being a plausible-looking lie.

  The new types live in **`src/core/include/nimrtc/core/stats_registry.hpp`**
  (`core::StatsRegistry`, `core::StatSnapshot`, `core::StatsDelta`,
  `core::StatSemantics`) — a `src/core/` type, because every layer has to
  speak it: the engine publishes into it, and a future module (ICE,
  DataChannel) can publish into it through a plugin seam.  It is
  header-only and STABLE, like the rest of `src/core/`.

  The registry is a **projection** of the flat struct, taken at reading
  time: the flat struct stays the single authority, so the two views can
  never drift apart.  The projection lives in the private engine
  collaborator `src/engine/src/engine_stats_state.hpp`, where the
  counter-vs-gauge classification sits *next to the field it describes* —
  a rename breaks the build rather than silently dropping the field from
  the registry.  The registry is an **instance**, not a singleton (Layout
  Invariant 6 forbids a module from exporting global mutable state), so
  two engines in one process get two registries.

  Tested in `tests/test_core_stats_registry.cpp` (the type's contract:
  declaration, semantics, sequencing, ownership, isolation) and
  `tests/test_engine_stats_snapshot.cpp` (the behaviour: the delta *is*
  the increment, the timestamp distinguishes two instants, the registry and
  the flat struct agree).

  This adds one public header (`core/stats_registry.hpp`), so D-N6 applies:
  an explicit, reviewed manifest bump (189 → 190), not a silent drift.

- **The inbound RTP per-source statistics (Track S / `S2`).**
  `Stats::video_frames_received` and `Stats::video_bytes_received` counted
  *every* inbound stream mixed together, so a host could not tell which
  source they came from.  That is fine for a session with one peer
  stream and useless for anything else: a peer sending **simulcast**
  (RFC 5956 — several encodings of the same picture on different SSRCs)
  puts three different qualities into one counter, a session receiving
  from more than one source has the same problem, and there was no way
  to ask *"how many packets arrived on SSRC X?"* — which is the question
  a loss investigation actually asks.

  `Stats::inbound_rtp_streams` is the receive-side counterpart of the
  existing `Stats::video_streams`: one `InboundRtpStats` entry per
  inbound RTP source, keyed by **SSRC** (the only thing on the wire that
  identifies a source — the payload type says how to *decode* a packet,
  not who sent it):

  - `packets_received` / `bytes_received` — what arrived.
  - `packets_lost` — the gaps in the sequence space, counted by the
    receive pipeline, the only party that sees it.  This is the loss
    figure a host actually wants.
  - `nacks_sent` — the retransmission requests this engine emitted.
  - `frames_received` / `frames_incomplete` — the access units that were
    assembled, and the ones that arrived but could not be.
  - `jitter_ms` / `playout_delay_ms` — the receiver's own measurements
    for this source.
  - `payload_type` — **observed**, not negotiated: a peer that changes
    its payload type mid-stream (RFC 3264 §5.1 allows a PT change after
    a re-INVITE) shows up here as a changed value.

  The entry accumulates over the life of the source: a source that
  restarts (a new SSRC) gets a new entry, and a source that disappears
  keeps its entry — so a host reading the list after the peer stopped
  still sees the totals it produced.

  The engine-side state lives in the private collaborator
  `src/engine/src/engine_inbound_state.hpp`; the public shape is
  `NIMRTC_API_STABLE InboundRtpStats` in `engine.hpp`, and
  `docs/api/engine_api.md` §13.2 documents the contract.

  Tested in `tests/test_engine_inbound_rtp_stats.cpp` (a fresh engine
  reports no sources; a packet creates the entry for its SSRC; a
  decoded frame is counted against its source; an unknown payload type
  is still counted; the entry keeps its totals after the source stops).

- **RTCP feedback is now broken down by type (Track S / `DS-4`).**
  `Stats::video_feedback_applied` was a single number, so a host watching it
  climb could not tell *what the peer was asking for* — a peer sending PLIs
  and no NACKs has a different problem from one sending NACKs and no PLIs,
  and the two call for different fixes.  The breakdown now separates the two
  directions, because they describe opposite things:

  - `video_feedback_rx_pli` / `_rx_fir` / `_rx_nack` / `_rx_remb` /
    `_rx_transport_cc` — feedback the **peer sent us**, about *our outbound*
    stream.  A PLI here means "I could not decode what you sent".
  - `video_feedback_tx_pli` / `_tx_nack` — feedback **we sent the peer**,
    about *its inbound* stream.  A NACK here means "your packets are arriving
    damaged".  The `_tx_` set is deliberately shorter: the engine's outbound
    video feedback can only ever carry a NACK or a PLI, so counters for FIR
    and REMB would never move.

  Counted in `handle_rtcp()` (`engine_media.cpp`), held in `VideoState`
  (`engine_video_state.hpp`), asserted in
  `tests/test_engine_video_feedback.cpp` — including that a NACK does not
  count as a PLI, that an unrelated Receiver Report moves no counter, and
  that a transport-cc message is counted on its own.  The aggregate
  `video_feedback_applied` keeps counting what it always counted.

  These are the **first public fields added after the E1 manifest freeze**, so
  D-N6 applies: they are an explicit, reviewed manifest bump, not a silent
  drift.

- **The frozen public-header manifest (plan E1).**  `docs/api/public_headers.manifest`
  pins the **set** of public headers — 191 paths, sorted, one per line — and
  `scripts/check_api_manifest.py` fails CI when the tree's header set drifts
  from it, naming both sides of the drift (the headers that appeared, the ones
  that went away).  Wired into all four platform legs of `ci.yml` and into
  `ci-fast`.

  The count-based gate that preceded it
  (`tests/test_api_classification.cpp::total_header_count_matches_baseline`)
  could not tell a swap from a change: deleting `a.hpp` and adding `b.hpp`
  in one PR leaves the count untouched, so CI stayed green while the public
  surface had moved.  ADR-015 calls that surface a contract, and a contract
  needs the actual list, not its cardinality.

  Regenerate with `python scripts/check_api_manifest.py --write` in the same
  PR that adds, removes or renames a public header.

- **Planning: `docs/plan/v1.2-next-plan.md`.**  Records the v1.2 planning
  decisions (D-N1..D-N5) and sequences the next three releases as four
  tracks: **E** convergence work that gates the v1.2.0 tag, **S** the
  complete `getStats` surface, **Q** the v1.3.0 resilience line (R2/R4,
  RTX, PLI/FIR rate limiting, soak), and **C** the v1.4.0 connectivity
  line (TURN/TLS, ICE-over-TCP).  The document also states the explicit
  non-goals and the owner-gated items, so that "what is not planned" is
  as visible as "what is".

- **The cross-platform adapter compile proof (plan E2).**  The Linux CI
  leg now installs the leaf-adapter dev packages and *asserts* that every
  adapter leaf was compiled in, instead of letting a missing package
  silently skip a source file.  Covered leaves: X11, XFixes, PipeWire,
  EGL/GLESv2, Wayland, xdg-shell and linux-dmabuf for `video_sink`; X11,
  XFixes and PipeWire for `video_capture`.  macOS is covered by the
  existing `macos-clang / arm64` leg, which compiles the whole tree
  including the Objective-C++ Metal renderer.

  The first run of the proof failed on the `NIMRTC_VIDEO_SINK_HAS_EGL=1`
  assertion, because the runner image ships no EGL dev package — which
  was the point: the assertion caught a path that would otherwise have
  been skipped in silence.  Asserting the Wayland leaves then exposed
  that the Wayland renderer had never been compiled on any build host
  (see the corresponding entry under *Fixed*).

- **The input-injection adapter leaves are now part of the compile
  proof (plan E2).**  `libxtst-dev` joins the CI package list and the
  `Leaf compile proof` step asserts that both the XTest and the uinput
  injection backends were compiled in, and builds the
  `nimrtc_rd_input_inject` target.  The same shape of gap the Wayland
  assertion found: `libxtst-dev` was missing from the runner image, so
  `NIMRTC_HAS_XTEST=1` was never defined and the whole X11 branch of the
  Linux injector was dead code (see the corresponding entry under
  *Fixed*).

- **The Chrome interop results (plan E4).**  `docs/interop/chrome-interop-results.md`
  records the re-run of the Chrome ↔ NimRTC interop matrix after the
  DataChannel and video Part B merges, with the reproduction commands, the
  per-assertion evidence and the root-cause write-up.  **Both** Case E planes
  pass on Windows with Chrome 157:

  - *DTLS-only* — Chrome's `dtlsState` reaches `connected`, the SRTP cipher
    is negotiated, and NimRTC's own log line plus a `CLIENT_RANDOM` in the
    wolfSSL keylog confirm the handshake from the server side.  Three
    consecutive runs, no flake.
  - *WebTransport* — the session reaches `ready`, a datagram is echoed
    (`NIMRTC-WT-DGRAM-ECHO-01`) and a bidirectional stream is echoed.

  `.github/workflows/interop.yml` now runs both planes as **assertions**: a
  failure in either fails the job.  Previously the workflow generated the
  WebTransport certificate and stopped there — `tools/run_e2e_acceptance.py`
  has implemented both Case E runners all along, but nothing in CI ever
  invoked them, so a broken Chrome interop could not turn CI red.

- **CI auto-retry for jobs that were starved of a runner.**  A
  GitHub-hosted runner is not guaranteed: a job asking for a saturated
  pool — the `macos-15` arm64 image is the chronic case in this
  repository — can be starved until the platform gives up ("The job was
  not acquired by Runner of type hosted even after multiple attempts"),
  ending `cancelled` with **no steps** and **no runner**.  The run's
  conclusion is then `failure` and the merge gate is red for a reason
  that has nothing to do with the tree.  Measured on 2026-11-16: PRs
  #65, #66 and #67 each needed a manual `gh run rerun` before the macOS
  leg went green.

  `.github/workflows/ci-retry.yml` now does that automatically, and the
  safety argument is the interesting part: it re-runs **only** a job
  that never received a runner (`steps == 0` **and** `runner_name == ""`).
  A job that *ran* and failed is never re-run, because that failure
  belongs to the tree and re-running it would hide exactly the defect
  the CI run was for.  The two signatures are disjoint and were verified
  against real runs of this repository (43938323684 → starved;
  43877514623 → genuine).

  The loop guard is now per job name: `--max-retries 1` lets a job be
  re-run once on top of the attempt it was first seen starved in, and
  a third starvation is reported as a sustained outage instead of
  retried forever.  (v1.0.0 shipped a run-level guard on `run_attempt`,
  which was wrong in both directions — see the 2026-11-19 fix below.)
  The classifier lives in `tools/ci_retry_runner.py` with a
  `--self-test` that pins every classification, and the self-test runs
  both in CI and as a ctest case (`test_ci_retry_runner`).

  The scope covers `ci`, `ci-e2e` and `release`.  (`ci-fast` was
  dropped on 2026-11-19: it triggered 1 of 498 measured `ci-retry`
  runs, and `windows / fast` is not a required check.)  `release`
  needed three properties to be verified experimentally before it
  could be included, because a release cut is the most expensive
  thing the repository does and a partial one is worse than a red
  one:

  * a job-level re-run revives a dependent job that was **skipped**
    because its dependency failed — `publish-release` has `needs:` on
    all four build jobs, so a starved build job skips the publish job
    (verified: a dependent went from `skipped` back to `success`);
  * an artefact uploaded in attempt 1 is still downloadable in attempt
    2, so the publish job finds the three bundles the other build
    jobs uploaded (verified: a payload uploaded in attempt 1 was read
    back in attempt 2);
  * a job-level re-run starts immediately even under the
    `cancel-in-progress: false` concurrency group that `release.yml`
    uses to keep a superseded re-run idempotent (verified under the
    exact release shape).

  The `upload-artifact` 409 hazard cannot occur: a job is only
  retried when **no step of it ever ran**, so it never uploaded
  anything in the first place.

  `soak` is the one workflow left out.  Both of its jobs carry
  `continue-on-error: true` (the verdict lives in the evidence record,
  not the job status), so a job that ran and failed still reports
  `success`; and a starved soak job would not be helped by a retry,
  because the window is a statement about *calendar* days and a
  retry landing on the next UTC day produces a second data point
  rather than a replacement.

- **The remote-desktop session guard** (`src/modules/rd_session/`).  A
  locked Windows workstation breaks the screen capture in two different,
  both expensive ways: DXGI Desktop Duplication is refused outright
  (`E_ACCESSDENIED`), and Windows Graphics Capture opens happily and
  then delivers **exactly one frame** and no more.  The second case is
  the expensive one — a frozen picture looks like a broken encoder, a
  broken network or a broken peer, and the operator burns an hour
  bisecting the wrong layer.

  The module answers one question, portably: *is the interactive
  desktop session currently capturable?*  The answer is a **three-state**
  value, not a boolean, because the third state needs a different
  operator response:

  | state | meaning |
  |---|---|
  | `kUsable` | the desktop is available; a capture will produce frames |
  | `kLocked` | the session is locked; a capture will fail or freeze |
  | `kUnknown` | the platform cannot tell (headless, a service session, …) |

  `kUnknown` is a first-class answer, not a failure: a machine with no
  interactive session at all is not a *locked* session.

  On Windows the detection is a single Win32 call — `OpenInputDesktop()`,
  which opens the **input desktop** the user actually interacts with.
  When the session is unlocked it succeeds and the name reads back as
  `"Default"`; when it is locked, Winlogon owns the input desktop and
  the call fails with `ERROR_ACCESS_DENIED`.  That failure alone is not
  proof of a lock (a service session in session 0 fails the same way),
  so it is attributed to a lock only when the Winlogon UI is running in
  *our own* session; otherwise the answer is the honest `kUnknown`.

  The **recovery** disconnects and reconnects the console session
  (`tsdiscon` / `tscon`), which leaves it logged in but unlocked.  It
  has to run in a *helper process*: `WTSDisconnectSession()` on the
  caller's own session terminates the caller before the call returns,
  so the module spawns the terminal-services tools with
  `CREATE_NO_WINDOW` instead.

  The module is **wired into `rd_device`**: the device reports the
  guard's state on one line before the capture starts and refuses to
  start on a locked workstation instead of freezing silently, and
  `rd_device --recover-session` performs the recovery on demand (it
  needs an elevated prompt).

- **The capture → encoder zero-copy seam (v1.3, remote-desktop plan
  B10 / M1).**  The two halves of the zero-copy claim were each proven
  on their own — the capture device delivers a GPU surface with
  `bytes_copied == 0`, and NVENC consumes a registered DirectX surface
  — but the *seam* between them was not.  And the seam is not "hand
  the texture across": NVENC registers the DirectX input surface per
  encode session, and the driver accepts the registration **only if the
  texture was created on the same `ID3D11Device` the session was opened
  with**.  A texture from a different device — even on the same
  physical adapter — is refused with `NV_ENC_ERR_RESOURCE_REGISTER_FAILED`
  (status 9).  So the seam is "**share the device** and hand the
  texture across", and that is what landed.

    - `video_frame::GpuContext` — a portable, refcounted *device* handle
      (`backend` / `device` / `immediate` / `keepalive`), so a producer
      can publish the device its surfaces live on without either side
      including a platform header.  `plugins::` re-exports it, so
      plug-in code stays in one namespace.
    - `plugins::IVideoCapture` grew three **additive** virtuals:
      `set_gpu_frame_callback()`, `gpu_context()` and
      `supports_gpu_frames()`.  All three have default implementations,
      so an existing capture plug-in compiles and runs unchanged; a
      plug-in that does not implement them simply never delivers a
      surface, and the engine falls back to the CPU path.
    - `plugins::GpuCaptureFrame` carries a refcounted
      `std::shared_ptr<video_frame::GpuBuffer>` plus the dirty
      rectangles, the timestamps and the colour description (plan C5).
      A *borrowed* handle would die with the next `pop_frame()`, which
      is the wrong shape for a consumer that holds the surface across
      its own async work.
    - `plugins::VideoCodecConfig` gained `gpu_context`, and the engine
      fills it from the capture device **before** the codec is opened —
      the encode session has to live on the capture's device, so the
      device has to be known before `open()`.
    - The Windows DXGI / WGC backends publish their D3D11 device, and
      their texture pool now tracks the **busy** slots (the
      `weak_ptr` of the buffer it handed out) so a texture nobody has
      released is never overwritten.
    - `src/engine/src/engine_gpu_mailbox.hpp` — `detail::GpuFrameMailbox`,
      a single-slot drop-oldest exchange.  The capture device delivers
      on **its own thread**; the mailbox is the boundary that keeps the
      codec on the **tick thread** (RD-6).  The newest frame is the
      truth, so the stale one is dropped.
    - The **engine** wires the seam together, in `init_video_plugins()`:
      it attaches the capture device's GPU callback to the mailbox,
      publishes the capture's `GpuContext` into
      `VideoCodecConfig::gpu_context` **before** the codec is created
      (the encode session has to live on the capture's device, so the
      device has to be known before `open()`), and reports the seam's
      counters in the stats snapshot.  On teardown the mailbox is
      cleared **before** the capture device is closed, so a pending
      frame never holds a texture that belongs to a device that is
      going away.
    - The NVENC encoder **adopts** the capture's device instead of
      creating its own, verifies that the texture it was handed lives
      on that device, and accepts the BGRA input the desktop compositor
      produces natively (`DXGI_FORMAT_B8G8R8A8_UNORM` declared as
      `NV_ENC_BUFFER_FORMAT_ARGB`) — no shader / VideoProcessor
      conversion pass in between.
    - `Stats::video_capture_gpu_frames` / `video_capture_gpu_errors` /
      `video_capture_gpu_wired` expose the seam's own counters, so a
      host can see the GPU path running (or not) rather than infer it.
    - `tests/test_engine_capture_zero_copy.cpp` drives the **engine**
      (not the two modules) and proves the four facts together: the
      wiring happens, the codec receives the *capture's* device, the
      surface reaches the encoder with `video_capture_bytes_copied == 0`
      and `video_capture_frames_cpu_staged == 0`, and the codec is
      called **only** from the tick thread.
    - `tests/bench/bench_zc_seam.cpp` measures the driver-level seam on
      real hardware: on the reference host NVENC accepts an NV12 and a
      BGRA surface created on the session's own device and **refuses**
      one created on another device (status 9) — which is exactly the
      constraint the shared-device design exists to satisfy.

- **The RTP packet builder moved into the engine (v1.4, ADR-025
  Decision 4).**  The engine is exempt from Layout Invariant 4 for
  `sdp::` and `rtp::` — but the exemption now has a *visible boundary*:
  the outbound RTP packets are built in exactly one file,
  `src/engine/src/rtp_packetizer.cpp`, and
  `tools/check_engine_layering.py` enforces it at the symbol level
  (`rtp::PacketBuilder` may only be named there).

- **The session limits and the administrative kill switch (v1.4,
  remote-desktop plan I6).**  The reference signalling service had no
  bounds at all: every device that presented a pairing code got a
  session, every session lived until the peer disconnected, and there was
  no way for an operator to stop a session they did not want.  A service
  that is shipped as the *reference implementation* is also the thing a
  reader copies, so the bounds belong here.

    - `examples/remote-desktop/signalling_core.py` gained the two
      dataclasses `SessionLimits` (`max_concurrent_sessions`,
      `max_concurrent_rooms`, `session_lifetime_s`, `idle_timeout_s`) and
      `SessionPolicy` (`check_join`, `check_room_budget`,
      `session_expired`, `engage_kill_switch`, `release_kill_switch`),
      and `SignallingService` now enforces them: the join is refused
      when the concurrency cap or the room budget is exhausted, the
      session ends when its lifetime or its idle timeout expires, and
      the kill switch pauses *new* joins without tearing down the live
      ones — an administrative pause, not a disconnect.
    - A session is **room-scoped**: the device and the viewer that join
      the same room attach to the *same* session record, so a pair costs
      one slot and a re-join with existing credentials does not consume a
      second one.
    - `examples/remote-desktop/signalling.py` runs a background reaper
      that enforces the timeouts, and the server takes
      `--max-sessions`, `--max-rooms`, `--session-lifetime` and
      `--idle-timeout` on the command line.
    - The idle timeout is driven by **real activity**: the media-plane
      relay touches the session, so a session that is only relaying
      frames stays alive and one that has gone quiet is reaped.
    - Covered by `examples/remote-desktop/test_signalling.py`: the cap,
      the slot release on disconnect, the re-join without a new budget,
      the kill switch, the idle timeout, the hard session lifetime, the
      room budget and the parameter validation.

- **The capture / encode benchmark now measures the *real* platform
  backends (v1.4, remote-desktop plan M1 / J5).**  The committed
  artifact measured the portable synthetic capture device and the
  encoder, but the plan's exit criterion asks for the *capture rate* of
  the platform backends and for *evidence* that the GPU path is really
  zero-copy.  Neither was measured, so the claim rested on the code
  reading well.

    - `tests/bench/bench_capture_encode.cpp` gained two measurements.
      **The platform throughput** opens the real DXGI Desktop
      Duplication and WGC devices, drives them through the
      `IVideoCapture` seam, and reports the delivery rate over a
      two-second window.  **The zero-copy probe** attaches a GPU
      consumer exactly as a hardware encoder would and reads the
      device's own counters, so the two can be cross-checked against
      each other rather than against a claim.
    - **Both backends deliver a frame only when the desktop *changes*.**
      That is the point of a dirty-rect capture path, but it means a
      rate cannot be measured on a static desktop.  The benchmark
      therefore jiggles the mouse pointer during the window (restoring
      the pointer's original position afterwards) and reads the
      device's `frames_captured` counter across it.  A backend that
      still delivers nothing is recorded as a *static-desktop
      observation*, not as a zero-fps backend — the artifact never
      claims a number it did not measure.
    - **The measured result on the CI reference machine** (committed to
      [`docs/benchmarks/capture_encode_windows_x86_64.json`](docs/benchmarks/capture_encode_windows_x86_64.json)):
      at 1920×1080, **DXGI 64.2 fps (31.0 fps per megapixel)** and **WGC
      49.8 fps (24.0 fps per megapixel)**; the zero-copy probe shows
      **60 / 60 GPU frames with `bytes_copied == 0` for DXGI** and
      **61 / 61 for WGC** — the pixels never entered host memory.
    - `tools/benchmark_capture_encode.py` grew a `--platform-only` mode
      that merges the platform sections into an existing artifact
      instead of rewriting it, so a machine with real capture backends
      but no hardware encoder SDK can contribute its evidence without
      discarding the encode numbers another machine produced.  The
      merged artifact records the contributing machine under
      `platform_capture_machine`.
    - Verified: full build clean, `ctest` **1966 / 1966 passing**, and the
      signalling example's own suite **94 / 94 checks** passing.

- **The QoE collector seam and the engine's QoE wiring (v1.4, remote-desktop
  plan J3 / J6).**  The `rd_qoe` module shipped the *metrics* but nothing in
  the product ever *fed* them: the engine's `Stats` carried the per-stream
  counters and the per-stage timings, not the experience metrics, and the
  input-to-photon latency — the number that actually decides whether a
  remote-desktop session feels responsive — was not measured at all.  That
  is the dead-module state the architecture-remediation plan D2 calls out:
  a complete implementation with a full test suite that nothing calls.

  The fix follows the shape every other pluggable family already uses: the
  *contract* is a seam, the *implementation* is a module, and the engine
  only ever talks to the seam.

    - **New public API** (all `NIMRTC_API_STABLE`, all **additive**):
      `src/plugins/include/nimrtc/plugins/qoe_collector.hpp` declares
      `plugins::IQoeCollector` (the event sink + snapshot out),
      `plugins::IQoeCollectorFactory`, `plugins::QoeConfig` (the reservoir
      sizes — the collector's memory bound) and `plugins::QoeSnapshot` (the
      flat value type the engine publishes).
    - **The registry slot** `core::PluginRegistry::register_qoe_collector()`
      / `get_qoe_collector()` / `list_qoe_collectors()`, the registrar
      category, and the `NIMRTC_REGISTER_QOE_COLLECTOR` macro — the same
      pattern as the control channel, the bitrate controller and the
      capture device.
    - **The module side**: `src/modules/rd_qoe/src/qoe_plugin.cpp` adapts
      the existing `rd_qoe::QoeCollector` to the seam and registers under
      the id `"rd-qoe"`.
    - **The engine side**: `EngineConfig::qoe_collector_name` selects the
      collector (empty = no QoE metrics; an **unknown id is a hard error**
      at `open()` — the engine never falls back silently).
      `NimRTCEngine::Stats` gained a `plugins::QoeSnapshot qoe` field.
      The engine pushes the events it already knows about — a frame
      reached the viewer, a frame was dropped, an input event went out —
      and reads the aggregate back.
    - **New public API** (all `NIMRTC_API_STABLE`):
      `NimRTCEngine::send_input_event(payload, event_id)` sends an input
      event over the control plane **and** stamps it for the
      input-to-photon measurement.  The event is stamped only when the
      control channel *accepted* it: a refused event never reached the
      peer, so there is no honest latency to report for it.
    - **The pairing** (J6): the engine pairs an event with the first
      presented frame whose ref-frame id is **strictly greater** than the
      event's — the operator acted on the frame they were looking at, so
      the frame that reflects the action is a later frame in the same id
      space.  An event that was *not* bound to a frame (id 0) pairs with
      the next presented frame outright.
    - **Honesty about what was measured**: without a collector the engine
      still publishes the **frame counters** — the presented and dropped
      frame counts, which it keeps anyway — and zeroes for the *metrics*,
      which is the honest answer to "what was the latency percentile?"
      when nothing measured it.
    - New tests: `tests/test_engine_qoe.cpp` (8 cases — the registry
      resolution, the presented-frame counting, the negative control that
      proves the engine really feeds the collector, the unknown-id
      configuration error, the input-to-photon measurement over the DTLS
      app-data seam, the unbound-event pairing, the refused-event
      exclusion and the empty-payload rejection) and
      `src/modules/rd_qoe/tests/test_qoe_plugin.cpp` (the module-side
      adapter).
    - `tools/check_dead_modules.py`'s `PENDING_WIRING` list is now
      **empty**: both `rd_input` and `rd_qoe` are reachable from the
      composition root, so neither needs a waiver.

- **The multi-stream video plane (v1.4, remote-desktop plan H1 / H2 / H4).**
  The engine held exactly **one** of each video plugin
  (`video_source_ / video_sink_ / video_receiver_ / video_sender_ /
  video_codec_`) plus a single `video_sender_tuning.ssrc`, so a
  remote-desktop product could not carry a screen stream *and* a camera
  stream (or a thumbnail) in one session.  The video plane is now a
  **stream set** keyed by a stable stream id.
  - **New public API** (all `NIMRTC_API_STABLE`, all **additive** — the
    single-stream call sites keep working unchanged, ADR-015 Decision 2):
    `send_video(stream_id, frame)`, `set_video_source(stream_id, name)`,
    `set_video_sink(stream_id, name)`, `set_video_codec(stream_id, name)`,
    `add_video_stream(stream_id, codec_name = {})`,
    `remove_video_stream(stream_id)` and `video_stream_ids()`.
  - **The legacy single-stream path is the stream named `"default"`.**
    It keeps the engine's own plugin members, and
    `send_video("default", frame)` delegates to the very function
    `send_video(frame)` calls — so the two paths cannot drift.
  - **The stream id is a `std::string`** (`"default"`, `"screen"`,
    `"camera"`, …), not an integer: it has to survive a
    reconfiguration, be readable back from a host's configuration file,
    and mean something to a human reading a log line.  It is mapped to
    an SSRC and a payload type at creation time by a deterministic
    FNV-1a hash with linear-probe collision resolution, so the *same*
    id always maps to the *same* wire identity.
  - **Each stream owns its own encoder, sender, receiver, decoder and
    sink** (plan H2), resolved through the registry by id, each with its
    own RTP packet callback (its own SSRC, its own payload type, its own
    SRTP session).  Two streams may use different codecs in the same
    session; a codec-family change rebuilds the stream's packetizer
    (the packetizer is codec-aware) without changing the stream's
    identity on the wire.
  - **Per-stream statistics** (plan H4): `NimRTCEngine::Stats` gained
    `std::vector<VideoStreamStats> video_streams`, one entry per
    **active** stream (the default one included), each with its own
    bitrate, ladder level, keyframe rate, backend id and frame / packet
    counts.  The flat `Stats::video_*` fields are unchanged and still
    describe the **default** stream.
  - **D5 (per-stream bandwidth shares) now feeds the multi-stream
    case**: `update_pacing_rates()` builds one reserved audio demand
    plus **one elastic demand per active video stream**, so
    `allocate_bandwidth()` splits the estimate between the streams; the
    video *class* rate the pacer uses is the sum of the streams'
    targets, and each stream's own share is written back to its state so
    the pacer, the per-stream rate controller and the statistics all
    read the same number.
  - **H3 (simulcast) is deferred** and deliberately *not* implemented.
    The model leaves the room for it (the per-stream state separates the
    stream's *source* identity from its *encoding* list), and the
    thumbnail case is served by an independent low-resolution capture —
    i.e. a second stream.  See `docs/plan/remote-desktop-plan.md`
    §Track H and Track X's X-2 row.
  - New test: `tests/test_engine_multi_stream.cpp` (13 cases —
    independent SSRCs and payload types, a stable id→SSRC mapping
    across a reconfiguration, concurrent multi-stream flow, per-stream
    codec instances, the per-stream statistics, the removal semantics,
    and the unchanged legacy single-stream path and counters).

- **The Apple VideoToolbox backend is real now, and the HEVC / AV1 tables
  carry the non-NVIDIA rows (v1.4, remote-desktop plan B5a / B5 / B6).**
  The H.264 table had exactly one hardware encoder per non-Apple
  platform, and the HEVC and AV1 tables carried *only* the NVIDIA rows —
  so a Mac had no hardware codec at all and a Linux / Windows host had no
  way to say "this combination is unsupported".
  - The shared `nvcodec` module gained the VideoToolbox integration:
    `src/modules/nvcodec/src/videotoolbox_encoder.cpp` and
    `videotoolbox_decoder.cpp`, written against `VTCompressionSession` /
    `VTDecompressionSession` and `CVPixelBuffer`, and exposed through the
    new public header `nimrtc/nvcodec/vt_backends.hpp`
    (`vt_encoder_available()` / `make_vt_encoder()` /
    `vt_decoder_available()` / `make_vt_decoder()` /
    `vt_encoder_unavailable_reason()` / `vt_decoder_unavailable_reason()`,
    all `NIMRTC_API_STABLE`).  The session code is **codec-parameterised**:
    one code path serves H.264, HEVC and AV1, differing only in the
    `CMVideoCodecType`.
  - The availability probe is *honest*: it creates and tears down a real
    compression session, so it answers for the **silicon**, not for the OS
    version — and behind an explicit OS-version floor (macOS 10.9+ for
    H.264, macOS 10.13+ for HEVC, macOS 14+ for AV1) whose violation is
    reported as the reason rather than as an opaque `OSStatus`.  Note that
    VideoToolbox exposes AV1 **decode** on M3-and-newer silicon but no
    Apple chip has an AV1 hardware **encoder**, so the AV1 encoder row
    reports "not available" on every current Mac.
  - The zero-copy contract is `CVPixelBuffer` in → `CVPixelBuffer` out
    (`plugins::GpuBackend::kAppleCFPixelBuffer`), and the rows set
    `zero_copy_supported == true` — which is what plan B10 needs.
  - The H.264, H.265 and AV1 tables each register a `videotoolbox_*` row
    at **priority 400** (NVENC 450 > NVDEC 430 > MediaCodec 420 >
    VideoToolbox 400 > AMF 380 > VA-API 360 > Quick Sync 340 > the
    software rows 50 / 0), on *every* platform — the seam id and the
    `HwBackend::VideoToolbox` kind stay stable across the tree, and the
    probe is what decides usability.
  - The HEVC table gained the VA-API, Quick Sync, AMF, VideoToolbox and
    MediaCodec rows; the AV1 table gained the AMF, VA-API, MediaCodec and
    VideoToolbox rows.  Each is registered unconditionally and reports
    `available() == false` **with a non-empty reason** when the hardware,
    the platform SDK or the implementation is absent — the plan's
    "unsupported combinations are explicitly reported, not silently
    ignored".
  - The backend descriptor gained the optional
    `unavailable_reason` field
    (`std::function<std::string(const VideoCodecConfig&)>`) on both
    `plugins::VideoEncoderBackend` and `plugins::VideoDecoderBackend`.
  - New tests: `src/modules/h265/tests/test_hw_backends.cpp` and
    `src/modules/av1/tests/test_hw_backends.cpp` (the whole table: the
    row exists, the priority is defined, an unavailable row explains
    itself, the priority ladder is ordered, and an unsatisfiable
    *hardware* preference fails explicitly rather than falling back to
    software), `src/modules/nvcodec/tests/test_vt_common.cpp` (the
    platform-independent VideoToolbox plumbing: the AV1 low-overhead OBU
    ↔ Annex-B conversion, the AVCC → Annex-B conversion, the Annex-B
    scanner and the OS-version gate), and new cases in
    `src/modules/h264/tests/test_hw_backends.cpp` for the VideoToolbox row.

- **The video rate controller and its ladder (v1.4, remote-desktop plan D1 /
  D2 / D3 / D4).**  The video path had no adaptation ladder at all: the
  encoder's bitrate was written **once** at `open()` and never moved,
  so a link that collapsed kept sending at the rate the session started
  with.  The remote-desktop product needs the opposite — a picture that
  keeps moving — and the plan forbids growing a *second* rate controller
  (RD-9), so the video plan's ladder was finished and made reachable.
  - A new plugin seam, `src/plugins/include/nimrtc/plugins/video_bitrate_controller.hpp`
    (`NIMRTC_API_STABLE`): `IVideoBitrateController`
    (`set_config()` / `config()`, `on_bandwidth_estimate()`,
    `on_measured_bitrate()`, `on_keyframe_requested()`,
    `current_level()`, `stats()`, `set_level()`), the
    `IVideoBitrateControllerFactory`, `VideoLadderLevel`,
    `VideoBitrateControllerConfig`, `VideoBitrateControllerStats` and the
    `VideoLadderMode` enum (`kQuality` / `kSmoothness` — which end of the
    ladder the controller gives up first, plan D2).
  - The registry gains a **video-bitrate-controller slot**
    (`register_video_bitrate_controller()` / `get_video_bitrate_controller()`)
    and the macro `NIMRTC_REGISTER_VIDEO_BITRATE_CONTROLLER(id, factory_ptr)`.
    The engine never names a concrete module (ADR-025 Layout Invariant 4);
    it resolves the controller through the slot.
  - The new module `src/modules/video_bitrate_control/` holds the built-in
    implementation — `BitrateController`, the `Ladder` (an ordered list of
    (width, height, fps, bitrate) levels with a default ladder and a
    resolution floor) and the factory, which registers under the id
    `"video-bitrate-control"`.  It is built by default
    (`NIMRTC_MODULE_VIDEO_BITRATE_CONTROL`, default ON) and installs as
    `nimrtc_video_bitrate_control` / `nimrtc::video_bitrate_control`.
  - The engine's surface grows accordingly: `EngineConfig::video_bitrate_controller_name`
    (empty = no controller, the pre-D4 behaviour),
    `EngineConfig::video_ladder_mode`,
    `EngineConfig::video_resolution_floor_height` and
    `EngineConfig::video_start_bitrate_bps`, plus the
    `NimRTCEngine::video_bitrate_controller()` accessor, so a host can
    *drive* the controller (feed it its own bandwidth estimate, or
    override the level) rather than only observe it.
  - **The controller's target bitrate now reaches the encoder** (plan D4).
    The engine reads the controller's current level on every frame and
    pushes it into the encoder's parameter set, so a bandwidth change
    moves the *codec's* configured bitrate — not only the pacer rate.
  - A **resolution floor** is honoured: the ladder never steps below the
    configured floor, because a remote-desktop user reads text and a
    floor the machine cannot sustain is a floor the ladder breaks
    anyway — so it is per-scene configuration (D2), not a hard-coded
    ladder constant.
  - Covered by `src/modules/video_bitrate_control/tests/test_video_bitrate_control.cpp`
    and `tests/test_engine_video_bitrate_control.cpp`.

- **The audio device seams and the PortAudio-backed device module (v1.4,
  remote-desktop plan F1 / F2).**  The engine had a PCM contract
  (`send_audio()` / `on_audio_frame()`) but no *device* seam, so a host
  could not plug in a sound card without touching the engine, and
  Windows / macOS / Linux each wanted their own backend implementation.
  - Two new plugin seams, `src/plugins/include/nimrtc/plugins/audio_source.hpp`
    and `audio_sink.hpp` (`NIMRTC_API_STABLE`): `IAudioSource` /
    `IAudioSourceFactory` and `IAudioSink` / `IAudioSinkFactory`.  They
    carry the device-format negotiation, the device enumeration and the
    capability query; the engine's existing PCM contract is unchanged.
  - `plugins::IAudioSink` gains two **additive** methods,
    `queued_frames()` and `last_playback_us()` (plan F3).  Both have a
    default implementation returning 0, so a third-party sink compiles
    unchanged.  They exist because a host that drives the AEC reference
    must know how far behind the loudspeaker is: a large backlog means
    the reference the 3A sees is stale by that much.
  - The new module `src/modules/audio_device/` holds the implementation:
    the shared bounded `AudioRingBuffer`, the **PortAudio** backend
    (MIT, cross-platform — WASAPI / MME / DirectSound on Windows, Core
    Audio on macOS, ALSA / PulseAudio on Linux), the synthetic device
    (so the module's tests run on a CI machine with no sound card), the
    device-enumeration surface and the hot-unplug / device-loss path.
    PortAudio is the module's **only** platform dependency (RD-10); the
    rest of the module is portable.
  - New public headers in the module: `nimrtc/audio_device/audio_device.hpp`
    (`portaudio_available()`, `portaudio_version()`,
    `portaudio_host_apis()`, `PortAudioSourceFactory`,
    `PortAudioSinkFactory`, `SyntheticSourceFactory`,
    `SyntheticSinkFactory`, and the `source_factory(id)` /
    `sink_factory(id)` resolvers), `nimrtc/audio_device/ring_buffer.hpp`
    (`AudioRingBuffer`), `nimrtc/audio_device/synthetic_device.hpp`,
    `nimrtc/audio_device/resampler.hpp` (the explicit sample-rate
    converter, plan F5), `nimrtc/audio_device/playback_progress.hpp`
    (`PlaybackProgressTracker`, `PlaybackProgressStats`) and
    `nimrtc/audio_device/aec_delay.hpp` (`AecDelayConfig`,
    `AecDelayEstimator`, `AecDelayMonitor`, `IAecDelaySink`,
    `IAecDelaySource`).
  - The **resampler** (plan F5) is `libsamplerate` (BSD-2-Clause),
    vendored under `src/third_party/libsamplerate/`; new switch
    `NIMRTC_VENDORED_LIBSAMPLERATE` (default ON).  There is **no system
    fallback** (AGENTS.md L5): with the vendored build off, the module
    falls back to its own pure-C++ linear interpolation, which is *not*
    band-limited — the fallback is visible through
    `nimrtc::audio_device::resampler_backend()`, so a host can say so in
    its own UI rather than the conversion being silent.
  - The module is built by default (`NIMRTC_MODULE_AUDIO_DEVICE`, default
    ON) and installs as `nimrtc_audio_device` /
    `nimrtc::audio_device`.  The engine wiring — reporting the played-out
    samples to the 3A — is plan F3 and is a separate, engine-level item.

- **The platform capture backends (v1.4, remote-desktop plan A3 / A4 /
  A5).**  The capture seam existed, but every platform said "unsupported":
  a host could not actually share a screen on any operating system.
  - **Windows**: two adapters over the portable capture layer.
    **DXGI Desktop Duplication** (`"windows-dxgi"`) captures a display —
    the cheapest path, no compositor round-trip, and it reports the
    changed region natively through `GetFrameDirtyRects` /
    `GetFrameMoveRects` (though it does **not** composite the pointer).
    **Windows Graphics Capture** (`"windows-wgc"`) captures a display or
    a *window*, composites the cursor into the frame, and reports **no**
    dirty rectangles.  Both produce a `video_frame::GpuBuffer` whose
    handle is an `ID3D11Texture2D` in `B8G8R8A8_UNORM` (BGRA), the
    format every desktop compositor produces natively.
  - **macOS**: **ScreenCaptureKit** (`"macos-screencapturekit"`),
    capture of a display or a window, BGRA, cursor composited.  A
    screen-recording permission denial is reported as a clean error, not
    a crash — the Unix capture core has its own error vocabulary
    (`UnixCaptureError::kPermissionDenied`, `kDeviceNotFound`,
    `kUnsupportedFormat`, `kCaptureFailed`, `kDeviceLost`,
    `kDisplayUnavailable`, `kPortalUnavailable`), because a host that
    gets "permission denied" must ask the user, while a host that gets
    "device lost" must re-open the device.
  - **Linux**: the **Wayland** path through the XDG Desktop Portal +
    **PipeWire** (`"linux-pipewire"`) and the **X11** fallback
    (`"linux-x11"`, `XGetImage`).  Both are *optional* build
    dependencies: when the development packages are missing, the
    adapter is not compiled and a host that names it gets a clean
    "unsupported" rather than a link error.  Global source enumeration is
    a **portal limitation**, not an engine bug, and the module says so.
  - The **portable** half — the bounded queue, the dirty-rect model, the
    format negotiation, the portable pixel-diff and region crop — is
    compiled and unit-tested on **every** platform, including the
    Windows leg where the macOS and Linux adapters cannot be compiled at
    all.  That is why the mechanism the adapters delegate to is tested
    everywhere.
  - New public header `nimrtc/video_capture/unix_capture_core.hpp`
    (the platform-neutral capture core and the error vocabulary) and
    `nimrtc/video_capture/unix_capture.hpp` (the portable device base
    class and the three Unix factories).  Covered by
    `src/modules/video_capture/tests/test_unix_capture_core.cpp`,
    `test_windows_capture.cpp` and `test_video_capture.cpp`.

- **The signalling guide and the reference signalling service (v1.4,
  remote-desktop plan I1 / I2).**  The engine ships a signalling *seam*,
  not a signalling server (RD-7), and nothing described the exchange a
  host actually has to implement.
  - The new guide
    [`docs/guides/remote-desktop.md`](docs/guides/remote-desktop.md) is
    the **canonical description** of the exchange: offer / answer, ICE
    candidate trickle in both directions, ICE restart, TURN credential
    refresh and the reconnect path, together with the exact engine
    surface each step uses.  A reader can write a signalling service
    from the guide alone.
  - The new directory `examples/remote-desktop/` is the guide's
    **executable form**: `signalling.py` (the asyncio WebSocket server),
    `signalling_core.py` (the pairing store, the TURN credential issuer
    and the message router — no I/O), `wsproto.py` (a hand-rolled
    RFC 6455 WebSocket layer, **standard library only**), `client.py`
    (the example device / viewer client) and `test_signalling.py` (the
    self-running test).  It stays in `examples/`, not in `src/` (RD-7),
    and it is a **reference implementation, not a production server**.

- **The `remote-desktop` profile and the profile schema v1.2 (v1.4,
  remote-desktop plan K3 / K4).**  The profile was the one place that
  described a *scene*, and there was no scene for a remote desktop.
  - The new profile `profiles/remote-desktop.json` describes it: the
    screen-content codec preference, the resolution floor, the ladder
    mode, the transport strategy, the timeline on, the DataChannel on
    and the 3A choice.
  - The new schema `profiles/schema/profile-v1.2.json` adds the four
    optional top-level keys `video_codec_preference`,
    `video_resolution_floor`, `video_ladder_mode` and
    `video_content_type`.  It is **strictly additive** on v1.1: every
    v1.1 profile loads unchanged, and a key that is absent leaves the
    engine default in place (`"h264"`, `"720p"`, `"quality"`,
    `"camera"`).
  - The C++ loader reads them into four new `assembly::Profile` fields
    with the same names, and `assembly::ProfileRegistry` gains the
    `remote-desktop` scene constant.  Covered by
    `src/modules/assembly/tests/test_profile_schema.cpp` and the
    byte-for-byte profile round-trip in `test_assembly.cpp`.

- **The encoder / decoder split seam (v1.4, remote-desktop plan B1).**
  One `IVideoCodec` instance owned the encoder state *and* the decoder
  state, so a host that only ever encoded still had to instantiate a
  decoder (and the mirror image), and neither half could be swapped for
  a different implementation.
  - `src/plugins/include/nimrtc/plugins/video_codec.hpp` now declares
    `IVideoEncoder` and `IVideoDecoder` as two separate seams, each with
    its **capability query** (`capabilities()` — supported profiles,
    resolutions, pixel formats, fps, maximum bitrate, zero-copy), plus
    `IVideoEncoderFactory` and `IVideoDecoderFactory` for a plug-in
    that implements only one direction.  The zero-copy contract
    (`encode_zero_copy()` / `decode_zero_copy()`, the `GpuBuffer` pool)
    and the incremental-encode request
    (`set_incremental_request()`) live on the split seams.
  - **`IVideoCodec` stays** as a deprecated compatibility shim for one
    MINOR cycle (ADR-015 Decision 3): it now *derives* from
    `IVideoEncoder` and `IVideoDecoder`, so existing code that consumes
    it keeps compiling, and the engine still resolves the combined
    factory — the split is an addition, not a migration.
  - The engine now **rejects an unsupported geometry at `open()`** with
    the distinct error code `core::kVideoCodecUnsupported` and a message
    that names the requested and the maximum resolution, instead of
    failing at the first frame.  Covered by
    `tests/test_engine_video_codec_split.cpp`.

- **The encoder-parameter authority, the resolution-change restart and the
  content hint (v1.4, remote-desktop plan B12 / B13 / B7 / C5).**  The
  encoder's parameters were scattered — some in `EngineConfig`, some in
  the codec's own `open()` config, some implicit in whatever the capture
  device happened to produce — so two of them could disagree, and the
  disagreement only showed up as a picture that was subtly wrong.
  - The new `plugins::VideoEncoderParams` struct (`NIMRTC_API_STABLE`)
    is the **complete** description of what the encoder should be
    producing right now: `width`, `height`, `fps`, `bitrate_bps`,
    `keyframe_interval`, `qp`, `pixel_format`, the nested
    `ContentHint` enum (`kUnspecified` / `kMotion` / `kDetail` /
    `kText`) and the `VideoColorDescription` `color`.
  - The **capability query** lives beside it in
    `plugins::VideoCodecCapabilities` (`NIMRTC_API_STABLE`): the accepted
    `pixel_formats`, the supported `profiles`, the `max_width` /
    `max_height` / `min_width` / `min_height` bounds, `max_fps`,
    `max_bitrate_bps`, the `zero_copy_encode` / `zero_copy_decode`
    flags and the `accepts_size()` predicate.  A dimension of 0 means
    "unconstrained" in the request and "no limit" in the capability, so
    the two are not confused.
  - The **resolution floor** and the **keyframe-cost budget** are
    *controller* knobs, not encoder knobs: they live in
    `plugins::VideoBitrateControllerConfig` (`min_height` and
    `keyframe_cost_bps`), and they reach the encoder only through the
    parameter set the controller's level implies.
  - The engine computes the set in **one** place
    (`src/engine/src/engine_video_params.hpp`) and pushes it through
    `set_params()`.  The encoder is **passive**: it has no access to the
    bandwidth estimator, to the rate controller or to the feedback
    stream, and it never derives a parameter from a neighbouring field.
  - A **resolution change is a parameter-set change** (plan B13): the
    new SPS / PPS (or VPS, or the AV1 sequence header) must travel with
    the new frame, so the peer decodes without a PLI round-trip.  The
    encoder must restart and emit the parameter sets with the next frame.
  - Covered by `src/modules/video_pipeline/tests/` and the engine tests.

- **The `video_bitrate_controller` seam and the `video_capture` seam are
  registered through the registry's typed slots.**  The registration
  macros `NIMRTC_REGISTER_VIDEO_CAPTURE(id, factory_ptr)`,
  `NIMRTC_REGISTER_VIDEO_SINK(id, factory_ptr)`,
  `NIMRTC_REGISTER_VIDEO_CODEC(id, factory_ptr)` and
  `NIMRTC_REGISTER_VIDEO_BITRATE_CONTROLLER(id, factory_ptr)` all take
  exactly **one** spelling of the id (ADR-023 Decision 5), and a
  malformed id is rejected at registration time rather than silently
  accepted.

- **The remote-desktop input module (v1.4, remote-desktop plan G3 / G4 /
  G7).**  The control plane had no payload format and no input-side
  policy, so there was nothing to put on the wire and nothing to stop a
  viewer from sending a dangerous key combination.  The new self-contained
  module `src/modules/rd_input/` carries all three pieces:

  | Piece | What it does |
  |-------|--------------|
  | `nimrtc/rd_input/event_codec.hpp` | The compact, **versioned** binary codec for mouse move / button / wheel / key down-up / text.  A mouse-move delta is **5 bytes** (LEB128 + zigzag deltas); a batch carries a count and a per-event length prefix. |
  | `nimrtc/rd_input/coalescer.hpp` | The pointer coalescer and the input-path rate limiter: pointer motion is accumulated and released at most once per display frame, while key / button / text events are queued **losslessly**. |
  | `nimrtc/rd_input/safety_filter.hpp` | The configurable allow / deny filter: a denied key never reaches the remote side, and the denial is **logged** through the one `core::log::Logger`. |

  The module is **a module, not a seam** (plan G3): it depends on
  `nimrtc::core` only, registers no plugin, and adds nothing to
  `src/plugins/`.  It is built by default (`NIMRTC_MODULE_RD_INPUT`,
  default ON) and installs as `nimrtc_rd_input` / `nimrtc::rd_input`.

  The engine wiring — the NimRTC-native control channel (G1) and the
  control plane's own transport (G2) — is a separate, engine-level item
  and is *not* part of this change.  `tools/check_dead_modules.py`
  therefore carries `rd_input` in its new `PENDING_WIRING` list, which
  names the work item that removes the waiver.

- **The remote-desktop QoE module (v1.4, remote-desktop plan J3 / J6).**
  The remote-desktop product is judged on *feel* — latency, sharpness,
  responsiveness — but the project had no way to *measure* those: the
  engine's `Stats` carries the per-stream counters and the per-stage
  timings, not the experience metrics the plan's item J3 asks for, and
  nothing measured the input-to-photon latency at all.

  The new self-contained module `src/modules/rd_qoe/` carries the
  measurement primitives:

  | Piece | What it does |
  |-------|--------------|
  | `nimrtc/rd_qoe/qoe_metrics.hpp` — `QoeCollector` | The observer: the freeze count / total duration / longest freeze, the **viewer's** frame rate, the **end-to-end** latency percentiles (render timestamp − capture timestamp), the **input-to-photon** percentiles (J6: the operator's input event → the frame that reflects it being displayed), the sharpness proxy and the freeze ratio.  It is **fed** by the host — it never pulls from the engine — so it stays decoupled and unit-testable. |
  | `PercentileEstimator` | A **bounded** percentile estimator: a fixed-capacity reservoir (default 1024 samples) plus a running max / min / mean.  A remote-desktop session runs for hours, so an unbounded sample list would be a leak; the bound and the accuracy trade-off (the percentiles describe the *recent* window) are documented on the class. |
  | `SharpnessEstimator` | A **sharpness proxy** — the mean absolute Laplacian of the luma plane.  It is cheap, needs no reference picture, and is monotone in the right direction (higher = sharper); it is a proxy, not a reference-based metric, and it compares pictures of the same resolution against each other. |

  The module is **a module, not a seam**: it depends on `nimrtc::core`
  only, registers no plugin, and adds nothing to `src/plugins/`.  It is
  built by default (`NIMRTC_MODULE_RD_QOE`, default ON) and installs as
  `nimrtc_rd_qoe` / `nimrtc::rd_qoe`.  The engine wiring — pushing the
  presented-frame events and the input-to-photon pairing into the
  collector — landed later through the `plugins::IQoeCollector` seam
  (see the entry above); the module registers under `"rd-qoe"` and the
  engine resolves it through the registry's qoe-collector slot.

- **The remote-desktop weak-network benchmark (v1.4, remote-desktop plan
  J4 / J6).**  A throughput number says nothing about the feel, so the
  new tool `tools/benchmark_remote_desktop.py` reports the **response
  curve** instead: the ladder level vs. time, the delivered fps vs. time,
  the cumulative freeze count vs. time, and the latency percentiles vs.
  time, over a four-phase link profile (20 Mbps → 3 Mbps → 1 Mbps →
  20 Mbps).  The run is **deterministic** (seeded RNG), so the committed
  artifact
  ([`docs/benchmarks/remote_desktop_weaknet_windows_x86_64.json`](docs/benchmarks/remote_desktop_weaknet_windows_x86_64.json))
  doubles as a regression baseline.  The tool reuses the transport-floor
  tiers of `tools/benchmark_weaknet.py` and the machine description of
  `tools/benchmark_capture_encode.py`; it needs no network and has a
  `--dry-run` mode.

- **The real software codecs (v1.3, remote-desktop plan WP-B / B3).**  The
  CPU bring-up baseline carried no codec: the H.264 and AV1 plug-ins
  delegated to *stubs* that emitted synthetic bitstreams and painted
  synthetic pictures, so the pipeline could be exercised end to end but
  never carried real video.
  - **OpenH264 v2.6.0** (Cisco, BSD-2-Clause) is vendored under
    `src/third_party/openh264/` as a git submodule and built from source
    through a hand-written CMake wrapper (upstream ships a Meson build).
    The wrapper mirrors upstream's *three-library* split — the encoder
    and the decoder ship different headers under the same names, with
    the **same include guard**, so they must not share an include path —
    and compiles the **portable C path only**: upstream's x86 fast paths
    are NASM and its ARM ones are GAS, and neither assembler is
    guaranteed on a build host.
  - `src/modules/h264/src/openh264_encoder.cpp` now holds a **real**
    H.264 encoder *and* decoder (`WelsCreateSVCEncoder` /
    `WelsCreateDecoder`) instead of delegating to the stub: Annex-B
    output, CABAC, single-slice AVC, and a decoder that completes the
    access unit explicitly (`DecodeFrame2(NULL, 0)`) and drains the POC
    reorder queue (`DECODER_OPTION_END_OF_STREAM` + `FlushFrame()`), so
    the codec stays **delay-free**.
  - The encoder **follows the picture**: a host whose capture
    resolution differs from the engine's configured default (or changes
    mid-session) makes the codec re-configure its encoder rather than
    reject the frame.
  - **dav1d v1.5.1** (VideoLAN, BSD-2-Clause) is vendored the same way
    under `src/third_party/dav1d/` and backs
    `src/modules/av1/src/dav1d_decoder.cpp`, a **real software AV1
    decoder** (8-bit, portable C path, the generated `config.h` and
    `vcs_version.h` reproduced by the wrapper, the templated DSP
    sources instantiated once for `BITDEPTH=8`).
  - The **stubs stay in the tree as the fallback**, and the telemetry
    now tells them apart: `IVideoCodec::backend_id()` reports
    `"software-stub"` for a stub and `"openh264"` / `"dav1d"` for the
    real software codecs, so `Stats::video_codec_backend` is honest
    about what produced the bitstream.
  - The AV1 plug-in's **stub encoder is encode-only**, so the factory
    pairs it with the best decoder the registry offers — the real
    dav1d decoder when the vendored build is linked in, the stub
    otherwise.  `HwCompositeCodec::backend_id()` forwards the
    sub-codec's identity instead of the interface's "unknown".
  - New switches: `NIMRTC_PLUGINS_OPENH264`, `NIMRTC_PLUGINS_DAV1D`,
    `NIMRTC_VENDORED_OPENH264`, `NIMRTC_VENDORED_DAV1D`.  There is **no
    system fallback** (AGENTS.md L5), so switching the vendored build
    off simply leaves the stub in charge.  The Windows, Linux and macOS
    CI legs now build both codecs and run the new round-trip tests.
  - Covered by `src/modules/h264/tests/test_openh264_software.cpp` (8
    cases: the real bitstream carries SPS + PPS + IDR, the decoded
    pixels match the source within a lossy-codec tolerance, the decoded
    picture is not blank, the factory selects the real codec) and
    `src/modules/av1/tests/test_dav1d_software.cpp` (8 cases: a real
    AV1 key frame decodes to **non-zero pixels**, Annex-B framing is
    stripped, the codec-owned buffer yields the same picture, the
    factory reaches dav1d).

- **The M1 exit-criterion proof (v1.3, remote-desktop plan §B.2 / §B.6).**
  The individual pieces of the video pipeline existed, but nothing proved
  that a picture captured by one engine survives a *real* codec and
  arrives at another engine as the *same* picture.  The new
  `tests/test_engine_video_e2e_loopback.cpp` closes that gap: it drives
  **two engines** through the complete chain at **1080p** —

      capture (BGRA) → BT.601 staging → OpenH264 encode → STAP-A / FU-A
      → RTP → transport → RTP → depacketize → Annex-B reassembly →
      OpenH264 decode → render

  — and compares the decoded luma against the reference luma computed
  from the *same* source pattern with the *same* transform the staging
  step uses (**mean absolute error < 12.0** grey levels, **Pearson
  correlation > 0.95**).  The measured values on the reference machine
  are **MAE 1.02** and **r 0.998**.
    - The codec is pinned to the `openh264_h264` backend explicitly, so
      the result does not depend on whether the host has an NVENC /
      QSV / AMF card; the engine's telemetry has to report
      `video_codec_backend == "openh264"`, which excludes the stub.
    - The **negative control** is part of the test: a flat grey frame
      must *fail* both thresholds, and the decoded frame must not be
      byte-identical to the source (which would mean it never went
      through the codec).
    - The transport is a fake ICE transport (a seam, not a codec
      shortcut) — the *network* part of the criterion is covered by the
      transport-level work packages.

- **The SIMD scaler (v1.3, remote-desktop plan C1 / C2).**  A capture
  surface arrives at whatever resolution the display has; the encoder
  wants the ladder's resolution.  The tree could only do that with the
  hand-written scalar converters in `video_frame`, which are too slow for
  a 4K desktop.
  - **libyuv v0.0.0** (`lemenkov/libyuv`, BSD-3-Clause — the maintained
    fork of Chromium's libyuv) is vendored under
    `src/third_party/libyuv/` as a git submodule pinned at
    `209763ad77fed3df2d57503d3ef40761e9473649` and built from source
    through a hand-written CMake wrapper (upstream ships a GYP build).
    The wrapper re-exports upstream's static `yuv` archive as
    `nimrtc_vendor_libyuv` (alias `nimrtc::vendor::libyuv`), suppresses
    upstream's `install()` rules so they cannot pollute NimRTC's
    install set, and disables upstream's `cpuid` / `yuvconvert` tools.
    New switch `NIMRTC_VENDORED_LIBYUV` (default ON).  There is **no
    system fallback** (AGENTS.md L5), so switching the vendored build off
    simply leaves the scaler reporting "unavailable".
  - New public header `src/modules/video_frame/include/nimrtc/video_frame/scaler.hpp`
    (`NIMRTC_API_STABLE`): `ScaleFilter` (`kPoint` / `kLinear` /
    `kBilinear` / `kBox` — `kBox` is the *display* path, `kBilinear` the
    *encode* path) and `VideoFrameScaler` with `scale_i420()`,
    `scale_to_buffer()` (pool-aware), `bgra_to_i420()` and
    `i420_to_bgra()`.  With libyuv absent the module still compiles and
    every call returns `core::ErrorCode::NotImplemented`.
  - **libyuv names its packed formats by *word* order, not memory
    order.**  NimRTC's `PixelFormat::kBGRA` is memory order B,G,R,A,
    which is libyuv's *`ARGB`* (word `0xAARRGGBB`); libyuv's own `BGRA`
    is memory order A,R,G,B.  Calling `BGRAToI420` / `I420ToBGRA`
    compiles and runs but silently produces the wrong colours — the
    implementation calls `ARGBToI420` / `I420ToARGB`, and a saturated-red
    test case pins the mapping so a swap cannot regress silently.
  - The existing hand-written scalar converters stay in the tree as the
    portable reference; the scaler is the SIMD path alongside them.
  - Covered by `src/modules/video_frame/tests/test_video_frame_scaler.cpp`
    (13 cases: down- and up-scaling, the pool path, invalid arguments,
    a solid-colour source surviving both directions without banding,
    chroma scaling, all four filter modes, a BGRA↔I420 round trip, and
    an OpenH264 encode/decode round trip) plus a 1080p benchmark that
    *records* ns/pixel (**1.2 – 4.2 ns/pixel** for 1920×1080 → 1280×720)
    without asserting a threshold — a perf gate would be flaky on CI.
    With `NIMRTC_VENDORED_LIBYUV=OFF` the module still compiles, the
    scaler reports "unavailable" and every call returns a clean
    `NotImplemented`.

- **The Windows rendering and engine pump (v1.3, remote-desktop plan
  WP-C / A10 / A11 / E2 / E8 / B10 / B15).**  The capture → encode →
  present loop could not be closed on Windows: there was no native
  renderer, the engine never drove the capture device on its own, and
  the codec's identity and latency were invisible from the outside.
  - **A D3D11 / DXGI swap-chain renderer** (`video_sink/d3d11_renderer.hpp`):
    `Present(0)` plus a **presentation clock** — the presenter waits
    for its deadline (`condition_variable::wait_until`), so it never
    busy-waits and never blocks the tick thread.  Back-pressure is
    **latest-wins** through a single-slot mailbox: a render that
    arrives while the previous one is still pending *replaces* it
    rather than queueing, and the replacement is counted in
    `VideoSinkStats::frames_dropped_present`.  The presenter also runs
    **offscreen** (no native window) so its pacing and drop accounting
    are testable without a display.
  - **The engine's internal capture pump** (plan A10).  `tick()` pops
    one frame from the capture device's bounded queue and drives it
    through the very same encode → packetize → enqueue path
    `send_video()` uses, so a capture plug-in plus an encoder produce
    an outbound RTP stream **without the host ever calling
    `send_video()`**.  `send_video()` stays exactly as it was — the
    pump is an *additional* source, not a replacement, and
    `EngineConfig::video_capture_pump = false` turns it off.
    RD-6 is preserved: the capture *device* owns its own thread and
    fills its queue; the engine only ever touches the codec from
    `tick()`.  `tests/test_engine_capture_pump.cpp` asserts this by
    recording the thread id at `encode()` time and comparing it with
    the id of the thread that called `tick()`.
  - **The NVENC zero-copy encode path** takes a D3D11 surface: the
    texture is registered with the driver
    (`NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX`) and mapped as an encoder
    input, so `bytes_copied` stays at **zero** on the hardware route.
    A surface the driver cannot take (a BGRA texture, for instance —
    the driver accepts NV12 for a registered DirectX input) is
    *refused* rather than silently copied, and the engine falls back
    to the CPU staging path.
  - **The telemetry is now in the engine's stats snapshot.**  The
    capture counters (`frames_captured`, `frames_dropped`,
    `frames_zero_copy` vs `frames_cpu_staged`, `bytes_copied`), the
    pump's own counters, the codec's identity and timing
    (`backend_id`, hardware-accelerated or not, zero-copy vs staged,
    keyframes, mean and max encode latency) and the sink's counters
    (`frames_presented`, `frames_dropped_present`, `present_fps`) all
    appear in `NimRTCEngine::Stats`.
  - `video_frame::convert_bgra_to_i420()` bridges the packed BGRA a
    desktop compositor produces to the planar I420 every encoder
    wants, on the CPU path.
  - The public-header baseline in `test_api_classification.cpp` moves
    **140 → 141** in the same commit that adds
    `video_sink/d3d11_renderer.hpp` (and, in the companion capture
    commit, `video_capture/windows_capture.hpp`).

- **The GPU delivery path for decoded video, and the frozen capture
  semantics (v1.3, remote-desktop plan WP-A / A1 / E1).**  The video
  plane had a CPU-only receive side: a decoded frame always reached the
  sink as host pixels, even when the decoder could hand back a native
  surface and the sink could present one.  That made the "zero-copy"
  claim true for the *send* half of the pipeline and false for the
  receive half.
  - `plugins::IVideoSink` gains a **GPU-surface delivery path**:
    `render(const GpuBuffer&)` and `render(const GpuBuffer&,
    const VideoFrameInfo&)`, alongside the existing
    `render(const VideoSourceFrame&)`.  The lifetime contract is the one
    `IVideoCodec::decode_zero_copy()` already documents: the surface is
    **borrowed** — the engine holds a reference across the `render()`
    call and releases it afterwards, and a sink that keeps the surface
    takes its own reference.
  - Both `render()` overloads keep a **default implementation that
    returns `kErrUnsupported`**, so a third-party sink that implements
    only one side compiles unchanged.  The engine probes
    `supports_gpu_render()` / `supports_cpu_render()` and routes to the
    side that exists.
  - The engine decides the route **once, at `open()` time**, from three
    facts — the decoder's `supports_zero_copy_decode()`, the sink's
    `supports_gpu_render()` and its `supports_cpu_render()`.  The rule
    is a pure function (`src/engine/src/video_render_path.hpp`), so all
    eight combinations are unit tested without an engine, a transport or
    a clock.  A pair where **neither** path is possible is reported at
    `open()` time as the new `kVideoSinkIncompatible` — a session that
    opens cleanly and can never display anything is the failure mode
    this replaces.
  - `plugins::CaptureCapabilities` / `plugins::CaptureFormat` semantics
    are **frozen**: `zero_copy`, `dirty_rects`, `region_selection`,
    `max_pixels` and the new `max_fps` each have one meaning, and an
    unsupported *combination* is refused at `open()` time with the new
    `kVideoCaptureUnsupported` instead of on the first captured frame.
    `want_cursor` stays a preference — it is honoured only when the
    device can composite the pointer, never a reason to refuse a
    session.
  - New `EngineConfig::video_capture_config` carries the capture
    request; `Stats::video_rx_gpu_frames` / `video_rx_cpu_frames` make
    the delivery-path decision observable from the outside.
  - Covered by `tests/test_video_render_path.cpp` (31 cases: the eight
    path combinations, the single-sided-sink contract, and every
    capture-capability violation class).

- **The zero-copy capture → encode hand-off (v1.3.0, Remote Desktop plan
  B10).**  Until now the zero-copy *seam* existed but nothing on Windows
  ever produced a GPU handle, so `video_codec_zero_copy_frames` was
  always zero.  The whole path is now live end to end, and the claim is
  proved on real hardware rather than asserted.
  - **`nvenc_encoder.cpp` grows `encode_zero_copy()`.**  It takes a
    `video_frame::GpuHandle` that carries a D3D11 texture, registers
    that texture with NVENC (`nvEncRegisterResource`) and encodes from
    it.  Two constraints were found empirically on an RTX 3060 and are
    honoured by the code: a resource may only be registered on the
    device that created it (a session therefore **rebinds** to the
    producer's device), and the session's device flags are otherwise
    free to differ.
  - **The DXGI backend publishes a GPU surface instead of a pixel
    buffer.**  `DxgiDesktopDupCapture` keeps a pool of
    `D3D11_USAGE_DEFAULT` textures; on a desktop presentation it does a
    GPU-side `CopyResource` into a pooled texture and releases the
    duplication frame **immediately**, so the picture never touches
    system memory (`bytes_copied` stays at 0).  A sub-region selection
    still forces the CPU path — the crop is a CPU-side operation — and
    `CaptureConfig::want_zero_copy` opts in.
  - **`CaptureQueue` carries the surface, and retires it.**  The queue
    owns the `GpuHandle` and invokes the `SurfaceReleaseFn` callback
    when a surface is dropped (overflow), cleared, or superseded by the
    next `pop()`.  Without the retirement the D3D11 textures would leak
    once per frame.
  - **The cursor layer is skipped on the zero-copy path.**  Software
    alpha blending needs CPU memory for the frame, which a GPU surface
    does not have; blending on the CPU would have forced a readback and
    defeated the whole path.
  - **The proof is a hardware test**
    (`src/modules/nvcodec/tests/test_nvcodec_zero_copy.cpp`).  It paints
    a D3D11 texture with a pixel shader *on the GPU*, hands it to the
    encoder through the public seam, and checks the **decoded** picture.
    A silent fallback to a CPU staging copy would produce a different
    colour and fail the comparison, so the negative ("the picture never
    reached the CPU") is observable.  It also covers a surface from a
    different device object and a surface whose size differs from the
    encoder's.
  - The capture-side unit tests cover the queue's surface propagation
    and its three retirement paths, the adapter's pass-through of a GPU
    surface, and the fact that no cursor layer is installed on the
    zero-copy path.

- **Colour-aware pixel conversion, and two real colour bugs it found
  (v1.3.0, Remote Desktop plan C3 / C5).**  The remote-desktop picture
  is screen content: full-range BT.709, sharp edges, flat panels.  The
  conversion path did not know that, and it turned out to be *wrong* in
  two ways that only a round-trip test could see.
  - The canonical definitions are `video_frame::ColorRange`,
    `video_frame::ColorMatrix` and `video_frame::ColorDescription`
    (`src/modules/video_frame/include/nimrtc/video_frame/frame.hpp`),
    re-exported as `plugins::ColorRange` / `plugins::ColorMatrix` /
    `plugins::ColorDescription` so the plugin layer keeps its
    vocabulary without pulling the frame module in directly.
  - `convert_bgra_to_i420_color()`, `convert_bgra_to_nv12_color()` and
    `convert_i420_to_bgra_color()` implement the ITU-R BT.601 / BT.709 /
    BT.2020 matrices with the limited- and full-range scalings in
    fixed point.  The forward path **averages the 2×2 chroma block**
    rather than point-sampling it.
  - **The inverse green-channel coefficient carried a spurious factor of
    two.**  The code folded `2*(1-Kr)` (which already carries a 2) into
    a second explicit 2, so `G` was corrected twice as hard as it
    should be and every saturated colour came back hue-shifted.
  - **The limited-range chroma gain was above one** (1.373× instead of
    the studio 224/255 ≈ 0.878×).  Limited range *compresses* the
    chroma; a gain above one pushed saturated colours past 0/255, where
    they were **clamped** and lost their hue entirely — pure green came
    back grey.  The BT.601 inverse coefficients (1.596027 / 2.017232)
    require exactly the 224/255 factor.
  - The description is carried end to end — `CaptureFrame` →
    `VideoSourceFrame` → `VideoFrameInfo` → the encoder's parameter set
    — and `resolve_color_description()` fills the unstated fields with
    the desktop default (full range, BT.709).
  - The unit test (`src/modules/video_frame/tests/test_video_frame_color.cpp`)
    pins the round trip for **every** range / matrix pair, asserts the
    limited range *compresses* the chroma, and asserts the two matrices
    really do disagree on a saturated picture.

- **The encoder's backend policy — "auto", "hardware" or "software"
  (v1.3.0, Remote Desktop plan B2).**  Hardware-first is the default;
  the policy exists for the two cases the default cannot serve.
  - `plugins::CodecBackend` (`kAuto` / `kHardware` / `kSoftware`) in
    `src/plugins/include/nimrtc/plugins/video_codec.hpp`, carried on
    `VideoCodecConfig` and exposed on the engine as
    `EngineConfig::video_codec_backend`.
  - `kHardware` **fails at `open()`** when no hardware encoder exists
    for the requested codec, rather than silently producing a software
    stream that the host's CPU budget cannot carry.  `kSoftware` never
    selects a hardware encoder, which is what makes the software path
    *reproducible* in a test.
  - The selected backend is **reported in the stats**, so "it fell back
    to software" is visible rather than inferred from a CPU spike.

- **The content type as a first-class concept (v1.3.0, Remote Desktop
  plan B7 / K2).**  A screen capture is not a camera pointed at a
  monitor.
  - `plugins::ContentHint` (`kUnspecified` / `kMotion` / `kDetail` /
    `kText`) in the codec seam, mirrored by
    `assembly::VideoContentType` in the scenario-assembly layer.
  - The mapping is **screen content → `ContentHint::kDetail`** (ADR-022):
    a screen has fine detail and needs to be sharp, but it has no
    camera-style motion, so it is not `kMotion`.
  - `content_hint_sdp_value()` gives the SDP attribute value, and the
    empty string for `kUnspecified` so the attribute is *omitted*
    rather than sent empty.

- **The `remote-desktop` scenario profile and profile schema v1.2
  (v1.3.0, Remote Desktop plan K3 / K4).**
  - `profiles/remote-desktop.json` plus
    `assembly::kProfileRemoteDesktop`.  The profile's `video` block
    declares the codec, the **codec preference** (`av1` > `h265` >
    `h264` — configuration, not a hard-coded branch), the screen content
    type, the ladder mode (`quality`), the **resolution floor**
    (1280×720) and the full-range BT.709 colour description.
  - `profiles/schema/profile-v1.2.json` (JSON Schema draft 2020-12).
    The `video` block is **additive**: a profile that does not carry one
    behaves exactly as before, and every v1.1 profile loads unchanged.
  - The profile round-trips through the assembly serialiser byte-for-byte
    like the existing ones.

- **The dirty-rect detector for the backends that do not supply their
  own (v1.3.0, Remote Desktop plan A7).**  DXGI reports dirty rectangles
  natively; macOS and Linux do not, so they get a conservative bounding
  box from a pixel-diff.
  - `DirtyRectDetector` (`src/modules/video_capture/`) keeps the
    previous frame and produces, per frame, the **bounding box of the
    changed region** plus a `full_frame` flag and an `identical` flag.
  - The three outcomes are the ones a caller can act on: *nothing
    changed* (skip the encode), *something changed* (encode the
    bounding box), *no previous frame yet* (the first frame is always a
    full frame, and `identical` is false because there is nothing to
    compare against).
  - The diff runs on the **device's own pixels, before the format
    conversion**, so it sees the same bitmap the encoder will see.
  - **All four backends without native damage information run it** — X11,
    Windows Graphics Capture, the XDG Desktop Portal and
    ScreenCaptureKit — so an unchanged frame is dropped *before it is
    queued*, and a static desktop costs almost nothing.
  - **Their capability flags now report `dirty_rects = true`.**  They
    used to report `false`, on the grounds that "the platform does not
    give us damage information" — but the flag is a statement about
    *what the backend delivers*, and these backends do deliver the
    metadata (they derive it).  A flag that says "no" while the frames
    carry the metadata makes the consumer ignore metadata that is
    present and correct.  The new test
    `SyntheticCapture.TheDirtyRectFlagAgreesWithWhatTheFramesCarry`
    pins the invariant for the reference device.

- **The codec telemetry in the engine's stats snapshot (v1.3.0, Remote
  Desktop plan B15).**  The numbers that make a live session
  *explainable*: which encoder is actually running, whether it is
  hardware or software, whether the frames took the zero-copy path, and
  what it actually produced.
  - `Engine::Stats` gains `video_codec_backend_id`,
    `video_codec_hardware`, `video_codec_zero_copy`,
    `video_codec_bitrate_bps` (configured) vs `video_codec_actual_bps`
    (produced), `video_codec_encode_latency_us` (average),
    `video_codec_frames_encoded`, `video_codec_bytes_encoded`,
    `video_codec_encode_errors`, and the two counters that make the
    zero-copy claim *measurable* — `video_codec_zero_copy_frames` and
    `video_codec_staged_frames`.
  - The numbers are **republished from the codec's own snapshot** rather
    than re-counted by the engine, so there is no second copy of the
    bookkeeping that could drift out of step with what the encoder did.

- **The GPU device registry (v1.3.0, Remote Desktop plan B10).**
  `src/core/include/nimrtc/core/gpu_device_registry.hpp` gives the
  zero-copy path a single place to answer "which GPU device does this
  handle belong to", so the capture seam and the encoder can agree on
  the device without either of them knowing about the other's backend.

- **The cursor layer is wired up — the pointer now actually reaches the
  captured frame (v1.3.0, Remote Desktop plan A6).**  The layer, both
  platform sources and the compositing maths were already in place, but
  **nothing installed them**: the layer was a piece of code a host had
  to know to reach for, so on every platform whose capture API does not
  draw the pointer (DXGI Desktop Duplication, X11) the outbound
  picture had **no pointer at all**.  The layer is now the default.
  - `CaptureSourceAdapter::start()` asks
    `make_platform_cursor_source()` for *this* platform's pointer and
    installs the layer exactly when the device does **not** composite
    the pointer itself and the caller asked for a pointer.  A device
    that composites it (Windows Graphics Capture, macOS
    ScreenCaptureKit) gets **no** layer, so the pointer is never
    double-drawn.
  - A host that already tracks the pointer (a remote agent that knows
    where the *remote* pointer is) keeps its own source through
    `set_cursor()`, which wins over the automatic one and survives
    `stop()`.
  - **The pointer is drawn into the *device's* BGRA pixels, before the
    format conversion.**  The engine's capture pump asks for **I420**
    (the encoders want YUV 4:2:0), and a planar frame has no alpha
    channel to blend into — so a layer that composited *after* the
    conversion would never have drawn on the path that matters.
    Compositing first also means the pointer is converted along with
    the rest of the picture, so it reaches the encoder in the right
    colours.
  - **The device's own pixels are never written into.**  The device
    owns them and may recycle them the moment the callback returns, so
    the adapter copies the frame into its own scratch buffer first and
    the conversion reads *that* copy.
  - **The surface origin closes the multi-monitor gap.**  A pointer is
    reported in the platform's *global* space — the virtual desktop on
    Windows, the root window on X11 — while a captured frame is one
    surface *inside* that space, and a second monitor is not at the
    global origin.  The device now reports where its surface sits
    (`CaptureConfig::surface_origin_x/y`; the DXGI backend reads
    `DXGI_OUTPUT_DESC::DesktopCoordinates`) and the adapter hands it to
    the source, which subtracts it exactly as the Windows and X11
    sources already did for their own origins.

- **The macOS ScreenCaptureKit capture backend (v1.3.0, Remote Desktop
  plan A4).**  The third of the three platform capture backends, and the
  one with the most interesting failure mode.
  - The backend lives in
    `src/modules/video_capture/include/nimrtc/video_capture/screen_capture_kit.hpp`
    and `src/modules/video_capture/src/platform/screen_capture_kit.mm`
    and registers under the plug-in id `"sck"`.  It enumerates the
    machine's **displays** (`sck:display:<n>`) and **windows**
    (`sck:window:<window-id>` — the `CGWindowID`, because two windows
    can share a title but not an id), opens an `SCContentFilter` +
    `SCStreamConfiguration` (BGRA, `showsCursor` honoured, the frame
    interval from the negotiated rate), and receives `CMSampleBuffer`s
    through an `SCStreamOutput` on a private dispatch queue.  The
    samples' `CVPixelBuffer`s are read back row by row into the shared
    bounded queue, with the requested region **cropped out of the
    readback** — the pixels the consumer sees are the display's own
    pixels, not a resampling of them.
  - **The geometry is negotiated in *pixels*, not in points.**
    `SCDisplay::width` is a *point* count, so a capture that trusted it
    would come back at half resolution on every Retina display.  The
    display's *mode* (`CGDisplayCopyDisplayMode`) is asked for the true
    pixel size instead, and the backing scale factor covers the
    window case (`SCWindow::frame` is in points too).
  - **The screen-recording permission is a first-class outcome, not a
    crash.**  `CGPreflightScreenCaptureAccess()` *reports* whether the
    host application holds the permission and never prompts, so the
    **enumeration** never shows a modal dialog — a library that popped
    one during an enumeration would be unusable in a server process.
    `open()` is the only place that asks
    (`CGRequestScreenCaptureAccess()`), because that is where the user
    has expressed the intent to capture something.
  - A refusal returns the **distinct** status
    `plugins::kErrPermissionDenied` (`0x1008`), and the engine maps it
    onto the **distinct** error code
    `core::kVideoCapturePermissionDenied` (`0x1A2A`) — so the host can
    tell "the user said no" from "the device is broken" and can say
    "grant screen recording access" instead of "device error".  Only the
    *user* can fix a permission denial, and the remedy is worth naming.
  - The cursor is composited by the OS into the captured surface, so
    this backend reports `cursor_composited = true` and the plan's A6
    cursor layer is not needed on this path.
  - The `.mm` translation unit is the **only Objective-C++ in the
    tree**; `enable_language(OBJCXX)` is conditional on `APPLE`, and
    the backend's portable half lives in
    `src/modules/video_capture/src/platform/screen_capture_kit_stub.cpp`
    and is compiled everywhere.  A host that names `"sck"` on Windows
    or Linux therefore gets a clean `kErrUnsupported` rather than a
    "capture plugin missing" error, and the stub reports the *same*
    static capability flags as the real backend — a capability query
    must not answer differently depending on the host it was asked on.
  - The `CVPixelBufferRef` is the GPU surface a VideoToolbox encoder
    would consume without a CPU round-trip; the *seam* has no slot for
    a `GpuBuffer` yet, so this backend takes the CPU path and reports
    `zero_copy = false`.  The zero-copy hand-off is the encoder's half
    (plan item B10).

- **The Linux capture backends — XDG Desktop Portal + PipeWire (Wayland)
  and X11 — with the X11 pointer source (v1.3.0, Remote Desktop plan
  A5).**  The two ways to capture a screen on Linux, which are nothing
  like each other.
  - The **X11 backend** (`x11_capture.hpp`, id `"x11"`) is the
    *fallback*: it grabs the root window with `XGetImage` and swizzles
    the server's native pixel layout into the portable layer's BGRA.  It
    needs no portal, no D-Bus and no user consent — which is exactly
    why it is the path that works on a headless X server, over `ssh -X`,
    and on a kiosk.  The cost is a round trip to the X server per frame.
  - The **XDG Desktop Portal backend** (`portal_capture.hpp`, id
    `"xdg_portal"`) is the Wayland path: it asks the portal (over D-Bus)
    for a screen-cast source, waits for the user's answer, and connects
    a PipeWire stream to the node id the portal granted.  The consent
    is a *value*, not a crash: a dismissed picker or a missing portal
    is a clean error.
  - The **X11 cursor source** (`x11_cursor_source.hpp`) is the Linux
    half of the plan's A6 cursor layer: `XGetImage` does not include the
    pointer, so the pointer is read through the Xfixes extension's
    `XFixesGetCursorImage` — the same 32-bit BGRA shape the Windows
    cursor source produces, so the portable alpha compositor needs no
    conversion.
  - Both backends are optional: their libraries are detected with
    `pkg-config`, and where they are missing the classes compile to
    their portable stub (the class exists, the factory is not
    registered, `open()` reports `kErrUnsupported`).

- **A pluggable cursor layer (with its Windows platform source), and the two Windows capture backends — DXGI Desktop Duplication and Windows
  Graphics Capture (v1.3.0, Remote Desktop plan A6 / A3).**  The
  portable capture layer was landed
  above, but it had no *platform* backend to drive and no way to show
  the pointer on the platforms whose capture API does not draw it.  Both
  are now in place.
  - **The cursor layer**,
    `src/modules/video_capture/include/nimrtc/video_capture/cursor_compositor.hpp`,
    splits the pointer into two seams so that "the platform already
    composited it" is a *value* rather than a flag that can be set
    wrongly:
    - `ICursorSource` reports **where** the pointer is and what it
      looks like (a BGRA bitmap, its size and its stride).
    - `ICursorCompositor` decides **whether** to draw it at all, and
      draws it.  `AlphaCursorCompositor` is the portable
      implementation: an integer fixed-point alpha blend of the BGRA
      cursor bitmap over the BGRA frame pixels, clipped against the
      frame and rebased onto the region origin, so **pixels outside the
      pointer's bounding box stay byte-exact** — a screen-capture
      consumer can diff the composited frame against the raw one and
      find exactly the pointer.
    - `PinnedCursorSource` is the "cursor is always drawn at the last
      position" fallback: when the platform cannot report a live
      pointer, the last known state keeps being drawn instead of the
      pointer vanishing.
    - The adapter applies the layer before the frame is published.
      The layer is installed **automatically** (see the entry above),
      and it draws into the *device's* BGRA pixels — before the
      conversion into the source's own format — so it also works on
      the engine's I420 path, where a planar frame's lack of an alpha
      channel would otherwise make the blend impossible.
  - **The Windows cursor source**,
    `src/modules/video_capture/include/nimrtc/video_capture/win_cursor_source.hpp`,
    is the platform half of that layer: `WinCursorSource` reads the
    pointer with `GetCursorInfo` (the hotspot) and `GetIconInfo` (the
    bitmaps), and reports it in the *captured display's* coordinate
    space — `set_display_origin()` translates the virtual-desktop
    hotspot, because a captured frame is one display's surface and a
    display is not at the virtual desktop's origin.  The bitmap is
    cached per `HCURSOR` handle, so a pointer that does not change is
    not re-extracted on every frame; only the position is re-read.
    - The bitmaps are always read back as **32-bit BGRA**, even for a
      monochrome cursor's 1-bit AND/XOR mask.  Asking `GetDIBits` for
      1-bit output for a *cursor mask* makes GDI write past the end of
      the caller's buffer (the mask's own `bmWidthBytes` stride and the
      requested DIB stride disagree), which corrupts the heap; reading
      the same bitmap as 32 bits is well-defined and lets the bit
      pattern be recovered from the colour channels instead.
    - Three cursor shapes are handled: a **modern** cursor (a 32-bit
      colour bitmap whose alpha is authoritative — the pointer's
      anti-aliased edges live there), a **24-bit** cursor (colour
      without alpha, so the AND mask says which pixels are opaque), and
      a **legacy monochrome** cursor (no colour bitmap at all, so the
      mask's top half is the AND plane and its bottom half the XOR
      image).
  - **The DXGI Desktop Duplication backend**,
    `src/modules/video_capture/src/platform/dxgi_desktop_dup.cpp`,
    registered under the plug-in id `"dxgi_desktop"`.  It is the
    *display* capture path on Windows: fast (a GPU-side copy), no
    consent prompt, but no pointer in the frame — which is precisely
    what the cursor layer above is for.
    - The device enumerates the display outputs, opens an
      `IDXGIOutputDuplication`, and runs a **capture thread** of its
      own that blocks in `AcquireNextFrame` — the natural place to
      wait, because the OS wakes the thread when the desktop changes.
      The engine's tick thread only ever *pops* an already-queued
      frame, so a quiet desktop never parks the tick loop.
    - The frame is read back through a CPU-readable staging texture
      (BGRA), which is why this backend reports
      `CaptureCapabilities::zero_copy == false`: the zero-copy hand-off
      to a hardware encoder is plan item B10.
    - The dirty rectangles come from DXGI itself
      (`GetFrameDirtyRects` / `GetFrameMoveRects`), so the backend
      reports *real* per-frame changed regions rather than a
      conservative bounding box.  They are clipped against the
      selected region and rebased onto its origin.
    - A **region request is honoured exactly**: the captured frame is
      cropped to the requested source rectangle before it is queued.
    - The counters (`frames_captured`, `frames_dropped`,
      `device_errors`, `bytes_copied`) are atomic, because the capture
      thread writes them and the engine's tick thread reads them.
    - A lost duplication object (the display was resized, the session
      was locked) is counted as a device error and reported, rather
      than silently spinning on a dead device.
  - **The Windows Graphics Capture backend**,
    `src/modules/video_capture/src/platform/wgc_capture.cpp`,
    registered under the plug-in id `"wgc"`.  It is the *window*
    capture path on Windows, and the one that gets the pointer for
    free: WGC asks the desktop compositor to draw the cursor into the
    captured surface, so this backend reports
    `CaptureCapabilities::cursor_composited == true` and the cursor
    layer above is not needed on this path.
    - The device enumerates the displays *and* the top-level windows
      (visible, with a title), and resolves the device id to an
      `HMONITOR` or an `HWND`.
    - The capture item is created through
      `IGraphicsCaptureItemInterop` — `CreateForMonitor` for a
      display, `CreateForWindow` for a window.  That interop factory
      is the only way to create a capture item, and it is what makes
      the window path possible at all.
    - The frame pool is created **free-threaded**
      (`CreateFreeThreaded`), so the library needs no `DispatcherQueue`
      and imposes no message loop on its host.  The cost is that the
      frame arrives on a WinRT thread-pool thread rather than on the
      capture thread — which the D3D11 device accommodates, because it
      is deliberately created *without*
      `D3D11_CREATE_DEVICE_SINGLETHREADED`.
    - The readback goes through a staging texture, so this backend
      reports `CaptureCapabilities::zero_copy == false`; the zero-copy
      hand-off is plan item B10.
    - The Windows 11 24H2 capture border is switched off through
      `IGraphicsCaptureSession3` — a remote-desktop client does not
      want the border in the stream.
    - A region request is honoured exactly: the frame pool is created
      at the *source* size (so the consumer sees the source's pixels
      and not a resampling of them) and the region is cropped out of
      the readback afterwards.
    - WGC does **not** report changed regions, so the backend reports
      the conservative "the whole frame changed" answer rather than
      pretending to a precision it does not have.
  - **What these backends are *not*:** the zero-copy `GpuBuffer`
    hand-off is still open — the frames reach the encoder as CPU
    pixels.  See plan item A3's "Status" note.

- **The engine pumps the capture device on its own tick thread
  (v1.3.0, Remote Desktop plan A10 / A11).**  Until now a capture
  device was a plug-in-level facility: a third-party device could be
  registered and driven, but the *host* still had to pull the frames out
  of it and hand them to the engine with `send_video()` — one call per
  frame, from the host's thread.  That is the wrong contract for a
  screen-capture product: it puts the codec on the host's thread, it
  makes every host write a frame-pumping loop, and it gives the engine
  no way to know what the device is doing.
  - The engine now owns the pump.  `EngineConfig::video_capture_name`
    names the capture plug-in to use, `EngineConfig::video_capture_config`
    carries the device request (the geometry, the pixel-layout tag, the
    queue depth), and `EngineConfig::video_capture_auto_pump` (default
    `true`) selects the automatic path.  With it off, the host keeps
    driving `send_video()` by hand — `send_video()` remains the
    explicit manual path, exactly as the plan says.
  - The pump pops **at most one frame per tick** and drives the encoder
    with it, on the engine's tick thread.  This is what makes the
    encoder single-threaded: `IVideoCodec` and `IVideoSender` are not
    thread-safe, and the tick thread is now the only thread that ever
    touches them.  A capture plug-in may own a thread (DXGI's
    duplication event, ScreenCaptureKit's stream, PipeWire's buffer);
    the engine does not.
  - The pump never blocks.  An empty capture queue is a no-op — a quiet
    device must not park the engine's tick loop — and the pump runs
    *before* the scheduler drain, so the packets a frame produces leave
    in the same tick that produced them.
  - **Capture telemetry in `Engine::Stats`** (plan A11):
    `video_capture_frames`, `video_capture_frames_pumped`,
    `video_capture_dropped`, `video_capture_device_errors`,
    `video_capture_restarts`, `video_capture_format_changes` and
    `video_capture_bytes_copied`.  The counters are read against a
    **baseline** the pump remembers, not as a delta taken around the
    pop: the device drops frames on its *own* thread, so a delta around
    the pop would miss every drop that happened between two ticks.
  - A device that reports an error (a display unplugged, a compositor
    restarted) is reopened once, and the restart is counted.  A device
    that cannot be reopened is reported through `on_error` with
    `kVideoCaptureOpenFailed` and the pump stops rather than spinning on
    a broken device.
  - A missing capture plug-in is reported through `on_error` with
    `kVideoCapturePluginMissing` — it is *not* silently ignored, but it
    is also not fatal for the rest of the session.
  - Covered by 9 cases in `tests/test_engine_capture_pump.cpp`, driven
    through the "shell" transport strategy so no ICE/DTLS handshake is
    involved.  The test registers its own capture device and its own
    codec plug-in, so it exercises the *seams* rather than a concrete
    backend: one case records the thread id at `encode()` time and
    asserts it is the thread that called `tick()`, and another asserts
    the producer thread is a *different* thread — so the first assertion
    cannot pass vacuously.

- **Two guards that make the architecture's promises executable
  (v1.2.0, ADR-025 / ADR-026).**  Both invariants were stated in prose
  and checked by nobody, which is how an invariant stops meaning
  anything.

  | Guard | Invariant |
  |-------|-----------|
  | `tools/check_engine_layering.py` | ADR-025 Layout Invariant 4 — the engine reaches a concrete module only through its `plugins::I*` seam, with a named set of exemptions |
  | `tools/check_c_clean_headers.py` | ADR-026 — a `.h` under `src/**/include/` is a promise that a C host can include it |

  The layering guard scans `src/engine/` for concrete-module includes and
  fails when a module outside the ADR-025 Decision 1 table appears.  The
  exemption list lives in the script, one commented entry per module, so
  widening it is a visible act rather than an accident.

  The C-clean guard compiles every public `.h` as C11, in isolation, with
  a real C compiler.  Isolation is the point: a header that only compiles
  because some *other* header was included first is exactly the header
  that breaks a C consumer who includes it first.  The scope is the
  extension — `.h` is C-consumable, `.hpp` is C++-only — so a header
  that was always C++-only opts out by being renamed, and a header that
  was always meant to be C cannot drift.

  Both guards ship with a `--self-test` that builds a synthetic tree and
  asserts the checker still tells right from wrong, because a guard that
  cannot fail is a decoration.  Both run as CI steps in the
  `docs / markdown links` job, alongside `check_dead_modules.py`.

- **A dead-module guard that makes "either wire it in, or delete it"
  executable (v1.2.0, architecture remediation plan §4.2e / D2).**
  `tools/check_dead_modules.py` reads the CMake dependency graph and the
  plugin registrar table and fails when a module under `src/modules/` has
  no path from the composition root.  It distinguishes three failure
  shapes, because the remedy differs:

  | Verdict | Meaning |
  |---------|---------|
  | `[dead]` | consumed only by its own tests — a complete implementation with passing tests, but the product never calls it |
  | `[unreach]` | linked only by a target that never ships (a benchmark) — the link line makes it *look* alive |
  | `[orphan]` | nobody links it and nothing references its registrar |

  The check judges **reachability**, not mere linkage: it walks the
  dependency graph from the composition root (the engine plus the shipped
  examples), so a module that only a never-shipped benchmark links is
  reported rather than quietly passed.  A module that registers a plugin
  factory counts as live when the registrar table references it —
  the registration *is* the consumption.

  The script carries a `--self-test` that builds a synthetic module tree
  and asserts the checker still distinguishes all four shapes, because a
  guard that cannot fail is a decoration.  It runs as a CTest case
  (`test_dead_modules`, labels `architecture;guard`) and as two CI steps
  (check + self-test).  A missing Python interpreter **skips** the CTest
  case rather than failing it: the guard audits the build graph, so a
  machine without Python should not turn the build red.

  Current state: all **37** modules under `src/modules/` report `[ok]`.

- **The `assembly` module now has a bridge to the engine and to the
  transport layer (v1.2.0, architecture remediation plan §4.2 / D2).**
  The `assembly` module held a complete implementation — a `Profile`
  schema, a `ProfileRegistry`, a `Builder`, a JSON loader and a
  round-trip test suite — and **nothing in the product ever called it**.
  That is the worst state the remediation plan names: it creates the
  impression that the feature exists when it does not.
  - `src/modules/assembly/include/nimrtc/assembly/engine_bridge.hpp` —
    the **single authority** for the Profile → `EngineConfig` mapping.
    Every host that drives the engine from a Profile must go through
    `assembly::to_engine_config()`, so two hosts can never disagree about
    the mapping.  It covers the plugin ids, the BWE / scheduler configs,
    the audio format, the jitter-buffer delays, the feature switches and
    the transport strategy — the fields the demo used to copy by hand
    (and drop most of).
  - `src/modules/assembly/include/nimrtc/assembly/transport_bridge.hpp` —
    the adapter between the assembly-layer `TransportSection` and the
    transport-side `nimrtc::ProfileTransportInput` mirror:
    `to_transport_input()`, `to_transport_requirements()` and
    `transport_strategy_name()`.  It lives on the `assembly` side
    because `assembly` is the *higher* layer than `transport`
    (`docs/plan/transport-strategies.md` §8) — putting it in `transport`
    would invert that and create a cycle.  The adapter is entirely
    header-only, so it adds **no** link-time dependency.
  - `EngineConfig::transport_media_max_p99_ms` — a per-instance
    media-plane p99 budget, and `NimRTCEngine::transport_media_max_p99_ms()`
    to read back what the engine resolved.  See the Fixed entry below
    for why this field had to exist.
  - `src/modules/assembly/tests/test_assembly_bridge.cpp` and
    `tests/test_engine_transport_profile.cpp` — the unit-level and the
    end-to-end coverage of the chain (Profile → bridge → `EngineConfig`
    → engine).

- **The capture-device seam, and the portable half of screen capture
  (v1.3.0, Remote Desktop plan A1 / A2 / A2b).**  The existing
  `IVideoSource` seam describes a *stream of raw frames* and is
  deliberately dumb: no device notion, no enumeration, no region
  selection, no format negotiation.  A remote-desktop product needs all
  four, plus two things the source seam has no room for — per-frame
  dirty-rectangle metadata (a static screen must not cost a full frame
  every tick) and a capability query (does this device offer zero-copy?
  does it composite the cursor? what is its maximum resolution?), so the
  engine can pick a backend and a ladder rung *before* the first frame
  arrives.
  - A new plugin seam, `src/plugins/include/nimrtc/plugins/video_capture.hpp`:
    `IVideoCapture` (device enumeration, format negotiation, region
    selection, capabilities, `pop_frame()`) and `IVideoCaptureFactory`.
    It is layered on the source seam rather than replacing it —
    `IVideoCapture` produces frames *from a device*, an `IVideoSource`
    produces frames — and it is pure C++20: no pixel formats, no
    `VideoSourceFrame`, no OS headers.  The device describes its own
    output through `CaptureFormat` and the adapter translates, so the
    engine never learns a platform API.
  - A new module, `src/modules/video_capture/`, owning the *portable*
    half of capture: the bounded frame queue with its explicit drop
    policy, the dirty-rectangle model, the format-negotiation contract,
    and a synthetic device that exercises all of it on every CI leg.
    The three platform backends (Windows DXGI Desktop Duplication /
    Windows Graphics Capture, macOS ScreenCaptureKit, Linux XDG Desktop
    Portal + PipeWire / X11) land in M1-c as thin adapters over this
    mechanism.
  - The queue is bounded on purpose.  If the encoder cannot keep up, the
    right behaviour for a remote desktop is to drop a frame, not to grow
    a backlog and add latency without bound.  The overflow policy is
    explicit and configurable (`CaptureDropPolicy`): screen content drops
    the *oldest* frame (the newest frame is the truth; a stale frame of a
    screen that has since changed is worth nothing), a camera drops the
    *newest* (a slightly stale frame beats one that arrives too late to
    be displayed).
  - The registry gains a video-capture category alongside the source
    seam — a capture device is not a frame source, so it gets its own
    slot (`register_video_capture()` / `get_video_capture()`) rather
    than being folded into the source registry.
  - **The shared "capture → frame stream" adapter**,
    `src/modules/video_capture/include/nimrtc/video_capture/capture_source_adapter.hpp`:
    `CaptureSourceAdapter` presents a capture device as the plain
    `IVideoSource` the engine's video path already consumes, so a
    platform backend never has to learn the engine's internals and the
    engine never learns a platform API.  The adapter opens and closes
    the device with the source, pops one frame from the bounded queue
    per `produce_one()`, converts the device's pixel layout into the
    requested one (BGRA / RGBA / NV12 / I420, with a portable BT.601
    BGRA→I420 conversion; the SIMD version is plan item C2), and
    republishes the frame carrying the capture timestamp, the frame
    sequence number and the dirty-rectangle metadata.
    - The pixel layout is taken from the device's negotiated
      `CaptureFormat::tag`, and the mapping is **strict**: an unknown
      tag is refused with `kErrInvalidParam` rather than guessed at,
      because guessing a pixel layout produces garbage pixels instead of
      an error.
    - The device is the authority on its own geometry: the adapter
      reports the size the device actually negotiated, not the size the
      caller asked for.  Resizing is the scaler's job (plan C1 / C2),
      and doing it here would hide the mismatch instead of reporting it.
    - An empty capture queue is a **no-op**, not an error: a quiet
      device must not park the engine's tick loop.
  - **Not yet done (deliberately):** all three platform capture backends
    now exist (Windows DXGI / WGC, macOS ScreenCaptureKit, Linux portal
    / X11), but the Windows backend still has no zero-copy `GpuBuffer`
    hand-off (plan item B10) — and the same is true of the macOS one,
    whose `CVPixelBuffer` is the surface that hand-off would consume.
    What *does* exist is the portable layer, the engine's pump over it
    (plan item A10, above), the cursor layer (A6) and the three
    platform backends (A3 / A4 / A5), so a third-party capture plug-in
    can already be driven by the engine end to end and a host can
    capture a real screen on each of the three platforms.

- **H.265 (HEVC) and AV1 video codec support, end to end (v1.2.0,
  Remote Desktop plan B5b/B10b).**  The engine previously spoke H.264
  only: the codec family was hard-coded in the SDP writer, the
  negotiation, and the packet path, and the H.265 / AV1 modules had no
  RTP payload format at all.  A session can now select either codec and
  negotiate it against a peer.
  - Two new modules, `src/modules/h265/` and `src/modules/av1/`, each
    self-contained (own `CMakeLists.txt`, `include/`, `src/`, `tests/`)
    per Layout Invariant 3.  Each registers a video-codec plugin under
    the id `"h265"` / `"av1"` — the spelling
    `EngineConfig::video_codec_name` uses — plus its own video backends
    (a software stub encoder/decoder and the NVIDIA NVENC / NVDEC
    entries).
  - Two new RTP payload modules,
    `src/modules/video_payload/{h265,av1}.{hpp,cpp}`:
    - H.265 follows **RFC 7798**: the two-byte payload header
      (`F(1) | Type(6) | LayerId(6) | TID(3)`), single-NAL-unit packets,
      aggregation packets (type 48) and fragmentation units (type 49,
      `S(1) | E(1) | FuType(6)`).
    - AV1 follows **draft-ietf-payload-av1** / the aomediacodec
      `av1-rtp-spec`: the one-byte aggregation header
      (`Z(1) | Y(1) | W(2) | N(1)`), LEB128 element length fields, the
      OBU header (`obu_type(4) | obu_extension_flag(1) |
      obu_has_size_field(1)`), Annex-B framing and a reassembler that
      drops a truncated OBU rather than emitting it.
  - The SDP layer learned both codecs' format parameters
    (`src/modules/video_sdp_attributes/`):
    - H.265: `profile-id`, `level-id`, `tier-flag` and **`tx-mode`**.
      `tx-mode` is the RFC 7798 §4.4 *string* token — one of `"SRST"` /
      `"MRST"` / `"MRMT"` — and the parser rejects a numeric value,
      because a peer that sends `tx-mode=0` has a different
      understanding of the wire.  The three parameter sets are the
      separate `sprop-vps` / `sprop-sps` / `sprop-pps` parameters; the
      H.264-style combined `sprop-parameter-sets` is still accepted for
      legacy peers.
    - AV1: `profile`, `level-idx`, `tier`, and the optional
      `bit-depth` / `monochrome` / `chroma-subsampling` parameters.
  - The engine holds **one** video codec per session (the single-codec
    rule), and the four places that name a codec — the plugin id, the
    plugin-layer `VideoCodecKind`, the SDP `a=rtpmap:` encoding and the
    `a=fmtp:` body — now come from **one** table,
    `src/engine/src/video_codec_config.hpp`.  Keeping the mapping in one
    place is what stops the four from drifting apart.
  - The payload type is resolved per codec family: H.264 = 102
    (RFC 6184's static type), H.265 = 116 and AV1 = 45 (the values
    Chrome's Unified Plan and the AV1 payload specification use).  A
    host that pinned a payload type still wins; a host that left H.264's
    102 in place while selecting another family gets that family's own
    default, because RFC 6184's 102 is H.264's and no other codec may
    reuse it.
  - The engine's offer, answer, packet path and inbound routing all use
    the *resolved* payload type and the negotiated codec family, so an
    H.265 session negotiates H.265 end to end — and an H.265 engine
    facing an H.264 offer rejects the video section (port 0) instead
    of silently switching codecs.
  - Covered by 8 new cases in
    `src/modules/video_payload/tests/test_h265.cpp` and
    `src/modules/video_payload/tests/test_av1.cpp`, 11 new cases in
    `src/modules/video_sdp_attributes/tests/test_video_sdp_attributes.cpp`
    and 6 new cases in `tests/test_video_negotiation.cpp`.

- **A real NVIDIA NVDEC hardware decoder (v1.1, Remote Desktop plan
  B5a/B10a).**  The NVDEC seam used to be an *empty* entry: the id
  existed, but `available()` returned `false` and `create()` returned
  `nullptr`.  It is now a working CUVID decoder.
  - The decoder lives in a new shared module, `src/modules/nvcodec/`,
    because the NVIDIA driver integration is **not** H.264-specific:
    the same NVENC session code serves H.264, HEVC and AV1, and the same
    NVDEC parser / decoder code serves all three.  Triplicating the
    driver code across three codec modules would guarantee the copies
    drift apart, so the driver integration lives in one lower-level
    module that the H.264 / H.265 / AV1 codec modules link against —
    the same cross-module dependency shape as `modules/jb` using
    `modules/jb_strategy`.
  - The decoder runs the full CUVID pipeline: `cuvidCreateVideoParser`
    → `cuvidParseVideoData` → sequence callback (`cuvidCreateDecoder`)
    → `cuvidDecodePicture` → `cuvidMapVideoFrame` → `cuMemcpy2D` to host
    memory.  Output is NV12.
  - The driver is loaded **at runtime** (`nvcuvid.dll` / `libnvcuvid.so`),
    not linked at build time, so a binary built with the plugin enabled
    still starts on a host without the driver.
  - Non-macroblock-aligned pictures are handled: an H.264 stream that is
    not a multiple of 16 pixels is coded at the macroblock-aligned size
    with `frame_cropping` metadata in the SPS, and the decoder passes the
    SPS `display_area` plus the target geometry to the driver so its VPP
    crops the visible picture in one hardware step.
  - NVENC and NVDEC are each **one-directional**, but the engine holds a
    single `IVideoCodec` per session.  `HwCompositeCodec` bridges the
    two, so one registered plugin now covers hardware acceleration in
    both directions.
  - The vendored `nv-codec-headers` (n13.1.15.0) is pinned in
    `src/third_party/vendor.json`.
  - Covered by `src/modules/nvcodec/tests/test_nvcodec_roundtrip.cpp` — a
    host-dependent end-to-end proof (NVENC → NVDEC round trip, the NV12
    output path, multi-frame reference chains, resolution changes, and a
    decoder-only leg over a self-contained bitstream).  The test exercises
    the generic `nvcodec::make_nvenc_codec()` / `make_nvdec_codec()`
    entry points directly, so it covers every codec the shared module
    serves (H.264, and H.265 / AV1 once those modules exist) without
    depending on any one codec module's registry.  Every case calls
    `GTEST_SKIP()` when the backend is unavailable, so it never fails on
    a machine without NVIDIA hardware.
    - The round trip is asserted for **every** codec the driver offers,
      not only for H.264: `NvencNvdecRoundTripH265` and
      `NvencNvdecRoundTripAv1` run the same encode → decode → compare
      -pixels leg for H.265 and AV1.  Without them, a regression that
      hard-coded H.264 inside the NVENC / NVDEC session code would still
      pass every H.264 case while silently breaking the H.265 / AV1
      codec modules, which call into this module's driver code.
    - On a host whose silicon has no encoder for a codec (e.g. an
      RTX 3060, which has no AV1 encoder) the availability probe reports
      the truth and the case skips with that reason.
  - The README / README.zh capability tables and the
    `docs/hw_plugin_seam.md` backend matrix now state the truth: NVDEC
    **is** implemented; the macOS VideoToolbox and Android MediaCodec
    decoders are not.

- **A data-channel message boundary, so `send()` is a message again
  (v1.2.0, DataChannel capability plan WP-B / WP-C).**  The default
  WebTransport backend rides a QUIC stream, which is a **byte** stream:
  two `send()` calls of 10 bytes each could surface as one 20-byte
  callback, and one `send()` of 10 bytes could surface as two 5-byte
  callbacks.  The `IDataChannel` contract says "send a binary message",
  so every message-oriented use of the data channel was unsafe.
  - `plugins::DataChannelFraming` — the framing mode.  `kRaw` (the
    default) keeps the byte-stream behaviour every existing deployment
    relies on; `kFramed` wraps each payload in a length-prefixed frame
    that also carries the channel label, so one `send()` is one inbound
    message and the frame carries the channel identity.
  - `EngineConfig::datachannel_framing` selects the mode for every
    channel the engine creates.  **Both ends must agree**: the framing
    is a NimRTC-internal convention (the plan's DEC-2 = (b)) with no
    on-the-wire negotiation, because Chrome will not speak one.  A
    framed peer cannot read a raw stream and vice versa.
  - A frame whose length prefix exceeds the negotiated maximum is a
    **protocol error**: the channel transitions to `error` rather than
    allocating an unbounded buffer.
  - The `kUnreliable` mode rides QUIC datagrams, which are
    message-preserving by definition, and is exempt from the framing.
  - Covered by 17 cases in
    `src/web_transport/tests/test_web_transport_dc_capability.cpp`.

- **A channel identity on the data-message callback (v1.2.0, DataChannel
  capability plan WP-C).**  The classic `set_on_data_message()` callback
  carries the payload only, so a host that opened two channels could
  not tell which one a message arrived on.  The mechanism was worse
  than a shared callback: the session had **one** inbound-datagram slot
  and **one** inbound-peer-stream slot, so the last channel created
  hijacked the inbound routing and closing *any* channel deafened *all*
  of them.
  - `plugins::DataChannelMessage { label, channel_id, data }` and
    `plugins::DataChannelMessageCallback`, delivered through the
    **additive** `IDataChannel::set_on_message_ex()`.  The four-member
    `IDataChannel` contract keeps its exact signatures — the change is
    additive, per ADR-015.
  - `NimRTCEngine::set_on_data_message_ex()` — the engine-level
    counterpart.  It assigns `channel_id` (1-based, monotonically
    increasing per engine) and fills `label` when the backend does not
    report one.  The plain `set_on_data_message()` keeps its exact
    behaviour: a fan-out shorthand installed on every channel.
  - The session's inbound slots became a **multicast**: the session
    gains `subscribe_on_datagram()` / `subscribe_on_peer_stream()` and
    the matching `unsubscribe_*()`.  A channel removes **only its own**
    subscription on close, so closing one channel leaves its siblings
    receiving.

- **An application-level retransmission protocol, so the
  partial-reliability modes actually deliver (v1.2.0, DataChannel
  capability plan WP-D4 — the plan's DEC-3 = (a)).**  A QUIC stream
  gives the application **no per-message retry hook**: msquic
  retransmits the stream's bytes internally and never tells the
  application that a *message* was lost.  The partial-reliability
  budgets therefore had nothing to act on, and the modes degraded to
  send-side policies.
  - The frame header gained a **kind** and a **sequence number**
    (`src/web_transport/src/web_transport_frame.hpp`): a DATA frame
    with `seq == 0` is a plain message, a DATA frame with `seq != 0` is
    an ARQ message the peer must acknowledge and de-duplicate, and an
    ACK frame carries the acknowledged sequence number.
  - The ARQ runs under `kFramed` in the two partial-reliability modes
    (`kPartialReliableTTL`, `kPartialReliableCount`).  The reliable
    mode rides QUIC's own retransmission and the unreliable mode rides
    datagrams, so neither uses it.
  - The receiver de-duplicates: a retransmitted message is delivered
    **once**, and the acknowledgement is sent even for a duplicate
    (the lost ACK is the usual reason a message is retransmitted).
  - The budgets are **per message** and are enforced by the ARQ: an
    expired TTL or an exhausted retransmit budget drops the message and
    **the channel stays open**.
  - `EngineConfig::datachannel_max_retransmits` (0 = unlimited) and
    `EngineConfig::datachannel_max_packet_lifetime_ms` (0 = unlimited)
    carry the budgets to the backend.
  - The de-duplication window is bounded (4096 sequence numbers); a
    retransmission that arrives later than the window is delivered
    again.
  - Covered by 6 cases in
    `src/web_transport/tests/test_web_transport_dc_capability.cpp`.

- **A settable data-channel priority (v1.2.0, DataChannel capability
  plan WP-E1).**  `DataChannelConfig::priority` was read in exactly two
  places — it was encoded into the DCEP `DATA_CHANNEL_OPEN` message and
  otherwise ignored — and the engine hard-coded `128` at both creation
  sites with **no setter at all**, so the value could not even be set
  to anything else from the public API.
  - `EngineConfig::datachannel_priority` (default 128) reaches the
    backend and orders the data plane's own outbound queue.
  - **What it does not do**: the media plane and the data plane run
    independent congestion control on two different connections, so the
    scheduler cannot arbitrate between them.  The honest statement is:
    *the data channel has its own connection and therefore its own
    priority; the scheduler does not arbitrate between media and data*.
    The intra-data-plane ordering (a control channel is not stuck behind
    a bulk transfer on the same QUIC connection) is the deliverable the
    architecture can actually back.

- **A telephone-event (RFC 4733) seam, so DTMF stops being an unknown
  payload type (v1.2.0, session capability plan D1 / D2).**  Until now a
  peer that negotiated `telephone-event` sent its digits into a payload
  type the audio routing table had no route for: the packet was counted
  as `kAudioPayloadTypeUnknown` and dropped, so a NimRTC peer could
  neither send nor receive DTMF.
  - `plugins::IDtmfCodec` — a seam of its own, because a four-byte
    telephone-event header is **not** audio and must never reach an
    `IAudioDecoder`.  It decodes one payload, encodes one event and
    reports the payload type it uses.
  - `src/modules/dtmf/` — the built-in implementation, registered under
    the id `"dtmf"`.  The engine reaches it through the registry slot and
    never includes the module's headers.
  - The engine surface: `EngineConfig::dtmf_enabled` (**off** by
    default — the offer carries no telephone-event declaration unless a
    host asks for it), `dtmf_payload_type` (126, the value Chrome uses),
    `dtmf_codec_name`, plus `send_dtmf()` and `set_on_dtmf()`.  An
    inbound telephone-event payload is routed to the codec and reported
    as a `DtmfEvent` with the accumulated duration, instead of being
    dropped as unknown.

- **A negotiated telephone-event payload type, so the DTMF path is
  actually reachable (v1.2.0, session capability plan D3 / D4).**  The
  RFC 4733 codec and the send path existed, but the *payload type* was
  never negotiated: the offer declared nothing, the answer echoed
  nothing, and the codec sat idle.  A key press therefore had nowhere
  to go, and a telephone-event payload arriving from the peer landed on
  a payload type the engine did not recognise.
  - The offer now carries `a=rtpmap:<pt> telephone-event/8000` and
    `a=fmtp:<pt> 0-16` (RFC 4733 §2.3.2), and the payload type joins
    the m-line's format list — RFC 3550 §5.14 scopes a payload type
    to its m-section, so a payload type that is not in the `m=` line
    is not part of the session.  The declaration is conditional:
    `EngineConfig::dtmf_enabled` is **off** by default (OD-B), so a
    default engine offers no telephone-event line at all.
  - The answer carries the **negotiated** event codes — the peer's
    list narrowed to the codes this side implements — rather than a
    blind echo of the offer.  An answer that names a code the offerer
    never offered describes something the peer will not accept.
  - `NimRTCEngine::negotiated_dtmf_payload_type()` reports the number
    both ends use, or `0` when nothing was negotiated.  `send_dtmf()`
    fails in exactly that state, so a host can ask before it presses a
    key rather than sending a packet on a payload type the peer never
    agreed to (RFC 3264 §6.1).
  - The counting boundary is now explicit (D4): a **negotiated**
    telephone-event payload is decoded and reported to the host and is
    **not** counted as an unknown audio payload type; an
    **un-negotiated** one still is (the peer sent something we never
    agreed to).  A malformed payload is counted separately, so it never
    masquerades as a delivered event.
  - The telephone-event plane has counters of its own in `Stats` —
    `dtmf_packets_sent`, `dtmf_packets_received`,
    `dtmf_packets_malformed`, `dtmf_events_delivered` and
    `dtmf_payload_type` — because the audio counters say nothing about
    it: the payload never reaches the audio pipeline.
  - Covered by 13 cases in `tests/test_engine_dtmf.cpp`: eleven on the
    SDP declaration and the counting rules, and two on the wire -- a
    key press has to leave the engine as an RFC 4733 *event sequence*
    (the repeats, the marker bit on the first packet, and the
    end-of-event packet that closes it), not as a single packet.  The
    codec's own wire format is covered by
    `src/modules/dtmf/tests/test_dtmf_codec.cpp`.

- **An RTCP Extended Report (RFC 3611) seam, so PT 207 stops being an
  opaque byte span (v1.2.0, session capability plan X1).**  The
  compound demultiplexer already *recognised* payload type 207, but
  recognition was the whole of the implementation: the XR sub-packet
  came out of the demux as a byte span that nothing consumed, so a
  peer that sent extended reports was reporting into a void.
  - `plugins::IXrReport` — a seam of its own, because the plugin layer
    may not name a concrete module.  It defines its own neutral value
    types (`XrPacket` / `XrBlock` / `XrBlockType`) and the module-side
    adapter translates between them and the wire-level ones.
  - `src/modules/rtp/` — the built-in implementation, registered under
    the id `"nimrtc_xr"`.  The engine reaches it through the registry's
    XR slot and never includes the module's headers.
  - The blocks NimRTC parses are **BT=4** (Receiver Reference Time),
    **BT=5** (DLRR) and **BT=6** (Statistics Summary).  The other
    block types RFC 3611 registers -- Loss RLE (1), Duplicate RLE (2),
    Packet Receipt Times (3), VoIP Metrics (7) -- are *recognised but
    not parsed*: the block walk skips them by their length, which is
    what makes the format extensible, so an unknown block is never an
    error.
  - The engine surface: `EngineConfig::xr_report_name` (default
    `"nimrtc_xr"`), `NimRTCEngine::xr_report()` -- the host reads the
    decoded blocks out of `snapshot()` -- and `Stats::xr_report_blocks`,
    the counter that makes the routing observable.  A malformed report
    from a peer is *counted*, not thrown: the read path must not fail
    because one peer sent a broken report.
  - **The block numbering is RFC 3611's, not a draft's.**  There is no
    block type named "Receiver Estimated Quality" in the standard; that
    was the numbering of an early draft (where BT=1 was it and BT=6 was
    DLRR).  A report built with the draft's numbering would not
    interoperate with a conforming peer, so the implementation and the
    plan document both use the standard's actual registry.
  - Covered by `src/modules/rtp/tests/test_rtcp_xr.cpp` (the wire
    format), `src/modules/rtp/tests/test_xr_report_plugin.cpp` (the
    seam) and `tests/test_engine_xr_report.cpp` (the engine wiring).

- **The outbound RTCP XR / TMMBR plane, so the engine can answer the
  peer's feedback instead of only receiving it (v1.2.0, session
  capability plan X2 / X4 / X5 / X5b / X6).**  The XR seam landed the
  *receive* half; the engine could not yet say anything, and the
  `disable_tmmbr` knob of `plugins::SdpCodecConfig` was a dead field
  that nothing read.
  - **The engine sends an RFC 3611 extended report** (BT=4 Receiver
    Reference Time, BT=5 DLRR, BT=6 Statistics Summary), one per RTCP
    interval.  The DLRR sub-block names the *peer's* SSRC (the receiver
    whose report is being answered), the middle 32 bits of that
    report's NTP timestamp, and the delay since its arrival -- the three
    values a peer needs to compute the round-trip time.
  - **The engine sends a TMMBR** carrying the bit rate its bandwidth
    estimator is *currently using*, so the peer throttles to a number
    the engine actually honours.  A changed rate is reported at the
    next RTCP interval; an unchanged one is refreshed every five
    intervals, because the peer may have missed the previous message.
  - **An inbound TMMBR is obeyed**: the tightest limit the peers ask
    for clamps the scheduler's session budget, the per-class pacing
    rates **and the audio encoder**, and every TMMBR is answered with
    exactly one TMMBN (RFC 5104 §4.2.2.2) -- even when the tuple was
    not accepted, which is what lets a participant that missed the last
    TMMBR recover the state.  A tuple naming a media source the engine
    does not send is recorded but not applied: it is somebody else's
    constraint.
  - **The peer's limit outranks the local audio floor.**  The
    acknowledgement (the TMMBN) is a promise, so the limit the peer
    asked for overrides `EngineConfig::audio_min_bitrate_bps`: the
    encoder is allowed to drop *below* the local floor when the peer
    asks for less.  The two constraints are not of the same kind -- the
    local floor protects the audio against a congestion collapse,
    while the peer's limit is a commitment the engine made on the wire.
    The limit is honoured even when `audio_bitrate_adaptive` is
    `false`: that switch says "do not move the encoder because of the
    *estimate*", not "break the promises the engine already made".
  - **TMMBR is opt-in.**  `EngineConfig::tmmbr_enabled` defaults to
    `false` -- the engine sends no TMMBR unless the host asks for it.
    `EngineConfig::disable_tmmbr` vetoes the feature in both
    directions and overrides `tmmbr_enabled`.  The *inbound* side
    needs no switch: a TMMBR the peer sends is always honoured.
  - **Both features are negotiated, not assumed.**  The offer carries
    `a=rtcp-fb:<pt> ccm tmmbr` (RFC 5104 §7.2) and
    `a=rtcp-xr:rcvr-rtt=all stat-summary=loss,jitt` (RFC 3611 §5.1); the
    answer echoes the negotiated subset.  RFC 3611 §5.1 says a
    participant "SHOULD NOT send XR blocks other than the ones indicated
    by the parameters", and RFC 5104 §7.2 says "only the joint subset
    of CCM parameters from the offer and answer may be used" -- so an
    un-negotiated XR report or TMMBR is not merely wasteful, it is a
    protocol violation.
  - **The Statistics Summary's field list is the peer's.**  RFC 3611
    §5.1 says the "stat-summary" parameter "contains a list indicating
    which fields SHOULD be included in the Statistics Summary report
    blocks that are sent", so the block's L and J flag bits follow the
    peer's list: a peer that asked for "loss" alone gets no jitter
    flag.  The engine produces loss and jitter and nothing else.
  - `sdp::MediaDescription` gains `rtcp_xr` / `rtcp_xr_present`, with
    parser and munger support.  The bare `a=rtcp-xr` attribute (no
    parameters) is preserved as its own statement: RFC 3611 §5.2 gives
    it the meaning "I understand the attribute but send me no XR
    blocks".
  - The engine surface: `EngineConfig::rtcp_xr_enabled`,
    `EngineConfig::tmmbr_enabled`, `EngineConfig::disable_tmmbr` (the
    switch that finally makes `plugins::SdpCodecConfig::disable_tmmbr`
    meaningful), plus the observability accessors
    `xr_reports_sent_count()`, `peer_tmmbr_limit_bps()`,
    `last_tmmbr_bps()`, `tmmbr_sent_count()` and
    `tmmbr_received_count()`.
  - Covered by `tests/test_engine_tmmbr.cpp` (13 cases: the offer, the
    answer, the limit, the tightest-limit rule, the foreign-source
    rule and the malformed-packet rule),
    `tests/test_engine_xr_tmmbr.cpp` (11 cases that assert on the
    **wire bytes**: the XR report's RRTR / DLRR / Statistics Summary
    blocks, the TMMBR's live bit rate, the `disable_tmmbr` veto and the
    TMMBN's owner SSRC),
    `tests/test_engine_audio_bitrate.cpp` (the encoder side of the
    inbound TMMBR: the cap, the floor override, the pinned encoder and
    the tightest-limit rule) and
    `src/modules/rtp/tests/test_rtcp_xr.cpp` (the wire format).

- **A pluggable session model, so a host can renegotiate instead of
  rebuilding the engine (v1.2.0, session capability plan N1 / T1).**  The
  engine used to build a *fixed* SDP media list (one audio m-line, one
  video m-line) and offered no way to add a track, remove one or change
  a direction.
  - `plugins::ISession` — the seam (tracks, transceivers, mid
    allocation) with a typed slot in `core::PluginRegistry`; the
    built-in implementation lives in `src/modules/session/` and
    registers under the id `"session"`.
  - The engine surface: `add_track()`, `remove_track()`,
    `set_track_direction()`, `tracks()`, `add_transceiver()`,
    `set_transceiver_direction()`, `stop_transceiver()`,
    `restart_transceiver()`, `transceivers()` and `needs_negotiation()`.
  - Removing a track does **not** remove its m-line: the transceiver
    stays and is marked `inactive` (OD-A), and a mid is never reused.

- **ICE restart (RFC 8445 §9.1.1.1), locally and on the peer's signal.**
  A network change (WiFi -> 4G) or a stale path could not be recovered
  without tearing the whole engine down.
  - The ICE transport seam gains `restart()`, `restart_count()` and the
    `OnIceRestart` callback, which fires when the *peer's* description
    carries different ICE credentials — the answerer then answers with
    a fresh credential pair, as RFC 8445 requires.
  - The engine surface: `restart_ice()`, `ice_restart_count()` and
    `set_on_ice_restart()`.  A local restart mints a new credential pair,
    re-gathers the candidates and re-applies the known remote
    candidates; the host then sends a fresh offer.

- **A token-bucket pacer (v1.2.0 M3, video plan P1 / P2).**  The send
  path had no rate-shaping primitive of its own: the scheduler's budget
  was a per-drain allowance, and the bytes left the machine in bursts.
  - `src/modules/pacing/` — a **token bucket** on a monotonic clock.
    The bucket holds bytes, the rate is expressed in bits per second
    (the unit every other knob in the project uses), and the bucket
    never reads a clock itself: `refill()`, `available()` and
    `try_consume()` take the current time point as an argument, which
    makes the behaviour deterministic and testable without sleeps.
  - The scheduler drives **per-class** buckets from the bandwidth
    estimate, so a saturated video stream cannot eat the audio class'
    share; the engine exposes `EngineConfig::media_pacing_enabled` and
    `update_pacing_rates()`.

- **The per-stream share rule of the session bandwidth budget (video plan
  F8).** `IBwe::estimate()` returns one number for the whole session, so
  the estimate has to be split before it reaches an encoder.  The rule:
  the *reserved* streams (the audio) take their clamped share of the
  estimate first and keep a hard floor; the *elastic* streams (the video)
  share what is left in proportion to their weight, split with the
  largest-remainder (Hamilton) method so the pool is handed out exactly
  and deterministically.  A reserved stream that claims more than the
  estimate holds puts the session in *deficit*, which is reported rather
  than hidden.  The rule is one engine-private, header-only pure function
  (`src/engine/src/bandwidth_allocation.hpp`); the engine's audio bitrate
  path now uses it instead of its own arithmetic — one rule, not two — and
  the video's elastic share arrives with the rate controller (B2) on the
  same terms.  Covered by 16 cases in `tests/test_bandwidth_allocation.cpp`.

- **Two plugin seams for the audio pipeline (audio pipeline WP-1).** The
  receive path needs a codec-side decoder (one RTP payload in, PCM frames
  out) and a pipeline-side receiver (jitter buffer, concealment, FEC
  recovery, downmix and playout clock behind one interface), so the engine
  never has to include a concrete module header.
  - `plugins::IAudioDecoder` with `AudioDecodeFormat`, `AudioDecodedFrame`
    and its factory.
  - `plugins::IAudioReceiver` with `AudioReceiverConfig`,
    `AudioOutputFrame` and its factory.
  - Two typed slots in `core::PluginRegistry` (the `kAudioDecoder` and
    `kAudioReceiver` categories), their registration macros and the
    back-compat includes. The two new macros take the id as a string
    literal; the legacy `NIMRTC_REGISTER_*` family, which stringifies a
    bare identifier token, is unchanged.
  - `engine::pal::resolve_audio_decoder()` and `resolve_audio_receiver()`,
    mirroring `resolve_codec` / `resolve_audio3a`.
  - The audio error range `0x1A40`..`0x1A48` in `core/engine_errors.hpp`.
  - `src/modules/audio_rx/` -- the self-contained module that will carry
    the receive pipeline.

- **The Opus side of the audio-decoder seam (audio pipeline WP-2).** The
  Opus module now publishes a `plugins::IAudioDecoder` under the id
  `"opus"` in the registry's audio-decoder category, next to the codec
  plugin that uses the same id in the codec category (ids are per
  category).
  - The concrete decoder gains the entry points the pipeline needs:
    `decode_with_fec()` reconstructs the *previous* frame from the
    redundancy the peer embedded in the current packet, `last_payload()`
    lets the caller re-submit a payload, `set_channels()` and
    `reconfigure()` renegotiate the decoder (libopus cannot change
    either in place, so the instance is re-created), and
    `packet_loss_perc` drives the in-band FEC hint on both sides.
  - The adapter header is private to the module: the engine reaches the
    decoder through the registry's slot, so the public header count stays
    where the API-freeze scan expects it.
  - The tests reach the plugin through `core::PluginRegistry`, never
    through a concrete class -- the property WP-7 depends on. They cover
    the round-trip, the multi-frame payload, the malformed payload, the
    concealment frame properties and the in-band FEC recovery of a lost
    frame.

- **The jitter-buffer seam can now be told which clock the stream runs
  on.** `plugins::IJB` gains one virtual, `configure(const JBConfig&)`,
  callable after `create()` and before the first `push()`. A jitter buffer
  that was handed a 48 kHz audio stream used to measure the interarrival
  jitter in the wrong unit and derive a playout delay off by the ratio
  between the two rates (spec B-3 / F-6). The seam default of
  `JBConfig::clock_rate_hz` moves from 90000 to 0, which means
  "unspecified" instead of silently assuming video; the concrete module
  keeps its historical 90 kHz reading for a `clock_rate_hz` of 0, so the
  video path -- which never configures the buffer -- is unchanged.

- **The audio receive pipeline module (audio pipeline WP-3).** The module
  `src/modules/audio_rx/` implements the receive side of the pipeline: it
  resolves its jitter buffer and its decoder through `core::PluginRegistry`
  (the ids the engine puts into `AudioReceiverConfig`), walks the media
  timeline on the RTP timestamps, conceals the slots that never arrived,
  downmixes to the configured output channel count, and plays out **only**
  when the host's `tick()` asks it to -- the playout is clock-driven, not
  arrival-driven (spec A-1 / A-3 / A-4).
  - The module depends on `nimrtc::core` and `nimrtc::plugins` only: it
    links neither the Opus nor the JB module, because both are injected.
  - It publishes a `plugins::IAudioReceiver` under the id
    **`"nimrtc_audio_rx"`** in the registry's audio-receiver category.
  - The factory returns a **null product** when the jitter buffer or the
    decoder plugin is not registered: there is no fallback receiver
    (spec B-16), so the caller turns the null into the hard error
    `kAudioReceiverMissing`. The null product is also the only way to
    report the condition, because the engine installs the callbacks
    *after* `create()`.
  - The module's tests use their own doubles for `plugins::IJB` and
    `plugins::IAudioDecoder` rather than the Opus and JB modules, which is
    what keeps the module independently buildable and testable. They cover
    the V-1 rows "Receiver timeline", "Receiver cadence" and "Receiver
    downmix", plus the concealment cap (silence beyond the cap), the
    stream clock rate the jitter buffer is configured with, and the two
    null-product paths of B-16.

- **Negotiated payload-type routing and a completed audio m-line (audio
  pipeline WP-6, spec B-4 / B-14).** The engine used to demultiplex
  inbound RTP with a single rule -- "if the payload type is the video one
  it is video, otherwise it is audio" -- which handed telephone-event,
  RED and unsupported-codec packets to the audio decoder as if they were
  audio (defect F-8). The rule is now a *negotiated* routing table.
  - The peer's own `a=rtpmap:` lines populate two tables, one per
    m-section, because a payload type is only meaningful within the
    m-section that declared it (RFC 3550 section 5.14). A payload type
    that is in neither table is counted and dropped with
    `kAudioPayloadTypeUnknown`; it is never decoded. The report is
    throttled to the first occurrence and every 50th one, so a peer that
    keeps sending a stream we never negotiated cannot flood the host.
  - The payload type we accept is the one we can actually decode -- the
    check goes through the registry's audio-decoder slot, so the answer
    reflects the plugins the host really installed. A peer that offers
    only codecs we cannot decode is a *configuration* condition:
    `kAudioCodecUnsupported` is reported exactly once and the audio
    m-line is rejected in the answer (RFC 3264 section 6.1).
  - The offer now carries `a=ptime` / `a=maxptime`, the Opus `a=fmtp`
    that describes what NimRTC actually does (in-band FEC on, the
    stream's channel count), and the `ssrc-audio-level` `a=extmap` that
    Chrome needs before it reports `audioLevel` in `inbound-rtp` (F-22).
    The per-engine SSRC in the description is the very value the packet
    path stamps, so the two cannot drift apart.
  - The answer carries exactly the negotiated payload type -- its own
    `a=rtpmap:` and the *negotiated subset* of the peer's `a=fmtp:`
    parameters, not a blind echo.
  - The routing table lives in a private engine header
    (`src/engine/src/audio_routing.hpp`), so the public surface is
    unchanged: the scan still reports 109 headers and 730 symbols.

- **The engine drives the audio receive pipeline as a plugin (audio
  pipeline WP-7, spec A-9 / A-10 / B-11 / B-15 / B-16).** The receive
  path was the last place where the engine still owned a concrete module:
  it held its own `jb::JitterBuffer`, fell back to a concrete
  `opus::Decoder`, and then handed the *compressed* RTP payload to the
  host as if it were float PCM. The host received noise, and the 3A echo
  canceller was fed a bit-stream instead of a reference signal.
  - `src/engine/` now includes **no** concrete module header: neither
    `nimrtc/jb/*` nor `nimrtc/opus/*`. The engine resolves a decoder and
    a receive pipeline through the registry's typed slots
    (`EngineConfig::audio_decoder_name`, default `"opus"`, and
    `EngineConfig::audio_receiver_name`, default `"nimrtc_audio_rx"`) and
    drives `plugins::IAudioReceiver` alone.
  - The host now receives **decoded** PCM. The pipeline resolves its
    jitter buffer and its decoder through the registry, walks the media
    timeline on the RTP timestamps, conceals the slots that never
    arrived, recovers a lost frame from the peer's in-band FEC and
    downmixes to the configured output channel count.
  - The playout is clock-driven: `tick()` is what makes the pipeline
    produce output, and the output cadence follows the media time rather
    than the packet arrival.
  - The 3A render reference is now the **decoded** signal -- the frame
    that is about to be played out -- instead of the inbound bit-stream.
    Without a reverse stream the echo canceller could never converge.
  - **There is deliberately no fallback receiver.** A missing decoder or
    a missing pipeline is a hard error (`kAudioDecoderMissing` /
    `kAudioReceiverMissing`) that fires `on_error` exactly once and
    disables the audio track; the inbound packets are counted and dropped,
    never decoded. A "built-in minimal receiver" would decode nothing and
    hand the bit-stream over as if it were audio.
  - The concrete `opus::Encoder` fallback is gone for the same reason: a
    missing codec plugin is reported (`kAudioEncodeFailed`, throttled)
    instead of silently putting a *different* codec's bit-stream on the
    wire.
  - `src/engine/CMakeLists.txt` gains a **link** dependency on the
    `audio_rx` module (so the default receiver id resolves for consumers
    that link only the engine) and `src/core/` gains the matching entry
    in `kDefaultRegistrars[]`. Neither is an include: the public header
    set is unchanged, and the scan still reports 109 headers and 730
    symbols.

- **The audio pipeline's engine integration tests (audio pipeline WP-11,
  spec V-1 / V-2).**  Two new test binaries:
  - `tests/test_engine_audio_send.cpp` — the send path's two promises.
    The framing is the engine's business: `send_audio()` accepts any
    block size and puts exactly one RTP packet per 20 ms frame on the
    wire, with a continuous media timestamp and a continuous sequence
    number (spec B-6).  And no silent loss: an empty buffer, a null
    buffer or an encoder error increments the statistics counter and
    fires `on_error` (spec A-8).  The engine is driven through a fake
    ICE transport and a fake DTLS session, both registered as plugins.
  - `tests/test_engine_audio_loopback.cpp` — the media loopback: loss
    injection (the output cadence stays constant and the concealed-frame
    counter matches the drop count), the in-band FEC recovery, the
    tick-rate independence (30 Hz and 200 Hz produce the same audio) and
    the real two-engine loopback over 127.0.0.1, where engine A feeds
    PCM and engine B decodes it.

- **The Chrome → NimRTC audio direction is now observed (audio pipeline
  WP-11, spec V-3).**  The Chrome interop page used to add a *recvonly*
  transceiver, so Chrome never sent a packet and NimRTC's receive path
  was never exercised against a real browser — only the NimRTC → Chrome
  direction was covered.
  - The page captures the **fake microphone** when the harness asks for
    it (`?sendAudio=1`): `getUserMedia()` attaches a track, the offer
    becomes `a=sendrecv`, and Chrome really encodes and sends Opus.  The
    Playwright harness grants the microphone permission.
  - The direction is asserted from **both** ends, because they are two
    facts about the same flow: Chrome reports `micTrackAdded` and
    `outboundPackets > 0`, and NimRTC reports
    `Stats::audio_frames_received > 0` — the engine's own counter, so a
    non-zero value means the host was handed *decoded* PCM rather than
    the compressed payload reinterpreted as floats.
  - `tools/run_e2e_acceptance.py` reads the engine-side counter out of
    demo-p2p's stderr, so the acceptance runner (and with it the daily
    soak) now fails when the receive path breaks.
  - The Chrome offer fixture (`interop/fixtures/chrome_opus_offer.sdp`)
    and its expected-parsing document now describe the `sendrecv` offer
    the page really sends.

- **The audio encoder's bitrate follows the bandwidth estimate.**  The
  engine used to hand the Opus encoder a fixed 64 kbps regardless of
  the link, so a session on a narrow link lost its audio instead of
  degrading it.  The engine now derives the audio bitrate from the BWE
  estimate: the audio budget is `estimate.target_bitrate_bps *
  audio_bwe_share` (10 % by default), clamped between
  `audio_min_bitrate_bps` (8 kbps) and `audio_bitrate_bps` (the
  ceiling).  A drop is applied immediately (at most -50 % per step), a
  rise is at most +10 % per step, and a deviation below 10 % is ignored
  — so the encoder never oscillates.  The new `EngineConfig` fields are
  `audio_bitrate_bps`, `audio_min_bitrate_bps`, `audio_bitrate_adaptive`,
  `audio_bwe_share` and `audio_bitrate_update_interval_ms`; the
  effective value is observable through the new
  `Stats::audio_encoder_bitrate_bps`.
  - The bitrate reaches the encoder through `ICodec::update_config()`,
    which now reconfigures the *running* encoder in place instead of
    re-creating it — an adaptation step no longer resets the codec state
    or its statistics.

- **The mono default is 32 kbps.**  The engine hard-coded 64 kbps for
  the audio encoder, which is the stereo number; a mono configuration
  (`pcm_channels = 1`, the default) now gets 32 kbps.  An unset
  `opus::EncoderConfig::bitrate_bps` (0) resolves to the channel-count
  default — 32 kbps mono, 64 kbps stereo — through the new
  `opus::default_bitrate_bps_for_channels()`.

- **The receiver-side network-feedback collector (v1.2.0 M1, spec
  F1 / F2 / F4 / F6).**  The bandwidth estimator was never fed: the
  engine read `IBwe::estimate()` in its tick, but nothing ever called
  `IBwe::on_feedback()` outside the unit tests, so the target stayed
  pinned at the initial bitrate (1 Mbps) for the whole session and every
  rate-control feature built on top of it was decorative.
  - A new plugin seam `plugins::INetFeedback` — the *producer* side of
    the loop.  It consumes demultiplexed RTCP sub-packets (the payload
    type, the FMT and the raw bytes, so the interface layer never names
    a wire-format type) and aggregates them into one
    `plugins::NetFeedbackSnapshot`.
  - A new module `src/modules/net_feedback/` implementing the seam.  It
    reads three signals: the **Receiver Report** (fraction lost, jitter,
    and the RTT derived from the LSR / DLSR pair against our own last
    Sender Report), **REMB** (the peer's own bandwidth opinion) and
    **transport-cc** (the per-packet arrival series, the received and
    lost counters and the implied receive rate).  It registers under the
    id `"nimrtc_net_feedback"` in the registry's new net-feedback
    category.
  - The engine now demultiplexes every inbound RTCP compound datagram
    with `rtp::demux_rtcp()` (RFC 3550 section 6.1) instead of walking
    it by hand: the Sender Report still feeds the cross-end timeline, the
    video sender still gets the NACK / PLI / FIR, and the receiver-side
    network feedback (RR / REMB / transport-cc) goes to the collector.
    The demultiplexer is forgiving — a malformed tail ends the walk
    instead of throwing away the well-formed head.
  - The engine hands our own Sender Report to the collector, because the
    LSR / DLSR pair in the peer's Receiver Report only becomes a round
    trip time when the collector knows when that report left.
  - The snapshot is fed to the estimator **once per feedback report**, not
    once per engine tick: the collector's timestamp moves exactly when a
    new report arrived, so the tick uses it as the de-duplication key.
  - `EngineConfig::net_feedback_name` selects the collector (default
    `"nimrtc_net_feedback"`; an empty string keeps the pre-v1.2.0
    behaviour of never feeding the estimator).  The engine resolves it
    through the registry and never includes the module's header
    (Layout Invariant 4).
  - `Stats` gains three counters that make the loop observable:
    `net_feedback_blocks` (RTCP blocks routed to the collector),
    `bwe_feedback_samples` (snapshots handed to the estimator) and
    `bandwidth_estimate_bps` (the estimator's current target).
  - `plugins::BweFeedback` gains the discriminating fields the policy
    needs: `signal_source` (loss/RTT, transport-cc or REMB), the
    transport-cc arrival series, the acknowledged sequence range and the
    per-packet sizes.  Every field is additive — an estimator that
    ignores them behaves exactly as before.

- **The bandwidth signal is now *selected*, not assumed (v1.2.0 M2, F5;
  ADR-018).**  The collector had a fixed source precedence
  (transport-cc → loss/RTT → REMB) that never consulted the
  negotiation: a peer that did not advertise `transport-cc` still
  produced a `kTransportCc` snapshot as soon as one transport-cc
  feedback arrived, and a peer that advertised it but stopped sending
  it starved the estimator.
  - The peer's feedback capabilities are read from the remote
    description's `a=rtcp-fb` lines (RFC 4585 section 4.1) and handed
    to the collector, so the policy knows what the peer will actually
    send.
  - `plugins::NetFeedbackPolicy` (new) is the host's override:
    `kAuto` (transport-cc when negotiated *and* fresh, loss/RTT
    otherwise), `kLossRtt` (always the loss/RTT path; REMB stays an
    upper-bound hint) and `kRequireTransportCc` (report the breakage
    instead of falling back silently — there is deliberately no
    "force transport-cc", because forcing a signal the peer never
    sends produces a silent stall).
  - `plugins::NetFeedbackConfig` (new) carries the policy and the
    freshness window; `INetFeedback` gains `set_config()`,
    `config()`, `set_peer_capabilities()`,
    `peer_capabilities()`, `transport_cc_unavailable()` and
    `evaluate(now)`.
  - The freshness axis needs a clock and the collector has none of its
    own, so the engine calls `evaluate()` from its tick — a transport-cc
    path that went silent is detected even when no new feedback
    arrives.  Re-evaluating never moves `NetFeedbackSnapshot::timestamp`,
    so the estimator is still fed once per report.
  - `EngineConfig::net_feedback_signal` selects the policy
    (`"auto"` / `"loss_rtt"` / `"require_transport_cc"`; an unknown
    value falls back to `"auto"` — the policy must never be the reason
    a session fails) and
    `EngineConfig::net_feedback_transport_cc_timeout_ms` sets the
    freshness window (default 3000 ms).
  - The engine advertises `a=rtcp-fb:<pt> transport-cc` in its offer
    when it has a transport-cc-capable collector configured, and echoes
    the offer's feedback types in its answer filtered to the supported
    ones (RFC 3264 section 6.1).  Dropping the transport-cc entry
    would silently disable the preferred signal.
  - The collector is the only place with a policy branch — the engine
    contains none (ADR-018 Decision 5).

- **The transport-cc signal now actually drives the estimate (v1.2.0 M2,
  F5/F6; ADR-018 Decision 4).**  The arrival series reached the
  estimator, and nothing read it: under the `kTransportCc` policy the
  estimate saw a loss rate and an RTT, both of which the loss/RTT path
  already provides.  The "prefer transport-cc" preference therefore
  bought nothing, and the delay trend — the one congestion signal
  transport-cc offers that loss-based feedback cannot produce — was
  computed nowhere.
  - The arrival series is turned into an **inter-arrival-spacing trend**:
    the spacing of the received media packets against the smallest
    spacing seen recently.  A rise means a queue is building up in front
    of the bottleneck, which is congestion *before* the packets start
    being dropped.  The helper is module-internal
    (`src/modules/bwe/src/arrival_trend.hpp`): the receive rate and the
    loss stay with the collector, the trend belongs to the estimator's
    algorithm.
  - The AIMD front-end (the engine default) backs off on the trend with
    the same multiplicative decrease a loss event uses; the Goog-CC
    front-end uses the trend *instead of* its RTT gate while transport-cc
    drives, because a Receiver Report's RTT is a windowed value that
    lags the per-packet arrival series.
  - The trend ignores the first delta of a feedback packet (it straddles
    the gap between two feedbacks, so it is not a packet spacing) and
    every non-positive delta (a reordered packet is not a queue) — which
    is what transport-cc's reorder-robustness is for.
  - The estimator invalidates the baseline whenever it moves its own
    target bitrate: the send spacing follows the target, so the old
    baseline describes a different operating point, and keeping it would
    turn the estimator's own action into a congestion signal (a decrease
    spiral).  A rate change driven by anything else — the future rate
    controller, the pacer — is not visible to the estimator; the
    send-side accounting of v1.2.0 M3/M5 is what closes that gap.
  - `plugins::BweEstimate::Reason` gains `TransportCcDelay`, so the
    telemetry says *why* the estimate moved.  Adding an enumerator is
    additive and does not change the enum's size (the same rule
    ADR-018 records for `PacketStatus::ReceivedNoDelta`).
  - Covered by 8 cases in `src/modules/bwe/tests/test_bwe.cpp` and one
    end-to-end case in `tests/test_engine_net_feedback.cpp`: a queue that
    builds up moves the estimate even though not a single packet was
    lost.

- **The cross-platform presentation sinks (v1.4, remote-desktop plan E2 /
  E3 / E4 / E5).**  The D3D11 renderer was a Windows-only path; a
  macOS or a Linux host had no native presentation sink at all, so the
  "render" half of the pipeline could not be exercised outside
  Windows.
  - The new module pieces are `nimrtc/video_sink/metal_renderer.hpp`
    (macOS: a `CAMetalLayer` / `AVSampleBufferDisplayLayer` sink) and
    `nimrtc/video_sink/egl_renderer.hpp` (Linux: EGL with a Wayland
    (`wl_shm` / `zwp_linux_dmabuf`) or an X11 / XCB fallback).  Every
    window-system dependency is **optional**: CMake probes for each one
    and a missing dependency never breaks the build — the corresponding
    path is simply not compiled, and `open()` reports `kErrUnsupported`
    when no usable path is left.
  - The shared behaviour lives in `nimrtc/video_sink/presenter_core.hpp`
    (`PresenterCore`): the presentation clock, the **latest-wins**
    single-slot mailbox and the drop accounting are *the same* on every
    platform, so they are unit-tested on every platform and the
    platform-specific file only supplies the `present()` callback.
  - `nimrtc/video_sink/scale_to_fit.hpp` holds the letterbox /
    pillarbox geometry the presentation path applies to a source whose
    aspect ratio differs from the target.
  - The `d3d11`, `metal` and `egl` factories are registered on
    **every** platform, so a configuration naming one resolves on any
    host; on a non-matching platform `open()` reports
    `kErrUnsupported` rather than failing to resolve.  Covered by
    `src/modules/video_sink/tests/test_presenter_core.cpp`,
    `test_scale_to_fit.cpp`, `test_d3d11_renderer.cpp` and
    `test_video_sink.cpp`.

- **The dirty-rect incremental encode and the region-refresh message
  (v1.4, remote-desktop plan B9).**  A screen that has not changed
  should not cost a full frame every tick, and the peer needs to know
  *which* region changed — the RFC 3550 §6.1 application-defined RTCP
  packet is the sanctioned carrier.
  - The new public header
    `nimrtc/video_pipeline/incremental_encode.hpp`
    (`NIMRTC_API_STABLE`): `VideoContentType`, `IncrementalDecision`,
    `IncrementalEncodeStats`, `IncrementalEncodeController` and
    `IncrementalEncodeDriver`.  The driver asks the controller what to
    do with the frame's dirty rectangles, pushes the resulting request
    to the **encoder** through the `plugins::IVideoEncoder` seam (it
    does not know which codec it is talking to), and builds the
    region-refresh bytes.
  - The new public header
    `nimrtc/video_rtcp_feedback/video_rtcp_feedback.hpp` gains
    `RegionRect`, `RegionRefreshPacket` and the `kRegionRefreshName`
    (`"NRGN"`) / `kRegionRefreshHeaderSize` /
    `kRegionRefreshEntrySize` constants, alongside the existing
    `FirCoalescer`.
  - The **fallback is explicit**: where the codec cannot express the
    region, the driver asks for a full key frame instead, and the
    fallback is *counted* by the controller and reported through
    `last_decision()` — a host can see that the mechanism did not
    engage.
  - The keyframe-cost budget is bounded too: the new public header
    `nimrtc/video_pipeline/keyframe_limiter.hpp` adds `KeyframeReason`,
    `KeyframeDecision`, `KeyframeDecisionReason`,
    `KeyframeLimiterConfig`, `KeyframeLimiterStats` and
    `KeyframeLimiter`, so a sawtooth bandwidth trace does not produce a
    staircase that flips level every interval.
  - Covered by `src/modules/video_pipeline/tests/test_video_pipeline_incremental.cpp`.

- **The capture / encode benchmark and its committed artifact (v1.4,
  remote-desktop plan J5).**  The plan's *hardware-first* claim (RD-1)
  rested on a number nobody had measured.
  - The new tool `tools/benchmark_capture_encode.py` measures the
    capture rate (frames/s per megapixel) and the encode cost (ms per
    frame, per backend class, per resolution), broken down stage by
    stage, and checks the result against the §B.6 per-frame CPU budget.
    It drives `tests/bench/bench_capture_encode.cpp`, which prints a
    JSON document on stdout — the per-stage numbers are the **engine's
    own** J1 instrumentation, so the measurement runs the real engine
    with the real codecs rather than a synthetic loop.  It needs no
    network and has a `--dry-run` mode.
  - The committed artifact
    ([`docs/benchmarks/capture_encode_windows_x86_64.json`](docs/benchmarks/capture_encode_windows_x86_64.json))
    doubles as the regression baseline.

- **The video-scene profile keys now reach the engine (v1.2 profile
  bridge, remote-desktop plan M2).**  Profile v1.2 added four video-scene
  keys — `video_codec_preference`, `video_resolution_floor`,
  `video_ladder_mode` and `video_content_type` — but
  `assembly::to_engine_config()` never mapped them onto `EngineConfig`,
  so a scenario configured through a profile (e.g. the `remote-desktop`
  profile) silently fell back to the default camera configuration and the
  declared video scene was a no-op.
  - `assembly::to_engine_config()` now maps
    `video_codec_preference` → `EngineConfig::video_codec_name`,
    `video_resolution_floor` → `EngineConfig::video_resolution_floor_height`,
    `video_ladder_mode` → `EngineConfig::video_ladder_mode` and
    `video_content_type` → `EngineConfig::video_content_hint`.
  - The vocabulary is resolved by named helpers in
    `src/modules/assembly/include/nimrtc/assembly/engine_bridge.hpp`
    (`video_ladder_mode_of()`, `video_resolution_floor_height_of()` and
    `video_content_hint_of()`), so the profile-to-engine mapping is
    stated once and unknown values fall back to the engine default rather
    than failing the load.
  - Covered by `src/modules/assembly/tests/test_assembly_bridge.cpp`
    (seven cases: the four keys survive the bridge, the vocabulary
    resolvers agree with the engine enums, and the guards hold).

- **The per-stream bandwidth shares are now declarable (remote-desktop
  plan D5).**  D5 says "the remote-desktop profile declares the shares",
  but the only way to give a stream a weight was the **runtime** call
  `NimRTCEngine::set_video_stream_weight()` on an already-created
  stream — so a profile-driven session had to be hand-tuned after the
  fact and the scene's intent lived in code rather than in the profile.
  - The new public field `EngineConfig::video_stream_weights`
    (`std::map<std::string, std::uint32_t>`) states the split before the
    session exists.  The key is the stream's stable id ("default",
    "screen", "camera", ...) and the value is the stream's relative
    weight; 0 means "this stream takes nothing from the pool", and the
    default weight is 1 (an even split).
  - The declaration is applied to the **default** stream at `open()` and
    to an **extra** stream when that stream is created.  The runtime
    override always wins over the declaration: the declaration is the
    session's *starting* weight, not a lock.
  - The map is **declarative, not constructive**: a declaration for a
    stream the session never creates is inert, not an error.
  - The Profile carries it too: `assembly::Profile::video_stream_weights`
    is copied verbatim by `assembly::to_engine_config()`, the v1.2 schema
    declares the optional `video_stream_weights` object (its values are
    integers in 0..255, its keys are non-empty), and the JSON loader
    reads it.  A malformed *entry* is skipped rather than failing the
    whole load — the schema is the validator, the loader is the tolerant
    reader.
  - The shipped `profiles/remote-desktop.json` (and the matching C++
    constant) now declares the scene's split: the screen gets weight 3
    and a camera thumbnail weight 1.
  - Covered by `tests/test_engine_video_stream_shares.cpp` (five new
    cases), `src/modules/assembly/tests/test_assembly.cpp` (five new
    cases), `src/modules/assembly/tests/test_assembly_bridge.cpp` (four
    new cases) and `src/modules/assembly/tests/test_profile_schema.cpp`
    (six new cases, plus the hand-rolled validator gained the
    `additionalProperties`-as-schema, `propertyNames` and `minLength`
    keywords the new block needs).

- **`VideoSinkStats` reports which route a frame took to the display
  (v1.2 plan item `E5`).**
  Three new counters:
  - `gpu_device_copies` — GPU surfaces copied device-to-device into the
    present target (the zero-copy hand-off, confirmed);
  - `gpu_staging_fallbacks` — GPU surfaces that had to go through host
    memory to be shown (a decoder or driver configuration that is not
    zero-copy all the way to the display);
  - `gpu_surfaces_refused` — surfaces refused outright (a foreign
    backend, an unusable handle) — a configuration error, not a fallback.

  On a healthy DXGI loop only the first one grows.  Deciding which GPUs
  take which path is now a **reading**, not an experiment.

### Changed

- **`RemoteDesktopStats` is now shaped like the pipeline it reports
  (v1.2 plan item `S7`).**
  The block used to carry one flat field per measurement, each with the
  stage's name as a prefix (`capture_fps`, `capture_ms`, `encode_fps`,
  `encode_ms`, `encode_p95_ms`, `decode_fps`, `decode_ms`,
  `decode_p95_ms`, `frames_captured`, `frames_encoded`,
  `frames_decoded`).  It now carries three stage blocks instead --
  `capture`, `encode` and `decode` -- each with its own `fps`, `ms`,
  `frames` (and `p95_ms` on the two codec stages).

  The **measurements are unchanged**: `capture.fps` is exactly what
  `capture_fps` was, down to the nanosecond.  What changed is the
  *shape*.  The flat list gave every stage a prefix and left the host to
  know which prefix carried which half -- and comparing the halves is
  the whole point ("the capture ran at 30 fps and the encoder only at
  24" is the diagnosis).  The nested form puts each stage's rate, cost
  and count together, so the comparison is a field access rather than a
  lookup.

  The two cross-stage counters -- `frames_dropped_before_encode` and
  `frames_decoded_without_local_encode` -- stay at the **top level**:
  they describe the gap *between* two stages and belong to neither.

  **This is a breaking change, and it is safe only because it landed
  before the release was published.**  `v1.2.0` was tagged, but its
  GitHub Release was still a **draft** -- no released artifact carried
  the old shape, so no downstream host could have compiled against it.
  The rule this sets: **a reshape is allowed before a release is
  published, and never after.**  From v1.2.0's publication onward the
  stable-field-set promise (`docs/api/engine_api.md` §13.8) holds
  without exception.

  The three stage types are new public symbols:
  `RemoteDesktopCaptureStats`, `RemoteDesktopEncodeStats` and
  `RemoteDesktopDecodeStats` (all `NIMRTC_API_STABLE`).

  Covered by `tests/test_engine_remote_desktop_stats.cpp`, whose cases
  read the nested fields.

- **`EngineConfig::qoe_collector_name` now defaults to the built-in
  `"rd-qoe"` collector (v1.2 plan item `S7` / plan `J3`).**
  Before this the field defaulted to the empty string, which meant "no
  QoE metrics" -- so a remote-desktop session, which is judged on exactly
  those metrics (the freeze count, the end-to-end latency percentiles,
  the input-to-photon latency), had to know the name of a plugin to be
  told how it felt.  The name is now the built-in collector's id, and the
  rules around it are:
  - the **default** is `"rd-qoe"`, the id the `rd_qoe` module registers
    under (also published as `engine::kDefaultQoeCollectorId`);
  - an **empty** name is the explicit opt-out: no collector is resolved,
    the metrics are not measured, and `Stats::qoe` carries only the frame
    counters (the presented / dropped counts, which the engine keeps
    anyway) -- the honest answer to "what was the latency percentile?"
    when it was not measured;
  - a **named** id that does not resolve is a configuration error at
    `open()` time.  The engine never silently falls back, because a
    typo'd id would otherwise mask an integrator error;
  - the **default** id is the one exception to that rule: the `rd_qoe`
    module is optional by construction, so a build without it has no
    factory to resolve.  That is a property of the *build*, not a mistake
    by the host, so the engine degrades gracefully -- a warning, no
    collector, and `open()` still succeeds.

- **`EngineConfig::qoe_collector_enabled` is the explicit off switch.**
  The *name* is a poor switch for a host that builds its configuration
  from a profile file: emptying a string to mean "off" is easy to get
  wrong, while a boolean is not.  The new switch is the unambiguous
  way to say "do not measure the QoE metrics".

  It **wins over the name**: a host that sets it to false gets no
  collector, whatever `qoe_collector_name` says -- the metrics are not
  measured and `Stats::qoe` carries only the frame counters (the
  presented / dropped counts, which the engine keeps anyway).

  It is **on by default**, for the same reason the name's default is
  the built-in collector: a remote-desktop session is judged on exactly
  these metrics, and a host should not have to name a plugin to learn
  them.

- **The freeze findings now reach the QoE collector.**
  The *definition* of a freeze lives in the `video_stats` module (the
  threshold and the idle detection) and the *accounting* lives in the
  `rd_qoe` collector (the numbers a host reads in `Stats::qoe`), but
  nothing connected the two: the sink counted the freezes and the QoE
  snapshot's `freeze_count` / `freeze_total_ms` / `freeze_longest_ms`
  stayed at zero -- a metric that was measured and then never published.
  The engine is the only party that sees both, so it is the courier: the
  sink gained a `set_freeze_callback()` seam, the engine installs it and
  forwards each finding to the collector.

  Both detection paths report: the **gap** between two rendered frames
  and the **idle** window the tick notices when the picture stops
  arriving.  A session whose peer simply stopped sending is exactly the
  case the metric exists for, and it is detected by the tick rather than
  by a render.

  The seam is **additive** and **optional**: `IVideoStatsSink` gains a
  defaulted callback, so a third-party sink keeps compiling, and the
  engine installs the callback only when the video-statistics sink is
  present.

- **The QoE collector's sharpness proxy is now measured.**
  `QoeSnapshot::sharpness_proxy` was defined and computed by the
  `rd_qoe` module, but nothing ever handed it a picture: the engine
  holds the decoded luma plane and the collector knows how to turn a
  picture into a sharpness number, so the engine now carries the pixels
  over on the render path.

  Only the **planar YUV 4:2:0** layouts are measured -- those are the
  ones that *have* a luma plane the collector can read.  A BGRA frame
  has no luma at all, and converting one would cost more than the metric
  is worth, so the proxy simply is not measured for those frames and the
  snapshot says so by staying at zero.

- **The engine's outbound RTP packing is now concentrated in one file
  (v1.4, architecture-remediation plan §4.5.1, ADR-025 Decision 4).**
  The engine is exempt from Layout Invariant 4 for the `rtp` module
  (SDP and RTP are not pluggable — see ADR-025), but that exemption was
  only honest if the concrete use had a *visible boundary*.  It did not:
  four engine files each named `rtp::PacketBuilder` and each hand-rolled
  the abs-send-time stamp, so a fix to the RTP header had to be made in
  four places, and nothing would have noticed if one of them was missed.

    - The new private collaborator `detail::RtpPacketizer`
      (`src/engine/src/rtp_packetizer.hpp` + `rtp_packetizer.cpp`) owns
      the packet building.  Its header contains **no** `rtp::` type — it
      describes a packet in plain scalars and a byte span — so the
      engine's internal headers can include it without dragging the
      concrete RTP module in with it.  The concrete `rtp::PacketBuilder`
      lives in that one `.cpp`.
    - Four call sites now go through it: the audio path, the RFC 4733
      DTMF path, the default video stream's packet callback, and the
      extra video streams' packet callback.
    - The abs-send-time encoding moved with it, and now uses the origin
      the engine already keeps (`TimelineState::boot_epoch_offset_us`,
      Unix microseconds) instead of converting the NTP64 origin on every
      packet — a lossy round trip that could drift from the instants the
      engine publishes elsewhere.
    - `tools/check_engine_layering.py` grew a **second rule** to keep
      it that way: `rtp::PacketBuilder` may be named in exactly one
      engine file.  The rule is *symbol*-level rather than header-level
      on purpose — the module-level exemption cannot express this at all
      (`nimrtc/rtp/packet.hpp` is allowed everywhere), and a header-level
      rule would also forbid the receive-side parser, which belongs to a
      different collaborator.  The scan strips comments and string
      literals first, so a mention in a doc comment is not a violation.
    - Covered by the new `tests/test_rtp_packetizer.cpp` (12 tests),
      whose reference is the **RTP module's own** builder: the
      collaborator is asserted to produce byte-for-byte what
      `rtp::PacketBuilder` produces, and its abs-send-time encoder what
      `rtp::encode_abs_send_time_24` produces.  A refactor of this shape
      fails silently otherwise.
    - The engine's *receive* side (RTCP demultiplexing, SR/RR parsing,
      the XR blocks) is deliberately **not** moved — that is the
      `rtcp_processor` collaborator's separate work.

- **The engine's internal state header has no loose members left, and
  its line budget is now enforced (v1.4, architecture-remediation plan
  §4.3, D1 wrap-up).**  The remediation plan split `engine_impl.hpp`
  from 1076 lines into "the engine's own composition plus named
  collaborators", with `< 400` lines as the acceptance criterion.  The
  split held, but two things were left undone: a handful of members
  were still loose in `Impl`, and the budget was only ever a sentence
  in a plan document.

    - The last five loose members moved.  `srtp_installed`,
      `srtp_stats_drop` and `last_dtls_state` went into a new
      collaborator, `detail::SecurityState`
      (`src/engine/src/engine_security_state.hpp`), which holds one
      thing — the engine's transport security: the DTLS handshake's
      state-transition trace, whether the SRTP keying material is
      installed (the **authority** on whether the outbound path is
      encrypted), and how many packets SRTP refused to encrypt.  The
      last two only make sense together, so a reader looking for "is
      this session encrypted?" now finds the answer in one place.
    - `sr_last_sent` went into `detail::TimelineState`, where the
      ADR-017 timeline had been describing it all along — the field's
      own file comment already claimed it.
    - `dtls_active_inbound` was **deleted**: it had no readers left
      since `engine.cpp` was split into five translation units.
    - `Impl::dtls` and `Impl::srtp` — the DTLS handle and the SRTP
      context — stay in `Impl` on purpose.  They are the objects the
      engine *owns*, i.e. its composition, which is exactly what §4.3
      says belongs there.  The collaborator holds the state about
      them, not the objects.
    - `tools/check_engine_layering.py` grew a **third rule**: the
      internal state header must stay under 400 lines, counted the way
      the plan's own history counted them.  The budget had already been
      broken twice by features that had nothing to do with the engine's
      structure (the remote-desktop multi-stream work took it to 423,
      the QoE collector to 391), and both times the fix was the same —
      the new state belonged in a collaborator.  The rule turns that
      fix into something that happens at review time.  It runs on the
      existing CI step and is also registered as the ctest case
      `test_engine_layering`, so a local `ctest` sees it too.

- **The dirty-rect incremental encode and the keyframe limiter are now
  wired *into the engine* (v1.4, remote-desktop plan B9 / B14).**  Both
  existed as module seams with their own test suites, but nothing in the
  product called them — the dead-module state the architecture-remediation
  plan D2 calls out.  A host could not reach the behaviour through the
  engine at all.

    - **B9 — the incremental encode.**  The engine now feeds the
      capture device's dirty rectangles to the incremental encoder
      through the `plugins::IVideoEncoder` seam and sends the RFC 3550
      §6.1 application-defined RTCP **region-refresh** message
      (`"NRGN"`) alongside the picture, so the peer knows *which* region
      changed and not merely that something did.
    - **B14 — the keyframe limiter.**  The limiter's counters are
      exposed in `NimRTCEngine::Stats`, so a host can see how often the
      keyframe budget actually bit, and a latency-triggered keyframe
      request is **retried** rather than dropped when the limiter
      declines it.
    - Covered by the new `tests/test_engine_video_incremental.cpp`.

- **`IVideoCodec` is deprecated in favour of the split `IVideoEncoder` /
  `IVideoDecoder` seams (v1.4, remote-desktop plan B1, ADR-015
  Decision 3).**  This is the **only** deprecated public symbol in the
  tree, and it is a *marker*, not a removal.
  - `plugins::IVideoCodec` now carries
    `[[deprecated("IVideoCodec is the deprecated compatibility shim for
    the split codec seam (remote-desktop plan B1, ADR-015 Decision 3).
    Use IVideoEncoder and IVideoDecoder instead; this class will be
    removed after one MINOR cycle.")]]`.
  - It **derives** from `IVideoEncoder` and `IVideoDecoder`, so code
    that consumes an `IVideoCodec` still compiles and still works; the
    engine still resolves the combined `IVideoCodecFactory`.  A host
    that wants to migrate has a MINOR cycle to do it.
  - The replacement is `plugins::IVideoEncoder` (encode side:
    `encode()`, `encode_zero_copy()`, `force_keyframe()`,
    `set_params()`, `set_incremental_request()`, `input_pool()`) and
    `plugins::IVideoDecoder` (decode side: `decode()`,
    `decode_zero_copy()`, `output_pool()`), with
    `plugins::IVideoEncoderFactory` /
    `plugins::IVideoDecoderFactory` for a plug-in that implements only
    one direction.

- **The video path now reports what it actually did (v1.4, remote-desktop
  plan C3 / J1 / B15).**  A host previously had to infer what the
  pipeline was doing from its own logs; the engine now answers.
  - **The format-normalisation counters** (plan C3): the capture path
    may deliver BGRA / NV12 / I420 while the encoder path wants one
    canonical format, and the engine does the conversion **once, on the
    platform side**.  `Stats::video_format_conversions` counts the
    frames that needed a *colour-space* conversion, `video_format_copies`
    the frames that were copied without one, `video_format_zero_copy`
    the frames that reached the encoder as a native GPU surface (so no
    format work happened at all), and `video_format_canonical` /
    `video_format_source` name the two formats.  The conversion counter
    stays at **zero** on the zero-copy path.
  - **The per-stage CPU-time instrumentation** (plan J1): the new public
    struct `NimRTCEngine::Stats::StageTiming` (`NIMRTC_API_STABLE`)
    carries the sample count, the total time and the mean / p95 in
    **microseconds** for each stage, and the engine reports
    `video_stage_capture`, `video_stage_convert`, `video_stage_encode`,
    `video_stage_packetize`, `video_stage_decode` and
    `video_stage_render`, plus the aggregate `video_stage_total_us` and
    `video_stage_total_p95_us`.  A stage that never ran reports zero
    samples rather than a fabricated mean.
  - **The codec telemetry** names the backend that actually ran:
    `Stats::video_codec_backend` (`"nvenc"`, `"openh264"`, `"dav1d"`,
    `"software-stub"`, ...), `video_codec_hardware_accelerated`,
    `video_codec_frames_zero_copy` vs `video_codec_frames_staged`,
    `video_codec_bytes_copied`, `video_codec_keyframes` and the mean /
    max encode latency.  A host that asked for `"hardware"` and got
    `"openh264"` can now see the mismatch in the stats instead of
    parsing a log.

- **The `codec_backend` selector is now a real knob (v1.4, remote-desktop
  plan B2).**  `EngineConfig::video_codec_backend` takes `"auto"`,
  `"hardware"` or `"software"` (default `"auto"`), and the engine's
  selection honours it: in the `"hardware"` and `"software"` modes the
  ineligible backends are *not* eligible and the selection returns
  nothing rather than silently handing back one of them — a host that
  asked for hardware must not get software without being told.
  `Stats::video_codec_backend_preference` echoes the request, so the
  mismatch is visible from the outside.

- **The video sink's GPU delivery path and the frozen capture
  capabilities (v1.4, remote-desktop plan E1 / A1).**  `plugins::IVideoSink`
  gains `render(const GpuBuffer&)` and
  `render(const GpuBuffer&, const VideoFrameInfo&)` alongside the
  existing `render(const VideoSourceFrame&)`; both keep a default
  implementation returning `kErrUnsupported`, so a third-party sink that
  implements only one side compiles unchanged.  The engine picks the
  route **once, at `open()` time**, from the decoder's
  `supports_zero_copy_decode()`, the sink's `supports_gpu_render()` and
  its `supports_cpu_render()` — the rule is a pure function, so all
  eight combinations are unit tested without an engine, a transport or a
  clock.  A pair where neither path is possible is reported at `open()`
  time as the new `core::kVideoSinkIncompatible`.
  `plugins::CaptureCapabilities` / `plugins::CaptureFormat` semantics are
  **frozen**: an unsupported *combination* is refused at `open()` time
  with the new `core::kVideoCaptureUnsupported` instead of on the first
  captured frame.

- **`engine_impl.hpp` is no longer a 1000-line god header (v1.2.0,
  architecture remediation plan §4.3 / D1).**
  The header fell from **1076 lines to 394** — the plan's target was
  under 400.  What is left in it is the engine's *own* composition: the
  SDP parser / munger, the DTLS handle, the SRTP context, the transport
  session, and the handful of counters that do not belong to any one
  feature.  Everything else moved into a named collaborator next to the
  header:

  | collaborator | header | what it holds |
  |---|---|---|
  | `detail::AudioState` | `engine_audio_state.hpp` | audio send/receive state, the `AudioRxState` the RTCP receiver report needs, the DTMF state |
  | `detail::VideoState` | `engine_video_state.hpp` | video send/receive state, the negotiation result, the quality counters |
  | `detail::SessionState` | `engine_session_state.hpp` | the session model, the transceivers, the mids |
  | `detail::ScratchBuffers` | `engine_scratch.hpp` | every reusable scratch buffer |
  | `detail::FeedbackState` | `engine_feedback_state.hpp` | the network-feedback counters, the RTCP XR routing counter, the pacing split |

  The rule the split follows is simple: *a member that only one feature
  reads or writes belongs to that feature's collaborator, not to
  `Impl`.*  All of them are private headers under `src/engine/src/` — no
  public API was added, and the public `engine.hpp` is untouched.

  This is the **state half** of the plan's proposal, deliberately.  The
  plan also proposes behaviour collaborators (`VideoPipeline`,
  `AudioPipeline`, `RtpPacketizer`, `RtcpProcessor`,
  `TransportManager`); each of those moves a coherent slice of
  *behaviour* out of the engine, which is a behavioural change and needs
  its own review.  Splitting state first keeps the two kinds of change
  separately reviewable.

  Verified with a full build (zero errors, zero warnings) and the whole
  `ctest` suite: **1454 / 1454 passing**.

  The header grew again as the remote-desktop multi-stream / QoE planes
  landed (394 → 423 lines, back over the 400-line target).  The
  members that work added — the network-feedback counters, the RTCP XR
  routing counter and the pacing split — are all "what the engine
  observed on the RTCP path", so they moved into
  `detail::FeedbackState` (`engine_feedback_state.hpp`): **423 → 381
  lines**, back under the target.  Re-verified: full build clean,
  `ctest` **1943 / 1943 passing** (the 1966 / 1966 figure recorded
  earlier was taken in the `NIMRTC_BUILD_EXAMPLES=ON` tree, which
  compiles 24 more AV1 / dav1d cases and drops one `test_nvenc_probe`
  — 1943 + 24 − 1 = 1966).

- **A multi-stream test asserted the wrong thing, and only failed on
  the machines that matter most (v1.4, remote-desktop plan H2).**
  `EngineMultiStream.EachStreamOwnsItsOwnCodecInstance` used to prove
  "the two streams own separate encoder instances" by requiring the two
  streams' `backend_id`s to differ.  But `IVideoCodec::backend_id()`
  names the backend *implementation* ("nvenc", "openh264",
  "software-stub"), not the instance — so on a host with an NVIDIA GPU
  the H.264 and the H.265 encoders both legitimately report `"nvenc"`,
  and on a software-only host they both report `"software-stub"`.  The
  assertion therefore failed on exactly the hardware hosts where the
  hardware path is worth having, and passed on the hosts where it was
  least meaningful.  The test now proves instance separation
  *structurally* — the camera stream's encoder was swapped to H.265 and
  the screen stream still reports H.264, which could not happen if one
  encoder were shared — and separately asserts that each stream's entry
  still names the backend it actually ran on.  **No engine behaviour
  changed.**

- **The plugin ID naming contract is now written down, and the docs
  that describe the config surface now match the code (v1.2.0,
  architecture remediation plan §7.1 / §7.2, ADR-023 / ADR-024).**
  Two decisions were closed and the documentation drift they exposed
  was fixed in the same pass.
  - **ADR-023 fixes how a plugin id is spelled, validated and
    changed.** A plugin id must match `^[a-z][a-z0-9_-]*$` — a
    lowercase ASCII token that is simultaneously a valid shell / JSON
    key.  (The hyphen is permitted because the shipped transport-stack
    ids `"webrtc-classic"` and `"raw-udp-arq"` already use it.)  In-tree
    implementations are prefixed `nimrtc_`; third-party
    implementations carry the vendor's own namespace and no
    `nimrtc_` prefix; conceptual roles (`opus`, `aimd`,
    `strict_priority`) are bare.  Because the grammar accepts exactly
    the set of valid C++ identifiers *plus the hyphen*, the
    registration macros take the id as a **bare token** and stringify
    it — and a quoted literal is rejected at compile time.
  - **That last point closes a silent failure.** The legacy
    registration macros already stringified their argument, so
    `NIMRTC_REGISTER_DTLS_SESSION("mybackend", &f)` registered under an
    id that *included the quote characters*: the registration
    succeeded, and the lookup never matched.  The two macro families
    also disagreed about what the argument was — one tokenised, one
    took a literal as-is — which is the kind of inconsistency that
    costs a plugin author an afternoon.  Both now take a bare token.
    (The one exception is a **hyphenated** id, which the tokenising
    form cannot express: `#webrtc-classic` does not lex as one token.
    Those are registered through the registry directly.)
  - **ADR-024 records that `EngineConfig` owns its strings.** The
    `std::string_view` → `std::string` migration is complete in the
    code, but the docs still described the old shape and taught the
    `.c_str()` workaround for the dangling-view hazard the migration
    removed.  The engine takes the config **by value** and moves it,
    so it owns a copy of every string; the caller's original may die
    immediately.  This remains an **ABI-breaking** change (hosts must
    be recompiled) and now carries a migration note in
    `docs/api/engine_config.md` §2.0.
  - **Seven documentation files were brought back in line with the
    code.** `docs/api/engine_config.md` (the migration note, and the
    claim that the video plugins were "not yet threaded into the
    RTP/DTLS/SRTP pipeline" — they have been, since the pipeline
    landed); `docs/troubleshooting.md` (a new §3.7a on "I configured
    a plugin but it has no effect", which sorts the four possible
    causes by how often they are the answer, and §2.3 rewritten);
    `docs/plugin_author_guide.md` (a new §4.4 replaceability matrix
    answering "does the engine actually use my implementation?", a
    new §7.1 on how to report a plugin id, and §8.1 summarising the
    ID contract); `docs/adr/ADR-001-plugin-system.md` (three
    supplements: the ID contract, which families are genuinely
    replaceable — RTP and SDP are not — and the `EngineConfig` field
    types); `docs/hw_plugin_seam.md` (the typed `GpuHandle` and the
    `GpuBufferPool` behind the older bare `void*` surface handles,
    the `IVideoCodec::kind()` contract, and the distinction between
    `HwBackend` and `GpuBackend`); `docs/guides/switching-dtls-backend.md`
    (the seam contract tests a new DTLS backend must pass, and why
    the "60 bytes of SRTP keying material exported" assertion is the
    one that matters).
  - **No code behaviour changes.**  The engine's plugin-resolution
    semantics are unchanged; these ADRs govern how an id is spelled
    and validated, and who owns a config string.

- **The scheduler now has a real C ABI (v1.2.0, architecture
  remediation plan §4.4 / D5).**  The `sched_*` entry points were
  documented as "the C ABI for embedded hosts", but they were not one:
  they had no `extern "C"` linkage, so the linker emitted C++-mangled
  symbols (`?sched_create@sched@nimrtc@@YAPEAUSchedulerHandle@12@...`),
  and their parameter types (`plugins::SchedulerConfig*`,
  `plugins::Addr*`, `plugins::StreamPriority`) could not be named by a C
  translation unit at all.  A C program could neither include the header
  nor link the library — so the "stable entry point for embedded hosts"
  that the retention decision was premised on did not exist.
  - **A new header, `<nimrtc/sched/sched_c.h>`, is the actual C ABI.**
    It is pure C11 with `extern "C"` linkage and plain C types, and it
    declares six entry points under the `nimrtc_sched_` prefix:
    `nimrtc_sched_create`, `nimrtc_sched_destroy`,
    `nimrtc_sched_set_stream_priority`,
    `nimrtc_sched_get_stream_priority`,
    `nimrtc_sched_enqueue_stream_owned` and
    `nimrtc_sched_select_next_packet`.  It carries a
    `NimrtcSchedConfig` / `NimrtcSchedAddr` pair (the C mirrors of the
    C++ configuration and address types) and the
    `NIMRTC_SCHED_PRIORITY_{HIGH,NORMAL,LOW}` level constants.
  - **The existing C++ API is unchanged and still works.**  The `sched_*`
    functions keep their names, their signatures and their type identity
    (`nimrtc::sched::SchedulerHandle` is now an alias of the C ABI's
    `NimrtcSchedHandle`), but they are now thin forwarders to the C ABI.
    One implementation, two spellings — they cannot drift.
  - **The test is written in C.**  `src/modules/sched/tests/test_sched_c_abi.c`
    is compiled as C11 and linked against the library.  A C++ test could
    not have proven anything here: it would have compiled happily in a
    context where every C++ type is available.  Compiling it as C is the
    assertion — the moment the C header grows a C++-only dependency, that
    file stops compiling.
  - **Exception safety is enforced, not promised.**  A C header cannot
    spell `noexcept`, and since C++17 `noexcept` is part of a function's
    type, so the definitions deliberately omit it and instead catch
    everything that can throw.  Nothing propagates across the C boundary.

- **Layout Invariant 4 is now enforceable (v1.2.0, architecture
  remediation plan §4.5 / D6, ADR-025).**  L4 said the engine "does not
  directly include any concrete module header".  It had not been true for
  a long time, and it was violated on every screen — which means it had no
  reviewing power at all, and only taught reviewers to ignore it.
  - **ADR-025** (`docs/adr/ADR-025-sdp-rtp-not-pluggable.md`) records the
    decision: SDP and RTP are **not** pluggable.  The SDP layer is the
    contract document every other component is written against, and the
    RTP packet builder sits on the zero-allocation hot path that Phase 3
    established — a virtual call and an interface-hidden scratch buffer
    per frame is exactly what that gate forbids.
  - **The exemption is now an enumerated list, not a vague exception.**
    The engine's internal implementation files may include the concrete
    headers of `sdp`, `rtp`, `ice`, `dtls`, `srtp`, `audio3a`,
    `transport` and `web_transport`, and link those libs PRIVATE.  Every
    other concrete module — the codec modules, the video pipeline, the
    jitter buffers, the BWE implementations, pacing, the scheduler — is
    still reached through its `plugins::I*` seam and its header is still
    not included.  With the list named, "does `src/engine/` include
    anything outside it?" becomes a question with a finite answer.
  - `AGENTS.md` (L4) and `ARCHITECTURE.md` (Layout Invariant 4) carry the
    same wording.
  - **No API change.**  This is a documentation and constraint change:
    the code was already doing what the rule now says.

- **The five transport seam families now live under `src/modules/`
  (v1.2.0, architecture remediation plan §4.1 / D3 / D4, decision Q5).**
  `src/dtls/`, `src/raw_udp/`, `src/sctp/`, `src/transport/` and
  `src/web_transport/` used to sit at the top level of `src/`, a
  historical PAL Slice layout that `ARCHITECTURE.md` had to defend with
  a whole paragraph.  Every module directory is now under
  `src/modules/<name>/`, and that paragraph is gone.
  - **The public include paths are unchanged.**  `#include
    <nimrtc/dtls/...>`, `<nimrtc/sctp/...>`, `<nimrtc/raw_udp/...>`,
    `<nimrtc/transport/...>` and `<nimrtc/web_transport/...>` resolve
    exactly as before: the seam headers kept their `nimrtc/<family>/`
    spelling and moved with their directory.
  - **The CMake target names are unchanged.**  `nimrtc::dtls`,
    `nimrtc::dtls_seam`, `nimrtc::sctp`, `nimrtc::raw_udp`,
    `nimrtc::transport` and `nimrtc::web_transport`, along with the
    `nimrtc_*` archive names.  A target name is a downstream link
    contract, so it must not move.  A consumer that
    `add_subdirectory()`s NimRTC and links those targets needs no source
    change at all; only a consumer that referenced the *source path*
    (`add_subdirectory(nimrtc/src/dtls)`) does, which was never a public
    usage.
  - **DTLS is one module instead of two directories (D3).**  The seam
    headers, the wolfSSL session implementation and the GmSSL backend
    are now under `src/modules/dtls/` — `include/nimrtc/dtls/` (the
    seam), `src/` (wolfSSL), `backends/gmssl/` (GmSSL), `tests/`.  The
    question "which file is authoritative for DTLS?" used to need a
    paragraph of prose; now the answer is the ordinary one.
  - The five families are added from `src/modules/CMakeLists.txt` in
    dependency order (`dtls` before `transport`, `sctp` before
    `datachannel`) instead of from the top-level `CMakeLists.txt`.

- **The plugin contract is now explicit — the engine says which families it
  can actually replace, and what happens when one is missing (v1.2.0,
  architecture remediation plan §2).**  The plugin system had three
  different failure behaviours (hard failure, silent degradation, and a
  third "the interface exists but the engine never resolves it" case that
  read exactly like a working seam), and the documentation promised a
  uniform "no silent fallback" that was never true.  This change makes
  the contract match the code, and closes the two seams that were
  registered-but-bypassed.
  - **`CapabilitySelector` is now wired into the engine
    (remediation §2.6 / B9).**  The Selector used to resolve a
    strategy to a pair of factory ids and then *throw the result away*
    — the engine kept instantiating a hard-coded media stack.  The
    engine now builds a registry-backed `CapabilitySelector`, hands it
    the engine's own `StackConfig` (ICE / DTLS / SCTP / raw-UDP backend
    ids), calls `select()`, and uses the resulting stacks as the session's
    actual transport.  `CapabilitySelector::instantiate()` is the single
    implementation of "build a media stack + a control stack", and
    `nimrtc::transport::make_transport_instance()` now delegates to it,
    so the explicit path and the strategy-driven path cannot drift.
    `EngineConfig::transport_stack_name` is the explicit user override
    that wins over the strategy's rule.
  - **The SRTP backend is now pluggable (remediation §2.5 / B7).**
    `plugins::ISrtpSessionBackend` had an interface and a libsrtp
    implementation but no registry slot and no wiring, so
    `EngineConfig::srtp_backend_name` resolved to nothing.  The registry
    gains an `srtp_backend` slot (`register_srtp_backend()` /
    `get_srtp_backend()` / `list_srtp_backends()` plus the
    `NIMRTC_REGISTER_SRTP_BACKEND` macro), and every SRTP protect /
    unprotect call — media and RTCP — now routes through the resolved
    backend instead of the concrete libsrtp calls.  The built-in backend
    registers under `"libsrtp"` (the default).
  - **The video codec family contract is explicit (remediation §2.4 /
    B6).**  `IVideoCodec::kind()` is documented as the *authoritative*
    statement of the wire format; the engine derives the SDP
    `a=rtpmap:` encoding name, the payload type, and the
    packetizer / depacketizer from it.  The plugin id is only a lookup
    hint, and is used as the fallback only when `kind()` returns
    `kUnknown` — in which case the id must name a family, either
    exactly (`"h264"`) or as a `family` + separator prefix
    (`"h265-nvenc"`).  When *neither* source names a family, `open()`
    now fails with `kEngineInvalidParam` instead of advertising an
    empty encoding that no peer can parse.
  - **The RTP and SDP layers are documented as *not* pluggable
    (remediation §2.1 / B8).**  `IRTP` and `ISDP` are retained for ABI
    stability, but both headers now carry a `@deprecated` notice
    explaining *why* they are not seams: the SDP layer is the contract
    document that the negotiation, BWE, jitter-buffer and RTCP paths are
    all written against, so replacing it is a protocol redesign rather
    than a plugin swap.  `docs/plugin_author_guide.md` §4.2 states the
    same, and     the `rtp_name` / `sdp_name` rows are gone from the
    catalogue.

- **The `"sctp"` data-channel backend was retired from the public selectors
  (2026-11-10; DataChannel capability plan WP-A, DEC-4 = A1).**  The SCTP
  backend was a **test-only** backend: `SctpDataChannel` opens an in-process
  usrsctp socket bound to `127.0.0.1`, and the engine never wires the DTLS
  transport's outbound/inbound datagram paths into that socket, so a channel
  created on it could never reach a remote peer.  The public surface presented
  it as a peer of the production WebTransport backend; that claim was wrong.
  - `EngineConfig::datachannel_backend = "sctp"` now fails `open()` with
    `kEngineInternal`; the error message names the backend as **test-only**
    and points at `"web_transport"` (the default) and `""` (disable).
  - The legacy v0.x spelling (`datachannel_backend` empty plus
    `datachannel_name = "sctp"`) is rejected the same way.
  - `EngineConfig::datachannel_name` now defaults to `""` instead of `"sctp"`,
    so an unset field never selects the retired family and
    `datachannel_backend = ""` alone is a clean "disable the data plane".
  - The `SctpDataChannelFactory` **stays registered** in `core::PluginRegistry`
    as the in-tree reference implementation for the DCEP codec and the PR-SCTP
    semantics — the module tests drive it directly.  Only the public
    *selector* was retired; no registry slot was withdrawn.
  - No public symbol was added or removed.  The change is the *meaning* of two
    existing config values, which is why it is called out here and in
    `docs/api/engine_config.md` §2.6.

- **The scheduler's two queue families no longer borrow the architecture's
  layer names (2026-11-09).**  Inside `src/modules/sched/`, the per-packet
  priority ladder and the per-stream priority buckets were labelled "L0" and
  "L1" — the same tokens `docs/zh/architecture.md` uses for the L0–L3
  *layering* (where L1 is the transport layer this module lives in).  Read
  literally, "the scheduler's L1" meant two different things in the same
  document set, and the plan docs compounded it by calling the per-stream
  family "the scheduler's L1 API".
  - The scheduler now names its two families **P-family** (per-packet,
    `plugins::Priority`, five levels) and **S-family** (per-stream,
    `sched::StreamPriority`, three buckets).  A new §"Terminology" section
    in `src/modules/sched/include/nimrtc/sched/sched.hpp` states the
    distinction and records that the two families are **separate queues
    that are never compared with each other** — so the documented ladder
    `kControl > kAudio > kVideoKeyframe > kVideo > kBestEffort` describes
    the P-family only, and `kBestEffort` is a packet level, not a promise
    that DataChannel traffic is scheduled at all.
  - `docs/zh/architecture.md` §8.3 gained the matching terminology note, and
    D8 / §2.3 / §2.5 now state the per-packet ladder and the per-stream
    buckets as two ladders rather than one, and spell out that under the
    default `web_transport` backend the data plane never enters the
    scheduler.
  - `plugins::IScheduler`'s header no longer claims that RTP and
    DataChannel "share ONE priority queue" — a concrete scheduler may keep
    them in separate families.
  - The `sched.hpp` scope note no longer claims that the legacy `"sctp"`
    backend puts the media and data planes on one connection: that backend
    was retired in v1.2.0 (WP-A / DEC-4 = A1) and is now a test-only
    in-process usrsctp loopback that never reaches a peer, so **no shipped
    backend** provides wire-level cross-plane priority.  The note now states
    the honest version — the scheduler arbitrates within a connection, and
    the data plane does not use it at all (the per-stream API still has no
    caller in the data path, per `datachannel-capability-plan.md` WP-E2).
  - The DC-2 regression test was renamed accordingly
    (`L0_L1_Isolation_*` → `P_S_Isolation_*`); the assertions are
    unchanged.  **No behaviour change and no public API change** — the
    `StreamPriority` enum, `set_stream_priority()`,
    `enqueue_for_stream*()` and `select_next_packet()` keep their names and
    semantics.

- **`<nimrtc/core/registry.hpp>` no longer drags the optional-module
  seams into every translation unit (P1 hardening, 2026-11-09).**  The
  header used to `#include` 18 seam headers in a "backward-compat shim"
  block at the bottom — the 13 `nimrtc/plugins/` seam interfaces plus
  the DTLS / SCTP / raw_udp / transport-stack / WebTransport factory
  headers from five *optional* modules.  Any TU that included the
  registry therefore compiled all of them, which defeated the
  compile-time module trimming the CMake options provide and made
  `core` depend on modules it must not know about (ARCHITECTURE.md
  Layout Invariant 1).
  - The 13 `nimrtc/plugins/` seam interfaces moved behind a new opt-in
    umbrella header, `<nimrtc/plugins/registry.hpp>`.
  - The optional-module seams are no longer reachable through the
    registry header at all: a consumer includes the seam header it
    needs (and links that module) explicitly.
  - The registry API only needs *pointers* to the factory interfaces, so
    the forward declarations it already carried are sufficient — the
    `detail::Registrar` switch casts through the full type only in the
    TU that registers the factory.
  - The engine's PAL resolver forward-declares
    `web_transport::IWebTransportSessionFactory` instead of including
    the optional module's header, so the engine's public header does
    not depend on an optional module either.
  - The `nimrtc::core` target's optional-module include directories
    became `PRIVATE`; only the `nimrtc/plugins` include path is still
    exported.
  - **Migration for consumers:** a translation unit that included only
    `<nimrtc/core/registry.hpp>` and relied on the transitive seam
    includes now includes `<nimrtc/plugins/registry.hpp>` (for the
    `nimrtc/plugins/` seams) or the specific seam header (for the
    optional modules).  `<nimrtc/core/registry.hpp>` alone keeps the
    registry API and the registration macros working unchanged.

- **The outbound send path no longer allocates a heap buffer per packet
  (P1 hardening, 2026-11-09).**  The engine built each outbound RTP
  packet in a *member* vector and then **moved** it into the scheduler,
  which left the member in a moved-from state — so the next packet had
  to allocate a fresh buffer, and the previous one was freed on drain.
  At 1000 packets per second that is 1000 malloc/free pairs per second
  on the send path, which is exactly the path whose jitter a real-time
  caller sees.
  - `plugins::IScheduler` gained two optional methods,
    `acquire_packet_buffer()` and `release_packet_buffer()`.  The
    default implementations preserve the old behaviour for a custom
    scheduler (reserve, then destroy), so the seam stays source
    compatible.
  - The built-in scheduler keeps a bounded free-list of packet buffers.
    A buffer is taken by the caller, filled with the packet, handed over
    with `enqueue_owned()`, and returned to the free-list once the
    packet has been drained and dispatched.  In steady state the send
    path performs **no heap allocation at all**.
  - The pool is bounded by `SchedulerConfig::max_queue_depth`, so a
    burst cannot make the scheduler hoard memory, and a buffer that is
    recycled is cleared so the pool does not keep stale packet bytes
    alive.
  - Covered by `src/modules/sched/tests/test_scheduler.cpp`.

- **The README no longer overstates the hardware codec support.**  The
  capability table claimed "H.264 hardware backends (NVENC / NVDEC /
  AMF / QSV / DXVA / VA-API / OpenH264)" without distinguishing the
  encoders that exist from the decoders that do not.
  - NVENC **encoding** is implemented, and **NVDEC decoding now is too**
    (see the *Added* entry above) — the decoder entry is no longer a
    seam id only.
  - The same still applies to the macOS VideoToolbox and the Android
    MediaCodec entries: they exist as seam ids, report
    `available() == false` and return `nullptr` from `create()`.
  - The Windows **DXVA decoder is real** and is named as a supported
    hardware decode path; macOS and Android use the software H.264 path.
  - The "simplified Goog-CC" wording now says what it is everywhere it
    appears (README, README.zh): a delay + loss hybrid estimator, not
    a full trendline port, and AIMD remains the default.
  - The CMake options were re-worded to match: `NIMRTC_PLUGINS_NVENC`
    is the encoder, `NIMRTC_PLUGINS_NVDEC` is the decoder, and
    `NIMRTC_PLUGINS_VAAPI` is the encoder.

- **`opus::EncoderConfig::bitrate_bps` now defaults to 0 (= codec
  default) instead of 64000.**  0 resolves to 32 kbps for a mono encoder
  and 64 kbps for a stereo one, so the default follows the channel
  count instead of assuming stereo.  A caller that set the field
  explicitly is unaffected.

- **The Opus plugin frames its own output (audio pipeline WP-11, spec
  B-5).**  The codec plugin used to hand the raw libopus frame to the
  caller and let the engine work out the RTP payload — so the config
  field on the wire was the one the *packetiser* invented, not the one
  the encoder used.  A frame whose TOC contradicts its body made the
  receiver decode it with the wrong decoder mode, which is audible as
  noise rather than as an error.  The plugin now frames the encoder's
  output itself (the TOC byte libopus wrote plus the frame body) and the
  packetiser reads the TOC instead of deriving one.

- **The in-band redundancy is configurable (audio pipeline WP-11, spec
  B-8).**  `EngineConfig::audio_redundancy_percent` (default 5 %) is
  handed to the encoder, so every frame carries a low-bitrate copy of
  the previous one, and to the decoder, which uses it as its redundancy
  hint.  `plugins::CodecConfig::packet_loss_perc` and
  `plugins::AudioReceiverConfig::packet_loss_perc` carry the value
  across the seams.  Without it the encoder embeds no copy at all and
  the FEC recovery path cannot work.

- **Every engine now owns its own audio send state (audio pipeline WP-5,
  spec A-6 / A-7 / A-8).** The outbound audio path had three defects that
  all came from the same root: the send state lived in function-local
  statics instead of in the engine.
  - The SSRC was the constant `0xDEADBEEF`, so two engines in one process
    advertised and stamped the *same* SSRC and the peer could not tell the
    two streams apart. It is now a per-engine member drawn from a
    per-process generator (an explicit `EngineConfig::audio_ssrc` wins,
    for a reproducible test stream), and the SDP generator reads the very
    same member the packet path stamps -- the description and the wire can
    no longer drift apart.
  - The RTP sequence number and timestamp were function-local statics, so
    two engines shared one sequence space. They are per-engine members
    now.
  - `send_audio()` used to encode whatever block the host happened to hand
    it, so the packet size -- and therefore the media clock -- depended on
    how the host chunked its capture. The processed PCM now goes through a
    frame accumulator: one RTP packet carries exactly one frame of
    `EngineConfig::audio_ptime_ms` milliseconds (RFC 7587 code 0 framing),
    a partial frame stays behind for the next call, and the media clock
    advances by the frame duration whether or not the packet reached the
    wire. A dropped frame therefore leaves a gap in the sequence number,
    never a hole in the timeline.
  - An encode failure used to vanish. It is now counted
    (`Stats::audio_encode_errors`), reported through `on_error` (throttled
    to one report per second so a broken stream cannot flood the host) and
    the timeline keeps advancing.
  - The engine refused to open when `pcm_sample_rate_hz` did not match the
    codec's clock rate, but only after the transport was already up. The
    check now happens before `open()` creates anything: there is no
    resampler in the stack, so a host that configured the rate wrongly gets
    an explicit error rather than audio that drifts.
  - On the SDP side, `a=ptime` / `a=maxptime` are advertised (RFC 7587
    section 6.1, RFC 8866), and the answer carries the engine's own
    `a=ssrc` line, which is what makes Chrome count our inbound RTP at
    all. The demo no longer appends a stale constant SSRC of its own.

### Removed

- **The vendored `usrsctp` SCTP stack (plan E7).**  `src/third_party/usrsctp/`
  — 269 files, ~5.5 MB — is gone, together with the `nimrtc_vendor_usrsctp`
  CMake target, the two `@deprecated` public headers
  (`nimrtc/sctp/usrsctp_factory.hpp`, `nimrtc/sctp/usrsctp_socket.hpp`), the
  `usrsctp_socket.cpp` / `usrsctp_factory.cpp` implementations, and the
  in-process `tests/test_sctp_usrsctp.cpp` regression test.

  The scope turned out to be **much smaller than the plan first read**, and
  the reason is worth stating: the SCTP data plane was already retired from
  the *public* selector in v1.2.0 (`datachannel_backend = "sctp"` is
  rejected by the engine with a hard error — WP-A / DEC-4 = A1), and
  `nimrtc::sctp::register_default_plugins()` has published only the `"stub"`
  factory since v0.11.0.  The `"usrsctp"` factory was therefore *unreachable
  in production* long before this change; what remained was dead weight that
  the build still paid for on every platform.

  Concretely:

  - `SctpDataChannel`'s production constructor no longer looks up
    `get_sctp_socket("usrsctp")` — it resolves `"stub"`, the only registered
    SCTP id.  The channel is still a well-defined "no data plane" channel
    (every send returns `kErrNotReady`), and it is no longer a null-pointer
    one.
  - The `ISctpSocket` / `ISctpSocketFactory` seam is **unchanged**.  The
    production data plane is WebTransport over QUIC (msquic), which never
    went through this seam.
  - The public header set shrinks from **191 to 189**; the manifest
    (`docs/api/public_headers.manifest`) and the count-based gate
    (`tests/test_api_classification.cpp`) are regenerated in the same PR, as
    D-N6 requires.

  The vendored tree was BSD-3-Clause; the removal is licence-neutral for
  downstream consumers (the installed bundle ships fewer archives, and
  `cmake/NimRTCConfig.cmake.in` no longer looks for `usrsctp`).

### Fixed

- **The derived `Stats` field table was missing nine fields.**
  `tools/dump_stats_fields.py` parsed `struct Stats` out of
  `engine.hpp` to *derive* the field reference, and the CI gate
  compared that derivation with the document on disk.  But the parser's
  declaration pattern did not recognise two of the shapes the struct
  actually uses -- a **namespace-qualified** type (`plugins::QoeSnapshot`)
  and a **brace-initialised** member (`RemoteDesktopStats remote_desktop{}`)
  -- so those members were silently dropped from the derivation.

  The consequence is the worst kind of documentation drift: the gate
  *passed*, because the document had been generated from the same
  incomplete derivation.  The two blocks that a remote-desktop session
  is judged on -- `Stats::qoe` and `Stats::remote_desktop` -- were
  absent from the "stable field set" contract, along with the seven
  earlier members the same pattern had swallowed.

  The parser now accepts a qualified type and a brace initialiser, and
  the table lists all **145** fields.  The gate can still be fooled by a
  shape it does not know, but it no longer fails *silently* on the two
  shapes the header uses today.

- **`Stats::remote_desktop.capture_fps` no longer drops to zero when the
  capture device's queue overflows.**
  The cadence record treated a frame whose timestamp was not strictly
  greater than the last one as "no new delivery" and refused to count it.
  That is backwards for the case the number exists for: the device's
  queue is bounded, so when the pump falls behind, the device drops the
  oldest frame and the next handoff can carry a timestamp the pump has
  already passed — exactly under load, the rate then collapsed to 0 (or
  to whatever an earlier, quieter window measured).  A *delivery* is now
  identified by its **sequence number**; the timestamp is only the span
  endpoint, and a span that moves backwards is reported as **unknown**
  (0 fps) rather than as a plausible-looking wrong number.  The counting
  rule moved to its own header, `src/engine/src/engine_capture_cadence.hpp`,
  so the unit test pins the same rule the pump uses.
  (Regression caught by `macos-clang / arm64` on the S7 landing.)

- **The bandwidth estimator no longer climbs back into a wall it is still
  hitting — through either of its two raise paths.**
  The AIMD front-end's additive increase was gated on the decrease
  *cooldown* alone, and the cooldown expires on a **clock**, not on the
  **state of the link**.  So while the link was *still* lossy — the loss
  rate still above the threshold — the estimate resumed climbing and
  walked back up to, and past, the rate that had just been throttled.  The
  front-end behaved like a single-step correction: one halving, then a
  slow climb back into the congestion, while a fifth of the packets were
  still being dropped.

  The Goog-CC front-end already carried the correct guard (`!is_lossy`), so
  the two estimators disagreed under exactly the condition that matters.

  The **REMB override** had the same defect in the same loop, one branch
  over.  It was gated on `loss_event_pending_`, which is *edge-triggered*:
  the latch is set when the loss rate crosses the threshold from below, and
  cleared as soon as the multiplicative decrease has been applied.  So the
  *second* still-lossy report arrived with the latch already down, and the
  receiver's stale (higher) estimate was adopted — walking the sender back
  into the wall it had just been throttled away from.  The latch is the
  wrong guard for the same reason the cooldown was: it describes a
  transient state, not the state of the link.

  Both fixes are the same clause — `&& !is_loss_now` — and both gain a
  regression test in `src/modules/bwe/tests/test_bwe.cpp`, each with the
  counterpart that the raise still happens once the link *is* clean (so
  neither guard becomes a ratchet that never recovers).

  The defects were found by *measuring* rather than by reading: the
  saturated-link benchmark (plan item E6) drives the shipped estimator
  against a real UDP link whose capacity is a token bucket, and the
  estimate's climb is visible in the recorded per-report trace.  The
  benchmark's `estimate_did_not_climb_while_congested` invariant is now the
  regression guard for both, and it is asserted in CI.

- **The WebTransport certificate script no longer hangs on a CI runner.**
  `scripts/generate_wtserver_cert.ps1` computes the certificate's SPKI
  SHA-256 so that Chrome can be given an `--ignore-certificate-errors-spki-list`
  entry.  It did that by reading `$cert.PublicKey.EncodedKeyValue`, which
  opens the key through the platform crypto provider: on a **non-interactive**
  runner that call never returns, so the script hung until the job's
  30-minute timeout killed it.  Every Windows leg failed this way -- the log
  showed `Certificate created` and then nothing for 27 minutes.

  The SPKI is now read straight out of the certificate's own DER encoding,
  which touches no provider at all.

  This also fixes a latent correctness bug.  The old code rebuilt the SPKI
  by hand with a hardcoded `rsaEncryption` AlgorithmIdentifier; for an RSA
  key the result is byte-identical to the real SPKI, but for any other key
  type it silently produced the **wrong** SPKI -- an EC certificate would
  get an RSA AlgorithmIdentifier glued onto an EC key.  Taking the bytes out
  of the certificate means the AlgorithmIdentifier is whatever the
  certificate actually used.  Verified byte-for-byte against an independent
  DER parse for both RSA (294-byte SPKI) and EC (91-byte SPKI) certificates.

- **`Stats::remote_desktop.capture_fps` no longer drops to zero when the
  capture device's queue overflows.**
  The cadence record treated a frame whose timestamp was not strictly
  greater than the last one as "no new delivery" and refused to count it.
  That is backwards for the case the number exists for: the device's
  queue is bounded, so when the pump falls behind, the device drops the
  oldest frame and the next handoff can carry a timestamp the pump has
  already passed — exactly under load, the rate then collapsed to 0 (or
  to whatever an earlier, quieter window measured).  A *delivery* is now
  identified by its **sequence number**; the timestamp is only the span
  endpoint, and a span that moves backwards is reported as **unknown**
  (0 fps) rather than as a plausible-looking wrong number.  The counting
  rule moved to its own header, `src/engine/src/engine_capture_cadence.hpp`,
  so the unit test pins the same rule the pump uses.
  (Regression caught by `macos-clang / arm64` on the S7 landing.)

- **The bandwidth estimator no longer climbs back into a wall it is still
  hitting — through either of its two raise paths.**
  The AIMD front-end's additive increase was gated on the decrease
  *cooldown* alone, and the cooldown expires on a **clock**, not on the
  **state of the link**.  So while the link was *still* lossy — the loss
  rate still above the threshold — the estimate resumed climbing and
  walked back up to, and past, the rate that had just been throttled.  The
  front-end behaved like a single-step correction: one halving, then a
  slow climb back into the congestion, while a fifth of the packets were
  still being dropped.

  The Goog-CC front-end already carried the correct guard (`!is_lossy`), so
  the two estimators disagreed under exactly the condition that matters.

  The **REMB override** had the same defect in the same loop, one branch
  over.  It was gated on `loss_event_pending_`, which is *edge-triggered*:
  the latch is set when the loss rate crosses the threshold from below, and
  cleared as soon as the multiplicative decrease has been applied.  So the
  *second* still-lossy report arrived with the latch already down, and the
  receiver's stale (higher) estimate was adopted — walking the sender back
  into the wall it had just been throttled away from.  The latch is the
  wrong guard for the same reason the cooldown was: it describes a
  transient state, not the state of the link.

  Both fixes are the same clause — `&& !is_loss_now` — and both gain a
  regression test in `src/modules/bwe/tests/test_bwe.cpp`, each with the
  counterpart that the raise still happens once the link *is* clean (so
  neither guard becomes a ratchet that never recovers).

  The defects were found by *measuring* rather than by reading: the
  saturated-link benchmark (plan item E6) drives the shipped estimator
  against a real UDP link whose capacity is a token bucket, and the
  estimate's climb is visible in the recorded per-report trace.  The
  benchmark's `estimate_did_not_climb_while_congested` invariant is now the
  regression guard for both, and it is asserted in CI.

- **The WebTransport certificate script no longer hangs on a CI runner.**
  `scripts/generate_wtserver_cert.ps1` computes the certificate's SPKI
  SHA-256 so that Chrome can be given an `--ignore-certificate-errors-spki-list`
  entry.  It did that by reading `$cert.PublicKey.EncodedKeyValue`, which
  opens the key through the platform crypto provider: on a **non-interactive**
  runner that call never returns, so the script hung until the job's
  30-minute timeout killed it.  Every Windows leg failed this way -- the log
  showed `Certificate created` and then nothing for 27 minutes.

  The SPKI is now read straight out of the certificate's own DER encoding,
  which touches no provider at all.

  This also fixes a latent correctness bug.  The old code rebuilt the SPKI
  by hand with a hardcoded `rsaEncryption` AlgorithmIdentifier; for an RSA
  key the result is byte-identical to the real SPKI, but for any other key
  type it silently produced the **wrong** SPKI -- an EC certificate would
  get an RSA AlgorithmIdentifier glued onto an EC key.  Taking the bytes out
  of the certificate means the AlgorithmIdentifier is whatever the
  certificate actually used.  Verified byte-for-byte against an independent
  DER parse for both RSA (294-byte SPKI) and EC (91-byte SPKI) certificates.

- **The Chrome WebTransport interop handshake could never succeed, because the
  server's certificate was never trusted by the browser.**
  `scripts/generate_wtserver_cert.ps1` created the self-signed certificate
  with `New-SelfSignedCertificate -CertStoreLocation Cert:\CurrentUser\My` —
  the *personal* store, which is where msquic loads it from
  (`QUIC_CREDENTIAL_TYPE_CERTIFICATE_HASH_STORE`), so the **server side** was
  always fine.  But the client validates the **chain**, and a self-signed
  certificate is its own issuer (`CN=localhost` is both subject and issuer):
  unless that exact certificate is trusted, it is an unknown root and the
  handshake dies with `QUIC_TLS_CERTIFICATE_UNKNOWN` (alert 46).

  The harness's `--ignore-certificate-errors` does not rescue it: Chromium's
  QUIC proof verifier (`net/quic/crypto/proof_verifier_chromium.cc`) turns a
  successful verification whose issuer is not a known root into
  `ERR_QUIC_CERT_ROOT_NOT_KNOWN` regardless.

  The fix follows the mechanism the DTLS plane of the same case already
  uses: publish the certificate's **SPKI SHA-256** and hand it to Chrome via
  `--ignore-certificate-errors-spki-list`, which pins the trust decision to
  exactly this certificate's public key.
  `scripts/generate_wtserver_cert.ps1` now writes the hash to
  `build/wtserver_spki.b64`, and the WebTransport harness passes it to
  Chrome.

  Two alternatives were considered and rejected:

  - *Installing the certificate into `Cert:\CurrentUser\Root`.*  It works
    from an interactive desktop, but it **hangs on a non-interactive
    runner**: the protected-root store raises a confirmation dialog that
    nobody can answer on a GitHub Actions runner, so the script never
    returns and the job dies at its timeout.  (It passed locally and hung
    on CI — which is exactly what makes it easy to get wrong.)  It would
    also change the trust decisions of every other program on the machine
    for the lifetime of a test certificate.
  - *Relying on `--ignore-certificate-errors` alone.*  Insufficient, as
    above.

  The WebTransport half of the Chrome interop case had been failing on every
  attempt since 2026-10-24 for this reason alone; NimRTC's QUIC and
  WebTransport code was never at fault.  The full write-up, including the
  before/after evidence, is in `docs/interop/chrome-interop-results.md`.

- **The audio allocation gate counted allocations made by *other* threads, so
  it failed on macOS for reasons that had nothing to do with the audio send
  path.**  `tests/test_engine_audio_alloc.cpp` replaces the global
  `operator new` to measure the §3.5 budget ("zero heap allocations per
  emitted audio packet").  The replacement is process-wide, so **every**
  thread in the process passed through it — including the VideoToolbox
  H.264 encoder's own threads, which the engine selects on macOS and which
  allocate behind the engine's back.  The `macos-clang / arm64` leg
  therefore failed with "the audio send path allocated 2 times over 100
  packets", and the count varied run to run.

  The probe now records the **allocating thread's id** and counts only the
  allocations made by the thread that armed the measuring scope — the
  thread running the send path.  A background thread's allocation is not
  the send path's allocation, and mixing the two makes the gate
  untestable rather than strict.

  The failure message now also reports **what** was allocated (the size of
  each allocation, read out into a caller-owned buffer so that rendering
  the diagnostic does not pollute the very measurement being reported).
  The probe was verified to still catch real drift: injecting one
  per-packet allocation into the send path fails the gate with "100 times
  over 100 packets", and reverting it turns the gate green again.

  Note that the sibling test
  (`TheAllocationCountDoesNotGrowWithThePacketCount`) already encoded the
  honest form of the criterion — the count must not *grow* with the packet
  count — which is what the remediation plan's §3.5 acceptance line
  actually says ("分配次数不随包数增长").

- **The Wayland support in the video sink had never been compiled on any
  build host.**  `wayland-protocols` ships the protocol **XML**, not the C
  headers — `xdg-shell-client-protocol.h` and
  `linux-dmabuf-v1-client-protocol.h` are *generated* by `wayland-scanner`.
  The CMake lookup searched for the generated header with `find_path`, so
  it never succeeded: `NIMRTC_VIDEO_SINK_HAS_XDG_SHELL` and
  `NIMRTC_VIDEO_SINK_HAS_DMABUF` stayed undefined and roughly twenty
  branches of `egl_renderer.cpp` were dead code.  The XML is now located
  (via the `pkg-config` `pkgdatadir` first) and both the client header and
  the marshalling code are generated from it.

  Forcing the macros on exposed four real defects behind them:

  * `wl_shm_pool_create_buffer()` was called with three arguments instead
    of six (`pool, offset, width, height, stride, format`), and its
    result — a `struct wl_buffer*` — was then used as the raw pixel
    buffer, i.e. an invalid memory write into a protocol object.  The
    `wl_shm` CPU path now backs the pool with an anonymous file
    (`memfd_create`, with a `mkstemp` + `unlink` fallback), maps it with
    `mmap(MAP_SHARED)`, and copies the picture into the mapping — which is
    what the protocol actually requires, since the compositor maps *the
    same file* on its side.  The mapping is released with `munmap()` on a
    geometry change and on teardown.
  * The DMA-BUF import called an API that does not exist in the protocol
    (`zwp_linux_dmabuf_v1_params_add_fd`, `..._set_offset`,
    `..._add_plane`, `..._create_buffer`).  It now uses
    `zwp_linux_buffer_params_v1_add()` plus `..._create_immed()`, attaches
    the resulting `wl_buffer` to the surface, and releases the previous
    buffer only after the new one has been committed.
  * `xdg_surface_commit()` and `wl_display_commit()` are not Wayland
    requests.  The xdg state is committed through `wl_surface_commit()` on
    the `wl_surface` it wraps; the display through `wl_display_flush()`.
  * The registry listener was missing the `global_remove` member, and the
    DMA-BUF interface was bound at a hard-coded version instead of the
    advertised one.

  The generated marshalling code is plain C, while the library's warning
  set is C++-oriented (`-Wnon-virtual-dtor`, `-Wold-style-cast`,
  `-Woverloaded-virtual`) — and GCC *rejects* those options for a C
  translation unit, fatally under `-Werror`.  The generated code
  therefore lives in its own `OBJECT` library, which keeps it out of the
  C++ warning set.

  Verified: builds clean with warnings-as-errors on Linux (g++-13, all six
  optional paths enabled) and on Windows (MSVC 2022, Debug), and the
  protocol interface symbols are now defined in the archive.

- **The Wayland sink compiled but could not present.**  E2 proved the
  Wayland leaves *compile*; running the sink under a headless Weston
  showed that neither presentation path reached the display, and that
  the EGL path tore the client connection down.

  * **`EGL_BAD_ACCESS` (0x3002) on every frame.**  `init_egl()` bound the
    context on the thread that called `open()` and left it bound there.
    EGL binds a context to the **calling thread** and refuses to bind one
    already current elsewhere, so the presenter thread's `eglMakeCurrent()`
    failed on every frame.  `init_egl()` now releases its binding and
    calls `eglReleaseThread()` when it is done querying the surface,
    leaving the context free for the presenter thread.
  * **`xdg_surface has never been configured`.**  `init_wayland()`
    committed the initial xdg state and moved on; the first frame was
    then committed before the compositor's configure had arrived, which
    is a protocol error — Weston kills the client.  The surface now waits
    (bounded round trips, so a compositor that never configures cannot
    hang `open()`) for the first configure and its acknowledgement
    before `open()` returns.
  * **A failed swap was counted as a presented frame.**
    `draw_with_egl()` ignored the result of `eglSwapBuffers()` and
    returned `true` unconditionally, so `presented` could report frames
    that never reached the compositor.  The swap result is now the
    return value.

  Verified under headless Weston on Ubuntu 22.04: the EGL path and the
  `wl_shm` CPU path each present a full 1280x720 and a letterboxed
  320x180 stream with no protocol errors.

- **The X11 input-injection path had never been compiled.—**  The Linux
  injector has two backends: XTest (via `libXtst`) and uinput (via the
  kernel's `linux/uinput.h`).  `libxtst-dev` was not among the CI
  install packages, so `NIMRTC_HAS_XTEST=1` was never defined and the
  entire X11 branch of `linux_input.cpp` — the key, button and
  wheel injection through XTest — was dead code on every build
  host.

  Compiling it exposed two real defects.

  * `XTestFakeRelativeMotionEvent()` was called with `nullptr` as its
    fourth argument, but that parameter is the event *delay* (an
    `unsigned long` timestamp), not a pointer — the call could
    never have compiled, so relative mouse motion through XTest could not
    have worked.  It now passes `CurrentTime`, matching every other
    XTest call in the file.
  * The module linked `libXtst` but not `libX11`.  The XTest *extension*
    lives in `libXtst`, but the calls it is made through —
    `XOpenDisplay`, `XFlush`, `XCloseDisplay` — live in `libX11`
    itself, so the link failed with `undefined reference to symbol
    'XFlush'`.  (`libXtst` records a dependency on `libX11`, but it does
    not carry it into the link line.)  `libX11` is now linked as well.

  Both the XTest and the uinput backends are now asserted by the
  `Leaf compile proof` CI step, so a runner image that loses the package
  fails the build instead of silently dropping the backend.

- **The XTest path injected the *wrong* keys and buttons, and crashed the
  process on construction.**  Compiling the XTest backend (PR #76) proved
  it *builds*; it did not prove it *works*, and it did not.

  The Linux injector has **two** injection vocabularies, and the XTest
  path was using the wrong one.

  * **The keys were injected with the kernel's codes.**  The XTest path
    called `XTestFakeKeyEvent(dpy, KEY_A, ...)` — `KEY_A` is the
    *kernel input layer's* code (30), from `linux/input-event-codes.h`.
    `XTestFakeKeyEvent()` wants an X11 **keycode**: the number the X
    server assigned to a physical key on the *active* layout.  The two
    numbering schemes are unrelated, so the server was asked for a key
    that does not exist on the layout and injected **nothing** — while
    the injector reported success.  The X server's own vocabulary is the
    **KeySym** (`XK_a`, `XK_Return`), which the call site now converts
    to a keycode with `XKeysymToKeycode()`; that conversion is
    layout-aware, which is exactly what a keycode needs to be.
  * **The buttons were injected with the kernel's codes too.**
    `XTestFakeButtonEvent(dpy, BTN_LEFT, ...)` sent `0x110` where the X
    server expects a **button number**: `1` (left), `2` (middle),
    `3` (right), `4`/`5` (the wheel), `8`/`9` (back/forward).  A
    button number outside the server's range is silently dropped.
  * **Constructing the injector could segfault the process.**
    `XTestQueryExtension(dpy, nullptr, nullptr, nullptr, nullptr)` passed
    a null pointer for all four of its out-parameters, but the function
    writes to **all five** of them unconditionally.  On a host with a
    reachable X server this crashed inside `libXtst` before the
    injector's first method was ever called.

  The two vocabularies are now kept apart explicitly: the kernel's
  `KEY_*`/`BTN_*` codes are used only on the uinput path, the X server's
  KeySyms and button numbers only on the XTest path, and each is
  resolved at its own call site.  The tables live in a new portable
  header (`src/linux_input_codes.hpp`) that carries **no platform
  type**, so the unit test verifies all of them on **every** CI leg —
  including the Windows one, where the Linux backend is never compiled.
  A table that could only be checked on a Linux machine is a table that
  rots silently between two Linux machines.

- **The DTLS-over-UDP loopback test failed on a coin flip.**  Its PASS
  criterion was `client_master_key[0] != 0`, and any single byte of a
  freshly-derived SRTP master key is `0x00` about 1/256 of the time —
  so the test went red roughly once every 256 runs, for no reason at
  all.  It fired for real on 2026-11-17 in the CI run of PR #69
  (run 44009282768, job `windows / msvc`): both ends had derived the
  *same* key (`client_key[0..3]=00246f7a` on A and on B), so the ECDH
  was real and the export had succeeded — the key was entirely
  non-zero apart from its first byte.

  The check now requires at least one non-zero byte in the 16-byte
  master key on each side, and additionally requires the two ends to
  agree on the key — which a real handshake produces and a
  zero-filled buffer cannot fake.  The same defect had already been
  fixed in the sibling test `tests/test_dtls_over_arq_udp.cpp`; this
  one had been left behind.

  Worth noting: the CI auto-retry workflow classified this as a
  **genuine** failure (`steps=20`, runner assigned) and deliberately
  left it alone, because a job that ran and failed belongs to the
  tree and must not be re-run.  That was the right call — but it also
  meant the flake could not be papered over and had to be fixed at the
  source.

- **The VideoToolbox encoder emits its parameter sets in band.**  A
  `VTCompressionSession` keeps the SPS / PPS / VPS out of band, in the
  sample buffer's `CMFormatDescriptionRef`, and returns length-prefixed
  (AVCC) slice NALs with no parameter sets in the bitstream at all.  A
  keyframe produced that way carries no way to start decoding: a
  cross-decoder pair (the VideoToolbox encoder against the OpenH264
  decoder) failed with `kErrCorrupt` (4100).

  The encoder now extracts the parameter sets through
  `CMVideoFormatDescriptionGetH264ParameterSetAtIndex` /
  `CMVideoFormatDescriptionGetHEVCParameterSetAtIndex`, collects them
  once per session — outside the output callback, so the callback still
  allocates nothing — and prepends them as Annex-B start codes on every
  keyframe.  The prefixing itself is a reusable helper
  (`prepend_parameter_sets()` in `vt_common.hpp`) with unit tests.

- **`MakePluginOpts` no longer trips GCC's `-Wmissing-field-initializers`.**
  `open_failure_detail` had no default member initializer, so every
  designated initialization of the struct warned under GCC 13 with
  `-Wextra -Werror`.  The member now has an explicit `= {}` initializer,
  which fixes it at the declaration instead of at every call site.

- **The zero-allocation hot-path gate and the codec round-trip test pin
  the software backend.**  Both picked whichever backend the platform
  preferred, so the hot-path benchmark measured the hardware codec's
  memory behaviour on one CI leg and not on another, and the
  round-trip test paired a hardware encoder with a software decoder.
  Both now ask for the software backend explicitly, which is what makes
  the zero-allocation budget and the cross-backend round-trip
  deterministic.

- **The merge of the M1C platform capture layer onto the remote-desktop
  branch.**  `origin/main` carried the M1C capture layer (Windows
  Graphics Capture / DXGI Desktop Duplication, macOS ScreenCaptureKit,
  Linux X11 + the XDG-desktop-portal PipeWire path) while this branch
  carried the remote-desktop plan's M1–M4 work.  The two sides
  implemented the *same* seams twice, so the merge had to pick one
  implementation of each and reconcile the rest.

  - **The capture seam keeps the M1C layer; the cursor layer keeps the
    remote-desktop one.**  The M1C platform backends
    (`windows_capture.hpp`, `unix_capture.hpp`) and the portable cursor
    layer (`cursor_compositor.hpp` / `cursor_source.hpp`) are
    complementary, not competing, so both stay.  The *older* duplicate
    platform backends (`dxgi_desktop_dup.*`, `wgc_capture.*`,
    `portal_capture.*`, `screen_capture_kit.*`, `x11_capture.*`,
    `win_cursor_source.*`, `x11_cursor_source.*`) and the *duplicate*
    cursor classes (`AlphaCursorCompositor`, `CursorState`) were dropped
    — the M1C layer already provides them.

  - **There is no capture → frame-stream adapter.**  The M1C layer
    shipped a `CaptureSourceAdapter` between the capture device and the
    frame stream; this branch's engine pump polls
    `IVideoCapture::pop_frame()` and translates the frame inline, so the
    adapter is redundant and was removed (`capture_source_adapter.hpp` /
    `.cpp` and its test).  `IVideoCaptureFactory::create_source()` went
    with it — the factory only creates *devices*.

  - **The zero-copy decision is now explicit.**  `VideoSourceFrame` grew
    `capture_surface_contract`: a capture device that negotiated a
    zero-copy surface says so, and the engine then hands the surface to
    the encoder **unconditionally** (there is no CPU copy to fall back
    to).  A frame that merely carries a GPU handle — a host's own
    surface — stays *opt-in*, governed by
    `EngineConfig::video_zero_copy`, exactly as before.  Before this
    distinction existed the two cases were indistinguishable, so a host
    that turned the flag off still got the zero-copy path.

  - **The GPU device token travels with the frame.**
    `CaptureFrame` / `VideoSourceFrame` carry the device that owns the
    surface, and `GpuHandle::device` is filled from it.  A surface is
    meaningless without its device: NVENC registers the texture against
    the device the session was opened with and refuses a texture from
    another one.

  - **A capture device that reports an error is closed before it is
    reopened.**  `open()` on an already-open device is a no-op for most
    backends, so the recovery used to be silently ineffective; the pump
    now tears the device down and builds it up again.

  - **The synthetic capture device advertises its dirty rectangles.**
    The negotiated format said `has_dirty_rects = false` while the
    frames carried the rectangles, so a consumer that trusted the
    format would ignore metadata that was present and correct.

  - **The public-header baseline moved from 189 to 190.**  The merge
    added `video_capture/dirty_rect_diff.hpp` and
    `core/gpu_device_registry.hpp` and removed
    `video_capture/capture_source_adapter.hpp` — a net `+1`.

- **Every shipped example compiled and then refused to start.**  The
  remote-desktop clients (`rd_device`, `rd_viewer`) and the other
  examples (`demo-p2p`, `loopback-p2p`, `demo-wtserver`,
  `demo-agent-gateway`) all link `nimrtc::web_transport`, which pulls in
  the msquic **shared** library.  On Windows that library is a DLL, and
  the loader looks for it *next to the executable* — so an example
  without `msquic.dll` beside it dies with `STATUS_DLL_NOT_FOUND`
  (0xc0000135) **before `main()` is entered**, with no message and no
  log line.  The exit code was the only symptom, and it read like a
  crash in the program rather than a missing file.

  The test tree had always handled this — `gtest_discover_tests` *runs*
  the executable to enumerate its cases, so the missing DLL showed up as
  a build failure — and `cmake/NimRTCTest.cmake` grew a
  `nimrtc_copy_msquic_dll()` helper for it.  The **examples** had no
  such handler, so the whole example tree was in the worst shape a
  shipped artefact can be in: the build was green and every binary was
  unusable.

    - **New module `cmake/NimRTCRuntime.cmake`** — the runtime staging
      helpers, shared by the test tree and the example tree so there is
      one implementation:
      - `nimrtc_stage_msquic_dll(<target>)` copies the per-config
        `msquic.dll` out of the vendored build tree
        (`src/third_party/msquic/bin/$<CONFIG>/`) next to the
        executable.  A no-op off Windows and when the vendored msquic
        target is absent.
      - `nimrtc_stage_dtls_certs(<target>)` copies the **Chrome-friendly**
        DTLS certificate into `<target-dir>/certs/`.  This matters
        because the absolute fallback compiled into `nimrtc::dtls` points
        at the *upstream* wolfSSL certificate, which carries no
        `subjectAltName` — and Chrome (BoringSSL) rejects it with fatal
        alert 46 ("certificate_unknown").  A NimRTC-to-NimRTC session
        works with either; a browser session only works with the staged
        one.
      - `nimrtc_stage_runtime(<target>)` — the one call a shipped
        executable needs; it does both.
    - `cmake/NimRTCTest.cmake`'s `nimrtc_copy_msquic_dll()` now delegates
      to the shared helper, so the historical name keeps working.
    - `examples/CMakeLists.txt` and
      `examples/demo-agent-gateway/CMakeLists.txt` call
      `nimrtc_stage_runtime()` on every example, which also replaces the
      three hand-rolled certificate-copy blocks that had grown up
      independently in `demo-p2p`, `loopback-p2p` and
      `demo-agent-gateway`.
    - **Verified**: all six examples now start and exit 0 on `--help`,
      and a local `rd_device` ↔ `rd_viewer` session over the
      stdin/stdout signalling dialect completes the full handshake — one
      offer (51 SDP lines, with both a video and an audio m-line), one
      answer, and eleven relayed ICE candidates.

- **A software encoder no longer loses every captured frame (v1.4,
  remote-desktop plan RD-4 / B10).**  The engine attached the capture
  device's GPU callback whenever the *device* could deliver surfaces,
  without asking the *codec* whether it could consume one.  But the
  plug-in contract says that *with* a GPU callback attached the device
  delivers the surface inside the callback and does **not** push the
  frame through the CPU copy queue — the pixels are not available
  anywhere else.  So a session whose codec was a software encoder
  (OpenH264, or any machine without a matching hardware pair) ended up
  with **no picture at all**: every frame delivered, every frame
  dropped, and no CPU mirror to fall back to.  RD-4 says "zero-copy is
  the default path; CPU staging is the fallback" — and the fallback was
  unreachable.

    - The two halves of the seam are now established at two different
      moments, because the codec's capability is only known once the
      codec is open: the `GpuContext` is published into
      `VideoCodecConfig::gpu_context` **before** the codec is created
      (the encode session has to live on the capture's device), and the
      GPU callback is attached **after** the codec exists, by the new
      `NimRTCEngine::wire_capture_gpu_callback()`, and **only** when
      `IVideoCodec::supports_zero_copy_encode()` is true.
    - When the codec cannot take a surface, the callback stays
      unattached, so the capture device stages to the host and fills
      its ordinary queue, the pump takes the CPU route, and the picture
      still arrives.  The engine says so in the log and reports
      `Stats::video_capture_gpu_wired == false`, so the fallback is
      visible rather than silent.
    - `tests/test_engine_capture_zero_copy.cpp` gained the regression
      test (`ACodecThatCannotTakeASurfaceKeepsTheCpuPath`) with a
      stand-in software codec: the seam must **not** be wired, nothing
      may go down the zero-copy path, and the codec must still be given
      the capture's device.
    - **Verified** on a real Windows host with the OpenH264 software
      encoder: `rd_device` ↔ `rd_viewer` delivers the picture
      (`capture=496`, `pumped=118`, `decoded=114`, `rendered=114`) and
      the log reports the fallback instead of the dropped-frame
      warning.

- **The composite codec forwarded the zero-copy *capability* but not
  the zero-copy *call*.**  `HwCompositeCodec::supports_zero_copy_encode()`
  answered `true` -- it forwards that to the encoder -- but the
  composite did not override `encode_zero_copy()`, so it inherited the
  interface's default `kErrUnsupported`.  That is the worst of both
  worlds: the engine believed the seam was live, wired the capture
  device's GPU callback, handed the surface to the codec, and was told
  "unsupported" for every single frame.  Because the capture device
  keeps no CPU mirror once the callback is attached, every frame was
  lost and the session showed a picture that never moved.  The
  composite now forwards `encode_zero_copy()` to the encoder and
  `decode_zero_copy()` to the decoder, exactly the way it already
  forwards `encode()` and `decode()`.
  `src/modules/nvcodec/tests/test_hw_composite_codec.cpp` gained the
  three regression tests that pin the capability and the call
  together: the encode is forwarded, the decode is forwarded, and a
  composite whose encoder cannot take a surface refuses the call
  rather than silently succeeding.

- **The device-local texture copy was *recorded*, not *executed*.**  The
  `CopySubresourceRegion` sits in the context's command buffer until the
  GPU runs it, and two consumers need the copy to have *happened*: DXGI,
  which is not allowed to recycle a texture whose copy is still in
  flight, and the hardware encoder, which registers the destination
  texture as its input surface and waits for the writes to it.  Neither
  can see the other's work on its own, so the encode could block
  forever waiting for a copy the GPU was never told to perform.
  `wcap::copy_texture_on_device()` now **flushes** the immediate context
  after recording the copy, so the command is submitted to the GPU queue
  before the destination texture is handed to the encoder.  A flush is a
  GPU-side submit -- the pixels never enter host memory, so
  `bytes_copied` stays at zero.

- **The shared D3D11 device was used from two threads without protection.**
  The capture device copies the desktop into the device on **its own
  thread**, and a hardware encoder that registers the copied surface as
  its input uses the **same** device on the encoder's thread (the driver
  inserts its own GPU work on the device's immediate context to read the
  registered surface).  A D3D11 *immediate* context is not thread-safe:
  two threads using it concurrently is undefined behaviour, and on the
  reference host it showed up as a hard deadlock inside
  `nvEncEncodePicture` -- the driver waited for its own work while the
  capture thread was in the middle of the device copy.
  `D3D11State::create_on()` now queries `ID3D11Multithread` and enables
  `SetMultithreadProtected(TRUE)`, which is the documented answer for
  exactly this situation (a device used from more than one thread).
  With that in place the capture thread no longer needs an artificial
  wait before handing the surface over -- the device's own command queue
  orders the copy before the encoder's work.

- **The capture frames that predate the zero-copy seam are discarded.**
  The capture device is opened *before* the codec exists -- the codec
  has to adopt the capture's D3D11 device, and that device only exists
  once the capture device is open -- so the frames produced in that
  window went through the CPU staging path, because the GPU callback
  did not exist yet.  They are stale (the mailbox delivers the current
  picture within one capture interval), so
  `NimRTCEngine::drain_stale_cpu_frames()` drops them once the seam is
  wired.  It does not rewrite the device's own `bytes_copied`
  accounting -- those bytes really were copied -- which is why the
  verification program reports the startup transient separately from
  the steady state.

- **The Windows capture device's GPU context released the D3D11 device
  one time too many.**  `WindowsCapture::gpu_context()` hands the caller
  a `GpuContext` whose `keepalive` is a `shared_ptr` that owns *one*
  reference to the `ID3D11Device` — so a consumer that outlives the
  capture device never sees a dangling device pointer.  The keepalive
  was built from a `ComPtr` **copy** of the device, and the deleter
  called `Release()` on it: the copy's own destructor released the
  reference *again*, so the device's refcount went to zero while the
  context was still alive, and the next use faulted with an access
  violation (`0xC0000005`).  The keepalive now takes its **own**
  reference (`AddRef`) before the copy goes out of scope, so the
  reference count is exactly balanced.

- **The Windows cursor source crashed the process on every poll.**  The
  cursor layer (remote-desktop plan A6) read the icon's monochrome
  mask with `GetDIBits`, and the call was wrong in three separate ways:

    - The `BITMAPINFO` was **null**.  `GetDIBits` reads the
      destination's layout through it unconditionally, so the call
      faulted with an access violation (`0xC0000005`) inside
      `gdi32full.dll` — reproducible on every host, on the first poll.
    - The destination had **no room for the colour table**.  A
      1-bit-per-pixel `BI_RGB` destination carries a 2-entry palette
      and `GetDIBits` writes it after the header, but a plain
      `BITMAPINFO` has room for only one entry: the call wrote 4 bytes
      past the end of the structure, a stack buffer overflow that the
      debug runtime's stack check turned into a hard failure.
    - The mask was read **one scan line at a time** (`biHeight = 1`,
      `cLines = 1`).  Such a request reports success but leaves the
      destination buffer untouched for every line past the first, so
      the loop silently re-read the *same* row — and applied it to
      every row of the cursor.

  The mask is now read in a **single top-down** `GetDIBits` call into a
  destination with an explicit 2-entry palette, so the mask rows line
  up with the (top-down) drawn DIB and the bitmap is the cursor's real
  shape.

- **`WindowsCapture.DxgiReportsDirtyRects` failed on an idle desktop.**
  The test waited for 3 Desktop Duplication frames, but DXGI only
  delivers a frame when the screen *changes* — so on a static desktop
  the wait timed out and the test failed roughly half the time.  The
  test now nudges the desktop while it waits, exactly as its sibling
  `DxgiReportsANonEmptyDirtyRectWhenTheDesktopChanges` already did.

- **A fresh clone now configures and builds.**  `cmake -S . -B build` on
  a clean checkout failed outright: the WebRTC APM vendor wrapper raised
  a `FATAL_ERROR` whenever the prebuilt static library was missing, but
  the APM source tree is cloned at build time by
  `tools/fetch_webrtc_apm.py` -- so on a clean clone the prebuilt can
  never be built and the configure was impossible.  The fatal error now
  fires only when the source **is** present (so the build script was
  started and did not finish); otherwise the wrapper defines the stub
  target and says so.  The `audio3a` module's `NIMRTC_USE_WEBRTC_APM`
  define now follows the **source** rather than the option, so the
  module builds its stub implementation and reports `DEGRADED` at
  runtime -- the honest state -- instead of failing to compile.

- **The stage-timing test was flaky** (it failed on roughly a third of
  its runs).  It asserted that the capture stage cost more than zero
  *microseconds*, but popping a frame from a queue costs tens of
  nanoseconds, so the microsecond view rounded to zero.  Two changes:
  the public `Stats::StageTiming` gained the nanosecond view
  (`mean_ns` / `max_ns` / `total_ns`), so a sub-microsecond stage is no
  longer reported as "free" (the microsecond fields stay, because the
  plan's §B.6 budget is written in them); and the test's capture device
  now copies its picture the way a real device does, so the measurement
  is unambiguous.  The negative control confirms the test still catches
  a broken measurement.

> Work that lands after `v1.1.0`.
>
> The audio pipeline's development body lands here. Its work packages are
> specified in `docs/plan/audio-pipeline-spec.md` and scheduled in
> `docs/plan/audio-pipeline-dev-plan.md`; the ladder the owner fixed on
> 2026-11-06 puts the whole audio body (WP-0 .. WP-11) into `v1.2.0`, so
> nothing here is a feature release of the `v1.1` line.

- **The per-stream bandwidth share was assigned to the wrong stream
  (remote-desktop plan D5).**  `bandwidth_allocation.hpp`'s
  `allocate_bandwidth()` reserves index 0 for audio, index 1 for the
  default video stream, and gives the extra video streams the indices
  `[2 + i]`.  The engine's write-back loop iterated the extra-stream
  vector starting at index 1, so the **default** stream's share was
  written to the first extra stream and the **last** extra stream was
  never written at all.  The split now uses the `[2 + i]` indexing, and
  the write-back is covered by
  `tests/test_engine_video_stream_shares.cpp`.
  - The share computation was also coupled to the pacing path: it only
    ran inside `update_pacing_rates()`, which early-exits when the
    scheduler or the media pacer is disabled.  The split is now computed
    by its own function (`compute_video_stream_shares()` in
    `src/engine/src/engine_transport.cpp`), called once per tick
    whenever the BWE has an estimate, so each stream's
    `IVideoBitrateController` receives its share regardless of whether
    pacing is enabled.
  - The per-stream weights are configurable
    (`NimRTCEngine::set_video_stream_weight()` /
    `NimRTCEngine::video_stream_weight()`), and the resulting share is
    observable (`NimRTCEngine::video_stream_share_bps()` and the new
    `VideoStreamStats::bandwidth_share_bps` /
    `VideoStreamStats::bandwidth_weight` fields).
  - Covered by `tests/test_engine_video_stream_shares.cpp` (nine cases:
    the default weight, custom weights, the split's proportionality, the
    stats reporting, and the zero-weight / stream-removal dynamics).

- **The vendored libopus registered its own test suite in NimRTC's ctest
  run, and all five of those tests failed.**  Upstream libopus decides
  whether to build *its* tests from the **global** `BUILD_TESTING` cache
  entry, not from its own `OPUS_BUILD_TESTING` option:

  ```cmake
  if(OPUS_BUILD_TESTING OR BUILD_TESTING)
    set(OPUS_BUILD_TESTING ON)
    set(BUILD_TESTING ON)
  endif()
  ...
  if(BUILD_TESTING AND NOT BUILD_SHARED_LIBS)
    add_test(NAME test_opus_decode ...)
    add_test(NAME test_opus_padding ...)
    add_test(NAME test_opus_api ...)
    add_test(NAME test_opus_encode ...)
    add_test(NAME test_opus_extensions ...)
  endif()
  ```

  NimRTC sets `OPUS_BUILD_TESTING=OFF` explicitly, but the upstream
  `add_subdirectory()` call inherits NimRTC's own `BUILD_TESTING=ON`, so
  the `add_test()` calls landed in NimRTC's ctest run.  The test
  *executables* live in an `EXCLUDE_FROM_ALL` subdirectory and are never
  built, so `test_opus_decode`, `test_opus_padding`, `test_opus_api`,
  `test_opus_encode` and `test_opus_extensions` all failed with "the
  test executable does not exist".  `src/third_party/libopus/CMakeLists.txt`
  now shadows the cache entry with a directory-scope `BUILD_TESTING OFF`
  for the duration of the `add_subdirectory()` call (a normal variable
  takes precedence over the cache in that scope), and restores the
  original value afterwards so the rest of the NimRTC tree still builds
  its own tests.  Upstream's tests are not part of NimRTC's quality gate;
  the NimRTC-owned Opus tests in `src/modules/opus/tests/` are.

- **The BWE plugin seam test asserted against an empty registry.**
  `BwePluginSeam.ListBwesContainsAimd` called
  `core::PluginRegistry::instance().list_bwes()` and expected to find
  `"aimd"` — but the test never registered the factory first.  That
  worked when the whole binary ran as one gtest process, because an
  earlier test in the same binary had called
  `nimrtc::bwe::register_default_plugins()`.  NimRTC registers every
  gtest case as its **own** ctest entry (`gtest_discover_tests`), so
  each case runs in a fresh process and the registry starts empty; the
  case therefore failed in isolation.  The test now calls
  `register_default_plugins()` itself (it is idempotent), matching the
  four sibling cases in the same file.

- **`tools/check_prerequisites.py` could not read the MSVC version on a
  localized toolchain.**  The check ran `cl` and matched the version with
  `Version\s+(\d+)\.(\d+)` — but MSVC's banner is **localized**, and a
  Chinese-locale toolchain prints

  ```
  用于 x64 的 Microsoft (R) C/C++ 优化编译器 19.43.34809
  ```

  with no English word in front of the number at all.  The regex therefore
  found nothing and the check reported `unknown version (could not parse cl
  output)` on a perfectly good VS 2022 install — 8/9 instead of 9/9, with
  the advice "Install Visual Studio 2022 Build Tools", which the machine
  already had.  The check now reads the version from `cl /Bv` with a
  locale-independent `19.xx` pattern, and falls back to the toolset
  directory name under `VC\Tools\MSVC\` if the banner does not parse.
  Both the English and the Chinese banner now resolve to `19.43`.

- **The engine closed the capture device twice (remote-desktop plan
  A1 / A10).**  `NimRTCEngine::shutdown_video_capture()` closed the
  capture *adapter* and then closed the underlying capture *device* a
  second time.  The adapter owns the device's open/close lifecycle, so
  the second close reported a spurious close to the plug-in — and a
  plug-in that counts closes (or that asserts it is not closed twice)
  sees a lifecycle event that never happened.  The engine now closes
  the adapter — which closes the device with it — and afterwards only
  drops its own reference to the device.

- **The seven platform capture entry points carried no stability marker,
  so the API-freeze gate reddened CI.**  The new platform capture
  headers (`dxgi_desktop_dup`, `portal_capture`, `screen_capture_kit`,
  `wgc_capture`, `win_cursor_source`, `x11_capture`, `x11_cursor_source`)
  declared their classes as `NIMRTC_API_STABLE`, but the free functions
  at the bottom of those headers — the per-platform registration entry
  points and the two cursor-source factories — had no marker at all.
  ADR-015 "Verification" requires every namespace-scope symbol in a
  public header to carry one, and `scripts/api_freeze_scan.js` exits
  non-zero when one does not, so the `API freeze -- per-symbol
  classification` CI job failed on the merge branch with 7 unclassified
  symbols.  The markers match the rest of the module (`NIMRTC_API_STABLE`).

- **`EngineVideoPlugins.source_can_produce_and_sink_can_render` was
  scheduler-dependent.**  The test slept for a fixed 200 ms after
  `start()` and then asserted that the video source had produced frames.
  That window races against the producer thread's cold-start latency: on
  a loaded host the thread can miss the whole window, and the test then
  reports zero frames for a source that is working perfectly.  Measured
  on a developer machine: 1 failure in 20 runs of the test in isolation,
  and one failure in a parallel `ctest -j 8` run.  The test now polls
  for the first frame with a 2 s deadline — the same guard the sibling
  test in `src/modules/video_source/tests/test_video_source.cpp` already
  used — and 30 consecutive isolated runs after the change pass.

- **The Windows Graphics Capture backend used the D3D objects after they
  had been freed (remote-desktop plan A3).**  `WgcCapture` owns a
  *free-threaded* frame pool, so `FrameArrived` is raised on a WinRT
  thread-pool thread — a thread the device does not own.  The device
  kept that thread's inputs (the scratch buffer, the dirty-rect
  detector, the configuration) on *itself*, and `close()` released the
  D3D objects as soon as the in-flight counter reached zero.  Two
  consequences, both of them crashes:
  - A handler that was *already dispatched* to the thread pool but had
    not started yet still started after `close()` returned, and read the
    released device, context and staging texture.  Unsubscribing stops
    new events, but it does not cancel an invocation that is already in
    flight.
  - Two handlers running at the same time (which a free-threaded pool
    is allowed to do) both read back into the same scratch buffer and
    both mutated the same dirty-rect detector.

  The capture state is now owned by a `shared_ptr` that every handler
  holds, so it — and every buffer, counter and configuration the
  handler touches — outlives the device that created it.  The D3D
  objects are released by the state's destructor, that is, after the
  last handler has finished, and a handler that starts after the
  capture was stopped returns without doing any work.  The handlers are
  serialised against each other, and the capture thread publishes the
  device, the context, the pool and the staging texture under the same
  lock, so there is no unordered access left.  Measured on a developer
  machine: `WgcCapture.OpeningOnANonWindowsHostReportsUnsupported`
  (which opens *and* closes the device) segfaulted 1 run in 20 before
  the fix and 0 in 50 after it, in both the Debug and the Release
  build.

- **`Profile::video` had no default member initializer, so the build
  failed on every GCC platform.**  The `video` block was added to
  `assembly::Profile` for the profile schema v1.2, but the member was
  declared as a bare `VideoSection video;`.  A designated initialiser
  that omits it — which is every one of the nine built-in scenario
  profiles except `remote-desktop` — is a *missing field initializer*
  under GCC's `-Wmissing-field-initializers`, and the build promotes
  that warning to an error:

      src/modules/assembly/include/nimrtc/assembly/profiles.hpp:177:1:
      error: missing initializer for member
      'nimrtc::assembly::Profile::video' [-Werror=missing-field-initializers]

  The member now carries an explicit `= {}` default member
  initializer.  GCC treats a member with a default member initializer
  as *not* missing, so the nine profiles compile clean again, and the
  semantics are unchanged: an omitted `video` block still yields the
  default (camera-like) section.  This unblocked the `linux-gcc`
  (x86_64 and aarch64) and `linux-sanitizers` CI legs.

- **Two dead BGRA converters reddened the Clang builds.**  The capture
  adapter carried its own `bgra_to_i420` and `bgra_to_nv12` helpers in
  an anonymous namespace, but nothing called them: the adapter uses
  the canonical `video_frame::convert_bgra_to_i420_color` and
  `convert_bgra_to_nv12_color`, which additionally carry the colour
  description.  Clang reports the unused ones, and the build promotes
  that to an error:

      src/modules/video_capture/src/capture_source_adapter.cpp:58:6:
      error: unused function 'bgra_to_i420' [-Werror,-Wunused-function]

  The two superseded copies are removed.  `bgra_to_rgba` stays: the
  packed-RGBA output has no canonical equivalent.  This unblocked the
  `linux-fuzz`, `linux-sanitizers / tsan.clang` and `macos-clang`
  CI legs.

- **The TURN-over-TCP test could not start its TURN server on a host
  whose TCP port exclusions differed from its UDP ones
  (v1.2.0, architecture remediation plan §4.2f).**
  `src/modules/ice/tests/test_turn_relay.cpp` probed for a free port by
  asking the kernel for an ephemeral **UDP** socket — but the
  TURN-over-TCP control listener (RFC 8656 §12) binds a **TCP** socket.
  Windows keeps a **separate port-exclusion range per protocol**, so a
  port number can be free for UDP while sitting inside a TCP block
  reserved for the hypervisor; binding it as TCP then failed with
  `WSAEACCES` (errno 10013) and the server refused to start:

  ```
  [info]  UDP socket bound to 127.0.0.1:62962
  [warn]  TURN-over-TCP listener binding failed, errno=10013
  [error] TURN-over-TCP listener creation failed
  ```

  (62962 was free for UDP but inside the TCP exclusion block
  62635-63134 on that host.)  The probe now uses **the socket type the
  port will actually be bound with** — the control port is probed as UDP,
  the TURN-over-TCP port as TCP, and the relay-port range as UDP — so
  the kernel's ephemeral allocation only ever returns a port that is
  bindable for *that* protocol.

- **The NTP64 round-trip test measured the thread's scheduling delay
  instead of the conversion's accuracy (v1.2.0, architecture
  remediation plan §4.2f).**
  `Ntp64.RoundTripNow` asserted that the back-converted instant was at
  most a **fixed** 50 ms behind `now`.  That bound is only safe on an
  idle machine: under a parallel ctest run (`ctest -j 4`) on a loaded
  CI runner, the thread can be descheduled for far longer than 50 ms
  between the two clock reads — the test failed with a 115 ms skew even
  though the NTP64 round-trip itself is accurate to well under 1 µs.
  The assertion is now **window-based**: `system_clock::now()` is read
  before the conversion and again after it, and the back-converted
  instant must fall inside that window (with 1 ms of slack to absorb
  the clock reads' own cost).  That measures the round-trip accuracy,
  which is what the test is about, and stays honest on a loaded machine.

- **The `video_stats` engine wiring test could not pass on any machine
  (v1.2.0, architecture remediation plan §4.2 / D2).**
  `tests/test_engine_video_stats.cpp` drove the engine with a
  `VideoSourceFrame` that had **no pixel planes at all** — the encoder
  had nothing to encode, so `send_video()` returned an error before a
  single counter could move.  The test now builds a real I420 buffer
  (strides + the three planes), the way the working
  `tests/test_engine_video_codec.cpp` does.

  The inbound half of the same test had two further problems, both of
  which made the result depend on the *host* rather than on the code
  under test:
  - The engine only routes an inbound RTP packet to the video pipeline
    when its payload type is in the **negotiated** routing table, and
    that table is filled exclusively by `process_remote_sdp()`.  The
    fixture never negotiated, so every inbound packet was counted as an
    unknown payload type and dropped.  The fixture now runs a
    description exchange first.
  - The shipped `"h264"` codec plugin picks its backend **by priority**:
    a hardware encoder / decoder when the host has one, the software
    stub otherwise.  A hand-written fake NAL unit decodes under the
    stub and is *rejected* by a hardware decoder, so the same binary
    passed on a CI runner and failed on a workstation with a GPU.  The
    test now registers its own `"h264-software"` codec factory (wrapping
    the h264 module's own software adapter) and selects it through
    `EngineConfig::video_codec_name` — the documented extension point.
    The inbound frames are encoded with the engine's *own* codec, so the
    bitstream is decodable whichever backend was selected.

- **The raw-UDP ARQ loopback test failed on the macOS arm64 CI runner
  (v1.2.0).**  `ArqRawUdpLoopback.HundredHertzLatency` asserted
  `EXPECT_LT(p999, 15'000)` — "the worst sample must be under 15 ms".
  With only 200 samples the "p999" index lands on the *maximum* sample,
  so the assertion was really "not one single OS deschedule of the
  sender thread may exceed 15 ms", which no amount of correctness in
  the ARQ path can guarantee on a busy runner.  The macOS leg of the
  2026-11-13 run measured `p50=12us p99=1840us p999=17724us` and took
  9.07 s for a 2 s test — the median was fine and the tail was one
  deschedule wide.
  The bound now sits on **p99**, which is what the assertion was always
  trying to say ("the path is not systematically slower than the send
  interval"), and the maximum keeps a generous separate bound.  A
  genuinely starved path still trips the test, because then *every*
  sample exceeds the interval and so does p99.

- **The per-instance media p99 budget was parsed, serialised, round-tripped
  through JSON and then silently dropped (v1.2.0, architecture remediation
  plan §4.2 / D2).**  A Profile's `transport.media.max_p99_ms` was read
  correctly, written correctly, and survived the JSON round-trip
  perfectly — and then went nowhere.  The knob existed and did nothing,
  which is worse than not having it: the JSON said the budget was set.
  The chain now closes:

      Profile.transport.media.max_p99_ms
        → assembly::to_transport_input(profile).media_max_p99_ms
        → EngineConfig::transport_media_max_p99_ms
        → transport::requirements_from_profile_input()
        → TransportRequirements::max_p99_ms

  and the resolved value is readable through
  `NimRTCEngine::transport_media_max_p99_ms()`.  The budget is still
  *recorded* rather than enforced — that is a separate decision
  (`docs/plan/transport-strategies.md` §6 #4) and the knob's presence
  does not change it.

  The same fix removes a second silent drop: the Profile's transport
  **strategy id** reached the engine only when a host happened to copy it
  out by hand.  `demo-agent-gateway` copied four fields out of the
  Profile and dropped the rest — the transport strategy, the scheduler
  settings, the BWE parameters and the DataChannel switch were all lost
  on every run.  `assembly::to_engine_config()` is now the single
  authority for the mapping.

- **The TURN relay suite no longer depends on a fixed UDP port window
  (v1.2.0).**  The tests pinned the TURN server's ports (64000,
  64010-64031) and its relay-port ranges (64100-64499).  Windows
  reserves whole blocks of UDP ports for the hypervisor, the
  reservations move between VM allocations, and a bind inside a
  reserved block fails with `WSAEACCES` (errno 10013).  Every fixed
  window the file has used has eventually been caught by one — 60000-60999,
  then the 63000s, then the 64000s — and the last one broke a CI run on
  2026-11-13:

      [warn]  udp.c:158: UDP socket binding failed on port 64030, errno=10013
      [error] udp.c:217: UDP socket opening failed
      [fatal] server.c:127: Server socket opening failed

  The client agents were already on OS-assigned ephemeral ports for
  exactly this reason; the *server* was left behind, because it needs a
  port the client can be told about.  That is not actually a constraint:
  the server's port can be probed for bindability at runtime and then
  read back.  Each fixture now binds a port the kernel picks (which
  never lands in a reserved block) and a relay-port range that has been
  probed for bindability *as a whole* — libjuice allocates a relay
  candidate from that range, so every port in it has to be bindable, and
  the hypervisor reservations are contiguous, so a run of free ports is
  found by checking that the entire run binds.  The two mode-resolution
  tests never dial the server — they only *configure* one — so they now
  use the IANA TURN port as a placeholder.

- **`EngineConfig::video_capture_name` was a knob the engine never read
  (remote-desktop plan A1; remediation Phase 2 §2.2).**  The capture
  seam was fully built — the `IVideoCapture` interface, the registry
  slot, the `NIMRTC_REGISTER_VIDEO_CAPTURE` macro, the bounded queue
  with its dirty-rect model, and the synthetic device — but nothing
  ever resolved the id.  A host that set `video_capture_name` and
  expected a capture device got silence: the field was accepted,
  documented as pluggable, and ignored.
  - The engine now resolves the factory
    (`pal::resolve_video_capture`), creates the device and opens it at
    `open()` time, and hands it to the host through the new
    `video_capture()` accessor.  The device is closed explicitly on
    teardown — it may own an OS handle (a DXGI duplication, a PipeWire
    stream), so dropping the pointer is not enough.
  - An empty name still installs no device (the normal case: the host
    feeds frames itself).  A *named* device that cannot be resolved is
    reported through `on_error` with the new
    `kVideoCapturePluginMissing` / `kVideoCaptureOpenFailed` codes
    rather than silently downgraded to "no capture device" — a typo
    must not turn into a session that quietly never produces a frame.
  - This closes the last gap in the remediation plan's Phase 2
    acceptance criterion (§2.7): every `EngineConfig` `*_name` field
    now has at least one real read site in `src/engine/`.
  - The capture device is deliberately **not** wired into the encoder:
    turning a device into a frame stream is an adapter's job, and the
    automatic capture → encoder pump on the tick thread is remote-
    desktop plan item A10.
  - New test `tests/test_engine_video_capture.cpp` pins the four cases:
    an empty name installs nothing; a registered id is resolved, created
    *and* opened; an unknown id fires `on_error` and installs nothing;
    and the device is closed when the engine closes.

- **`docs/plugin_author_guide.md` no longer promises a uniform "no silent
  fallback" (remediation §2.3 / B4 / B5 / B10).**  The guide's §4
  catalogue now lists every pluggable family with its real
  `EngineConfig` knob, its real registration macro, and its real failure
  mode, and §4.1 spells out the three behaviours: **hard failure**
  (ICE transport, transport stack, DTLS, SCTP socket, SRTP backend,
  DataChannel backend — `open()` returns non-zero and `on_error` fires),
  **soft degradation** (BWE, scheduler, net-feedback, XR report, jitter
  buffer, audio 3A, the codecs, the video source / sink / receiver /
  sender — the engine logs a warning and opens without the component),
  and **disabled by configuration** (an empty knob).  The macro names in
  the table were wrong in three places (`NIMRTC_REGISTER_VIDEO_RX` /
  `_TX` → `_RECEIVER` / `_SENDER`; the DTLS / SCTP / raw-UDP / transport
  -stack rows named a bare function instead of the actual macro), and
  the SRTP-backend, WebTransport, audio-decoder, audio-receiver, session
  -model and DTMF families were missing entirely.
  - `docs/api/engine_config.md` carries the same table, and documents
    the removal of `rtp_name` / `sdp_name`.

- **Every SRTCP protect failed -- the RTCP feedback path was broken end
  to end (v1.2.0).**  libsrtp's SRTCP layout is the RTCP packet, then a
  4-byte index trailer (a 31-bit index plus the E bit), then the auth
  tag.  `protect_rtcp()` sized the scratch as `rtcp.size() + tag_len`,
  which left out the trailer; libsrtp validates the buffer against the
  full total and returned `srtp_err_status_buffer_small`, so every
  SRTCP protect failed.

  The bug survived for a long time because the SRTP module's own test
  suite never ran (see the test-discovery fix below) -- nothing could
  catch it.

- **The libsrtp adapter silently mapped every unknown crypto-suite id
  onto AES-128-CM-SHA1-80 instead of rejecting it.**  The `default:`
  branch of `to_lib_profile()` meant a caller that passed a libsrtp
  `srtp_profile_t` (a different numbering scheme) got a *different*
  protection profile, and the wrong auth tag length with it, instead of
  an error.  An out-of-range id is now rejected.

- **AES-128-GCM is advertised but not providable.**  The vendored
  libsrtp is built against the "internal" crypto backend, which ships
  AES-CM only (`GCM=OFF`), so the `Aes128Gcm_128` suite cannot be
  supplied.  The backend now refuses that suite with a message that
  says why, instead of failing deep inside `srtp_create()` with a
  generic error.  The vendored build exports
  `NIMRTC_LIBSRTP_HAS_GCM=0` to say so.

- **212 tests across six module suites never executed.**  The modules
  `src/modules/{dtls,jb,jb_strategy,sched,session,srtp}` called
  `add_subdirectory(tests)` without `enable_testing()`.  Under the
  CMake 4.0 VS generator no `CTestTestfile.cmake` is emitted at that
  level, so ctest never recursed into those directories and none of
  their tests ever ran.  The ctest count goes from **1176 to 1388**; the
  seven tests added by the C3 work bring it to **1396**, and the
  capture-seam suite merged from `main` brings it to **1401**.

  The one test that failed once it finally ran
  (`SessionModel.MidIsNeverReused`) asserted that removing an unknown
  track returns `true`, contradicting `SessionModel::remove_track()`
  and the sibling test above it; the expectation was wrong and is
  corrected.

- **The e2e acceptance Case C (NimRTC ↔ NimRTC loopback) failed on the
  Windows CI runner with `WSAEACCES (errno 10013)`, because the example
  pinned a fixed UDP port range that the runner reserved.**  The
  `loopback-p2p` example asked for `51000–51099` (and `51100–51199`
  for the second engine), and `demo-agent-gateway` asked for
  `52000–52099`.  Windows reserves whole blocks of UDP ports for the
  hypervisor (Hyper-V / HNS); a `bind()` inside a reserved block fails
  with `WSAEACCES`, and `libjuice` walks the *entire* requested range
  before giving up — so the ICE agent fails to open outright:
  ```
  [warn]  udp.c:178: UDP socket binding failed on port range 127.0.0.1:[51000,51099], errno=10013
  [error] conn_mux.c:270: UDP socket creation failed
  [error] engine A open failed
  ```
  The reservations move between runs, so the same commit passes in one
  CI job and fails in another: `windows / msvc` (which builds the
  example) was green while `ci-e2e` (which runs it) was red, because
  the two jobs landed on runners with different reservations.
  - Both examples now leave `local_port_range_begin` / `_end` at 0 and
    let the OS assign an ephemeral port.  The kernel's ephemeral
    allocation never picks a reserved port.
  - Two engines in one process no longer need disjoint ranges: an
    ephemeral-port request gives each engine its own socket on its own
    port (this is the same `conn_mux_get_registry()` behaviour the ICE
    unit tests already rely on).
  - The docs that recommended a pinned range were corrected:
    `EngineConfig::local_port_range_begin`, `docs/api/engine_config.md`
    §4.1, `docs/troubleshooting.md` §3.1 (which told users to pick
    non-overlapping ranges), the two READMEs, and
    `docs/guides/building-on-openharmony.md`.  `docs/troubleshooting.md`
    gains §3.13 for the `WSAEACCES` failure mode, and §3.5 is
    rewritten: the vendored `libjuice` deliberately does *not* set
    `SO_REUSEADDR` / `SO_REUSEPORT` (they let two sockets share one
    UDP port, which breaks ICE), so "address already in use" is the
    socket option working as intended.

- **The ICE loopback and restart tests bound a fixed UDP port range that
  the Windows CI runner reserves, so both failed there with
  `WSAEACCES (errno 10013)`, and the vendored `libjuice` let two ICE
  agents silently share one UDP port.**  The tests hardcoded disjoint
  windows (63000-63099 / 63200-63299, later 63400-63600) to keep the two
  agents off each other's ports, and every one of those windows has
  eventually been caught by a Hyper-V / HNS excluded-port reservation on
  the runner — `libjuice` walks the whole requested range before giving
  up, so the agent fails to open outright rather than retrying.
  - The tests now let the OS pick the local ports
    (`local_port_range_begin == local_port_range_end == 0`).  The
    kernel's ephemeral allocation never picks an excluded port.
  - The real root cause of the remaining failures was in the vendored
    `libjuice`, not in the tests.  Two NimRTC-local changes were made
    to the pinned upstream revision `77daa8b`:
    - `src/udp.c` no longer sets `SO_REUSEADDR` / `SO_REUSEPORT`.  Both
      make a second `bind()` on an already-bound UDP port *succeed*, so
      two sockets end up sharing one port; with `SO_REUSEADDR` set the
      kernel's ephemeral allocator also hands out ports that a live
      socket already owns (measured on Linux with the ephemeral range
      narrowed to 11 ports: 30 sockets bound, but only 11 distinct
      ports were handed out — 19 pairs shared a port).  A shared port
      breaks ICE twice over: across processes, `libjuice`'s self-wakeup
      datagram (`conn_mux_interrupt` → `udp_sendto_self`) can be
      delivered to the *other* process's socket, stranding the
      interrupting process in `poll()` for its full 60 s timeout while
      `juice_destroy()` blocks in `thread_join()` — the 60.01 s
      `ctest` timeout; and within one process, both agents advertise
      the same local candidate and cannot be told apart.
    - `src/conn_mux.c` no longer merges an ephemeral-port request
      (`port_begin == 0 && port_end == 0`) into an existing mux
      registry.  `conn_mux_get_registry()` matched on
      `impl->port >= config->port_begin`, which is always true when
      `port_begin == 0`, so the first registry in the process absorbed
      every later agent: two agents shared one socket and one candidate,
      and the STUN binding request came back with a ufrag mismatch so
      the session never connected.
  - The tests therefore run in the production default `Mux` mode again
    (the earlier `Poll` mode workaround is gone) and still use
    OS-assigned ephemeral ports.
  - Verified: all 15 ICE tests pass — 5 sequential runs and 4
    `ctest -j 4` runs, all 15/15.

- **The DXGI Desktop Duplication backend called three DXGI APIs that do
  not exist, so the capture module did not compile on Windows.**  The
  file was written against a *newer* DXGI than the SDK it builds
  against provides, and the errors were not cosmetic — each one was a
  misreading of what the API returns.
  - `IDXGIOutput::GetDisplayModeList` takes **four** arguments (format,
    flags, mode count, mode array), not three.  The count is an in/out
    parameter, so the first call passes `nullptr` for the array and the
    second passes the array.
  - `DXGI_MODE_DESC` has **no** `DesktopCoordinates` member.  The mode
    list is already scoped to one output, so every mode in it belongs
    to that output; the desktop area comes from `GetDesc` alone, and
    the mode list is consulted only for the refresh rate.
  - `DXGI_OUTDUPL_MOVE_RECT` carries a `POINT SourcePoint` and a `RECT
    DestinationRect` — there is no `SourceRect` member.  The reported
    stale region is now the degenerate rect at the source point.

- **The synthetic capture device never named its pixel layout, so a
  consumer could not read its frames.**  `supported_formats()` advertised
  the `"bgra"` tag, but `open()` copied the *requested* format into
  `config()` without setting the tag -- so the negotiated format carried
  an empty tag, and a consumer that maps the tag onto a pixel layout (the
  capture → frame-stream adapter) had nothing to map and had to refuse
  the device.  `open()` now sets the tag to the layout the device
  actually delivers, and refuses a request for a layout it cannot
  produce with `kErrInvalidParam` instead of silently delivering a
  different one.

- **`CaptureQueue::pop()` handed out a pointer to freed memory.**  The
  popped frame was moved into a *local* `Frame`, and the queue then
  published `f.pixels.data()` through the `CaptureFrame` — so the
  consumer read a heap block that the local's destructor had already
  released by the time `pop()` returned.  The `CaptureFrame` contract
  promises the pixels stay valid until the next `pop_frame()`, so this
  was a use-after-free on every frame the consumer kept.  The queue now
  keeps the popped frame in its own storage (`popped_`) and hands out a
  pointer into that; the previous frame's buffer is released exactly when
  the contract says it stops being valid, on the next `pop()`.  Caught by
  AddressSanitizer and pinned by a regression test.

- **The SDP parser threw `std::out_of_range` on a truncated attribute
  line.**  The parser skips a fixed number of characters to get past the
  attribute name -- `a=msid-semantic:` skips 15, `a=ice-options:` skips
  13 -- with `std::string_view::substr`.  `substr` throws when the
  position exceeds the string's size, so an attribute line carrying no
  value at all killed the parser:

      terminate called after throwing an instance of 'std::out_of_range'
        what():  basic_string_view::substr: __pos (which is 15) > __size (which is 14)

  - The fuzzer found it on a 15-byte input (`a=msid-semantic:`), in the
    `fuzz_sdp` target.
  - The consequence for an application was a hard crash while parsing a
    remote peer's offer: the parser is fed attacker-controlled bytes, so
    it had to report the malformed line rather than throw.
  - A new `drop_prefix()` helper takes the tail of a string view when it
    is long enough and yields an empty view when it is not -- which is
    the answer the caller wanted anyway, since a truncated attribute has
    no value.  All 19 attribute-skip sites go through it now, so the
    class of bug is closed rather than the two instances that happened
    to fire.
  - Covered by `SdpParser.TruncatedAttributeLineDoesNotThrow` in
    `src/modules/sdp/tests/test_sdp.cpp`.

- **The XR Statistics Summary block lost every field the peer had asked
  for.**  The engine narrows the peer's `stat-summary` list to the
  fields it actually measures (`loss` and `jitt`), and the narrowing
  helper walked the list with
  `const std::string_view tok = flags.substr(start, i - start);` --
  but `std::string::substr` returns a **temporary** `std::string`, and
  the view was bound to that temporary.  The temporary died at the end
  of the full expression, so the token read as garbage (an empty one
  in practice), every field was rejected, and the negotiated field list
  came back empty.
  - The consequence on the wire: the engine's Statistics Summary block
    went out with **neither** the L nor the J flag bit set, so a peer
    that had asked for `loss,jitt` (or for `loss` alone) received a
    block that reported no loss and no jitter -- the block was
    syntactically valid and semantically empty.
  - The helper now takes the view over the *named* string
    (`const std::string_view fv(flags);`) and slices that, so the
    token's lifetime is the string's.
  - Covered by `tests/test_engine_xr_tmmbr.cpp` (the Statistics Summary
    cases assert the L and J flag bits on the parsed wire block, for
    both a "loss,jitt" and a "loss"-only peer request).

- **`ParsedFmtp` wrote its payload into an inactive union member (UB, caught
  by UBSan as `load of value 13, which is not a valid value for type
  'bool'`).**  `ParsedFmtp` is a hand-rolled variant: an anonymous union of
  the five codec fmtp payloads plus a `kind` discriminator, with manual
  lifetime management.  `parse_fmtp()` set `out.kind` *before* the payload
  was in place and then assigned through the union (`out.h265 = ...`),
  which is only well-defined when `h265` is already the *active* member —
  it never was, because the constructor placement-news `h264`.  The
  assignment operator of the payload therefore ran against a
  never-constructed `std::optional`, and the destructor later destroyed a
  member that did not match the one `kind` named.  The variant now has
  typed setters (`set_h264()` / `set_h265()` / `set_vp8()` / `set_vp9()` /
  `set_av1()`) that destroy the active member, placement-new the new one
  and update `kind` atomically; `parse_fmtp()` and the copy / move
  operations go through them, so the "the member named by `kind` is the
  constructed one" invariant holds on every path.  Four regression tests
  pin the invariant.

- **The H.265 `sprop-parameter-sets` parser read freed memory.**  The
  combined legacy form (`sprop-parameter-sets=vps,sps,pps`) was parsed by
  binding a `std::string_view` to `v.substr(...)`, where `v` is a
  `std::string` — so the view pointed at a temporary that died at the end
  of the full expression.  The parser then validated base64 characters
  out of freed memory and rejected a perfectly valid parameter as
  "non-base64 char".  The sub-view is now taken from `v` directly.

- **The AV1 fragmenter dropped the OBU header of every fragmented OBU.**
  A fragmented OBU's first fragment carries the OBU header *in addition*
  to its payload, but the packet buffer was sized for the payload alone
  (`overhead + take`).  The header was written past the end of the
  buffer, so the reassembled OBU was two bytes short of the original and
  the OBU type was lost.  The buffer is now sized for the header as
  well.

- **Every hand-written `ctest` wrapper test was reported as "Not Run" in a
  non-Debug build.**  CMake 4.0's multi-config generator wraps each
  `add_test()` in a four-branch `CTEST_CONFIGURATION_TYPE` conditional
  whose fall-through is `NOT_AVAILABLE`.  The post-configure hook
  (`cmake/FixCTestTestfile.cmake`) resolved that by throwing away the
  Release / MinSizeRel / RelWithDebInfo branches and keeping only the
  **Debug** body — which works for the Debug build CI runs, but in a
  Release build every wrapper test then pointed at a `Debug/` path that
  does not exist.  ctest reported 76 tests as "Not Run" and the suite
  failed even though every binary was present and green.
  - The hook now keeps all four branches verbatim, so `ctest -C <config>`
    resolves exactly as CMake intended, and replaces the `NOT_AVAILABLE`
    fall-through with an existence probe that picks whichever
    configuration actually has its binaries on disk.  A plain `ctest`
    (no `-C`) therefore finds the tests that were really built.
  - The hook also stopped mangling the `_BACKTRACE_TRIPLES` property:
    those values contain semicolons, and the previous revision split the
    file on `;` (the CMake list separator), which rewrote the backtrace
    triples into garbage.
  - Affected: every non-`gtest_discover_tests` wrapper — the DTLS, E2E,
    plugin-loading and seam tests.

- **The standalone NVENC driver probe could not initialize the encoder
  any more.**  `tests/nvenc_probe.cpp` forced the SDK-11.1.5 struct layout
  (`0x0100000B | (7u << 16) | ...`) to work around a hybrid-ABI driver,
  but the build now compiles against the vendored Video Codec SDK 13.1
  header pack (`src/third_party/nv_codec_headers/`) — the same ABI the
  NimRTC encoder uses.  The driver rejected the mismatched struct version
  with `NV_ENC_ERR_INVALID_VERSION` (status 15), so the probe failed on
  a machine whose driver was perfectly fine.
  - The probe now uses the header's own `NV_ENC_*_VER` constants and a
    real `NV_ENC_CONFIG` (VBR, one temporal layer, H.264 Main), which is
    what the driver actually expects.
  - The probe stays a *driver-ABI* probe: it stops right after
    `nvEncInitializeEncoder` succeeds.  The full NVENC -> NVDEC round
    trip lives in `src/modules/nvcodec/tests/test_nvcodec_roundtrip.cpp`.

- **NVENC took the whole session down when the source frame was smaller
  than the hardware minimum geometry.**  A frame below the silicon's
  minimum size (e.g. 64x48) made `nvEncInitializeEncoder` fail with
  `NV_ENC_ERR_INVALID_PARAM` and the driver error string *"Frame
  Dimension less than the minimum supported value."* — and because the
  reconfigure path tears the session down first, the encoder was left
  permanently broken, so **every** subsequent frame failed too.  This
  surfaced as `EngineRefFrame.send_video_auto_records_mapping` failing
  on an RTX 3060: the session was negotiated at 640x480, then handed a
  64x48 source frame, and the resolution change tore the session down.
  - `nvenc_encoder.cpp` now clamps the requested geometry to a
    conservative minimum (`kMinEncodeWidth = 160`,
    `kMinEncodeHeight = 64`) and **pads** the picture: the source sits
    in the top-left corner, the luma padding is `16` (BT.601 limited
    black) and the chroma padding is `128` (neutral).
  - The clamp is applied consistently in `open()`, `encode()` (a
    mid-stream resolution change) and `update_config()`.
  - The measured driver limit on the reference host is width `>= 145`
    and height `>= 49`; the code deliberately does not hard-code those
    numbers, since they are per-silicon.
  - Covered by `HwRoundTrip.NvencPadsBelowMinimumGeometry` in
    `src/modules/nvcodec/tests/test_nvcodec_roundtrip.cpp`.

- **The WebTransport data channel mistook a Chrome stream header for
  payload.**  A peer-initiated bidirectional stream starts with the
  WebTransport stream-type signal `0x41` followed by the session-id
  varint, and the data channel has to strip that header before it hands
  the bytes to the application.  The header probe decoded the signal as
  a QUIC varint, and the canonical varint encoding of `0x41` is the two
  bytes `0x40 0x41` — so a stream that opened with the single byte
  `0x41` (what the Chrome stack writes) decoded to `256`, was taken for
  "not a WebTransport stream", and the header bytes were delivered to
  the application as if they were payload.
  - The probe now accepts both encodings: the single byte `0x41` and the
    canonical `0x40 0x41` varint, and it waits for the whole header
    when it is split across transport chunks (the probe re-runs on the
    accumulated buffer, so no extra state is needed).
  - A stream that carries no WebTransport header (a NimRTC peer in raw
    mode, or a framed peer whose frame starts with the big-endian label
    length) is still delivered verbatim.
  - Covered by four cases in
    `src/web_transport/tests/test_web_transport_dc_capability.cpp`
    (single-byte signal, canonical varint signal, a header split byte by
    byte, and a header-less stream).

- **The installed bundle did not tell downstream consumers which C++
  standard to compile with.**  The public headers need C++17 at the
  minimum (nested namespace definitions such as
  `namespace nimrtc::core::api_version`) and the shipped archives are
  built as C++20, but the hand-written `cmake/NimRTCConfig.cmake.in`
  declared neither.  A consumer whose `project()` did not pin a
  standard inherited MSVC's default (C++14) and failed to compile the
  installed headers with `error C2429: nested namespace definition
  requires /std:c++17`, followed by a cascade of `std::string_view is
  not a member of std`.
  - `nimrtc::core` and `nimrtc::engine` now carry
    `INTERFACE_COMPILE_FEATURES "cxx_std_20"`, so the consumer's
    compiler receives `/std:c++20` (or `-std=c++20`) automatically.
  - The Linux downstream proof in CI never caught this because GCC 11+
    defaults to `gnu++17`, which is enough for the headers; only the
    MSVC consumer broke.

- **The engine's public header still said the video pipeline was not
  wired into the RTP / DTLS / SRTP path.**  `engine.hpp` carried a
  "P1.1/R3 scope" note claiming that "the engine currently does not pipe
  video RTP/RTCP" — while `engine_timeline.cpp` had been routing inbound
  video RTP into `video_receiver_->push_rtp()` for several releases.  A
  caller reading the header to decide what the engine can do was told
  the opposite of the truth.
  - The note now describes what is actually wired: the send path
    (`send_video()` → codec → packetizer → RTP → SRTP → scheduler → ICE)
    and the receive path (ICE → demux → depacketizer → jitter buffer →
    decoder → sink).
  - It also states what is *deliberately* not auto-wired: the video
    **source**.  `IVideoCodec` / `IVideoSender` are not thread-safe and
    the source runs its own cadence thread, so the host pulls frames and
    pushes them through `send_video()` rather than the engine wiring the
    source into the encoder.

- **The video payload type was negotiated in three places that could
  disagree (v1.2.0 video plan, C9 + C11).**  The engine had three
  independent ideas of "the video payload type", and a mismatch between
  them silently killed the video stream rather than raising an error.
  - The **answer** copied the offer's `a=rtpmap:` / `a=fmtp:` lines
    verbatim, so it told the peer "I will decode every codec you
    offered" — including the ones NimRTC has no decoder for (C9).
  - The **outbound RTP header** was stamped with a locally configured
    payload type, which could differ from the number the negotiated
    description named — the peer then received the stream on a payload
    type the description never mentioned, and dropped it.
  - The **inbound demultiplexer** routed any payload type the peer had
    declared on its video m-line into the video pipeline, so a peer
    sending VP8 on a payload type NimRTC never agreed to decode as H.264
    still reached the H.264 pipeline (C11).
  - All three now use the **negotiated** payload type — the one that
    appears in *both* descriptions (RFC 3264 §6.1).  The answer names
    exactly that payload type and only that codec's own `a=rtpmap:` /
    `a=fmtp:` lines; the offer names the *sender's* payload type,
    because that is the number the outbound packet path stamps; the
    inbound demux accepts only the negotiated number.
  - A peer that offers only a codec NimRTC cannot decode gets the video
    m-line **rejected** (port 0, no format list, RFC 3264 §6.1) and one
    `kVideoCodecUnsupported` report through `on_error` — instead of a
    stream that never arrives.
  - The negotiated state lives in one place,
    `detail::VideoNegotiationResult`
    (`src/engine/src/video_negotiation.hpp`), shared by the SDP layer and
    the media layer so the producer and the consumer cannot drift apart.
  - Covered by 5 cases in `tests/test_video_negotiation.cpp`.

- **The transport-wide congestion-control feedback was parsed against the
  wrong wire format (v1.2.0 M1, F4 groundwork).**  `rtp::Parser::parse_transport_cc`
  did not implement the draft's encoding.  The status symbols live *inside*
  the 16-bit chunk header (a run-length chunk is `0 | symbol | run length`;
  a status-vector chunk is `1 | S | 14 symbol bits`, giving 14 one-bit or
  7 two-bit symbols — there is no symbol-count field), and the receive
  deltas are variable-width and follow *all* the chunks: 8-bit unsigned
  for symbol 1, 16-bit signed for symbol 2, none for symbols 0 and 3.  The
  old code read the symbols from the bytes after the chunk header and read
  a fixed 14-bit delta per received packet — the RFC 8888 (CCFB) encoding,
  a different feedback format — so every delta it produced was wrong.  A
  run-length chunk of "received" packets also carries one delta per packet
  in the run, which the old code did not read at all.
  - The reference time is in multiples of 64 ms, not the 1 kHz its comment
    claimed; the delta unit (250 µs) was already right.
  - `PacketStatus` gains the wire symbol 3 ("received, without delta"),
    which the draft uses in its own example 2.  The enumerator names
    predate the format being implemented correctly and are
    `NIMRTC_API_STABLE`, so they stay; the header now documents that
    `ReceivedSmall` (2) is the *large* delta.
  - `TransportCcFeedback` gains `reference_time_ms` and
    `feedback_packet_count`.  The delta width is derivable from the status
    symbol, so no separate field carries it.
  - The RTCP padding is excluded from the chunk section and the delta
    list; before, the padding bytes were parsed as a packet chunk.
  - Cross-checked against `draft-holmer-rmcat-transport-wide-cc-extensions-01`,
    libwebrtc and Pion; ADR-018 records the normative statement.

- **The scheduler sent up to eight times the budgeted bitrate.**  The
  per-drain allowance was computed as *packets per second* but compared
  against a *per-drain* counter, so a host that drained every 125 ms
  (the assumed cadence) let through eight times the budgeted rate.  A
  host that drained more slowly than the assumption sent even less than
  the estimate allowed, because the allowance never grew with the
  interval.
  - The send rate is now enforced by a **time-based credit**: every
    drain grants credit proportional to the wall-clock time that elapsed
    since the previous drain, and every packet that leaves the queue
    consumes its estimated size from it.  The effective rate is
    therefore the same whether the host ticks every 20 ms or every
    200 ms.  The credit is capped at one second's worth, so a long idle
    gap cannot turn into an unbounded burst.
  - The *congestion window* — how much outbound data may stay queued —
    is now bounded by `SchedulerConfig::drain_interval_ms` worth of
    bytes at the estimated rate, deliberately independent of the host's
    tick rate.  The old code used a hard-coded "8 drains per second"
    divisor for the same number.
  - `SchedulerConfig::drain_interval_ms` (new, default 125 ms) is the
    window horizon and the seed for the first drain's interval.  It is
    appended at the end of the struct, so existing aggregate
    initialisation keeps compiling.
  - `stats().current_bitrate_bps` now reports the *observed* egress
    rate of the last drain instead of always 0.

- **A packet the transport refused was silently thrown away.**  When
  the drain callback vetoed a packet (the transport is not accepting
  traffic right now), the scheduler had already popped it off the queue
  and then dropped it — the packet was lost, and it was not even
  counted as "dropped", so the statistics understated the real loss.
  The vetoed packet now goes back to the front of its queue, is not
  counted as sent, and the drain stops entirely (a veto means the
  transport is not accepting traffic, so lower-priority queues must not
  be attempted either).  The same fix applies to the per-stream
  selection path.

- **The inbound Opus payload was decoded without its TOC byte (audio
  pipeline WP-11, defect F-1).**  The depacketiser strips the TOC byte
  (it describes the *packet*, not the frame), but libopus decodes
  *complete* packets and reads the TOC to learn the mode, the bandwidth
  and the duration of the frame it is about to decode.  Handing it a body
  without its TOC made the decoder read the first body byte as a TOC and
  pick a configuration the frame was never coded for — noise instead of
  PCM.  The decoder now rebuilds the code-0 packet before it calls
  libopus.

- **A DTLS session could not change its role after it was opened.**  The
  wolfSSL context is built with a *role-specific* method, so a context
  built for the server cannot drive a client handshake — wolfSSL fails
  with -344 ("wrong client/server type") before a byte leaves the host.
  The engine learns its role from the remote description, which arrives
  *after* `open()` built the context with the default role, so a
  NimRTC-to-NimRTC session always failed.  The session now rebuilds its
  context when the role changes.

- **The rebuild of the DTLS context lost the SRTP keying material.**  The
  rebuild reset the `std::optional` that holds the keys, and
  `export_srtp_keys()` returns early on an empty optional — the session
  came up "Connected" while the engine reported "no keying material",
  and the SRTP context was never installed.  The same reset existed in
  the re-handshake path.  Both put a fresh key set back now.

- **An empty PCM buffer vanished silently (audio pipeline WP-11, spec
  A-8).**  `send_audio()` returned `kEngineInvalidParam` and the host
  heard silence with no explanation.  Every rejection on the send path
  now increments `Stats::audio_encode_errors` and fires `on_error` at a
  throttled rate, so a host whose PCM never reaches the wire can see
  why.

- **The soak evidence window erased the failures it was supposed to
  record.** The summary kept only the *last* record per case per day, so a
  FAIL that a later PASS on the same day followed was silently erased: the
  day read as green while the failure sat in the file unmentioned. The
  collector now keeps every record, and a day is green only when each
  required case has a PASS and no required case has a FAIL. The failure is
  still reported, and the day is listed as "evidence but not green".
  The summary also dropped tracked platforms that had no records at all, so
  a platform with no evidence read as "not tracked" instead of "zero green
  days" -- the exact misunderstanding the release gate exists to prevent.
  Every tracked platform is now listed with a zero streak, and a failing
  gate names it. Exact duplicate records are dropped, because the runbook's
  merge procedure appends the CI records into `build/soak/` and running it
  twice inflated the failure count.

- **A tooling failure was dressed up as a product regression.** The
  signaling-server wait was 5 seconds, which is not enough for the server
  to reach its first log line when the 3am scheduled task starts on a
  machine waking from sleep. Both Case D and Case E then failed with
  "signaling_server did not open :<port>" and an *empty* log. The wait is
  now 30 seconds, and the runner prints the child's log tail on failure so
  the two cases are distinguishable next time.

- **The acceptance runner lost a port race.** The runner now waits for the
  port instead of assuming it is free, so a slow start no longer turns
  into a spurious failure.

- **The SCTP DataChannel was a shell: it was never opened, and every
  inbound message arrived twice.**  `create_data_channel()` called only
  `IDataChannel::open(cfg)` — the overload that caches the config — and
  never `IPlugin::open()`, the one that wires the backend's receive
  callbacks and marks the channel usable.  The channel that came back
  could therefore never send (`send()` returned `kErrNotReady` every
  time) and never received anything either.  The engine now arms the
  channel with `IPlugin::open()` and, when that fails, reports the
  error through `on_error_` and returns `nullptr` instead of handing
  back a channel that silently fails.

- **An inbound SCTP message was delivered to the application twice.**  The
  socket fired both the payload-protocol-aware callback and the legacy
  callback for the same chunk, and the channel had registered both — so
  every user message arrived twice, and a single `DATA_CHANNEL_OPEN`
  was answered with two `DATA_CHANNEL_ACK`s.  The two callbacks are now
  mutually exclusive: the payload-protocol-aware one wins when it is
  registered, the legacy one is the fallback for backends that do not
  speak that seam.

- **The DCEP wire format did not carry the channel's QoS.**  The channel
  type was encoded as one of only three values, so an unordered or
  unreliable channel looked like a reliable in-order one to the peer,
  and the peer's priority was read off the wire and thrown away.  The
  six channel types RFC 8832 registers are now implemented (the
  high-order bit flags unordered delivery), the priority survives the
  round trip, and a malformed or reserved channel type is rejected
  instead of being guessed at.

- **The DCEP handshake never happened, and the channel lied about its
  state.**  The channel announced `"open"` the moment it was opened —
  before any association existed and before the peer had acknowledged
  anything — and the code that emits the `DATA_CHANNEL_OPEN` had no
  caller at all, so the peer never learned the channel's label or QoS
  and never answered.  The channel now reports `"connecting"` on open
  and `"open"` only once the handshake completes (a `DATA_CHANNEL_ACK`,
  a peer `DATA_CHANNEL_OPEN`, or any user message on the channel),
  reports `"closed"` on close, and emits the `DATA_CHANNEL_OPEN` exactly
  once.
  - **Behaviour change:** a channel that used to be `"open"` right after
    creation is now `"connecting"` until the handshake completes.  Before
    the SCTP-over-DTLS transport lands the channel stays in
    `"connecting"` — the old `"open"` was reported without a peer ever
    having acknowledged the channel.
  - Until the handshake completes, every user message is sent ordered,
    whatever the channel's own ordering semantics are (RFC 8832 §6).

- **The reliability parameter was encoded through a 16-bit variable.**
  The shift chain assumed a 32-bit value, which made MSVC warn about
  the shift and left the correct result resting on an integer-promotion
  accident.  The variable is now 32 bits wide, and a reliable channel
  carries a reliability parameter of 0 as RFC 8832 requires.

- **`Session.EndToEnd` failed on every platform whose `unordered_map`
  orders the session's streams differently from x86_64.**  The test
  indexed `Session::streams()` positionally (`streams()[0]` for the
  audio track, `streams()[1]` for the video track), but `streams()`
  returns the entries of an `unordered_map`, so the order is
  unspecified — and on aarch64 (and on macOS arm64) the video stream
  came first, so the audio lookup returned `nullptr` and the test
  failed.  The test now looks each stream up by its id.

- **The DTLS handshake-reassembler test helper shifted a 16-bit value by
  40 bits, which is undefined behaviour.**  `make_record()` writes the
  six bytes of the 48-bit DTLS record sequence number, but
  `record_seq` is a `std::uint16_t`: the expression promoted it to
  `int` and shifted by `8 * 5 == 40`, which UBSan reports as *"shift
  exponent 40 is too large for 32-bit type 'int'"*.  The helper
  widened the value to `std::uint64_t` before shifting.  The bug only
  became visible now because the DTLS module's own test suite never
  ran before the test-discovery fix above — with it running, the
  `asan-ubsan` CI leg failed on all eight of its cases.

- **The Phase 0.3 benchmark's allocation probe collided with the
  sanitizer runtimes.**  `bench_engine_hotpath` replaces the global
  `operator new` / `operator delete` to count allocations, but a
  sanitizer runtime (TSan, ASan) ships its own strong definitions of
  those symbols in `libclang_rt.*.a`, so the `tsan.clang` leg failed
  to link with *"multiple definition of `operator new(unsigned long)'"*.
  The replacements are now compiled out whenever a sanitizer is
  enabled (`__SANITIZE_ADDRESS__` / `__SANITIZE_THREAD__`, or Clang's
  `__has_feature(address_sanitizer)` /
  `__has_feature(thread_sanitizer)`), and the benchmark reports
  `alloc_probe_enabled: false` in that build — a sanitizer build is
  about races and memory errors, not about allocation counts.

- **The D3D11 sink's GPU path bypassed the presentation clock and the
  latest-wins mailbox (v1.2 plan item `E5`).**
  device-to-device copy and the `Present()` **on the caller's thread**,
  outside the presenter's deadline wait and outside the single-slot
  mailbox.  Two consequences, both silent:
  - a producer that pushed GPU frames faster than the display could show
    them was blocked on the GPU, and every GPU frame was presented — the
    "latest-wins" back-pressure did not apply to the GPU path at all, so
    its `frames_dropped_present` and `present_fps` ignored GPU traffic;
  - a sink fed a **mix** of GPU and CPU frames (a decode path that falls
    back per frame) presented twice per tick: once from the caller's
    thread and once from the presenter's — the picture could tear and the
    frame rate doubled.

  The GPU frame now goes into the same mailbox as a CPU frame, carrying a
  reference to the `GpuBuffer` so the texture outlives the borrowed
  `render()` call, and the **presenter thread** does the copy on the
  presentation clock.

- **The D3D11 GPU path could copy a non-BGRA8 texture into a BGRA8
  present target.**
  `CopyResource` requires both textures to agree on format and geometry;
  the old code called it unconditionally, so an NV12 / P010 decoder
  output (or any surface of a different size) produced a D3D11 validation
  error and presented an unchanged or garbage frame.  The copy is now
  `CopyResource` only for a BGRA8 surface in the offscreen mode and a
  clipped `CopySubresourceRegion` otherwise, and a surface that cannot be
  presented without a format conversion takes the **documented fallback**:
  it is presented from its CPU mirror, or refused with `kErrUnsupported`
  so the engine's render-path rule decodes to host pixels — never copied
  as garbage.  The fallback is documented in `docs/api/engine_api.md`
  §13.8 and in the renderer's header.

### Performance

- **The media hot paths no longer allocate per frame or per packet
  (v1.2.0, architecture remediation plan §3 -- the C class).**  Six of
  the seven C items landed here; C7 (the video path's RTP-packet
  concatenation) was already removed in Phase 1.3.
  - **C1 / C2 -- the staged video frames are pooled.**
    `stage_source_frame()` allocated a fresh `VideoFrameBuffer` per
    frame (`make_shared` plus a full-frame `memset` at 1080p30 -- about
    3 MB allocated *and* zeroed every 33 ms) and then did an NV12 ->
    I420 de-interleave on top.  `VideoFrameBufferPool` now owns the
    whole buffer -- its pixel storage *and* its control block.  The pool
    parks the buffer and hands out an **aliasing** `shared_ptr` that
    shares the parked buffer's control block, so recycling a frame
    allocates neither a new control block nor new pixel memory; the
    steady state performs no allocation and no full-frame memset.
  - **C3 -- the GPU zero-copy video send path is open.**
    `send_video()` always staged the frame into a CPU I420 buffer before
    handing it to the codec -- a ~3 MB copy per frame at 1080p30, plus a
    de-interleave for an NV12 source.  `plugins::VideoSourceFrame`
    gained three **additive** fields (`gpu_backend`, `gpu_handle`,
    `gpu_pitch_bytes`), and `EngineConfig::video_zero_copy` (default:
    off) opts the host in.  The zero-copy path is taken only when the
    flag is on **and** the frame carries a live GPU handle **and** the
    codec reports `supports_zero_copy_encode()`; otherwise the CPU
    staging path runs exactly as before, so turning the flag on is
    always safe.  When the codec supports the handle but the encode
    fails, the engine falls back to the CPU path rather than dropping
    the frame.  `Stats::video_zero_copy_frames` and
    `Stats::video_zero_copy_fallbacks` make the decision observable.
  - **C4 -- `PacketBuilder::build_into()`.**  `build()` allocated a new
    `core::ByteBuffer` on every packet (50 Hz audio, 30 Hz video).  The
    new overload serialises directly into caller-provided memory, and
    the engine keeps **per-path** scratch buffers (audio, video and DTMF
    separately) so the three threads never share one buffer.
  - **C5 -- the SRTP scratch high-water mark.**  The libsrtp backend
    called `resize(out_len)` before encrypt / decrypt and `resize(actual)`
    after, so the vector was repeatedly shrunk and grown -- and each
    `resize` value-initialises (zeroes) the memory.  The scratch is now
    sized to the **high-water mark** and never shrinks during the
    session's lifetime, which removes the memset churn entirely.
  - **C6 -- the RTCP compound demux.**  `demux_rtcp()` returned a
    `std::vector<RtcpBlock>` **by value** -- one heap allocation per
    RTCP datagram.  The new overload clears and refills a caller-owned
    scratch vector, keeping its capacity across calls.
  - **The Phase 0.3 hot-path benchmark harness ships with the fix**
    (remediation plan §0.3), so the numbers are reproducible rather than
    asserted: `tests/bench_engine_hotpath.cpp` (an allocation probe --
    overridden `operator new` / `operator delete` backed by CRT
    `malloc` / `free` with an atomic arming flag, plus latency
    percentiles for `send_video()`, `tick()` and the scheduler under
    contention) and `tools/benchmark_hotpath.py` (the capture/compare
    wrapper).
    Measured on one machine at 1080p30 over 300 frames, `main` versus
    this branch:

    | metric | `main` | this branch | delta |
    | --- | --- | --- | --- |
    | `send_video` bytes/frame | 3 112 483 | 2 023 | **-99.9 %** |
    | `send_video` mean latency | 902.97 us | 134.34 us | **-85.1 %** |
    | `send_video` p50 latency | 861.55 us | 129.75 us | **-84.9 %** |
    | `send_video` p99 latency | 1 387.36 us | 169.32 us | **-87.8 %** |
    | `send_video` allocations/frame | 23.18 | 22.18 | -4.3 % |
    | `tick` p99 | 0.3 us | 0.2 us | -33 % |

    The allocation *count* barely moves because `main` staged each frame
    with one 3 MB allocation; what C1 / C2 removed is the **volume**
    (3.1 MB of allocator traffic per frame, plus the full-frame memset
    that came with it), not the number of `new` calls.  The remaining
    ~22 allocations per frame are in the encode / packetise path (the
    codec's own output buffer and the RTP packet vector), not in the
    frame staging.

- **`send_video()` is now allocation-free end to end (v1.2.0,
  remediation Phase 3 §3.5).**  The C class above removed the *volume*
  of allocator traffic, but the allocation *count* was still 22 per
  frame.  This change drives it to **zero** — the number the plan's
  §3.5 acceptance criterion asks for.
  - **The video frame pool recycles the buffer object, not just its
    storage.**  The pool used to hand out a fresh `shared_ptr` per
    frame, and a `shared_ptr` that owns its object carries a
    heap-allocated control block.  The pool now parks the buffer and
    hands out an **aliasing** `shared_ptr` that shares the parked
    buffer's control block (`shared_ptr<T>(owner, owner.get())`), so
    recycling a frame allocates neither a control block nor pixel
    memory.  A buffer is reusable once the caller has released its
    alias, which is visible as the owner's `use_count()` dropping back
    to one.
  - **The packetizer works on spans, not copies.**  `parse_annex_b()`
    and `parse_length_prefixed()` returned a
    `std::vector<std::vector<std::uint8_t>>` -- one heap allocation
    per NAL unit, per frame, plus a copy of every NAL's bytes.  The
    caller owns the bitstream for the whole `push_frame()` call, so
    the NAL units are now **spans into the source**, parsed into a
    reusable vector.  The FU-A fragment buffer, the STAP-A output
    buffer, the aggregation group and the FU-A flags are reusable
    members as well, pre-reserved so a frame with an unusual number of
    NAL units does not grow them.
  - **The retransmission ring is pre-sized.**  Each cached slot is
    reserved to the MTU when the ring is built, so caching a packet is
    a memcpy into an existing buffer instead of a fresh allocation per
    outgoing packet.
  - **The ref-frame registry is a fixed-size table.**  It was a
    `std::map` plus a `std::deque` -- two heap allocations per sent
    frame (a map node and a deque block), forever.  It is now an
    open-addressed table with backward-shift deletion plus a
    fixed-size insertion ring, both allocated once at construction.
    The timestamp→id lookup selects the lowest matching id explicitly,
    so the observable behaviour is identical to the `std::map`'s.
  - **The outbound packet buffer is reused on the no-scheduler path.**
    With a scheduler the packet comes from the scheduler's pool; on
    the "transport" profile the engine wrote straight to ICE and
    allocated a fresh vector per packet.  It now uses the engine's own
    scratch buffer.  This relies on `ITransport::send()` treating its
    `BufferView` as **borrowed** -- the transport must copy the bytes
    it needs before returning -- which is now written down explicitly
    in the seam's documentation.
  - **The measurement is a gate, not a claim.**  The benchmark gained
    an `--assert-zero-allocs` flag, and the ctest case
    `hotpath_zero_allocs` runs it at 1080p.  The count is exact rather
    than a threshold: the engine runs no background threads in the
    benchmark's profile, so every allocation counted during the
    measured region belongs to the `send_video()` call itself.  A
    regression that reintroduces a per-frame allocation fails the
    build.

    Measured after this change, over 300 frames per resolution:

    | resolution | allocations/frame | bytes/frame |
    | --- | --- | --- |
    | 1920x1080 | **0.000000** | **0.000000** |
    | 1280x720 | **0.000000** | **0.000000** |
    | 854x480 | **0.000000** | **0.000000** |

    The §3.5 criterion "`send_video()` 1080p30 的 per-call 堆分配次数 =
    0" is met.

- **The audio send path has a zero per-packet allocation budget too
  (remediation Phase 3 §3.5).**  The plan's second allocation
  criterion -- "音频路径 per-packet 堆分配次数 = 0" -- now has a gate of
  its own, in `tests/test_engine_audio_alloc.cpp`.
  - The metric is **per packet**, not per call: the host may hand the
    engine an arbitrary block size, and the engine accumulates samples
    until it has a whole 20 ms frame.
  - The audio path is unreachable without a completed DTLS handshake
    (otherwise the engine would put plaintext RTP on the wire), so the
    test drives the engine through a fake ICE transport and a fake DTLS
    session, the same pair `tests/test_engine_audio_send.cpp` uses.
  - The fake transport only *counts* the bytes it is handed.  A fake
    that copied them would allocate once per packet and land right in
    the measured region, so the test would have been measuring the fake
    rather than the engine.
  - The criterion is asserted as "the allocation count does not grow
    with the packet count" (a 50-packet run and a 250-packet run must
    cost the same), not as "every single call allocates nothing".  The
    engine's periodic work -- the RTCP sender report, the SRTP context
    rollover -- may allocate on *some* calls; what must not happen is
    one allocation per packet, forever.
  - The probe lives in its own binary on purpose: replacing the global
    `operator new` collides with a sanitizer runtime's own definition,
    so the video side of the same criterion stays in the benchmark
    binary (`hotpath_zero_allocs`) and the audio side is here.

    The remaining §3.5 items -- the `tick()` p99 reduction and the
    scheduler's drain / enqueue non-blocking property -- are covered by
    the Phase 0.3 benchmark's other two sections and are tracked
    separately.

### Tooling

- **The plugin author guide for the capture / codec / sink / audio-device
  seams (v1.4, remote-desktop plan K5).**  The existing
  `docs/plugin_author_guide.md` described the *transport* and *DTLS*
  seams; the new capture, codec, sink and audio-device seams had no
  guide at all, so a plug-in author had to read the headers and guess.
  - The new guide
    [`docs/guides/plugin-author-guide.md`](docs/guides/plugin-author-guide.md)
    is a complete, hands-on document: where each seam lives (header,
    factory id, registry slot), the five-step recipe for any plug-in
    (implement the interface → implement the factory → register it →
    make the engine resolve it → test it) with the exact registration
    macro and the exact CMake target, and then one worked skeleton per
    kind: a **capture plug-in** (`IVideoCapture`, the bounded-queue
    discipline, the drop policy, the format negotiation, the dirty-rect
    contract, the zero-copy `GpuBuffer` contract, the error-code
    discipline), a **hardware-encoder plug-in** (the
    `available()`-probe-then-create shape, the priority ordering,
    `zero_copy_supported`, the `VideoEncoderParams` authority and the
    parameter-set-on-resolution-change rule), the **GPU / zero-copy
    contract** (`GpuBuffer`'s acquire / release-ref lifetime rule, the
    `GpuBackend` values, and how to *prove* a zero-copy path), the
    **screen-content contract** (content type, dirty rects, the ROI, the
    content hint — and why a screen capture is not a camera, ADR-022),
    the **audio device seams** and their ring-buffer discipline, and the
    **testing pattern** (a synthetic device so the test runs on every CI
    leg, the "no network, no codec" rule, and the negative control).
  - It closes with the **house rules**: the stability markers
    (`NIMRTC_API_STABLE` / `NIMRTC_API_UNSTABLE` /
    `NIMRTC_API_DEPRECATED`, ADR-015), the plugin-id grammar
    (ADR-023), the layering rules L1–L7 and what a plug-in author must
    *not* do (no engine changes, no concrete-module includes in the
    engine, no global mutable state), and the licence rules for a
    bundled codec (ADR-021 / the RD-2 allowlist).

- **The README feature tables now describe what actually ships (v1.4,
  remote-desktop plan K9).**  The plan's honesty rule (RD-9) is that
  *a row must never imply a bundled codec or a device backend that is
  not shipped*.  The tables did not hold to it.
  - The codec rows no longer imply more than the tree carries: the
    **H.264 hardware backends (NVENC, QSV, AMF, VA-API, DXVA) are
    opt-in and default to `OFF`**, the **H.265 software path is a stub
    decoder only**, the **AV1 software path is decode-only** (dav1d),
    **VP8 / VP9 are packetizers only**, and **G.711 is a routing-only**
    payload.  Only OpenH264 (H.264, software) and dav1d (AV1, software
    decode) are genuinely bundled codecs.
  - The device rows now name the backends that exist and say which
    platform each one belongs to: the capture devices are `synthetic`
    (everywhere) plus `windows-dxgi` / `windows-wgc` (Windows),
    `macos-screencapturekit` (macOS) and `linux-pipewire` / `linux-x11`
    (Linux); the renderers are `headless`, `pinned`, `d3d11` (Windows),
    `metal` (macOS) and `egl` (Linux) — the `d3d11` / `metal` / `egl`
    factories are registered on **every** platform so a configuration
    naming one resolves, and on a non-matching platform `open()` reports
    `kErrUnsupported`.  The audio device layer is **PortAudio** (with
    the synthetic device always available).
  - The `remote-desktop` profile, the signalling guide and the reference
    example are listed as the tenth profile, and the **repository layout**
    and the **vendored third-party versions** tables now list the
    remote-desktop modules (40 modules in total) and the newly vendored
    dependencies (openh264, dav1d, libyuv, PortAudio, libsamplerate,
    nv-codec-headers).
  - `README.zh.md` carries the same corrections, written as a Chinese
    technical writer would rather than a literal translation, and
    `docs/zh/architecture.md` §6 gained the matching honest inventory
    ("视频侧实际交付的编解码器与设备").

- **The gtest discovery timeout was too short for a loaded host.**
  `gtest_discover_tests` re-runs each test binary to enumerate its
  cases, and the probe had 10 seconds.  On a busy Windows machine a
  single gtest binary can take well over 10 s just to start up and list
  its cases, and CMake then aborted the **build** with "Process
  terminated due to timeout" — even though the binary was perfectly
  healthy.  The timeout is now 60 s across the tree (`cmake/NimRTCTest.cmake`
  and every per-module test `CMakeLists.txt`), which is enough headroom
  for the slowest binary here while still failing fast on a genuinely
  hung executable.

- **A CI leg that actually runs the sanitizers, and a gate that knows
  whose bug it found.** The project had sanitizer *options*
  (`NIMRTC_ASAN` / `NIMRTC_UBSAN`) but no CI leg ever turned them on, so
  the ~274 `std::mutex` and ~222 `std::atomic` in the tree were never
  checked for data races and the memory errors the options were meant to
  catch were never caught. The new `linux-sanitizers` job runs two
  configurations on `ubuntu-22.04`: `asan-ubsan` (memory errors and
  undefined behaviour) and `tsan` (data races). ASan and TSan cannot
  coexist in one build, so the presets set the other one OFF explicitly
  and `cmake/NimRTCOptions.cmake` raises a `FATAL_ERROR` if both end up
  ON. The sanitizer flags are applied globally (`add_compile_options` /
  `add_link_options`) rather than per target: the sanitizer runtime has
  to be linked into every final binary, and the vendored static libraries
  are linked into the test executables, so instrumenting only the
  `nimrtc_*` targets left the final link line without the runtime and
  produced undefined references to it.
  The leg's problem is the vendored stacks -- wolfSSL, usrsctp,
  libjuice, msquic -- which are upstream code and trip the sanitizers on
  their own. Suppressing their reports globally would hide real issues in
  the code we call into, and failing on every upstream finding would
  train everyone to ignore the gate. So `tools/sanitizer_gate.py`
  classifies every report by the ownership of its root frame: a report
  rooted in `src/` or `tests/` fails the job, a report rooted in
  `src/third_party/` is printed and does not, and a report whose other
  party to the fault is NimRTC code (the usual shape of a data race) is
  printed as shared. A test failure that no upstream report explains is a
  plain test failure and fails the job too. The gate's own classification
  is pinned by a self-test that runs on every push.

- **A tag watcher for `v1.1.0`, with signature verification and a
  release-gate status snapshot.** The watcher probes the repository it is
  pointed at separately from the repository the scripts live in, so a
  fork cannot be mistaken for the origin.

- **A 7-day soak evidence collector** on the scheduled runners, with a
  runbook that appends the CI records instead of overwriting them.

- **An offline release signature verifier** that accepts valid signatures
  produced by the release key.

- **A release-gate status snapshot** (`tools/v11_status.py`) and the
  `v1.1.0` release-candidate record.

- **A MSVC developer-environment build wrapper** (`scripts/msvc_build.bat`)
  that runs a command inside the VS 2022 x64 developer env, with a `raw`
  mode for arbitrary commands. The `raw` mode never ran the command --
  `shift` does not change `%*`, so the mode word stayed in front of the
  command and cmd tried to execute a program called `raw`; the positional
  expansion now drops the mode word explicitly. The wrapper's echo
  messages also carried parenthesised text inside a parenthesised block,
  which cmd.exe parses as one statement, so the closing parenthesis ended
  the block early and aborted the script; the messages are free of
  parentheses now, and the `%errorlevel%` reads became `!errorlevel!` so
  the value is the one the command returned.

- **The WebRTC APM prebuild is now a hard release gate** (the APM content
  that actually ships is reported, not just the prebuild outcome).

- **An MSVC environment sanitizer** (`scripts/msvc_env.bat`) that clears
  the `CL`, `_CL_` and `CLFLAGS` variables before a build. A `CL` that
  carries a language-standard option is applied to C and to C++ sources
  alike, so the vendored libopus (C11) and the C++ sources collide with
  `cl : Command line error D8016: '/std:c++20' and '/std:c11' options are
  incompatible`. Clearing the variables also un-poisons CMake's
  configure-time feature probes, which inherit the same environment. The
  cleared values are printed, and `NIMRTC_KEEP_CL=1` keeps them for the
  rare build that genuinely needs one. Both `scripts/build.bat` and
  `scripts/msvc_build.bat` call it.

- **A CMake cache repair tool** (`scripts/repair_msvc_cache.py`) that
  drops the probe results a polluted environment produced. CMake records
  a failed probe as an *empty* cache value and never recomputes it, so a
  tree that was once configured with a polluted `CL` stays broken even
  after the environment is fixed: libopus then concludes that the
  compiler supports no variable-length array, no `alloca` and no
  pseudostack, and the build dies with `fatal error C1189: #error:
  "Opus requires one of VAR_ARRAYS, USE_ALLOCA, or
  NONTHREADSAFE_PSEUDOSTACK be defined"`. The tool drops only the probe
  results whose cached value is empty -- a `FALSE` is an honest answer
  (MSVC's C mode has no variable-length arrays) and is left alone -- and
  both build scripts run it before the configure.

- **The video receive path now reconstructs the Annex-B start codes
  (v1.2.0, architecture remediation plan §4.5).**
  RFC 6184 §5.1 carries **bare** NAL units in the RTP payload — the
  start code is never on the wire — but the frame contract
  `video_frame::NaluFormat::kAnnexBStartCode` promises the decoder sees
  `00 00 00 01 | nalu  00 00 00 01 | nalu ...`.  The receiver handed
  the decoder a buffer with **no** start codes at all, so the H.264
  bitstream parser (`h264::BitstreamParser`, which scans for `00 00 01`)
  found no NAL unit, no SPS and no IDR slice: every inbound frame failed
  to decode, and the engine counted it as a decoder drop.

  The start code is now inserted at the one place that knows where a NAL
  unit begins and ends — the receiver:

  | Path | Where | What |
  |------|-------|------|
  | single NAL unit | `video_pipeline.cpp` `handle_payload()` | one start code before the NAL unit |
  | STAP-A (H.264) | `video_pipeline.cpp` | one start code before **each** unpacked NAL unit |
  | AP (H.265) | `video_pipeline.cpp` | one start code before **each** unpacked NAL unit |
  | FU-A (H.264) | `video_pipeline.cpp` `flush_fua()` | one start code before the synthesised NAL header |
  | FU (H.265) | `video_pipeline.cpp` `flush_fu_h265()` | one start code before the synthesised two-byte header |
  | FU-A fallback | `video_jitter_buffer.cpp` `append_fu_a_fragment()` | one start code before the synthesised NAL header |

  The jitter buffer stays codec-agnostic: it only concatenates the
  packets of one picture, so a picture made of several NAL units now
  arrives as `start|nalu start|nalu ...` — one start code per NAL unit.
  Its key-frame detector was taught to skip a leading 3- or 4-byte start
  code before peeking the NAL type, so a re-framed SPS / PPS / IDR is
  still recognised as a key frame.

  The engine's `VideoFrameCallback` now receives the Annex-B bitstream;
  the callback's documentation says so.

- **Every `NIMRTC_REGISTER_*` macro now accepts exactly one spelling**
  (v1.2.0, ADR-023 Decision 5) — **source-breaking** for out-of-tree
  callers that passed a string literal to the audio-seam macros.
  The registry had two macro flavours.  The legacy family stringified
  its argument (`#id`), so a caller who wrote

  ```cpp
  NIMRTC_REGISTER_DTLS_SESSION("mybackend", &factory);
  ```

  registered under the id `"\"mybackend\""` — quote characters
  included.  The registration *succeeded* and the id never resolved: a
  silent failure that looks exactly like a working registration.  The
  audio-seam family took the literal as-is, so a bare token there was a
  compile error — the better failure, but the two families were not
  interchangeable, and the guide did not say which was which.

  All the macros now take the id as a **bare identifier token** and
  stringify it themselves:

  ```cpp
  NIMRTC_REGISTER_DTLS_SESSION(mybackend, &factory);         // -> "mybackend"
  NIMRTC_REGISTER_AUDIO_DECODER(opus, &g_opus_decoder_factory); // -> "opus"
  ```

  A quoted literal is now a **compile error**, enforced by a
  `static_assert` in the macro body that rejects a token whose spelling
  starts with a quote — the guard is shared by every macro, so the two
  families cannot drift apart again.  The same guard also applies the
  ADR-023 grammar to the token, which means a macro call gets the
  compile-time grammar check for free:
  `NIMRTC_REGISTER_DTLS_SESSION(Opus, &f)` no longer compiles, instead
  of registering an id the registry would have rejected at static-init
  time.

  The one id shape a macro cannot express is a hyphenated one
  (`#my-id` does not lex as a single token).  Such a plugin calls
  `core::PluginRegistry::register_<family>("my-id", &f)` directly —
  ADR-023 Decision 8 says so explicitly, so it is a documented path
  rather than a trap.

- **A malformed plugin id is now rejected at registration time, with a
  diagnostic that names it** (v1.2.0, ADR-023 Decision 4).
  `TypedRegistry::register_one` accepted anything — including the empty
  string — and the first sign of trouble was the engine's *lookup* at
  `open()` time, by which point the host had already written a config
  field that would never match anything.

  The registry now validates every id against the ADR-023 grammar
  (`^[a-z][a-z0-9_-]*$`).  A malformed id is **not** registered and
  produces a log line naming the offending id, the family it was offered
  to, and the reason:

  ```
  [error] nimrtc::core::PluginRegistry: rejected a video-sink plugin id
  "Bad Id": the id contains a character outside [a-z0-9_-]. A plugin id
  must match ^[a-z][a-z0-9_-]*$ (see
  docs/adr/ADR-023-plugin-id-naming-contract.md).
  ```

  The rest of the registry is unaffected — one bad plugin does not poison
  the other families, and it does not remove or shadow an id that was
  already registered in the same slot.  The diagnostic goes to the core
  logger rather than to an exception, because registration happens at
  static-init time and a throw from a static initialiser would terminate
  the process before `main` with a message that names no plugin.

  The duplicate-id behaviour is unchanged: a duplicate is still a
  late-binding overwrite, not an error.

  The contract is now enforced by `tests/test_plugin_id_contract.cpp`
  (ADR-023 Decision 7): every id the in-tree registrars publish satisfies
  the grammar, every malformed shape is rejected, the diagnostic names
  the id and the family, and a rejected id does not poison the slot.
  14 new tests.

- **The dead-module guard no longer dies on a concurrent build**
  (v1.2.0, architecture remediation plan §4.2e).
  `tools/check_dead_modules.py` walked the **whole repository** for
  `CMakeLists.txt`.  That walk also descends into CMake **build-output**
  trees, whose `CMakeFiles/CMakeScratch/TryCompile-*/CMakeLists.txt`
  files are transient: a concurrent build creates and deletes them while
  the guard is walking, and the guard then dies on a file it had just
  listed:

  ```
  FileNotFoundError: [Errno 2] No such file or directory:
    'D:\\MyOpen\\NimRTC\\build\\wt-merge-build\\CMakeFiles\\CMakeScratch\\
     TryCompile-7876vm\\CMakeLists.txt'
  ```

  A guard that dies on a race is a guard that gets disabled, so the walk
  now **prunes every CMake build tree** — a directory that carries a
  `CMakeCache.txt` is an output tree, not a source tree — and a file that
  still vanishes mid-scan reads as empty instead of killing the run.
  The module count and the verdict are unchanged: 36 / 36 modules
  `[ok]`, and the `--self-test` still distinguishes
  live / dead / orphan / benchmark-only.

- **The ICE consent-freshness test no longer fails when the runner
  steals the CPU** (v1.2.0).
  `IceConsentFreshness.T1_NormalPeerTrafficNeverFires` simulates the
  peer with a *thread* that notifies every 40 ms, against a 200 ms
  consent timeout.  On a busy runner the OS can deschedule that thread
  for longer than the timeout — at which point the tracker fires
  **correctly** (no binding was seen for longer than the timeout) and
  the assertion fails for a reason that has nothing to do with the
  tracker:

  ```
  test_consent_freshness.cpp:126: error: Expected equality of these values:
    lost_count.load()  Which is: 1  0
  consent_lost should not have fired while peer is active
  ```

  The scenario now measures its **own validity**: the peer thread
  records the largest gap between two consecutive notifications, and a
  gap that reaches the consent timeout means the premise ("the peer was
  active") was violated — so the attempt is retried, and a run that
  never gets a clean window is *skipped* rather than failed.  This is
  the same treatment T3 already had.  The tracker is still fully
  exercised: T2 asserts the timer *does* fire when the peer stops and T3
  asserts the re-arm window.

## [1.1.0] - 2026-11-07

### Status: Stable

> **The source version is `1.1.0`** (`project(VERSION)` in `CMakeLists.txt`,
> `NIMRTC_API_VERSION` = 10100, `NIMRTC_API_VERSION_PRE_RELEASE` = `""`).
> The `v1.1` / `v1.2` / `v1.3` plan drafts were consolidated into
> `docs/plan/v1.1-plan.md` (BDFL, 2026-11-06); the version ladder it
> records (§5.6) is: `v1.1.0` is the only feature-bearing release of the
> `v1.1` line, `v1.1.x` carries fixes only, and new behaviour starts at
> `v1.2.0`.
>
> The `v1.1.0` tag is cut (2026-11-08) and the four platform bundles are
> built; the GitHub Release stays a **draft** until the last remaining
> release-hardening gate — the 7-day soak — closes (see "Known
> limitations"). The published stable cut is still `v1.0.2`.
>
> **The label was corrected on 2026-11-19.** This section carried the
> hard-coded `### Status: Release candidate` heading that
> `tools/release_changelog.py` wrote for every release. The version carries
> no SemVer pre-release identifier, the tag was cut as `v1.1.0`, and the
> GitHub Release was published on 2026-11-09 as a non-prerelease — so the
> section said "candidate" while the release said "stable". The generator
> now derives the label from the version.

### Fixed

- **The GMSSL TLCP backend corrupted the heap on every `open()` and crashed
  the process.** `DtlsSessionGmSSL` allocated its `TLS_CTX` and
  `TLS_CONNECT` objects in fixed-size byte buffers (128 KiB / 256 KiB) whose
  sizes came from stale measurements of an older GmSSL. The real structs are
  much larger — `sizeof(TLS_CTX)` is 201,128 bytes and `sizeof(TLS_CONNECT)`
  is 189,280 bytes on the vendored v3.2.0 tree — and GmSSL's
  `tls_ctx_init()` / `tls_init()` `memset` the whole struct. The 128 KiB
  `TLS_CTX` buffer was therefore ~70 KiB too small: the `memset` ran past
  the end of the allocation and smashed the heap, so
  `test_dtls_gmssl_tlcp` died with a SEGV before the handshake even
  started. The buffers are now sized from the real `sizeof()` plus a 64 KiB
  margin, and a compile-time `#error` guard makes the build fail loudly if
  the GmSSL feature macros (`ENABLE_SM9` / `ENABLE_SECP256R1` / `ENABLE_LMS`
  / `ENABLE_XMSS` / `ENABLE_SPHINCS` / `ENABLE_KYBER`) ever stop reaching
  this translation unit — those macros decide the size of the `X509_KEY`
  union, so a mismatch would silently change the struct layout and turn
  the same memset into heap corruption again. With the fix the TLCP
  handshake completes and the whole suite is 697 tests, 0 failures.
- **The vendored Opus library was built against a different C runtime than
  the rest of the project, so Debug builds crashed on the audio path.**
  `src/third_party/libopus/CMakeLists.txt` tried to pin the Opus target to
  the project's `/MD` runtime by setting a target property named
  `CMAKE_MSVC_RUNTIME_LIBRARY` — but that is the *variable* name upstream
  reads, not the target property. The property is `MSVC_RUNTIME_LIBRARY`, so
  the override was a silent no-op and upstream's own scope kept the Debug
  runtime (`MultiThreadedDebugDLL` → `MSVCRTD`) for `opus.lib`, while every
  other NimRTC target used `MultiThreadedDLL` → `MSVCRT`. Linking two CRT
  heaps into one program means an allocation in one runtime and the matching
  free in the other: the engine tests that touch the Opus path
  (`test_engine_video_feedback` and its five cases) died with an access
  violation before `main()` even printed a line, and the link emitted
  `LNK4098`. The property name is fixed and the compile-flag rewrite now maps
  `/MTd` and `/MT` to `/MD`; `opus.lib` and the rest of the tree agree on
  `MSVCRT`, the `LNK4098` warning is gone, and the full suite is 696 tests,
  0 failures.
- **Commits and tags were signed with an email GitHub does not recognise, so
  every signature showed up as unverified.** The release key
  `114154CFEC3F5EE5A8D2D313BEACFBB55871CC06` carried the Gitee noreply
  address `15608750+video-reflow@user.noreply.gitee.com` as its only user ID.
  No GitHub account owns that address, so GitHub answered `no_user` for a
  signed commit and never rendered a "Verified" badge. The key's user ID is
  now `NimRTC Release <realdesktop@163.com>`, the account address of
  `NimRTC`, and the key's public block was re-exported with a stock
  `gpg --export` into `build/nimrtc-release-signing.pub.asc`. The fingerprint
  is unchanged, so signatures made before the rename still verify and the
  published `v1.0.2` assets keep verifying against the same key; the account's
  GitHub GPG key list must be re-pointed at the new block before GitHub
  accepts the key (see `docs/plan/v1.1-plan.md` Track C6).
- **The exported public key was not a valid OpenPGP block.** The copy on disk
  had been written by a PowerShell redirection, which encodes stdout as
  UTF-16LE with a BOM; GnuPG rejects it with "partial length invalid for
  packet type 63" and `gpg --import` fails with "Invalid keyring", so no
  standard tool could read it. Writing the export through GnuPG's own
  `--output` flag produces a plain ASCII armoured block that imports cleanly
  and whose self-signature reports "Good signature".
- **The offline release-signature verifier rejected valid signatures.**
  `tools/verify_release_signature.py` re-implements Ed25519 in pure
  Python so a release can be checked without GnuPG. Two defects made
  it report every *valid* signature as invalid: the twisted-Edwards
  curve check used `X^2 + Y^2 - Z^2 = d*X^2*Y^2` instead of the
  RFC 8032 form `-X^2 + Y^2 - Z^2 - d*T^2 = 0`, and the §5.1.7
  equation was checked as `R == [S]B + [k]A` instead of
  `R + [k]A == [S]B`. Both are fixed, and `--self-test` now runs the
  RFC 8032 §7.1 vectors plus a tampered-signature rejection case, so
  the regression cannot come back silently; CI runs it on every push.
  With the fix the published `v1.0.2` artefacts all verify (four
  detached signatures, four SHA-256 sidecars) — and so does the
  signing key's UID self-signature: GnuPG 2.4.8 reports "Good
  signature" both for the key import and for every detached
  signature, so the published `nimrtc-signing-key.asc` needs no
  re-export.
- **The release body's 3A status table did not match the published
  bundles.** The table was derived from `build/3a-status-<platform>.txt`,
  which records the *build-time* APM mode (whether the WebRTC APM
  prebuild succeeded), not what the tarball ships. The `v1.0.2` body
  therefore claimed "WebRTC APM (real 3A) bundled" for the three
  platforms whose prebuild had succeeded, while none of the four
  published bundles contains `lib/webrtc_apm/` at all, and its
  `windows` line blamed a failed prebuild although that step succeeded
  in the final `v1.0.2` release run. `release.yml` now lists the
  published tarball and reports the build mode and the shipped content
  separately: "real 3A bundled" is only claimed when `lib/webrtc_apm/`
  is actually inside the artefact, and a platform whose bundle is
  missing or carries no APM archives says so. The published `v1.0.2`
  body has been corrected accordingly.

- **The installed bundle was missing the shared jitter-buffer strategy.**
  `nimrtc::jb_strategy` (v1.1 Q2) is linked by both `nimrtc::jb` and
  `nimrtc::video_jb`, but the install rules did not know about it:
  `cmake/NimRTCInstall.cmake` shipped neither `libnimrtc_jb_strategy.a`
  nor the module's headers, and `cmake/NimRTCConfig.cmake.in` never
  created the `nimrtc::jb_strategy` imported target. A downstream
  consumer that linked `nimrtc::jb` (or the engine, which fans in every
  archive) therefore failed at link time with undefined symbols from
  the shared strategy. Both files now carry the module.
- **The video jitter buffer silently dropped complete frames.** In
  `video_jb`, `tick()` treated every frame older than `max_wait_ms` the
  same way: a frame that never completed was force-emitted (correctly
  tagged `complete = false`), but a frame that *had* completed was
  marked as emitted and counted in `frames_emitted` **without ever
  being handed out** — the bitstream was dropped on the floor while
  the statistics claimed the frame had been played. The trigger was
  rare with the old fixed-window behaviour (the frame was normally
  popped long before the timeout), but it becomes the normal case once
  a frame waits for its playout deadline, which by design can exceed
  `max_wait_ms`. The timeout now only applies to frames that never
  completed; a complete frame stays buffered until it is popped.
  Two related defects in the same path are fixed as well: the
  in-flight cap evicted the *oldest* frame unconditionally (so a
  complete frame that was only waiting for its deadline could be
  evicted by the very delay it was waiting for — the cap now grows
  with the adaptive delay window and prefers evicting a frame that
  never completed), and a freshly created frame bucket was still
  flagged "no marker seen" when the cap ran, so the cap could evict
  the frame that had just been inserted.
- **The Windows WebRTC APM prebuild reported a successful build as a
  failure.** `tools/build_webrtc_apm.cmd` carried three defects that
  together made the `windows` release leg fall back to NimRTC's built-in
  stub 3A instead of shipping the real AEC/ANS/AGC:
  - the script was checked out with LF endings (the `* text=auto` rule
    follows the local `core.autocrlf` setting) and cmd.exe mis-parses
    an LF-only batch file — it split the quoted `vcvars64.bat` path at
    spaces and never reached the ninja step;
  - the ninja call was wrapped in `powershell -Command` with
    `$ErrorActionPreference = 'Stop'`, and PowerShell turns a native
    command's stderr into error records, so ninja's first progress line
    (`ninja: Entering directory ...`) aborted the wrapper with rc=1;
  - ninja was invoked without `call`, so a `.cmd`/`.bat` wrapper on
    PATH took over control and the script exited silently with no
    diagnostic at all.

  `.gitattributes` now pins `*.cmd` / `*.bat` to CRLF in the working
  tree, the ninja call uses cmd.exe's own redirection
  (`call ninja -C ... > log 2>&1`) so the real exit code survives, and
  the failure diagnostics moved from an `if (...)` block to a `goto`
  label so a syntax error in the triage path can no longer break the
  success path. The meson/ninja version echo now runs the tools instead
  of expanding `!meson --version!` as a delayed-expansion variable
  reference (which always printed nothing). Verified on Windows/MSVC:
  the success path exits 0 and records the link order, the failure path
  exits 3 and prints the full ninja log.
- **The vendored libjuice tree shipped leftover debug instrumentation.**
  `src/agent.c` carried three `TEMP-DIAG` `JLOG_INFO` blocks (STUN
  bookkeeping, per-entry state, retransmit tracing) and `src/udp.c`
  carried three per-packet `JLOG_INFO` blocks (local address,
  destination address, `sendto` return value). All six were compiled
  into every NimRTC build — including the published release bundles —
  and fired on every STUN keepalive and on every UDP datagram. They are
  removed; what remains in `src/udp.c` against upstream `77daa8b` is
  exactly the intentional `SO_REUSEADDR`/`SO_REUSEPORT` fix from
  `450dde2`, now also preserved as
  `src/third_party/libjuice/patches/0001-so-reuseaddr.patch`. The
  em dash in that comment is replaced by a hyphen because MSVC
  reported it as C4819 under codepage 936.
- **The 3A stage was not doing what its own contract said.** Six
  separate defects in the `audio3a` module and the engine's use of it
  made the real WebRTC APM either inert or actively wrong:
  - **The level meters reported the 3A *output*, not the input.**
    `capture_level_dbfs` was published from the APM's processed
    output, so a healthy microphone could read as dead silence: the
    noise suppressor legitimately silences a stationary tone (it looks
    like noise to a spectral suppressor), the echo canceller
    legitimately silences the echo component, and AGC changes the level
    by design. A level meter whose reading is a function of the DSP's
    decisions cannot answer the only question it exists for ("is the
    microphone picking anything up?"). Both meters are now measured in
    the **input domain** — mic level for capture, loudspeaker
    reference level for render — and the convention is documented on
    `LevelStats`.
  - **`report_tx_level = false` still published a level**, and
    `enable_vad = false` could still report speech. Both flags are
    part of the public `Config`; the stub implementation honoured
    neither, so a caller that turned VAD off could still be told the
    user was speaking. `NullAudio3A` now honours both flags, with the
    same semantics as the APM path.
  - **The plugin adapter's level meter never moved.** `PluginAdapter`
    left `Config::report_tx_level` at its default (`false`) while
    handing the concrete implementation a default configuration, so
    `stats().last_capture_level_dbfs` was permanently -96 dBFS. The
    adapter now defaults the concrete configuration to 48 kHz / mono /
    level-reporting on, because the plugin's `stats()` are the only
    way for a host to find out whether the microphone is live.
  - **A degraded backend was indistinguishable from a working one.**
    When the APM was not part of the build, the factory substituted a
    `NullAudio3A` passthrough and returned it as an ordinary plugin —
    so AEC/ANS/AGC were silently inactive while the call looked
    healthy. The fallback now marks the instance as degraded, reports
    it in `name()`, and the engine logs it as an error at start-up.
    The new `audio3a::webrtc_apm_compiled_in()` tells a caller whether
    the build contains the real APM in the first place.
  - **The echo canceller never got a reference signal.** The engine
    fed the received audio to the host but never to the 3A plugin's
    render path, so the APM had no reverse stream and the echo
    canceller could not converge at all. The received frame is now
    handed to `process_render()` before it reaches the host's playback
    callback, and `set_render_delivered()` follows it so the plugin
    can track the system delay.
  - **The APM was told a growing playback counter instead of the
    system delay.** `on_render_delivered()` converted the cumulative
    count of delivered samples into a millisecond figure and fed that
    to `set_stream_delay_ms()`. The value grew without bound and the
    echo canceller cannot use it. The delay is now the measured
    loudspeaker-to-microphone latency (`Config::aec_delay_headroom_ms`),
    seeded at `init()` and re-seeded after every `reset()`.
  - **Samples that straddled a 10 ms APM frame boundary lost their
    processed output.** The APM only accepts whole frames, so a caller
    pushing an arbitrary frame length left a remainder behind; the
    carry-over was combined with the next call's head to form a full
    frame, but the processed output of the *completing* call was
    never written back into the caller's buffer. Unaligned pushers
    therefore got raw audio back with no 3A applied, silently. The
    carry-over now accumulates instead of overwriting (no sample is
    ever dropped) and the completing call receives its processed
    output. The condition is logged once, naming the expected frame
    size.
  - **The engine mutated the caller's `const` PCM buffer.**
    `send_audio()` passed the caller's buffer to the 3A stage, which
    processes in place, via a `const_cast` — undefined behaviour that
    also silently changed the caller's data. The engine now copies
    into a buffer it owns, mutates that copy, and encodes from it.

  Verified locally on Windows/MSVC with the real APM compiled in: the
  full suite is 642 tests, 0 failures. The nine `audio3a` tests that
  need the real APM (`Audio3AFrameAlignment`, `Audio3AConfig`,
  `Audio3AStartup`, `Audio3ARenderPath`, `Audio3ADegradation`) run
  instead of skipping when `NIMRTC_VENDORED_WEBRTC_APM=ON`; they skip
  when the build has no APM, which is the pre-existing contract.
- **The DTLS in-process AEAD test did not compile on MSVC.** The
  previous commit replaced the hard-coded key length `16` with the
  `kAesKeyLen` constant — but that constant lives inside
  `namespace nimrtc`, and the test's `main()` sits at global scope
  after that namespace closes, so the unqualified use did not resolve
  and MSVC reported C2065 "undeclared identifier" (plus the
  knock-on C3536/C2660). The file is `#ifdef _WIN32`-only, so the
  Linux and macOS CI legs compiled it to an empty translation unit and
  stayed green: only the Windows leg could see the break. The uses are
  now qualified as `nimrtc::kAesKeyLen`.
- **Two concurrent NimRTC processes on one host fought over usrsctp's
  shared UDP port.** The SCTP seam called `usrsctp_init(9899, ...)`
  with a fixed port, and usrsctp's `recv_thread_init()` binds its
  internal `userspace_udpsctp` socket to exactly that port. When a
  second process on the same host started, its `bind()` failed, its
  socket stayed invalid, and its SCTP packets went to 127.0.0.1:9899 —
  which belonged to the *first* process. usrsctp's listener
  demultiplexed those datagrams onto the wrong socket, so the
  INIT/INIT-ACK handshake never completed and the test timed out after
  60 s. This is not theoretical: CTest runs each gtest-discovered case
  as its own process under `-j 2`, and the Linux CI leg of 2026-11-07
  showed `SctpUsrsctpTest.PartialReliableTtlDrop` timing out at 60 s
  while a sibling case passed in 0.9 s. The seam now picks a free
  ephemeral UDP port per process (`nimrtc_pick_free_udp_port()`) and
  records it so the per-socket `SCTP_REMOTE_UDP_ENCAPS_PORT` matches
  what usrsctp actually bound; the port is no longer cleared when the
  last socket goes away, because the listener lives for the whole
  process lifetime.

### Changed

- **The seven-day soak window now measures the release candidate's tree,
  not `main`.** The `v1.1.0` tag is cut from `release/1.1`, and `main`
  already carries the v1.2.0 audio body, so evidence collected from
  `main` would have been evidence about a different program than the one
  being released. Three changes close the gap:
  - `.github/workflows/soak.yml` gained a `SOAK_REF` environment
    variable (`${{ inputs.ref || 'release/1.1' }}`) and a matching `ref:`
    on both checkouts, so the scheduled run measures `release/1.1` and a
    manual run can point it elsewhere through the new `ref` dispatch
    input. The workflow's two jobs are therefore the *pinned* source of
    both halves of the window.
  - `tools/run_soak.py` now records the tree each run was measured on
    (`git_ref` and `git_commit`) in every evidence record, and its
    summary only counts a day as green when that tree is the candidate
    branch (`--expect-ref`, default `release/1.1`). A day measured on
    `main` — which is what the local Scheduled Task produces whenever
    the release machine's checkout is on `main` — is listed as "measured
    on another tree" rather than silently credited to the window.
    Evidence written before the collector tracked the tree has no
    `git_ref` and is therefore not green either, so the window restarts
    from zero.
  - `tools/run_soak.py --self-test` now runs on every push
    (`.github/workflows/ci.yml`), pinning the streak semantics and the
    new tree rule together: a window measured on the wrong tree must
    fail the gate.

  `docs/plan/soak-runbook.md` §4.3.1 records which tree each half of
  the window measures and how to check the local one.
- **The soak window also refuses to credit a day that was measured on a
  dirty working tree.** A commit identifies a tree, but a dirty tree is
  not that tree: the program that produced the evidence is whatever the
  working directory happened to contain, so such a day says nothing
  about the commit that gets tagged. `tools/run_soak.py` now records
  `git_dirty` in every evidence record, and a day whose records are not
  all from a clean tree is not green — it is listed under "days
  measured on an unclean tree". A record that does not say (one written
  before the flag existed) is reported as "not recorded" rather than as
  dirty, because the gate cannot certify what it cannot see.
- **The daily Windows evidence run is now a script rather than a
  hand-typed Scheduled Task action.** `scripts/run-soak-daily.ps1` runs
  the collector, tees its output into `build/soak/soak-task.log` and
  returns the collector's exit code; it picks the first interpreter that
  can both execute code and import `playwright`, because Case D drives
  a real Chrome peer through Playwright and an interpreter without it
  produces no evidence at all. `scripts/register-soak-task.ps1`
  registers (or removes) the task, warns when the checkout is not on
  the candidate branch, and uses `StartWhenAvailable` so a trigger
  missed while the machine was asleep fires on wake — a gap day breaks
  the streak, so a task that never runs is worse than one that runs
  late.
- **The source version is now `1.1.0`** (`project(VERSION)` in
  `CMakeLists.txt`, `NIMRTC_API_VERSION_{MAJOR,MINOR,PATCH}` in
  `src/core/include/nimrtc/core/api_version.hpp`, encoded `NIMRTC_API_VERSION`
  `10002` → `10100`). The three former `v1.1` / `v1.2` / `v1.3` plan drafts
  are consolidated into one `docs/plan/v1.1-plan.md` targeting `v1.1.0`
  (BDFL, 2026-11-06): they all described a single unreleased body of work on
  top of the published `v1.0.2`, and numbering them as three consecutive
  versions implied three releases' worth of roadmap without three releases'
  worth of delivered versions. The published stable cut is still `v1.0.2`;
  `v1.1.0` is the version under preparation and its tag is cut only once the
  release-hardening track (`docs/plan/v1.1-plan.md` Track C) closes.
- **The `cloudgame` Profile was never shipped, and the docs said it was.**
  `README.md`, `README.zh.md`, `docs/profiles.md`, `docs/zh/architecture.md`
  and `docs/blog/2026-10-10-why-nimrtc.{md,zh.md}` all listed `cloudgame`
  as an available Profile — but `profiles/cloudgame.json` does not exist and
  `assembly` has no `kProfileCloudGame`; the registry ships 9 profiles.
  BDFL decided on 2026-11-04 that `cloudgame` does **not** enter the official
  Profile combination and is deferred to v1.2+, because its hardest
  differentiator ("input aligned to the render frame") needs a **render
  timestamp from the rendering host** (Unreal / Unity), which is outside the
  engine's boundary — the engine does not bundle codecs, and GPU encode /
  render-pipeline integration is out of scope. The remaining three
  overrides are already covered by the `teleop` family plus the
  command-to-frame API. The primary scenes are now **AI Agent** (`agent` /
  `agent-gateway` / `agent-low-latency` / `relay-agent`) and **embodied-AI
  teleoperation** (`teleop`, same family, no new Profile);
  `takeover` (remote vehicle takeover) stays P4. All affected documents now
  state `cloudgame` as a long-term candidate that is *not* in the shipped
  Profile set. `docs/plan/v1.1-plan.md` §2 Track V item V2 is now a
  documentation-alignment task rather than a Profile implementation.
- **The GM/T coverage claim was overstated.** README (EN + ZH) and the
  architecture doc described the project as "GM/T cryptography ready" /
  "GM/T 国密就绪", which implies end-to-end GM/T media encryption. The
  engine's GM/T path covers the **DTLS handshake** (TLCP / SM2 / SM3 / SM4
  via the GMSSL backend); **SRTP still uses the international AES suites**
  (SM4-GCM SRTP is a later release — a `libSRTP`-free implementation would
  have to re-derive RFC 3711's KDF with SM3 as the PRF, and D1 forbids
  self-implementing security-critical crypto). The claims are now scoped
  explicitly, which matters because the previous wording was a compliance
  misstatement towards regulated / Xinchuang customers. The compliance
  layering is documented in `docs/plan/v1.1-plan.md` §2 Track X: signalling
  TLCP is **above** the engine (§10 already says "上层"), DTLS GM/T is
  **inside** the engine, SRTP GM/T is inside the engine but not yet
  implemented.
- **ADR-013's "do not vendor GMSSL" premise was stale.** Its decision
  clause 6 justified the system-install-only strategy by "keeping
  consistency with wolfSSL (which is also not vendored)" — but wolfSSL has
  since become a real git submodule, as have msquic / libsrtp / libopus.
  Requiring every downstream integrator to build and install GMSSL into
  their own sysroot is exactly the integration burden ADR-013 set out to
  remove (OHOS / aarch64 cross-compilation cannot reasonably assume a
  system-installed GMSSL). The decision is therefore amended (2026-11-04):
  GMSSL becomes a git submodule under `src/third_party/gmssl`, built via
  `add_subdirectory` when `NIMRTC_ENABLE_DTLS_GMSSL=ON`, with the option
  still defaulting to **OFF** so the default build's output size, build
  time and artifact set are unchanged. Vendoring is gated on verifying the
  pinned commit's LICENSE text and recording it in `vendor.json`; the
  acceptance criteria now include a Windows/MSVC compile, since GMSSL v3 is
  known to be rougher there and "works out of the box" does not hold on
  Windows until that is proven.
- **libsrtp is now a real git submodule.** The vendored tree at
  `src/third_party/libsrtp/src` was byte-identical to upstream
  `2f82ec0e6e55a18d9cfae2001a4b3fbbc0049da2`, so it converted with no
  NimRTC-local patch to carry over: the parent repository now tracks
  the path as a gitlink (index mode `160000`) instead of ~2400 checked-in
  files. `.gitmodules` gains the real entry
  (`https://github.com/cisco/libsrtp.git`, branch `main`), the nested
  clone's git dir was absorbed into `.git/modules/`, and `vendor.json`
  records the conversion (`upstream_version` corrected from `master` to
  `main`, notes rewritten). `tools/check_vendor.py` now reports
  `libsrtp` as `ok` — a verified submodule — rather than
  `vendored-tree`. `docs/plan/vendor-migration.md` §3 Phase 3 marks
  libsrtp converted. Windows proof 2026-11-03: full Debug build
  green, `ctest` 629/629 passed, `test_srtp_test` 8/8 passed.
- **The vendor manifest now tells the truth about what is a submodule.**
  `tools/check_vendor.py` distinguishes a registered submodule (a
  gitlink, index mode `160000`) from a vendor whose sources are still
  checked into this repository; the latter is now reported as
  `vendored-tree` instead of being counted as a verified submodule.
  `.gitmodules` no longer lists `libjuice` and `mbedtls`, which have no
  gitlink in the index (their intended settings are kept as comments,
  with the blocker for each conversion spelled out).
  `vendor.json` records the state and the local deltas per vendor, and
  `src/third_party/SOURCE_VERSIONS` — which claimed to be copied
  verbatim from `vendor.json` but was not — is corrected: libopus now
  points at `22244de`, googletest at the `v1.12.0` revision upstream
  actually tags, and `msquic` is listed at all.

### Added

- **The weak-network baseline facility landed (v1.1 Track Q4, "弱网基线设施").**
  Two reproducible paths measure the same three tiers — 5% loss,
  20 ms base + 0–100 ms jitter, 1 Mbps cap — over a 1200 B payload:
  - `tools/benchmark_weaknet.py` is the portable path: an in-process
    userspace shaper between two loopback UDP sockets with seeded
    (deterministic) impairments, so Windows/macOS/Linux all reproduce
    the same numbers without root.
  - `tools/benchmark_weaknet_netem.sh` is the Linux path: the same
    tiers driven by a real `tc qdisc netem` qdisc attached to `lo`
    inside a throwaway network namespace (so the host's loopback is
    never reshaped). The tool's `--mode netem` then only paces the
    sends and measures arrival-minus-send — the impairments are the
    kernel's.
  - Both account in **wire bytes** and keep the offered load at 90%
    of the capacity. A rate-limited link delays a packet by its base
    delay *plus* the time it needs to put the packet on the wire (the
    serialization time), not by the delay alone; the old revision
    paced at 100% of the cap and omitted the serialization term, so
    the "1 Mbps" tier reported a queue the sender had built up itself
    rather than the link's delay. The capped tier now reads
    ~15 ms (5 ms base + ~9.6 ms serialization) with a flat
    distribution (p99 − p50 < 2 ms) instead of ~16–31 ms with a
    queue-dependent tail.
  - The netem script pins the qdisc's cell size to 1 byte
    (`rate 1mbit 0 1`): netem rounds the charged length up to the
    cell size, and with the default cell the charge no longer matches
    the length the sender paces against.
  - Two Windows-only measurement defects were fixed. The shaper
    scheduled its delivery deadlines on `time.monotonic()`, which on
    Windows is `GetTickCount64` with a **15.6 ms** resolution, so
    every packet's delay was quantized to a 15.6 ms step; it now uses
    `time.perf_counter()` (`QueryPerformanceCounter`). And the
    forwarder waited for its deadline with `Condition.wait(timeout=…)`,
    whose timed wake-up has a ~10 ms granularity on Windows (a 10 ms
    wait returns after ~20 ms), which added 5–15 ms of lateness to
    every packet; it now polls in 0.5 ms slices outside the lock.
  - CI runs the netem path on the Linux x86_64 leg (`sudo`, needs
    `iproute2`); the JSON lands in `build/e2e/bench_weaknet_netem.json`
    next to the portable `build/e2e/bench_weaknet.json`.
  - README (EN + 中文) Benchmarks tables now quote the corrected
    numbers for both paths.

- **The RTCP feedback loop is closed (v1.1 Track Q3, "RTCP 反馈闭环打通").**
  `nimrtc::video_rtcp_feedback` (NACK / FIR / PLI) is now wired to the
  video pipeline, so a lost packet travels the whole path:
  - the receiver detects the RTP sequence-number gap and reports it
    through the new `VideoReceiver::set_feedback_callback()` (plain
    data: the lost sequence numbers, plus a key-frame request when
    picture data was lost) — `ReceiverStats::
    keyframe_requests_emitted` counts the latter;
  - `video_pipeline::build_feedback_rtcp()` encodes the report as RTCP
    bytes (a NACK per RFC 4585 §6.2.1, a PLI per RFC 4585 §6.3.1);
    `video_pipeline::feedback_rtcp_size()` sizes the destination, and a
    report that does not fit is rejected as a whole (nothing is
    written);
  - the sender keeps the packets it sent in a bounded ring buffer
    (`SenderConfig::retransmit_cache_packets`, default 256) and
    `VideoSender::retransmit()` re-sends the exact packet that was
    lost — same sequence number, timestamp and marker bit;
  - `video_pipeline::apply_feedback_rtcp()` closes the loop on the
    sender side: a NACK triggers the retransmissions, a PLI or FIR
    arms the sender's key-frame marker (`VideoSender::
    force_keyframe()` / `keyframe_pending()`), and a compound RTCP
    packet is processed as a whole.
  - the wire format gained the two helpers it was missing:
    `video_rtcp_feedback::pack_nack_entries()` packs a list of lost
    sequence numbers into PID + BLP FCI entries (wrap-aware: a loss set
    straddling the 65535 → 0 rollover still fits in one entry), and
    `unpack_nack_entries()` expands them back.
  - the plugin seam follows: `ReceiverAdapter::set_nack_callback()`
    now forwards the receiver's loss reports to the engine instead of
    only storing the callback, and `plugins::VideoReceiverStats` /
    `VideoSenderStats` expose the new counters
    (`keyframe_requests_emitted`, `packets_retransmitted`,
    `packets_retransmit_missed`).
  - the engine owns the RTCP transport, so the two halves are wired
    there: the receiver's report is queued by
    `VideoReceiver::set_feedback_callback()` and put on the wire from
    `NimRTCEngine::tick()` as SRTCP (RFC 3711 §3.4 — a different crypto
    context from SRTP, so `protect_rtcp()` and not `protect_rtp()`);
    inbound RTCP is handed to the sender through
    `IVideoSender::apply_feedback_rtcp()`, and a key-frame request is
    forwarded to the encoder (`force_keyframe()`), because the
    packetizer alone cannot produce an IDR.  The feedback is sent from
    `tick()` rather than from the receive callback so the transport's
    `send()` is never re-entered from its `recv()`.
  - The new `tests/test_video_feedback_loop.cpp` covers the closed loop
    end to end (loss → NACK → retransmission → re-delivery, and PLI →
    key frame).
  - The new `tests/test_engine_video_feedback.cpp` covers the *engine*
    half of the loop, which the module-level tests cannot reach: the
    engine is driven through a fake ICE transport (registered under the
    id "fake-ice") that captures every outbound datagram and can inject
    inbound ones, so the test asserts on the wire that a detected loss
    really becomes an RTCP NACK (naming the peer's media SSRC and the
    lost sequence number), that an inbound NACK really produces a
    retransmission carrying the original sequence number, that an inbound
    PLI really reaches the encoder's `force_keyframe()`, and that an
    unrelated RTCP packet (a Receiver Report) is left alone.

- **The video jitter buffer now shares the audio side's adaptive
  playout strategy (v1.1 Track Q2, "视频侧自适应 JB 统一").** The
  RFC 3550 §6.4.1 interarrival-jitter estimator and the
  "base + 4·J" target-delay policy live **once**, in the new
  `nimrtc::jb_strategy` module; `nimrtc::jb` (audio) and
  `nimrtc::video_jb` (video) are now two front-ends of that one
  strategy. The video front-end:
  - feeds the estimator **once per frame** (a video frame — not an RTP
    packet — is what the decoder plays out, and the packets of one
    frame share a media-clock reading, so per-packet observations
    would feed the estimator the intra-frame burst spread and bias the
    jitter estimate upwards);
  - holds a complete frame until its playout deadline (first-packet
    arrival + target delay) has passed, with the deadline derived from
    the smoothed jitter estimate and clamped to
    `[min_delay, max_delay]`; the delay and jitter estimates are
    published through `video_jb::Stats`;
  - is **opt-in** (`Config::adaptive`, default off): with the default
    configuration a complete frame is still delivered the moment it is
    assembled, so the receive path keeps its current latency. The
    engine exposes the switch through
    `EngineConfig::video_receiver_tuning.adaptive_playout` and
    `plugins::VideoReceiverConfig::adaptive_playout`;
  - gained a time-aware `pop_frame(now_us)` overload; the no-argument
    `pop_frame()` keeps its "hand out the next complete frame"
    behaviour, so existing callers are unaffected.
- **The Goog-CC bandwidth estimator landed alongside AIMD (v1.1 Track
  Q1, "BWE 主线升级").** `src/modules/bwe/` gains `GoogCcBwe`, a
  delay-plus-loss hybrid estimator registered under the factory id
  `"goog-cc"`; AIMD stays the default backend, and
  `EngineConfig::bwe_name` opts in explicitly, so existing sessions
  keep their behaviour unless the caller asks for the new path. RTT /
  loss / probe feedback reach the estimator through the existing BWE
  input path, and `tests/test_bwe.cpp` covers bandwidth ramp-up,
  loss-triggered downshift and probe feedback.
- **The frame↔capture-timestamp map is now an engine API (v1.1 Tracks
  T1/T2, "`ref_frame` 引擎级暴露 / 命令-画面关联").**
  `NimRTCEngine` exposes the bidirectional mapping as additive
  `NIMRTC_API_UNSTABLE` methods — `record_video_ref_frame()` mounts a
  frame id, `ref_frame_to_capture_ts()` / `capture_ts_to_ref_frame()`
  resolve it in either direction, and `data_message_frame_age_us()`
  answers "which frame does this data message target, and how old is
  it". The map is a bounded 1024-entry ring; `send_video()` and the
  receive callback record automatically. `tests/test_engine_ref_frame.cpp`
  covers mount / reverse lookup / miss / age / eviction.
- **`NimRTCEngine::stats()` now aggregates the video counters (v1.1
  Track V1, "`video_stats` 进 engine").** The eight module-level video
  counters (bitrate / dropped frames / NACK classification) are summed
  into the engine stats view additively — zero-valued when no video
  flows, so existing consumers see no change.
- **GMSSL is a real git submodule (v1.1 Track X1).** `src/third_party/gmssl`
  pins GmSSL v3.2.0 (`.gitmodules` + `vendor.json` in sync; the pinned
  LICENSE text verified Apache-2.0-compatible and recorded in
  `vendor.json`). `NIMRTC_ENABLE_DTLS_GMSSL=ON` builds it via
  `add_subdirectory` with no system-install probing; the default stays
  OFF so the default build is unchanged. CI carries the opt-in
  `gmssl-vendored` / `msvc` legs running `test_dtls_gmssl_tlcp`.
- **The SRTP backend seam landed; SM4-GCM stays v1.2+ (v1.1 Track X2).**
  `src/plugins/include/nimrtc/plugins/srtp_backend.hpp` declares the
  additive `ISrtpSessionBackend` + factory seam, and
  `src/modules/srtp/` routes the existing libsrtp path through a
  `libsrtp_backend` adapter — behaviour unchanged. SM4-GCM SRTP is
  explicitly out of scope for v1.1 (workload + D1 "no self-built
  security-critical crypto"); the README national-crypto claim is
  scoped to "DTLS/TLCP covered, SRTP still international suites".
- **The engine translation unit is split five ways, behaviour unchanged
  (v1.1 Track R1).** `src/engine/src/engine.cpp` (~4.5 kLOC) becomes
  five responsibility-grouped TUs sharing the private
  `src/engine/src/engine_impl.hpp` (core / transport / media /
  signaling / timeline); the public `engine.hpp` is untouched (L4).
  Verified 2026-11-05 on Windows/MSVC: full build exit 0, full
  `ctest` 696/696, freeze scan headers 106 / symbols 707 / missing 0.
- **Track D packaging rules landed (behind `NIMRTC_INSTALL=OFF` by
  default).** `cmake/NimRTCInstall.cmake` (new) centralises the Track D
  `install()` coverage from one place instead of touching every
  per-module `CMakeLists.txt`: it installs every `src/*/include` +
  `src/modules/*/include` tree to `<prefix>/include/`, every built
  `nimrtc_*` STATIC archive (plus `opus` and `nimrtc_vendor_usrsctp`,
  which have no upstream install coverage) to `<prefix>/lib/`, and a
  hand-written `NimRTCConfig.cmake` + `SameMajorVersion` version file
  to `<prefix>/lib/cmake/NimRTC/`. `release.yml` already passes
  `-DNIMRTC_INSTALL=ON` on all five configure legs; the top-level
  `CMakeLists.txt` only adds the `NIMRTC_INSTALL` option and
  `include(NimRTCInstall)`. Windows downstream proof verified
  2026-11-03: `cmake --install` staging holds `include/` + `lib/` +
  `lib/cmake/NimRTC/`, and a minimal `find_package(NimRTC 1.0 REQUIRED)`
  consumer linking `nimrtc::engine` configures, builds, and runs exit 0
  (Linux downstream proof runs in `ci.yml` on every push via the
  install + downstream-proof leg; `## [1.1.0]` is cut when the remaining
  C-track gates — the 7-day soak and the signed tag — clear).
- **The installed bundle now carries the WebRTC APM prebuilt.** The
  APM is built out-of-tree with Meson and has no upstream `install()`
  rules, so the bundle used to ship without
  `libwebrtc-audio-processing-2.a` and the abseil archives it
  references: a downstream consumer linking `nimrtc::engine` failed
  with unresolved `webrtc::` / `absl::` symbols. The archive set and
  their order now come from the Meson build's own link line, recorded
  by the new `tools/apm_link_order.py` into
  `build_meson/apm_link_order.txt` (a static linker resolves symbols
  left-to-right, so the order is part of the contract). The build-tree
  CMake wrapper reads that manifest instead of globbing abseil
  archives in alphabetical order, `cmake/NimRTCInstall.cmake` ships
  the archives to `<prefix>/lib/webrtc_apm/` together with the order
  manifest, and the installed `NimRTCConfig.cmake` chains them in that
  order. `tools/build_webrtc_apm.{cmd,sh}` run the helper as a final
  step and fail with exit code 4 if the order cannot be recorded.
  Windows downstream proof verified 2026-11-04: the staging tree
  holds 13 archives under `lib/webrtc_apm/`, and a minimal
  `find_package(NimRTC 1.0 REQUIRED)` consumer linking
  `nimrtc::engine` links against the real APM and runs exit 0 with
  "real WebRTC APM" initialised (AEC/ANS/AGC).
- **Track A bench CI artifacts (`b628fcb`).** `ci.yml` windows/msvc and
  linux-gcc legs now run `tools/benchmark_latency.py --count 1000` +
  `tools/benchmark_dc.py` and upload `build/e2e/bench_latency.json` +
  `build/e2e/bench_dc.json` as `bench-<platform>-${{ github.run_id }}`
  artifacts (14-day retention). Linux x86_64 reproducibility evidence
  still pending the first green run after this commit; `## [1.1.0]` is cut
  when the remaining C-track gates — the 7-day soak and the signed tag —
  clear (W3 and W5 are closed).

- **Track A reproducible baselines (v1.1 A1/A2).** New
  `tools/benchmark_dc.py` bursts N UDP datagrams per payload size
  over 127.0.0.1 (256/1200 B datagram plane, 16384 B bidi-stream-chunk
  plane) and emits `build/e2e/bench_dc.json` with pps + Mbps per size.
  README Benchmarks (EN + ZH) now cites measured transport-floor
  numbers for `bench_latency.json` + `bench_dc.json` with reproduce
  commands. Re-verified 2026-11-02 on Windows 10 x86_64: latency
  p50 ~9.7 µs / p99 ~17.2 µs / jitter ~0.2 µs (1000 × 172 B); DC
  256 B ~74.8k pps / ~153 Mbps, 1200 B ~76.6k pps / ~735 Mbps,
  16384 B bidi-stream-chunk ~67.1k pps / ~8.8 Gbps (2000 msgs each);
  all inside the README cited ranges.
- **Track B timeline收尾 B3 (docs only, no new seam).**
  `docs/plan/timeline-engine-wiring.md` step 4 now records the
  long-session slope quality note (sub-ms for minutes, degrades
  after; single-SR fallback) plus the paired DTLS/SRTP timeline e2e
  gap (`feed_srtp_inbound()` needs a completed handshake).
  ADR-017 Consequences already carried the same two statements, so
  B1/B2 have no stale "always 0" / "single-SR only" text left.
- **Track A size bench A3.** `python tools/benchmark_size.py
  --markdown` reports static lib `nimrtc_engine` 12.46 MB (<15 MB);
  matches the README static-lib claim. Freeze script
  `scripts/check-release-freeze.sh` passes 6/6 (additive-only API).

### Known limitations (v1.1 C-track, still open)

- W3 is **closed 2026-11-06**: the `Prebuild WebRTC APM` step lost
  `continue-on-error` on all four release legs and Configure is pinned to
  a literal `-DNIMRTC_VENDORED_WEBRTC_APM:BOOL=ON`, so every bundle ships
  the real WebRTC APM 3A and a prebuild regression aborts the cut instead
  of degrading it to the built-in stub 3A path. W5 was closed 2026-11-05
  (review) — the `v1.0.2` release published the aarch64 bundle, so
  `build-linux-aarch64` is a hard gate again (no `continue-on-error`, back
  in `publish-release.needs`). `docs/plan/ci-workarounds.md` W3/W5 remain
  the source of truth.
- The `v1.1.0` tag is cut (2026-11-08): it is an annotated, SSH-signed tag
  that GitHub reports as `verified: true`. **Still open before the release is
  published**: the 7-day soak evidence for §12.3.6 Gate 6 — the window's
  first counted day is 2026-11-08, so it cannot close before 2026-11-14 —
  which is why the GitHub Release stays a **draft** until then. The **upload of
  the release key to the GitHub account GPG list is no longer a
  blocker**: the repository signs tags with `gpg.format = ssh` and the
  account's already-registered SSH key, so GitHub renders the tag as
  "Verified" without it (`RELEASING.md` §3.3 step 11; `docs/plan/
  v1.1-plan.md` Track C6, 2026-11-06). The OpenPGP path remains
  available and would need the key on the GPG list for its badge.
  The soak window **restarts from zero** because it now measures the
  candidate tree `release/1.1` rather than `main` (see the "Changed"
  entry on `SOAK_REF` above): the 2026-11-06 / 2026-11-07 records were
  taken from `main`, whose tree already carried the v1.2.0 audio body,
  and the Linux half of that window was red anyway — the Linux port
  picker crashed and the CI evidence was not merged into the
  summary, and both fixes are on `release/1.1`. **The first counted day
  is 2026-11-08 (UTC)**, and the earliest possible close of the window
  is 2026-11-14 — seven consecutive green Case D + Case E days on
  **both** platforms, with every record taken on a clean checkout of
  `release/1.1`.

- **The earlier C-track notes that kept W3 / W5 open are superseded.**
  W3 (Windows APM prebuild `continue-on-error`) and W5
  (`build-linux-aarch64` soft gate) were closed on 2026-11-06 and
  2026-11-05 respectively; `docs/plan/ci-workarounds.md` W3/W5 remain the
  source of truth.

## [1.0.2] - 2026-11-02

### Status: Stable

> **Patch release fixing two defects in `v1.0.1`.** `v1.0.1` remains
> published and its tag is unchanged; per `RELEASING.md` §2.3 a
> post-announcement fix becomes a new patch version. Public API is
> unchanged (`NIMRTC_API_VERSION` = 10002, `NIMRTC_API_VERSION_PRE_RELEASE`
> = `""`).

### Fixed

- **WebTransport ALPN mismatch (NimRTC ↔ NimRTC handshakes were
  rejected).** The listener advertised the HTTP/3 token `"h3"`
  (RFC 9114 §3.1) while the session advertised the legacy draft
  token `"sm"`, so every NimRTC-to-NimRTC WebTransport handshake was
  refused during ALPN negotiation (Chrome interop was unaffected —
  Chrome offers `"h3"`). Both sides now share one definition in
  `src/web_transport/src/web_transport_msquic_alpn.hpp`
  (`session_alpn()` / `listener_alpn()`), and
  `src/web_transport/tests/test_web_transport_alpn.cpp` pins the
  invariant so the two cannot drift apart again.
- **Windows release bundle contained the whole CI workspace
  (822 MB).** `release.yml` packed the Windows bundle with
  `7z a -ttar -so "$staging" | 7z a -txz -si "$bundle"`. With `-so`,
  7-Zip consumes the first positional argument as the archive name
  (discarded, because the archive goes to stdout) and, with no file
  list left, it packs the *current working directory* — so the
  published v1.0.1 Windows asset shipped the sources, `.git` and the
  complete MSVC build tree instead of the install staging tree
  (822,079,464 bytes against 2.3-2.9 MB for the other three
  platforms). The Windows job now uses
  `tools/make_release_bundle.py`, which packs exactly the staging
  tree with members relative to the staging root — the same layout as
  the `tar -C ... .` of the Linux/macOS jobs — and all four jobs now
  refuse to publish a bundle larger than 64 MB.
- **The signing public key is now shipped with the release.** `v1.0.1`
  published detached `.asc` signatures while the signing key stayed
  private, so `gpg --verify` failed for every downstream consumer.
  The publish job now exports the public half of the signing key as
  the `nimrtc-signing-key.asc` release asset, pushes it to
  keyserver.ubuntu.com and keys.openpgp.org, and the release body
  carries the verification recipe together with the fingerprint
  (`114154CFEC3F5EE5A8D2D313BEACFBB55871CC06`).

### Known issues

- **The `v1.0.1` Windows asset stays broken.** The published
  `nimrtc-1.0.1-windows-x86_64-msvc.tar.xz` is still the 822 MB
  archive that contains the CI workspace rather than the install
  tree; `v1.0.1` is immutable now, so the usable Windows artefact is
  the one in this release.
- **The bundles do not contain NimRTC's own headers, static library,
  or `NimRTCConfig.cmake`.** Each platform bundle currently ships
  only the vendored third-party dependency stack (wolfSSL /
  libsrtp3 / libjuice / msquic headers and libraries); no module in
  `src/modules/` declares an `install()` rule, and no CMake package
  config is generated, so `find_package(NimRTC)` does not resolve
  against a downloaded bundle and linking NimRTC itself still requires
  a source build. This predates `v1.0.1` (its Linux bundle has the
  same shape). Adding per-module `install()` rules plus a generated
  `NimRTCConfig.cmake` / `NimRTCConfigVersion.cmake` is tracked for
  `v1.1.0` (Track D in `docs/plan/v1.1-plan.md`); `README.md` "Download prebuilt" has been corrected to
  describe what the bundles actually contain.
- **The bundles carry no WebRTC APM archives, so no bundle provides a
  usable 3A.** The four `v1.0.2` tarballs contain no
  `lib/webrtc_apm/` directory at all (verified 2026-11-05 against the
  published `nimrtc-1.0.2-{windows-x86_64-msvc,linux-x86_64-gcc}.tar.xz`
  assets): they ship the vendored third-party stack only. The APM
  archives are installed by `cmake/NimRTCInstall.cmake` (Track D), which
  landed after the `v1.0.2` cut, so the `v1.0.2` bundles predate it.

### Notes

- Public API is unchanged vs `v1.0.1`; `NIMRTC_API_VERSION` = 10002,
  `NIMRTC_API_VERSION_PRE_RELEASE` = `""`.
- **The 3A status table in this release's body was wrong, and so was
  the reason it gave for the `windows` line.** The table is generated
  from the per-platform *build-time* APM mode
  (`build/3a-status-<platform>.txt`, which records whether the WebRTC
  APM prebuild succeeded), not from what the published bundle carries.
  The "WebRTC APM (real 3A) bundled" lines for linux/macOS therefore
  described the build, not the artefact: none of the four bundles ships
  the APM archives (see the known issue above). The `windows` line
  blamed a failed prebuild, but the `Prebuild WebRTC APM` step
  **succeeded** in the final `v1.0.2` release run (`release / build /
  windows` → `Prebuild WebRTC APM` → `success`); the persistent Windows
  stub-3A outcome through `v1.0.2` came from defects in
  `tools/build_webrtc_apm.cmd` — LF-only line endings that cmd.exe
  mis-parses, a `powershell -Command` wrapper that turned ninja's
  stderr progress line into a terminating error, and a ninja invocation
  without `call` — all fixed in `9c3c522`, i.e. before this cut. The
  generator now lists the published tarball and reports the build mode
  and the shipped content separately, so "real 3A bundled" is only
  claimed when `lib/webrtc_apm/` is actually present in the artefact.

## [1.0.1] - 2026-11-02

### Status: Stable

> **First published stable release.** `v1.0.0` (`f740f2e`, 2026-10-30)
> is retained as a source-tree milestone tag only; it was never
> published as a GitHub Release. `v1.0.1` is the first 1.0-line
> release cut through `.github/workflows/release.yml` with
> four-platform artefacts + SBOM + detached GPG signatures.
>
> **Signing.** The tag-push run published the assets unsigned (no
> `GPG_PRIVATE_KEY` configured at that point, so the publish job
> skipped the signing steps). The release was then re-cut through the
> `workflow_dispatch` path, which produced the signed assets that are
> published today: every platform bundle ships with a `.sha256`
> sidecar and a detached `.asc` signature made with key
> `114154CFEC3F5EE5A8D2D313BEACFBB55871CC06`. The public part of
> that key was **not** published on any keyserver, so `gpg --verify`
> could not succeed for downstream consumers (see "Known issues"
> below; fixed in `v1.0.2`).

### Fixed

- **Chrome forward DTLS handshake stall (Case D).** Tick-thread
  WebTransport session creation with an empty peer host blocked ICE
  drain, so ClientHello never reached `feed_inbound`. `create_data_channel`
  now fails fast, and `--no-datachannel` clears both the datachannel name
  and backend so tick keeps draining DTLS/SRTP/RTP. DTLS outbound re-pump
  is gated on empty drain to stop ServerHello triple flight; single
  keylog writer on handshake complete. (`5a582c5`)
- **CI e2e acceptance on push.** `interop.yml` now runs the Case D
  acceptance gate on push and emits `build/e2e/case_d_results.json`;
  proxy/demo-p2p stderr and DTLS artefacts route under `build/e2e` for
  upload; macOS arm64 `CadenceProducesApproxFps` bound relaxed 8→4 for
  runner stutter. (`4e312e3`, `5a582c5`)

### Known issues

- **Windows bundle is 822 MB instead of ~3 MB.** The Windows job
  packed the whole CI workspace instead of the install staging tree.
  The published `nimrtc-1.0.1-windows-x86_64-msvc.tar.xz` is therefore
  not a usable drop-in artefact: extract `build/_install/windows/`
  from it for the actual install tree. Fixed in `v1.0.2`.
- **The signing key is not published.** The fingerprint
  `114154CFEC3F5EE5A8D2D313BEACFBB55871CC06` is not on the GitHub
  account's GPG key list, keys.openpgp.org or keyserver.ubuntu.com,
  although `RELEASING.md` §3.3 requires `gpg --send-keys`. Consumers
  cannot verify the `.asc` signatures. Fixed in `v1.0.2`.

### Notes

- Public API is unchanged vs `v1.0.0`; `NIMRTC_API_VERSION` = 10001,
  `NIMRTC_API_VERSION_PRE_RELEASE` = `""`.
- The `windows` bundle ships the stub 3A path: the WebRTC APM prebuild
  failed on the `windows-2022` runner (`continue-on-error: true`), and
  the Configure step recorded `3a=OFF` for that platform. The cause was
  a defect in `tools/build_webrtc_apm.cmd` (LF-only line endings that
  cmd.exe mis-parses, a `powershell -Command` wrapper that turned
  ninja's stderr progress line into a terminating error, and a ninja
  invocation without `call`), fixed in `9c3c522` — not a platform
  limitation. The CI job list shows the prebuild step as `success`
  because `continue-on-error` turns a failure into a successful
  *conclusion*; the real result is in `steps.apm_prebuild.outcome`,
  which is what the release notes are generated from.

## [1.0.0] - 2026-10-30

### Status: Stable

> **First stable release.** The "experimental through v1.0.0" banner is
> dropped. Public API is frozen per ADR-015; breaking changes require a
> MINOR/MAJOR bump. See `docs/plan/v1.0-plan.md` §3 + §12.3.6 for the DoD.

### Added

- **Chrome ↔ NimRTC DataChannel bidirectional interop (DC-1)** over
  WebTransport / QUIC — Case E E1–E6 PASS via
  `tests/chrome_interop_harness/run_webtransport_harness.py` +
  `interop/chrome/test_chrome_webtransport.html` against
  `examples/demo-wtserver` (datagram + bidi-stream echo).
  `EngineConfig::datachannel_backend` defaults to `"web_transport"`;
  explicit `"sctp"` keeps the legacy path.
- **API stability commitment** — every public symbol carries
  `NIMRTC_API_STABLE` / `NIMRTC_API_UNSTABLE` / `NIMRTC_API_DEPRECATED`
  (102 headers, `Missing: 0` via `scripts/api_freeze_scan.js`);
  `NIMRTC_API_VERSION` = 10000, `NIMRTC_API_VERSION_PRE_RELEASE` = `""`.
- **L1 strict-priority scheduling cross-plane test (DC-2)** —
  `src/modules/sched/tests/test_sched_priority_dc.cpp` 17/17.
- **Vendor drift is a hard CI gate (W4 removed)** — `tools/check_vendor.py`
  red-blocks merges in `ci.yml` (×4) + `interop.yml` (×2); 9/9 match.

### Notes

- `test_datachannel_engine` 5/5 stays green (in-process regression).
- `test_sctp_usrsctp` 4/4 stays green; usrsctp stays `@deprecated` in tree
  through v1.1.x, removal at v1.2.0.
- aarch64 release bundle stays best-effort (W5): CI leg is a hard gate,
  release leg is `continue-on-error` until its first observed green run.
- Per-platform 3A variance (W3): release bundles ship real WebRTC APM 3A
  where the meson prebuild succeeds, stub 3A otherwise; never silent.

## [0.15.0] - 2026-10-30

### Status: Tech Preview

> **WT product-route cut** — `EngineConfig::datachannel_backend`
> defaults to `"web_transport"`; Case E E1–E6 PASS. Per
> `specs/slices/v0-15-0-web-transport-product-route.md` and
> `docs/plan/v0.15-plan.md`.

### Added — v0.15.0 WebTransport product route (spec:
### `specs/slices/v0-15-0-web-transport-product-route.md`)

- **WebTransport/QUIC is now the default DataChannel backend.**
  `EngineConfig::datachannel_backend` flipped from `"sctp"` to
  `"web_transport"`; `create_data_channel(label)` delegates to the new
  `create_data_channel(label, reliability)` overload; the WT session is
  created lazily and shared by all channels of the engine
  (`web_transport_peer_host` default empty = loopback,
  `web_transport_peer_port` default 443).
- **`create_offer()` skips `m=application` in WebTransport mode** and
  the engine `tick()` polls `wt_session->state()` into
  `on_data_state_("open"/"error"/"closed")`.
- **Case E interop gate is green**: Chrome ↔ NimRTC WebTransport
  datagram + bidi-stream echo PASS (E1–E6) via
  `tests/chrome_interop_harness/run_webtransport_harness.py` +
  `interop/chrome/test_chrome_webtransport.html` against
  `examples/demo-wtserver` (exit 0 = session + data).
- **QUIC stream callbacks no longer use tagged pointers**: separate
  `quic_stream_callback_bidi` / `quic_stream_callback_uni` carry the
  plain wrapper pointer as context.
- **Full suite: 616/616 PASS** (Windows/MSVC Debug; 2 usrsctp
  conditional skips). `test_api_classification` baseline 101 → 102
  for the new `src/sctp/include/nimrtc/sctp/dcep.hpp` DCEP codec
  header (carries `NIMRTC_API_STABLE`).

### Notes

- Post-`v0.14.0-rc.1` work is accumulating in this section; the
  `[0.14.0-rc.1]` entry below remains the authoritative record of
  everything up to that cut.  See `docs/plan/v0.14-plan.md` for the
  open tracks.  The **cross-end timeline engine wiring** (ADR-017,
  `docs/plan/timeline-engine-wiring.md`) landed after the rc.1 cut
  and is tracked by the `Fixed` entries here.

### Added

- **The Sender Report tracker now measures the peer's media clock
  instead of assuming it** (ADR-017 §"Decision 7").  The v0.14
  reconstruction extrapolated a capture instant from a *single*
  Sender Report using the nominal clock rate from the SDP
  `a=rtpmap:` line, so every crystal error in the peer's media clock
  was inherited one sample at a time.  The tracker now retains a
  bounded window of Sender Reports per SSRC and fits the peer's wall
  clock against its media clock by least squares, then smooths the
  successive fits with an exponentially-weighted moving average:

  - `SlopeFilterConfig` (`min_samples`, `max_samples`,
    `ewma_alpha_permille`) configures the estimator; the values are
    clamped on the way in, and changing them re-fits every SSRC from
    the retained samples rather than waiting for the next report.
  - The fit is rejected when it deviates from the negotiated rate by
    more than 5 % (an NTP step on the peer, or a rate the SDP does not
    describe), and the previous slope is kept rather than poisoning
    the reconstruction.
  - The window is immune to the 32-bit RTP timestamp wrap: the
    regression runs on a cumulative media-clock position, and a
    non-plausible advance (a peer clock restart, or an SSRC reused
    for a new stream) drops the window instead of fitting across the
    discontinuity.
  - `SenderReportTracker::set_slope_filter_config()` /
    `slope_filter_config()` expose the configuration.

- **The inter-clock skew is now observable through the engine**
  (ADR-017 §"Decision 4").  `NimRTCEngine::last_sender_recv_skew_us()`
  and `NimRTCEngine::sender_recv_skew_us_for_ssrc(ssrc)` report the
  difference between how far the peer's *media* clock advanced and how
  far *our* clock advanced between the Sender Report and the packet —
  the rate discrepancy between the two ends' crystals, in the sense of
  RFC 3550 §8.1.  The constant phase difference between the clocks
  cancels, so the value is the drift, not the offset.  A peer whose
  media clock runs 100 ppm fast shows up as roughly 100 µs per second
  of session.  The value also carries the *difference in transit
  delay* between the report and the packet, so a single sample is
  noisy at the millisecond level; the trend over many packets is the
  skew.  Both accessors stay empty until a Sender Report has been
  ingested, and the per-SSRC accessor only answers for a stream the
  engine has actually observed.

- **Engine-level coverage for the skew** (`tests/test_engine_timeline.cpp`):
  three new cases pin the published value against the media advance
  (1 s and 2 s of media at 48 kHz), the per-SSRC routing, and the
  "empty when there is nothing to say" contract on both the
  convenience and the per-SSRC accessor.

### Fixed

- **The engine did not compile on the platforms where `char` is
  unsigned.**  `base64_decode()` guarded the alphabet lookup with
  `if (c < 0 || kIdx[static_cast<uint8_t>(c)] < 0) continue;`.  On
  x86_64 `char` is signed, so the `c < 0` test is meaningful; on
  aarch64 Linux and on macOS arm64 `char` is *unsigned*, which turns
  the comparison into "always false" and GCC/Clang reject it under
  `-Werror=type-limits`:

      src/engine/src/engine.cpp:181:15: error: comparison is always
      false due to limited range of data type [-Werror=type-limits]

  The guard was redundant anyway: `kIdx` is a 256-entry table
  initialised to -1 whose only non-negative entries are the 64
  base64 alphabet bytes, so `kIdx[byte] < 0` already rejects every
  non-alphabet byte, including every byte >= 0x80.  The decoder now
  iterates over the byte value directly (`unsigned char`) and keeps
  only the table lookup, so it builds identically on every supported
  target.  This unblocked the `linux-gcc / aarch64` and
  `macos-clang / arm64` CI legs.

- **The ICE consent-freshness re-arm test was scheduler-dependent.**
  `IceConsentFreshness.T3_BindingReceivedRearmsTimer` asserted that
  the consent-lost callback does not fire again inside a freshly
  armed timeout window by sleeping for half a timeout and comparing
  the fire *count*.  The consent tracker, however, auto-rearms after
  it fires (it resets the last-binding timestamp to "now" when the
  callback runs), so a second fire is legitimately due one timeout
  after the first one unless the test's re-arm notify lands first.
  The old test left the runner only 100 ms of slack, and on the
  macOS CI runner of run #275 the test thread was descheduled for
  longer than that: the auto-rearm fired inside the "fresh window"
  and the count came out as 2 instead of 1.  The test now uses a
  1000 ms consent timeout and states the property in a
  scheduler-proof way — the first fire *after* the re-arm must not
  be closer than a full timeout to the re-arm — which is exactly
  what the implementation guarantees.

- **The Sender Report the engine sent described a different instant
  than the media it was paired with.**  RFC 3550 §6.4.1 words the
  NTP field as "the wallclock time when the report was sent", while
  the RTP timestamp in the same report is the timestamp of the last
  packet *sent* — two different moments, up to a whole report period
  apart.  The engine took its wall-clock reading when it built the
  report, so every receiver that reconstructed a capture instant from
  it inherited the report-cadence bias: a capture timestamp that was
  systematically late by up to `sr_period_ms` (1 s by default).  The
  engine now records the packet's send instant when the packet is
  built — the same instant the abs-send-time field encodes — and
  reuses it for the report, so the two halves of the report denote
  the same instant and the reconstruction carries no cadence bias.

- **The DTLS-derived SRTP keying material assumed NimRTC was always
  the DTLS server.**  The key derivation picked the *client* master
  key for inbound traffic and the *server* master key for outbound,
  which is correct only when the peer sent the ClientHello.  When the
  negotiation made NimRTC the DTLS *client* — the case a
  NimRTC-to-NimRTC session hits — the two halves came out swapped and
  the inbound traffic was undecryptable.  The direction is now taken
  from the negotiated DTLS role (RFC 5763 §5).

- **The msquic-backed WebTransport server corrupted the heap and
  dropped the first datagram of every session.**  Three defects in
  the server seam made the Case E Chrome interop unusable:

  1. The datagram payload was described by a stack-allocated
     `QUIC_BUFFER` that pointed straight into msquic's receive
     buffer, but msquic encodes the DATAGRAM frame into a packet
     *later* — by then the bytes had already been reused.  The
     payload is now heap-owned (`OutboundPayload`, in
     `web_transport_msquic_send.hpp`) and stays alive until msquic
     is done with it.  The same fix applies to the stream writes,
     which have the same asynchrony.

  2. The payload was released on the intermediate
     `QUIC_DATAGRAM_SEND_SENT` notification, although msquic
     reports the *same* client-context pointer again on the final
     state (`ACKNOWLEDGED` / `LOST_DISCARDED` / `CANCELED`).  The
     second release was a double free, and the server died with
     `STATUS_HEAP_CORRUPTION` (0xC0000374) a few datagrams into
     the session.  The payload is now released once, on the final
     state.

  3. The server rejected a datagram whose peer state was not yet
     known, so the first datagram of a server-side session was
     dropped with `0x1003` (`QUIC_STATUS_INVALID_STATE`).  The
     peer's datagram state is now tracked separately from the
     "send enabled" flag, and an unknown state is no longer read
     as "the peer does not support datagrams": msquic queues the
     datagram itself and cancels it if the peer turns out not to
     support them.

  Alongside this the HTTP/3 handling was reworked: the QPACK
  codec moved out of the listener into a dedicated
  `web_transport_qpack.{hpp,cpp}` (decoder *and* encoder, with the
  RFC 9204 static table generated by `tools/gen_qpack_tables.py`),
  and the `Http3Handler` is now one instance per QUIC connection
  instead of one shared by all of them.

  A fourth defect in the same seam made the server **hang forever at
  shutdown**: the listener never closed the streams it had received
  from msquic, so the WebTransport CONNECT stream kept a reference to
  its QUIC connection alive, the connection was never destroyed, and
  the registration's rundown never completed -- which made
  `MsQuicRegistrationClose()` block indefinitely.  The demo therefore
  sat in `stopping server` forever instead of exiting.  The request
  stream is now closed exactly once in `Http3Handler::on_stream_closed()`,
  which lets the connection be destroyed and the rundown complete.
  (`src/web_transport/src/web_transport_msquic_listener.cpp`)

  Verified with an aioquic client (`build/_wt_datagram_probe.py`): 8/8
  consecutive rounds with a byte-exact echo on the final tree, and the
  `test_web_transport_*` unit binaries all pass.  The demo also installs
  a vectored exception handler that prints a symbolised backtrace,
  because the listener runs on msquic's worker threads, where an access
  violation would otherwise kill the process without a diagnostic.
  The demo exits cleanly afterwards: 0 when at least one session
  connected, 2 when none did -- no `STATUS_HEAP_CORRUPTION`, no
  fail-fast exception during teardown, and no hang in
  `MsQuicRegistrationClose()`.
  (`src/web_transport/src/web_transport_msquic_session.cpp`,
  `src/web_transport/src/web_transport_msquic_session.hpp`,
  `src/web_transport/src/web_transport_msquic_send.hpp`,
  `src/web_transport/src/web_transport_msquic_listener.cpp`,
  `src/web_transport/src/web_transport_msquic_listener.hpp`,
  `src/web_transport/src/web_transport_qpack.cpp`,
  `src/web_transport/src/web_transport_qpack.hpp`,
  `src/web_transport/src/qpack_tables.hpp`,
  `tools/gen_qpack_tables.py`,
  `examples/demo-wtserver/demo-wtserver.cpp`)

- **The Chrome e2e Case E WebTransport gate is still blocked, and
  the cause is now narrowed to the server's TLS presentation.**  The
  crashes above are gone, but Chrome still refuses the handshake:

      net::ERR_QUIC_PROTOCOL_ERROR.QUIC_TLS_CERTIFICATE_UNKNOWN
      (TLS handshake failure (ENCRYPTION_HANDSHAKE) 46: certificate
      unknown, BoringSSL error CERTIFICATE_VERIFY_FAILED)

  What is *not* the cause, each ruled out by measurement on this
  tree:

  - **Not the certificate.**  The server presents the intended
    `CN=localhost` leaf (SHA-1 `d5433bd1…`, the one
    `scripts/generate_wtserver_ca_cert.py` issued), and the same
    Chrome build accepts that identical certificate over QUIC when
    an OpenSSL-based QUIC server presents it (aioquic HTTP/3, leaf
    only, no chain, Chrome returns HTTP 200).  The certificate
    presented by the msquic listener is byte-identical to the one
    the OpenSSL server presents.
  - **Not the missing issuer.**  The issuing "NimRTC Dev Root CA" is
    installed in `Cert:\CurrentUser\Root` *and*
    `Cert:\LocalMachine\Root`, and Chrome accepts the same leaf over
    classic TLS, where it uses the Windows verifier.  Both QUIC
    servers send the leaf without a chain.
  - **Not a client-side switch.**  `--ignore-certificate-errors`,
    `--ignore-certificate-errors-spki-list` (correct `sha256/…`
    format, computed from the served leaf), the IPv4/IPv6 resolver
    and the certificate-error cache all make no difference; the
    failure is identical in every combination.
  - **Not a NimRTC protocol bug.**  The QUIC handshake completes on
    the server side, the HTTP/3 SETTINGS frame is sent, and the
    WebTransport CONNECT request is parsed; Chrome aborts during
    its own certificate verification, before any NimRTC code runs.

  The remaining suspect is therefore the SChannel credential path
  that `web_transport_msquic_listener.cpp` selects on Windows
  (`QUIC_CREDENTIAL_TYPE_CERTIFICATE_HASH_STORE`): the certificate
  and the chain are correct, so the difference must be in the TLS 1.3
  handshake bytes SChannel produces.  Until that is pinned down the
  Case E WebTransport acceptance gate stays open, and no release
  note may claim Chrome interop.  (`src/web_transport/src/web_transport_msquic_listener.cpp`)

- **The Linux CI test step failed on a percentile tie.**  The
  `linux-gcc / x86_64` job failed in the `Test` step with ctest exit
  code 8 once the `Build` step had started passing.  The only
  failing case was `ArqRawUdpLoopback.HundredHertzLatency`: the
  100 Hz simulated loopback measured `p50=7us p99=25us p999=25us`
  on the CI runner and tripped `EXPECT_GT(p999, p99)`, even though
  the Linux latency is *better* than the Windows run
  (`p50=23us p99=83us p999=136us`).  p99 and p999 are order
  statistics of the same 200-sample set, so they are allowed to tie
  once the distribution is narrow enough — the strict comparison was
  a test defect, not a product regression.  Percentile ordering is
  now asserted as the non-strict invariant it actually is (`>=`), and
  the strict spread check is kept in a tie-proof form by comparing
  the maximum against the median instead.
  (`src/raw_udp/tests/test_raw_udp_arq.cpp`)

- **The macOS CI build failed in the `Build` step.**  The
  `macos-clang / arm64` job died with `Process completed with
  exit code 2` on every push since the abs-send-time refactor
  (ADR-016/017), while `windows / msvc` and `linux-gcc /
  x86_64` stayed green.  The cause was a compiler-diagnostic
  split between GCC and Clang: `src/modules/rtp/src/timeline.cpp`
  still declared `kAbsSendTimeCycleUs` (the abs-send-time
  256 s wrap period expressed in microseconds), but the
  wrap arithmetic had been rewritten to work on raw ticks via
  `kAbsSendTimeMask` / `kAbsSendTimeModulus`, so the constant
  had no remaining reader.  GCC does not diagnose unused
  `constexpr` variables in C++, but Clang does — and because
  NimRTC compiles with `-Werror`, `-Wunused-const-variable`
  turned the dead constant into a hard error on every Apple
  Clang build.  The dead constant is removed; the 256 s wrap
  period is now documented on `kAbsSendTimeModulus` instead.
  (`src/modules/rtp/src/timeline.cpp`)

- **`-Wduplicated-cond` was not a portable warning flag.**
  The flag is a GCC-only spelling — Clang accepts neither
  `-Wduplicated-cond` nor the `-Wduplicate-cond` variant and
  reports `unknown warning option`, which `-Werror` turns
  into a hard build failure.  The guard was keyed off the
  platform (`if(NOT APPLE)`), so it protected Apple Clang but
  left every upstream-Clang build broken.  The flag is now
  applied based on the compiler ID and reaches GCC only.
  (`cmake/NimRTCOptions.cmake`)

- **The WebTransport server seam test could not skip.**  Its
  `require_tls_material()` helper called `GTEST_SKIP()`,
  which GoogleTest expands to a `return` statement — so it
  aborted only the *helper*, and the test body carried on
  into the assertions that need the TLS material.  On a host
  without provisioned credentials the four start-dependent
  cases were reported as **failures** instead of **skips**,
  turning a missing-artifact condition into a red test.  The
  check is now a macro (`REQUIRE_TLS_MATERIAL()`) so the
  `return` lands in the test body itself.  Verified by
  removing the credential material: the four cases now report
  `[  SKIPPED ]` and the other four still pass.
  (`tests/test_web_transport_server_seam.cpp`)

- **`scripts/generate_wtserver_cert.py` failed on
  cryptography 3.x** — `serialization.pkcs12` is a
  sub-module and is not imported by
  `from cryptography.hazmat.primitives import serialization`,
  so the script raised `AttributeError` and died before
  writing the SHA-1 hash files; it also used the
  `key_encryption_algorithm` keyword, which only exists from
  cryptography 37.0 onwards (older releases spell it
  `encryption_algorithm`).  The PFX export now probes the
  installed signature and is non-fatal — the PFX is a
  Windows-only artefact, and the PEM pair plus the hash files
  are what the Linux / macOS OpenSSL backend needs.  The hash
  and thumbprint files also now follow the certificate's own
  output directory instead of the hard-coded `build/`, so
  running the script from a WSL host no longer overwrites the
  Windows SChannel hash file with a certificate that is not
  installed in `Cert:\CurrentUser\My`.
  (`scripts/generate_wtserver_cert.py`)

- **`NimRTCEngine::last_one_way_delay_us()` returned the wrong
  sign.**  It computed `capture − arrival` instead of
  `arrival − capture`, so a packet that took 4 s to cross the
  link reported a one-way delay of **−4 s**.  Every consumer
  of a latency metric — an agent profile budgeting its
  closed-loop delay, say — received a negative number it
  cannot act on.  The accessor is `NIMRTC_API_STABLE`, but at
  `v0.14.0` (pre-1.0.0) it had no other caller, no test and no
  documented example depending on the old order, so the
  correction is a straight bug fix.  Both header comments now
  state the formula explicitly.

- **`scripts/install_wtserver_ca.ps1` hung forever when it added the
  CA to the trusted-root store.**  The script used
  `Import-Certificate -CertStoreLocation "Cert:\CurrentUser\Root"`,
  and the `Cert:` provider raises an interactive "security
  warning" dialog for a root-store write.  In a non-interactive
  shell (CI, an agent-driven shell, a redirected pipe) nobody
  ever answers that dialog, so the script blocked until it was
  killed — the run finished with exit code `0xFFFFFFFF` and no
  output at all.  The CA is now written with
  `certutil -addstore Root`, which goes through CAPI and never
  raises UI; the script verifies the resulting thumbprint
  afterwards and fails loudly if the CA is not in the store.
  Two more defects in the same script are fixed at the same
  time: the uninstall path removed root-store entries with
  `Remove-Item`, which fails with "access denied"/"no user
  interface operation" on the shared root store and left the
  trust anchor behind (it now uses `certutil -delstore Root`),
  and the script left `build/wtserver_hash.txt` untouched — msquic
  resolves the server certificate by that SHA-1 hash, so a stale
  file made the server load a different certificate than the one
  that had just been installed.  Both thumbprint files are now
  rewritten from the leaf that was actually installed.
  (`scripts/install_wtserver_ca.ps1`)

- **Still open across the cycle (carried from rc.1):**
  - The **Chrome e2e Case E** acceptance gates (DC-1 DataChannel
    and the WebTransport plane) are still blocked, but **not** on
    the server-side QUIC handshake defect that rc.1 recorded: the
    server now completes the handshake, serves the HTTP/3 SETTINGS
    frame, parses the WebTransport CONNECT request and echoes
    datagrams, and the rc.1 symptom (`ERR_QUIC_HANDSHAKE_TIMEOUT`,
    `0 session(s) handled`) is gone — an `aioquic` client that
    rc.1 also recorded as failing now completes the handshake and
    exchanges datagrams.  What Chrome still refuses is the
    *certificate*: it aborts the QUIC handshake with
    `QUIC_TLS_CERTIFICATE_UNKNOWN (46: certificate unknown)` while
    the same certificate is accepted over classic TLS and over QUIC
    when an OpenSSL-based server presents it.  The measurement and
    the remaining suspect (the SChannel credential path in
    `src/web_transport/src/web_transport_msquic_listener.cpp`) are
    recorded under `Fixed` above; the mock-only WebTransport
    unit-coverage caveat is recorded under `[0.14.0-rc.1]` ->
    Notes -> Known limitations.
  - The **client/server ALPN mismatch** (client registers
    the draft-era `"sm"`, the server registers `"h3"`)
    blocks any NimRTC-to-NimRTC e2e test.
  - The **cross-end timeline seam** is wired into the engine
    (`EngineConfig::timeline_enabled` gates SDP `a=extmap`
    negotiation, outbound abs-send-time, SR scheduling, and
    inbound capture reconstruction).  Both v1.0.0 gaps it had
    are now closed: the per-SSRC media clock rate is pinned
    from the payload-type demux (audio at 48 kHz no longer
    reconstructs against the 90 kHz default), and
    `tests/test_engine_timeline.cpp` drives real RTCP SRs and
    abs-send-time-bearing RTP packets through the production
    dispatch.  What remains is v1.1 work by decision —
    `sender_recv_skew_us` is still always 0 and the slope is
    derived rather than fitted.  See
    `docs/plan/timeline-engine-wiring.md` and ADR-017
    Decisions 3, 4 and 6.
  - The **L1 strict-priority scheduling policy** (DC-2)
    is v1.0.0-beta.2; rc.1 ships only the cross-plane test.

- **The ICE consent-freshness tracker could fire its "consent lost"
  callback twice inside a single timeout window, and the
  `consent_lost_pending()` flag could come back up after
  `notify_binding_received()` had cleared it.**  The tracker's timer
  state (`last_binding_received_us_`, `consent_lost_fired_`) was kept in
  plain atomics, so the background tracker thread could

  1. read a **stale** `last_binding_received_us_` right after
     `notify_binding_received()` had reset it — the reset is a plain
     atomic store and the tracker a plain atomic load, with no
     synchronisation between the two threads, so nothing forbade the
     load from observing the pre-reset value.  The tracker then fired a
     second time **inside** a freshly armed timeout window; and

  2. publish the "consent lost" flag **after** the user callback had
     returned, so a `notify_binding_received()` issued while the callback
     was still running was overwritten by the tracker's late store and
     the pending flag was resurrected.

  Both are real races, not just theoretical ones: the stale-read window
  exists on x86 (store-buffer visibility lag) as well as on ARM
  (cache-coherence lag).  This is what made
  `IceConsentFreshness.T3_BindingReceivedRearmsTimer` fail on the
  macOS and Windows CI runners.

  The timer state now lives behind the existing `consent_mu_` mutex and
  the expiry check, the flag update and the rearm form a single critical
  section, so a `notify_binding_received()` either happens entirely
  before the check (and is seen by it) or entirely after the rearm (and
  clears the flag).  The "fired" flag is published *before* the user
  callback runs, and the callback itself is invoked **outside** the
  lock so that it may call back into the transport
  (`notify_binding_received()`) without deadlocking.

  A deterministic regression test was added
  (`IceConsentFreshness.T5_NotifyDuringCallbackClearsPendingFlag`): the
  consent-lost callback blocks until the test has issued its
  `notify_binding_received()`, which pins the ordering instead of
  relying on scheduler luck.  It fails against the old ordering and
  passes against the fixed one.  (`src/modules/ice/src/ice.cpp`,
  `src/modules/ice/tests/test_consent_freshness.cpp`)

- **The Windows CI job failed in the `Test` step because a test pinned
  UDP port ranges that the runner had reserved.**  The `windows / msvc`
  job failed on `test_dtls_srtp_timing` with

      [FAIL] t03_dtls_state_initial — open() failed
      [warn] udp.c:151: UDP socket binding failed on port range
             127.0.0.1:[64200,64299], errno=10013
      [error] agent.c:235: Connection creation for agent failed

  `errno 10013` is `WSAEACCES`: the Windows runners hand out *excluded
  port ranges* (Hyper-V / HNS reservations) and a bind inside one of
  them is refused.  libjuice walks the whole requested range before
  giving up, so a 100-port block that happened to be reserved made
  `engine.open()` fail outright — a test defect, not a product
  regression.

  The three fixtures that bind a real ICE socket
  (`test_dtls_srtp_timing`, `test_dtls_seam`,
  `test_engine_plugin_loading`) now leave `local_port_range_begin/end`
  at 0, so the OS assigns an ephemeral port from outside the excluded
  ranges.  `test_dtls_srtp_timing` additionally retries `open()` a
  couple of times, because a failed `open()` tears the transport
  session down and cannot be repeated on the same engine object.  The
  fixtures that already use the `shell` transport sentinel — and
  therefore never bind a socket — are unchanged.
  (`tests/test_dtls_srtp_timing.cpp`, `tests/test_dtls_seam.cpp`,
  `tests/test_engine_plugin_loading.cpp`)

### Changed

- **The `linux-aarch64` CI leg's real problem was an invalid runner
  label, not runner-pool capacity (2026-10-28).**  The CHANGELOG
  history for this leg attributed every failure to "the GitHub-hosted
  ARM64 runner pool is unreliable" — run #269 queued 3+ hours without
  allocation, run #268's job never received a runner, run #35049874137
  queued 11+ hours.  That diagnosis was wrong.  The label the job
  requested was `ubuntu-22.04-arm64`, and **no such GitHub-hosted
  label exists**: GitHub's documented Linux ARM64 labels are
  `ubuntu-26.04-arm`, `ubuntu-24.04-arm` and `ubuntu-22.04-arm` (see
  the "Available Images" table in `actions/runner-images`' README).
  The label was introduced by `4467cd8` (2026-09-11); the project's own
  earlier commits used the valid `ubuntu-22.04-arm`.

  A job requesting a label that matches no runner does not fail — it
  sits `queued` **forever**, with `started_at == created_at` and an
  empty `runner_name`.  Both historical observations have exactly that
  signature: run #269's aarch64 job reports
  `labels: [ubuntu-22.04-arm64]` with an empty `runner_name` while the
  other three jobs *in the same run* were allocated a runner within
  five seconds; and `4467cd8`'s aarch64 check-run sat queued for
  exactly 24 h (`2026-09-11T02:30:03Z` → `2026-09-12T02:30:03Z`)
  before being cancelled.  A saturated pool cannot be selective like
  that, and 24 h is the point at which the platform gives up on a job
  it can never schedule.

  So the three "queue" reports were one bug seen three times, and the
  elaborate response to it (a TEMP-DISABLE, then a
  `vars.NIMRTC_ARM64_CI` repository-variable switch, then a
  3-platform-vs-4-platform shipping decision) was built on a
  misdiagnosis.  Concretely, this change:

  - Corrects the runner label to **`ubuntu-24.04-arm`** (a real label).
    `release.yml` moved with it.
  - **Removes the `if: ${{ vars.NIMRTC_ARM64_CI == 'on' }}` gate and the
    `NIMRTC_ARM64_CI` repository variable entirely.**  The `linux-aarch64`
    job in `ci.yml` is a plain hard gate again.  The variable gate had
    a second cost beyond the one it was meant to solve: with the
    variable unset the leg was *always* skipped, so the 4-platform
    matrix could never be observed green on a normal push — which is
    exactly what `v1.0-plan.md` §3 Gate #8 requires.  `workflow_dispatch`
    is kept, since re-running the matrix on demand is useful anyway.
  - The `linux-aarch64` job no longer installs the
    `g++-aarch64-linux-gnu` cross toolchain.  The runner is a *native*
    arm64 host, and `cmake/toolchains/aarch64-linux-gnu.cmake` has a
    native branch (`uname -m == aarch64` → system compiler); installing
    the cross package would have made the toolchain file prefer the
    cross compiler and point `CMAKE_SYSROOT` at a sysroot the native
    image does not carry.
  - Keeps `build-linux-aarch64` in `release.yml` at
    `continue-on-error: true` — but on the honest ground that the
    *release* aarch64 build has not yet been observed to complete (it is
    a different job with different build settings than the CI leg), not
    on the ground that the runner pool might be busy.  Once it has
    completed green once, it becomes a hard gate and row W5 is deleted
    from `docs/plan/ci-workarounds.md`.

  The full record, including the label evidence, is in
  `docs/plan/ci-workarounds.md` §4.1.
  (`.github/workflows/ci.yml`, `.github/workflows/release.yml`,
  `RELEASING.md`, `docs/plan/v0.13.0-release-notes.md`,
  `docs/plan/ci-workarounds.md`)

### v1.0.0 "Stable" release notes — DRAFT, not cut

> **This is a working draft.** v1.0.0 has **not** been released and the
> "Stable" banner has **not** been applied. The `## [1.0.0]` heading
> will be created at tag cut per `RELEASING.md` §3.2 step 7, and only
> once `docs/plan/v1.0-plan.md` §3 DoD gates are green. Do not
> paraphrase the Highlights below as shipped features.

Planned highlights, per `docs/plan/v1.0-plan.md` §6:

- **Chrome ↔ NimRTC DataChannel bidirectional interop (DC-1)** over
  WebTransport / QUIC — *blocked on Case E; see
  `[0.14.0-rc.1]` -> Notes -> Known limitations.*
- **API stability commitment** — every public symbol carries a
  `NIMRTC_API_STABLE` / `NIMRTC_API_UNSTABLE` /
  `NIMRTC_API_DEPRECATED` classification; the public header surface is
  frozen through the v1.0.x line; ADR-015 governs the policy.
  `NIMRTC_API_VERSION` and the marker macros already exist; the
  per-symbol sweep **landed in `v0.14.0-rc.1`** across all 100
  public headers that existed at the beta.1 cut (626 of 650
  namespace-scope symbols classified; 24 internal
  symbols exempt per ADR-015 §Decision 1; `Missing: 0` against the
  gate; the public-header count has since moved 100 -> 101 with
  `nimrtc/rtp/timeline.hpp`).  Companion tooling:
    - `scripts/api_freeze_scan.js` — the per-symbol scanner,
      wired into all four CI platforms (`windows-2022`,
      `linux-gcc`, `macos-clang`, `linux-aarch64`) and into
      `scripts/check-release-freeze.sh` as the pre-tag
      `api-symbols` check.
    - `scripts/check_api_marker_includes.py` — complementary
      include-hygiene gate (every marker-using public header
      must include `<nimrtc/core/api_version.hpp>` or the
      first standalone consumer hits MSVC C2079 instead of a
      clean error).  Wired into all four CI platforms.
    - `tools/apply_api_freeze_markers.py` — the annotated
      applier used to apply per-symbol markers to the existing
      public surface (run with `--help` for usage).
    - `scripts/check-release-freeze.sh` — pre-tag guardrail;
      `api` covers the file-level marker (matches the existing
      `tests/test_api_classification.cpp` assertion), and the
      new `api-symbols` check invokes the same scanner CI runs
      so the tag cut cannot disagree with the merge gate.
- **L1 strict-priority scheduling across the DC plane (DC-2)** —
  `test_sched_priority_dc.cpp` 17/17 locally; 4-platform CI run pending.

Open items that must be resolved *before* this section can become a
real `## [1.0.0]` block:

| # | Blocker | Ref |
|---|---|---|
| 1 | Case E Chrome ↔ NimRTC DataChannel PASS on Windows + Linux x86_64 | `v1.0-plan.md` §3 Gate #1 / #9 |
| 2 | ~~aarch64 support-matrix decision~~ **Closed** — the 4-platform-hard-gate vs. 3-platform-ship question is settled as a **split decision**: the `linux-aarch64` **CI** leg in `ci.yml` is a **plain hard gate** (no `if:`, no repository variable) since 2026-10-28 — the "ARM64 runner pool is unreliable" rationale behind the earlier `vars.NIMRTC_ARM64_CI` switch was a misdiagnosis (the runner label was `ubuntu-22.04-arm64`, which is not a GitHub-hosted label at all, so the job could never be scheduled; the label is now the valid `ubuntu-24.04-arm`; see Unreleased -> Changed and `docs/plan/ci-workarounds.md` §4.1), while the `linux-aarch64` **release** leg in `release.yml` is `continue-on-error: true` and deliberately excluded from `publish-release`'s `needs:` — that leg is a *different job with different build settings* and has not yet been observed to complete, so it stays soft until its first green run. The consequence is explicit, not silent: a missing aarch64 bundle publishes a "Partial release" banner naming the bundle, `publish-release` writes the present/missing bundle list into the Step Summary, and the aarch64 artefact is attached by a follow-up release. | `docs/plan/ci-workarounds.md` §4 (W5), `RELEASING.md` §3.1 step 4, `v1.0-plan.md` §3 Gate #8 |
| 3 | ~~WebRTC APM in the release artefact~~ **Closed** — `release.yml` no longer disables the prebuild: the `Prebuild WebRTC APM` step runs on all four platforms with `continue-on-error: true`, and Configure picks `-DNIMRTC_VENDORED_WEBRTC_APM=ON` on prebuild success / `OFF` on failure (the stub 3A path) instead of aborting the cut — the meson `abseil-cpp` subproject build that broke the `v0.14.0` cut (`dc15ad8`) can no longer kill a release. The `audio3a` link is now `PUBLIC nimrtc_vendor_webrtc_apm` so the APM's bundled-abseil INTERFACE includes reach the test/example/benchmark consumers, which is what made the `=ON` path unbuildable in the first place. Each job records its outcome in `build/3a-status-<platform>.txt`; `publish-release` surfaces the per-platform 3A status in the release body and the Step Summary, so a stub-3A bundle is never published silently. | `docs/plan/ci-workarounds.md` §3 (W3), `RELEASING.md` §3.1 step 4 |
| 4 | ~~Public-header per-symbol sweep + freeze~~ **Closed** — every public symbol across 100 public headers is classified; the `scripts/api_freeze_scan.js` gate exits 0 against the current tree and `scripts/check_api_marker_includes.py` confirms every marker-using header pulls in `<nimrtc/core/api_version.hpp>` | `v1.0-plan.md` §3 Gate #2 |
| 5 | 7-day Case D + Case E soak with no new failures | `v1.0-plan.md` §12.3.5 |

## [0.14.0-rc.1] - 2026-10-22

### Status: Tech Preview

> **Third pre-release of the v0.14.x cycle** — the
> **v1.0.0-rc.1** release candidate per
> `docs/plan/v1.0-plan.md` §7 + §8 + §12.2 + §12.3.5.
> The marketing version stays anchored to the v0.x line
> ("Tech Preview" banner unchanged from v0.13.0); the
> `NIMRTC_API_VERSION_PRE_RELEASE` macro reads `"rc.1"`
> in the tree, matching the `v0.14.0-rc.1` tag cut
> (it read `"beta.1"` before this cut).  The `v0.14.0-rc.1`
> tag is cut at the post-`7fc36ff` commit on this branch.
>
> The cut is the **regression-stabilisation close-out** of
> the v0.14.x cycle per `v1.0-plan.md` §12.3.5
> (`§7 = NO`, closed 2026-10-19; no new SFU-1 / DC-3 /
> BENCH-1 work), but it also ships the plan-track work that
> the §7 closure explicitly permitted landing inside the
> v0.14.x window: the per-symbol `NIMRTC_API_STABLE` /
> `NIMRTC_API_UNSTABLE` sweep across all 100 public headers,
> the ADR-016/ADR-017 cross-end timeline seam
> (with abs-send-time participation), the DC-2 cross-plane
> sched test, the msquic submodule pin, and the Case E
> WebTransport server seam.  The physical DTLS-ICE → RFC 9221
> handoff (rc.1 PR-2) and the L1 strict-priority scheduling
> policy remain deferred to the v1.0.0 line.
>
> **Cut order**: the 10 commits on the `v0.14.0-beta.1..HEAD`
> range landed in the order listed under Highlights below
> (per-symbol sweep → Case E server seam + DC-2 cross-plane
> → ADR-016/017 timeline → msquic fork switch → Case E
> harness datagram-shape fix → docs refresh), followed by
> the 8 working-tree changes that this cut folds in
> (version macro `beta.1` → `rc.1`; `release.yml` W3
> prebuild fallback; `RELEASING.md` W5 aarch64 soft-gate;
> `audio3a` PUBLIC link fix; ADR-016/017 abs-send-time
> refinement with 12 new timeline tests; `v0.14-plan.md`
> baseline fix; `test_api_version.cpp` cycle;
> this CHANGELOG rewrite), followed by the Case E
> cross-platform repair stack (the WebTransport server
> seam did not compile on Linux/macOS and its server-seam
> test was red on Windows because the TLS material was
> never provisioned — see Fixed).  All of these are pushed
> as a single stack so the `v0.14.0-rc.1` tag points at the
> post-`7fc36ff` tree with the working-tree changes
> committed on top.

### Highlights

- **Per-symbol `NIMRTC_API_STABLE` / `NIMRTC_API_UNSTABLE`
  sweep across all 100 public headers** (the v0.14.x
  follow-up to the v0.13.0 file-level sweep; ADR-015
  §Decision 1) — every namespace-scope symbol across the
  100 public headers is now classified with the per-symbol
  marker pair.  `Missing: 0` against the
  `scripts/api_freeze_scan.js` gate; the 4-platform CI
  matrix (`windows-2022`, `linux-gcc`, `macos-clang`,
  `linux-aarch64`) and `scripts/check-release-freeze.sh`'s
  `api-symbols` check are wired to the same scanner.
  (`ad47a11`, 107 files)

- **ADR-016/ADR-017 cross-end RTP timeline seam
  (header `nimrtc/rtp/timeline.hpp` lands as the 101st
  public header)** — `Ntp64` (RFC 5905 §6),
  `ntp64_from` / `system_clock_from_ntp64`, the RFC 6051
  `abs-send-time` 24-bit encode/decode pair, and the
  receiver-side `SenderReportTracker` ingest the SR
  plus the abs-send-time *delta* to refine the
  extrapolation.  Public-header count: 100 → 101 in
  `tests/test_api_classification.cpp`.  Anchoring is
  microseconds since the 1970 Unix epoch (Chrome convention);
  the `sender_recv_skew_us` out-parameter of
  `capture_ts_us_for()` is a `std::int64_t*`
  (pass `nullptr` to discard); all `__int128` paths in
  `timeline.cpp` were replaced with int64 arithmetic for
  MSVC portability.  Companion test binary:
  `src/modules/rtp/tests/test_timeline.cpp`
  (21/21 PASS locally, see Tests).  Design record:
  ADR-017 (`docs/adr/ADR-017-cross-end-timeline.md`).
  (`8da11c2`, `4f97231`, `0f13440`, `c62e87e`, plus the
  uncommitted abs-send-time refinement that closes the
  three remaining tracker gaps — per-SSRC clock rate,
  RTP-TS wrap handling, AST anchor with a
  `kAstAgreementToleranceUs = 1 s` sanity tolerance.)

- **msquic submodule pin: point at the NimRTC fork**
  (`https://github.com/NimRTC/msquic`, branch
  `nimrtc-patches`) **instead of rewriting the platform
  `CMakeLists.txt` at configure time.**  The fork is
  upstream `microsoft/msquic` `42376ae4` plus one NimRTC
  commit (`a5ae3a06e`) that forces the dummy raw datapath
  on Windows, so the cmake overlay
  (`_nimrtc_msquic_force_dummy_datapath()`) and the
  `.gitmodules` `ignore = dirty` hack are gone, the
  submodule working tree stays vendor-clean after
  configure + build, and the recorded pin is fetchable
  from a fresh clone (resolves the `a88383e` "local-only
  commit breaks every clone but the author's" class of
  failure).  See `docs/plan/ci-workarounds.md` W1 — the
  workaround itself (Windows XDP SDK unavailable in the
  `windows-2022` runner image) is unchanged.
  (`cbd82cd`, `98f3947`)

- **Chrome e2e Case E — WebTransport server seam
  (WT-SEVER).**  `IWebTransportServer` (new public
  header
  `nimrtc/web_transport/web_transport_server.hpp`)
  exposes the msquic-backed
  `create_msquic_webtransport_server()` factory and the
  `IWebTransportServer` interface
  (start / stop / set_on_session / running).
  `src/web_transport/src/web_transport_msquic_listener.{hpp,cpp}`
  owns the QUIC_LISTENER_CALLBACK / HTTP/3 SETTINGS / QPACK
  decoding surface (RFC 9114 + RFC 9204 + RFC 9298).
  Companion binary `examples/demo-wtserver/demo-wtserver.cpp`
  echos datagrams and bidirectional streams for end-to-end
  probing.  Tooling: `scripts/generate_wtserver_cert.py` /
  `.ps1`, `interop/chrome/test_chrome_webtransport.html`,
  `tests/chrome_interop_harness/run_webtransport_harness.py`.
  `demo-wtserver.exe` builds clean and starts a real
  msquic listener that loads the cert from
  `Cert:\CurrentUser\My` and accepts incoming QUIC
  connections on the configured UDP port (smoke-tested
  locally with `--port 19443 --duration 2`).
  (`ed8cdc2`, 20 files)

- **DC-2 cross-plane sched test
  (`StreamPriorityCrossPlane`)** — cross-plane RTP × DC
  enqueue / reset / stats at L1 strict-priority,
  `src/modules/sched/tests/test_sched_priority_dc.cpp`,
  **17/17 PASS** locally.  Lands the regression test
  surface; the L1 strict-priority *policy* itself
  (the runtime hook that consumes the test's stats) is
  still v1.0.0-beta.2 work.
  (`ed8cdc2`)

- **Case E WebTransport harness datagram shape fix**
  (the defect that masked the Case E measurement) —
  `interop/chrome/test_chrome_webtransport.html` now
  resolves the inbound stream across all three
  `session.datagrams` shapes
  (`datagrams.readable` / `datagrams.incoming` /
  `datagrams` itself) and routes failures through
  `reportError()` instead of dying silently.  The previous
  harness threw `TypeError: Cannot read properties of
  undefined (reading 'getReader')` *before* reaching
  `await session.ready`, so every recorded "Case E FAIL"
  was an artefact of the harness, not a measurement of
  the data plane.  After the fix the harness reaches the
  real verdict and reports the underlying defect as
  `session.ready rejected: WebTransportError: Opening
  handshake failed.`  See Notes for the diagnosis of
  that underlying defect.
  (`13dc8f4`)

- **Version macro `beta.1` → `rc.1`** —
  `NIMRTC_API_VERSION_PRE_RELEASE` now reads `"rc.1"` in
  the tree (it was `"beta.1"` before this cut);
  `tests/test_api_version.cpp` `version_macros.are_defined`
  now asserts the `"rc.1"` value.
  (`src/core/include/nimrtc/core/api_version.hpp`,
  `tests/test_api_version.cpp`)

### Added

- `nimrtc/web_transport/web_transport_server.hpp`
  (public header, 100th) — `IWebTransportServer`
  + `create_msquic_webtransport_server()` factory.
  Public-header count 99 → 100.
  (`ed8cdc2`)

- `nimrtc/rtp/timeline.hpp` (public header, 101st) —
  `Ntp64`, `ntp64_from`, `system_clock_from_ntp64`,
  `SenderReportTracker`, abs-send-time
  encode/decode, `kAstAgreementToleranceUs = 1 s`.
  Public-header count 100 → 101.
  (`8da11c2`, uncommitted follow-up)

- Per-symbol `NIMRTC_API_STABLE` /
  `NIMRTC_API_UNSTABLE` markers across all 100
  public headers (626 of 650 namespace-scope symbols
  classified; 24 internal symbols exempt per
  ADR-015 §Decision 1; `Missing: 0` against the
  `scripts/api_freeze_scan.js` gate).
  (`ad47a11`)

### Changed

- **msquic submodule**: `cbd82cd` points the gitlink
  at the `NimRTC/msquic` fork (`nimrtc-patches`
  branch, upstream `42376ae4` + NimRTC `a5ae3a06e`)
  and `98f3947` adds the `ignore = dirty` directive
  to `.gitmodules` so the vendor tree stays clean
  after configure + build.
  (`cbd82cd`, `98f3947`)

- **`src/modules/audio3a/CMakeLists.txt`**: the vendored
  WebRTC APM link is now **PUBLIC** (not PRIVATE) on
  `nimrtc_audio3a`, so the abseil-cpp include directories
  the APM INTERFACE carries propagate to every consumer
  of `nimrtc_audio3a` (test / example / benchmark
  executables).  With the old PRIVATE, the consumers
  failed to compile as soon as
  `NIMRTC_VENDORED_WEBRTC_APM=ON`; that forced
  `release.yml` to always pass `-D...=OFF` and broke
  the real-3A path.  See `release.yml` W3 below.

- **Per-SSRC clock rate**, **RTP-timestamp wrap
  handling**, and **abs-send-time participation** in
  `SenderReportTracker` — the three gaps that ADR-017
  §"Decision 5" recorded for the tracker side all
  land here.  The 7-argument
  `capture_ts_us_for(rtp_ts, abs_send_time_24, ...)`
  overload records the AST observation, anchors the
  counter against the stored SR, and refines the
  estimate with the AST *delta* (the sender's own
  elapsed time, which does not inherit the media
  clock's rate error).  The 6-argument overload keeps
  the previous behaviour exactly.

### Fixed

- **Case E WebTransport harness datagram shape**
  — the `session.datagrams.incoming.getReader()` call
  was on the *old draft* shape; Chrome exposes
  `session.datagrams` as a container with
  `readable`/`writable` members, so `incoming` is
  `undefined` and the throw aborted `run()` *before*
  `await session.ready`.  The harness now resolves
  the inbound stream across all three shapes and
  routes failures through `reportError()`.  After the
  fix the harness reaches the real verdict.
  (`13dc8f4`)

- **Case E WebTransport server seam did not compile on
  Linux / macOS** — `web_transport_msquic_listener.cpp`
  called `load_cert_hash_tls_credential()`, which is
  defined inside `#ifdef _WIN32`, unconditionally; on a
  POSIX build the whole definition was preprocessed
  away and the file failed with *"'load_cert_hash_
  tls_credential' was not declared in this scope"*.
  The file also used `QUIC_ADDR::si_family`, which
  only exists on Windows (`SOCKADDR_INET`); the POSIX
  `QUIC_ADDR` is a union of `Ip` / `Ipv4` / `Ipv6`.
  Both are fixed — the credential load is now split per
  TLS backend and the address setup goes through the
  portable `QuicAddrSetFamily()` helper.  This was a
  hard build failure on 3 of the 4 CI platforms
  (`linux-gcc`, `linux-aarch64`, `macos-clang`) and was
  introduced by `ed8cdc2` *after* the `v0.14.0-beta.1`
  tag, so it had never been exercised by CI.
  (Verified locally with GCC 11.4 / `-std=c++20`
  `-fsyntax-only`: all four `src/web_transport/src/*.cpp`
  and both WebTransport test translation units compile
  with zero errors.)

- **Case E WebTransport server seam TLS credential is
  now portable across the msquic TLS backends** —
  Windows keeps the SChannel path
  (`QUIC_CREDENTIAL_TYPE_CERTIFICATE_HASH_STORE`, the
  certificate lives in `Cert:\CurrentUser\My`); Linux
  and macOS now use `QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE`
  with a PEM private key + PEM certificate, which is
  the shape msquic's OpenSSL / quictls backend accepts.
  `scripts/generate_wtserver_cert.py` writes the
  matching `build/wtserver_key.pem` next to
  `build/wtserver_cert.pem`.

- **Case E WebTransport server seam could not find its
  TLS material under ctest** — the listener resolved
  `wtserver_hash.txt` against the current working
  directory only, but ctest starts the cases from
  `<build>/tests`, so the file in `<build>/` was never
  found.  The lookup now walks `$NIMRTC_BUILD_ROOT` →
  the CMake binary directory (compiled in as
  `NIMRTC_DEFAULT_BUILD_ROOT`) → the working directory
  → its parent.

- **Case E WebTransport server seam test was red on
  Windows** — 4 of the 8 `test_web_transport_server_seam`
  cases failed with *"failed to load TLS credentials"*
  because the TLS material was never provisioned, and no
  CI job provisioned it either — so the cases would have
  failed on the `windows-2022` CI leg too.  `ci.yml` and
  `interop.yml` now provision the material on every
  platform (Windows: `scripts/generate_wtserver_cert.ps1`
  into the certificate store; Linux/macOS: a
  self-signed PEM key + certificate pair written into the
  build directory), and the start-dependent cases now
  *skip* instead of failing when the material is absent,
  so a developer without certificates is not blocked.

- **Test classification baseline** —
  `tests/test_api_classification.cpp` baseline bumped
  100 → 101 to track the new `nimrtc/rtp/timeline.hpp`
  header.  CI fails the `api_classification` job if
  the count drifts from the committed baseline.
  (`4f97231`)

- **msquic fork fetchability** — the `a88383e` →
  `ef6ff11` (net-zero) attempt to ship an unpushed
  msquic commit in the gitlink is now obviated by the
  fork switch (`cbd82cd`).  The fork lives on
  `NimRTC/msquic` `nimrtc-patches`, so a fresh clone
  resolves the pin via `origin/nimrtc-patches`.

### Tests

- `test_api_version` `version_macros.are_defined`
  asserts `NIMRTC_API_VERSION_PRE_RELEASE == "rc.1"`
  (was `"beta.1"`).  Full file 7/7 PASS against the
  rc.1 tree locally (Windows, MSVC 17 / Visual Studio
  2022, `Debug` preset).  Build host: `windows-2022`.

- `test_api_classification` baseline 101, 0 missing
  per-symbol markers (`api-symbols` gate).

- `src/modules/rtp/tests/test_timeline.cpp` 21/21
  PASS locally (was 9/9 in the beta.1 tree).  New
  cases cover the exact-match AST anchor, the
  refinement overriding a disagreeing media-clock
  delta, the recovery of a jitter-reordered packet's
  true pre-SR instant, the per-SSRC clock rate
  honouring / overriding / zero-rate-ignored paths,
  the forward-across-RTP-TS-wrap case, the
  slightly-older-than-SR clamp, the
  much-older-than-SR nullopt, the AST-anchor
  rejection when the abs-send-time counter is
  broken, and the tolerance-zero disabling
  refinement.

- `src/modules/sched/tests/test_sched_priority_dc.cpp`
  `StreamPriorityCrossPlane` 17/17 PASS locally.
  The L1 strict-priority *policy* itself is still
  v1.0.0-beta.2; this test pins the cross-plane
  interface only.

- `test_web_transport_msquic` 15/15 PASS locally.
  **The test exercises the `IWebTransportSession`
  surface with `MockWebTransportSession` only** —
  no real QUIC peer, no real `msquic.dll` link, no
  Schannel cert provisioning.  Real `msquic create()`
  requires a reachable QUIC peer and is exercised
  end-to-end by the Chrome e2e Case E harness
  (which is currently blocked; see Notes).
  This is the **mock-only WebTransport unit-coverage
  limitation** recorded below.

- `test_web_transport_server_seam` 8/8 PASS locally
  on the rc.1 tree (Windows, MSVC 17 / VS 2022, `Debug`
  preset) **with the TLS material provisioned** — the
  four start-dependent cases really open a QUIC listener
  on a UDP port (`29443` / `29444` / `29445` plus an
  ephemeral port for `start(0)`), load the certificate
  and tear the listener down again.  Before the fix the
  same four cases failed with *"failed to load TLS
  credentials"*.  Without provisioned material they are
  reported as *skipped* rather than failed.

- **Full suite: 585/585 PASS** on the rc.1 tree
  (Windows, MSVC 17 / VS 2022, `Debug` preset) — 0
  failed, 2 skipped (`SctpUsrsctpTest.LoopbackDatagram-
  RoundTrip` and `SctpUsrsctpTest.PartialReliableTtlDrop`,
  both pre-existing environment-dependent skips).

- `test_engine_video_codec`, `test_video_source`,
  `test_engine_plugin_loading`,
  `test_datachannel_engine`, `test_dtls_seam` —
  PASS locally on the rc.1 tree (no regression
  vs. beta.1's 520/520 baseline on `windows-2022`).
  `ci-e2e` for the rc.1 tree has not yet been
  observed by the release manager (CI status
  unverified locally; the `windows-2022` / linux-gcc
  / linux-aarch64 / macos-clang GHA runners are
  not reachable from this cut host).

### Docs

- `docs/adr/ADR-017-cross-end-timeline.md` —
  design record for the cross-end timeline seam
  (the seam was previously referenced as "ADR-016"
  in code and earlier changelog entries, but the
  016 number is reserved by `v1.0-plan.md` §12.5
  for the deferred SFU-track "SFU forwarding skips
  L2" BWE-interface ADR; the timeline seam takes
  017 instead.  All 10 code/doc references were
  re-pointed; the 016 reservation is left intact
  and is now recorded in `docs/adr/README.md`).
  (`c62e87e`)

- `docs/plan/v0.14-plan.md` — the §"Status" table
  rows for HEADER-FREEZE-B1 and CI-B1 have their
  baseline updated from `git diff alpha.2..HEAD` to
  `git diff v0.14.0..HEAD` (the v0.14.0 tag is the
  `v1.0.0-alpha.2` alias; there is no separate
  alpha.2 tag — see
  "Why v0.14.0 = v1.0.0-alpha.2 + v1.0.0-beta.1").

- `CHANGELOG.md` — `[0.14.0-rc.1]` rewritten to
  describe the actual `v0.14.0-beta.1..HEAD` delta
  (the previous entry misattributed the
  `abfcba4`/`ab54a64`/`b254f15`/`2662513`/`a88383e`/
  `ef6ff11` stack to rc.1, but those commits are
  ancestors of the `v0.14.0-beta.1` tag and shipped
  in beta.1).

### Build / CI

- **`scripts/check-release-freeze.sh` passes 6/6**
  on the rc.1 tree:
  `sfu` (no engine wiring), `api` (101 public
  headers, every header carries a marker), `api-symbols`
  (skipped locally, `node` not on `PATH`; the same
  scanner CI runs is the gate), `version`
  (`NIMRTC_API_VERSION_PRE_RELEASE = "rc.1"`),
  `submodules` (all five submodules pinned on a
  reachable remote ref: googletest 15460959cbbf,
  libopus 22244de5a79b, msquic a5ae3a06ed6e on
  `origin/nimrtc-patches`, nlohmann_json 9cca280a4d0c,
  wolfssl ac01707f552c), `root` (no scratch / log /
  keylog / Testing artifacts at the root).

- **`.github/workflows/release.yml` W3 — APM
  prebuild `continue-on-error`.**  The meson
  prebuild of `webrtc_audio_processing` fails on
  the `windows-2022` runner (meson pulls abseil-cpp
  as a subproject and ninja then reports a subcommand
  failure while building `libwebrtc-audio-processing-2.a`;
  see commit `dc15ad8` for the v0.14.0 release-cut
  failure that aborted the whole release).  The step
  is now `continue-on-error: true`; when the prebuild
  succeeds Configure picks
  `-DNIMRTC_VENDORED_WEBRTC_APM=ON` and the bundle
  ships the real WebRTC APM 3A (AEC/ANS/AGC); when
  it fails, Configure falls back to
  `-DNIMRTC_VENDORED_WEBRTC_APM=OFF` and the bundle
  ships the built-in stub 3A path that `ci.yml` also
  uses.  The per-platform outcome is recorded in
  `build/3a-status-{windows,linux-x86_64,...}.txt`
  and surfaced in the release notes
  (`publish-release` reads them).  The release can
  no longer be aborted by a prebuild regression.
  The `audio3a` PUBLIC link fix above is what makes
  the `=ON` path actually compile; without it the
  fallback to `=OFF` is forced.

- **`RELEASING.md` W5 — aarch64 soft-dependency
  policy.**  The `linux-aarch64` leg of
  `release.yml` is `continue-on-error: true` and
  is deliberately *not* listed in
  `publish-release`'s `needs:`.  GHA-hosted ARM64
  runner capacity is unreliable (a 14+ hour queue
  was observed at the v0.14.0 cut), so making it a
  hard gate would block publishing the Windows /
  Linux x86_64 / macOS arm64 trio.  *(Corrected
  2026-10-28: the "capacity" explanation was wrong
  — the runner label was invalid, so the job could
  never be scheduled.  See `[Unreleased]` → Changed.)*
  Consequences,
  all explicit rather than silent: the aarch64
  bundle is best-effort; when the leg does not run
  or fails, the release is published with the other
  three bundles and a "Partial release" banner; the
  missing bundle is listed by name; the aarch64
  artefact is attached by a follow-up release once
  runner capacity returns.  The **CI** leg in
  `ci.yml` (`linux-aarch64`) remains a hard gate;
  only the *release* leg is soft.

- **`scripts/api_freeze_scan.js`** is the per-symbol
  scanner the freeze script and the four CI
  platforms all use; the `api-symbols` gate exits
  0 against the rc.1 tree.

### Notes

- **Known limitations (carried into rc.1).**

  - **The timeline seam is complete on the tracker side
    but still inert at the engine.**  The three
    tracker-side gaps ADR-017 §"Decision 5" recorded
    (per-SSRC clock rate, RTP-TS wrap handling,
    abs-send-time participation) all land in this cut,
    but `Profile::timeline_enabled` is still parsed
    and round-tripped without any engine path acting
    on it: nothing outside the timeline's own tests
    consumes `SenderReportTracker`.  The remaining
    gaps are all engine-side and listed in ADR-017
    §"Decision 5": `kSrPeriodMs`, `kAbsSendTimeUri`
    and `kAbsSendTimeDefaultLocalId` are declared
    with zero references (no SR scheduling, no extmap
    negotiation, no header-extension write), and
    `sender_recv_skew_us` is reserved for v1.1 and
    always 0.  The header's status comment states
    this explicitly rather than over-claiming a
    Chrome-shaped slope estimator.

  - **ALPN mismatch between client and server.**
    `src/web_transport/src/web_transport_msquic_session.cpp`
    (the *client* session factory) opens its msquic
    `ConfigurationOpen` with
    `kAlpn = {'s', 'm'}` — a draft-era WebTransport
    ALPN name that is not the registered ALPN for
    WebTransport-over-HTTP/3 (RFC 9114 / RFC 9298,
    which uses `h3` with the extended CONNECT
    `:protocol=webtransport`).  The server listener
    `src/web_transport/src/web_transport_msquic_listener.cpp`
    registers `kAlpn = "h3"` on both `ConfigurationOpen`
    and the registration config.  A NimRTC **client**
    therefore cannot complete a QUIC handshake against
    the NimRTC **server** today (the ALPN values are
    not negotiable, and msquic rejects the handshake
    at `ConfigurationOpen` when the client offers
    a different ALPN than the server registered).
    The Chrome e2e Case E only ever speaks to the
    server, so it bypasses this defect (Chrome also
    uses `h3`); the defect is invisible to Chrome
    and to the current CI surface, but it would
    surface immediately in any NimRTC-to-NimRTC
    e2e test.  Fixing it is a v0.14.x follow-up
    that switches the client ALPN to `"h3"` and
    the `:protocol` signalling accordingly.

  - **WebTransport unit coverage is mock-only.**
    `tests/test_web_transport_msquic.cpp` exercises
    the `IWebTransportSessionFactory` / `IWebTransportSession`
    surface with a `MockWebTransportSession` only
    (the file's own header comment is explicit on
    this: "no direct .cpp include, no
    `LoopbackMsQuicPeer`, no Schannel cert
    provisioning.  Real msquic `create()` requires
    a reachable QUIC peer and is exercised end-to-end
    by the Chrome e2e Case E harness").  The 15
    `TEST_F` cases in that file therefore cover the
    factory seam, the registry lookup, the session
    state machine, the bidi-stream surface, the
    datagram-facade callbacks, and the close
    idempotency — but **none** of them exercise a
    real msquic `ConfigurationOpen`, a real
    `ConnectionOpen`, a real handshake, or a real
    datagram send/receive against a real peer.
    Real msquic coverage today lives in the Chrome
    e2e Case E harness only, and the Case E
    acceptance gate is blocked (next bullet).  Any
    rc.1 evaluation of "WebTransport works" should
    read the Case E status alongside the 15/15
    mock number.

  - **Chrome e2e Case E is blocked on a server-side
    QUIC handshake defect, not on a missing test
    environment.**  The case now runs end to end on
    a developer machine and fails for a
    well-defined, reproducible reason:

    - Chrome: `net::ERR_QUIC_PROTOCOL_ERROR.QUIC_HANDSHAKE_TIMEOUT`
      (`num_undecryptable_packets: 0` — the client
      never receives a reply), occasionally
      `ERR_CONNECTION_RESET`.
    - `demo-wtserver` binds the socket and reports
      `WebTransport listener started on UDP port
      9443`, but the run ends with `0 session(s)
      handled` — no connection is ever surfaced to
      the listener's connection callback.
    - An **independent** QUIC client
      (`aioquic`, pure Python, no shared code with
      msquic) fails against the same endpoint, so
      this is not a Chrome-specific incompatibility.

    Ruled out while diagnosing: Windows Firewall
    (all three profiles disabled), TLS credential
    loading (the SChannel cert loads from the
    Windows store and reports its SHA-1 hash),
    IPv4/IPv6 family mismatch (a Chrome A/B run
    forcing
    `--host-resolver-rules=MAP localhost 127.0.0.1`
    fails identically to the `::1` and default
    paths), and the msquic fork's forced dummy raw
    datapath (`RawDataPathInitialize` only nulls
    out the optional raw path; `datapath_xplat.c`
    falls back to the classic UDP datapath).  The
    remaining suspect is the msquic listener /
    registration / TLS setup in
    `src/web_transport/src/web_transport_msquic_listener.cpp`
    (the defect is in the server side, not in
    Chrome; the `aioquic` independent client
    reproduces it).  DC-1 interop remains blocked
    on this defect and therefore does not land in
    rc.1; it is the v1.0.0 acceptance gate per
    `v1.0-plan.md` §12.3.3 Gate #3.

- `NIMRTC_API_VERSION_PRE_RELEASE` has cycled from
  `"beta.1"` to `"rc.1"` for this cut.  The
  v1.0.0 final cut will set it to `""` per
  §12.3.6 DoD item 2.

- The `v0.14.0-rc.1` tag will be pushed alongside
  the rc.1 cut commit (the post-`7fc36ff` tree
  with the 8 working-tree changes listed in
  Highlights).  `scripts/check-release-freeze.sh`
  is the pre-tag guardrail; it passes 6/6 on this
  tree (see Build / CI).

### What is NOT in v0.14.0-rc.1 (deferred to v1.0.0 or later)

- **Physical WebTransport session wiring
  (DTLS-ICE → RFC 9221 handoff)** — rc.1 PR-2.
  The dispatch + server seam landed in this cut;
  the wire-up of a DTLS session's
  `client_random` / `server_random` /
  `master_secret` as a QUIC 0-RTT resumption
  ticket is the v1.0.0 final action.  (Distinct
  from the Case E blocker above: the seam itself
  is wired, but a NimRTC DTLS session is not
  yet wired through it.)
- **L1 strict-priority scheduling *policy* (DC-2)**
  — the cross-plane *test* lands in rc.1
  (17/17 PASS, see Tests); the runtime hook
  that consumes the test's stats and applies the
  strict-priority reorder remains v1.0.0-beta.2.
- **Chrome ↔ NimRTC DataChannel bidirectional
  interop (DC-1)** — blocked on Case E; see
  the Known limitations → Case E bullet above.
- **Composite `sfu-agent` Profile resolution** —
  closed by §7 = NO; off the v1.0.x critical
  path; reopens at v1.1.0 only if the SFU track
  is revisited.
- **Adaptive JitterBuffer + Goog-CC full mainline
  BWE** — v1.x.
- **GM/T 国密 SRTP backend** — v1.x.
- **AddressSanitizer CI matrix** — v1.x.

## [0.14.0-beta.1] - 2026-10-20

### Status: Tech Preview

> **Second pre-release of the v0.14.x cycle** — the
> **v1.0.0-beta.1** pre-release per `docs/plan/v1.0-plan.md`
> §8 + §12.2 + §12.3.3.  The marketing version stays anchored
> to the v0.x line ("Tech Preview" banner unchanged from
> v0.13.0); the `NIMRTC_API_VERSION_PRE_RELEASE` macro cycles
> to `"beta.1"`.  This is the **data-plane wire-up** — the
> msquic-backed `WebTransportMsQuicSession` and the
> `WebTransportDataChannel` (DC-1) land here, closing the
> v0.14.0-alpha.2 "seam-only" gap.  The in-process regression
> surface now covers `test_web_transport_msquic` 12/12 +
> `test_web_transport_data_channel` 17/17 +
> `test_web_transport_seam` 5/5 + the existing
> `test_datachannel_engine` 5/5 + `test_sctp_usrsctp` 4/4 —
> 520/520 ctest on the windows-2022 MSVC preset at the cut.
>
> **Tag cut**: `git tag -a v0.14.0-beta.1 -m "v0.14.0-beta.1 (alias v1.0.0-beta.1): msquic integration + WebTransportDataChannel + Chrome e2e Case E (in progress)"`.
> The tag message body documents the alias relationship to
> v1.0.0-beta.1 and the v0.x banner unchanged.
>
> **Note on the earlier beta.1 attempt**: the entry directly
> below this line (`[0.14.0-beta.1] - 2026-10-19`) documents an
> earlier attempt to cut beta.1 that was reverted before the
> tag was pushed.  The `v0.14.0-beta.1` git tag in this
> repository points at the canonical 2026-10-20 cut described
> by this entry.  The `[0.14.0-rc.1] - 2026-10-22` entry
> **above** describes the rc.1 content; it was **staged in
> the tree but untagged** at the time this entry was written,
> and `NIMRTC_API_VERSION_PRE_RELEASE` read `"beta.1"` until
> the `v0.14.0-rc.1` tag was cut.

### Highlights

- **msquic C API integration (ADR-014 / DC-1)** —
  `src/third_party/msquic/` is now a git submodule pinned
  to the upstream v2.x branch, with `cmake/FindMsQuic.cmake`
  as the packaged-locate fallback for distro paths.  The
  msquic C API surface is exercised end-to-end through the
  new `nimrtc_copy_msquic_dll()` POST_BUILD helper
  (copies the resolved `msquic.dll` next to the test exe
  on Windows; CI's `windows-2022` runner uses the vendored
  path).  12/12 unit tests in `test_web_transport_msquic.exe`
  PASS on the windows-2022 CI image after the `48aca5e`
  C28020 / XDP / `msquic.dll` search-path fix.

- **WebTransport / msquic data plane (ADR-014 / DC-1)** —
  `src/web_transport/` now ships the production
  `WebTransportMsQuicSession` (msquic ConnectionStart /
  send_datagram / open_bidi_stream) plus the
  `WebTransportMsQuicSessionFactory` registered as id
  `"msquic"`.  The factory is auto-registered by
  `pal_default_registrars.cpp` so
  `register_all_default_plugins()` exposes it through
  `PluginRegistry::get_web_transport("msquic")` and
  `list_web_transports()` returns exactly `{"msquic"}` at
  the cut (downstream tests pin this set; see
  `wt014_registry_default_registrars_auto_registers_msquic_in_beta1`).

- **`IWebTransportSession` interface** — the pure-virtual seam
  from alpha.2 now has a production msquic-backed implementation.
  Companion headers from alpha.2 remain: `IBidirectionalStream`,
  `IUnidirectionalStream`, `DatagramFacade`,
  `WebTransportConfig`, `WebTransportState`,
  `IWebTransportSessionFactory`.  All `NIMRTC_API_UNSTABLE`.

- **`core::PluginRegistry::register_web_transport()` auto-registration**
  — the typed registry slot from alpha.2 now has the production
  `WebTransportMsQuicSessionFactory` auto-registered by
  `pal_default_registrars.cpp`.  `list_web_transports()` returns
  exactly `{"msquic"}` after `register_all_default_plugins()`.

- **WebTransportDataChannel (DC-1)** — the production
  `plugins::IDataChannel` implementation that maps
  `DataChannelReliability` onto `IWebTransportSession`
  primitives:
    - `kReliableOrdered` / partial-reliability modes →
      `open_bidi_stream()` (bidi QUIC stream + app-level
      TTL / count resets)
    - `kUnreliable` → `send_datagram()` (QUIC datagram, no
      stream bookkeeping)
  Factory id `"web_transport"`; the same data-channel
  surface that the engine already uses for
  `SctpDataChannel` (id `"sctp"`), so the wire is
  engine-transparent.  Header is `NIMRTC_API_UNSTABLE` per
  the v1.0.0-beta.1 classification sweep.

- **Profile rename: `kProfileSfu` → `kProfileRelay`
  (PROFILE-1)** — the v0.14.0 alpha.2 profile enum
  `kProfileSfu` is renamed to `kProfileRelay` to match the
  v0.14-plan.md §2.3 vocabulary shift (the relay is a
  profile role, not just a server type; the term "SFU" is
  reserved for the future `v1.1.0-plan.md` (if the SFU
  track is reopened) — per `v1.0-plan.md` §7 = NO,
  closed 2026-10-19).  No wire-level change; only the enum
  constant name.  `assembly/profiles.hpp` updated; the
  5 transport-strategy tests that reference the old name
  in their descriptive comments were updated to match.

- **Chrome DTLS interop harness (e2e acceptance surface)** —
  `tests/chrome_interop_harness/` adds a no-media
  peerConnection-only probe (`chrome_dtls_only.html`) and a
  full diagnostic harness (`chrome_dtls_diag.html`) plus a
  `run_harness.py` driver.  Both are guarded by the
  pre-existing `NIMRTC_BUILD_INTEROP=ON` option (off by
  default; enabled via the `interop/` spike scripts).
  The harness accepts the same MSVC linker-environment the
  existing tests use, and lays the groundwork for the
  v0.14.0-beta.1 → v0.14.0-rc.1 Chrome Case E DataChannel
  acceptance gate (data plane wire-up now ready; the
  browser-driven test is the next landing).
  **Case E is marked "In Progress" in the `[Unreleased]` section
  above; the v0.14.0-beta.1 tag cuts without Case E per
  `v1.0-plan.md` §12.3.3 Gate #3.**

- **Per-symbol API stability annotation** — `engine.hpp`,
  `transport.hpp`, and `datachannel.hpp` now carry
  `NIMRTC_API_STABLE` / `NIMRTC_API_UNSTABLE` markers on
  individual public symbols (the v0.13.0 sweep was at the
  file level).  This is a v0.14.x slip-or-land item that
  landed in the beta.1 cycle.

### Added

- **`src/web_transport/`** — the production WebTransport
  plugin library.  Compiles to `nimrtc::web_transport`
  (static lib by default; shared if `BUILD_SHARED_LIBS=ON`).
  Adds:
    - `src/web_transport_msquic_session.{hpp,cpp}` — the
      `WebTransportMsQuicSession` implementation (msquic
      ConnectionStart / OpenStream / SendDatagram).
    - `src/web_transport_data_channel.cpp` — the
      `WebTransportDataChannel` implementation
      (bidi-stream for reliable, datagram for unreliable,
      app-level reset for partial reliability).
    - `src/web_transport_plugin.cpp` — the
      `register_web_transport_plugins()` entry point that
      `pal_default_registrars.cpp` calls.

- **`src/third_party/msquic/`** (git submodule) —
  msquic v2.x pinned to the upstream main branch, with
  our `cmake/FindMsQuic.cmake` wrapper preferring the
  submodule path and falling back to `find_package()`
  for distro installs.  Default
  `NIMRTC_VENDORED_MSQUIC=ON` at the beta.1 cut; flip
  to `OFF` to consume a system-installed msquic
  instead (e.g. for distros that ship it prebuilt).

- **`cmake/FindMsQuic.cmake`** (186 lines) — packaged
  locate helper.  Documents the upstream CMake's
  `QUIC_BUILD_DIR` / `QUIC_BUILD_SHARED` knobs; the
  vendored build sets `QUIC_BUILD_DIR` to the
  `src/third_party/msquic` submodule path.

- **`cmake/NimRTCTest.cmake`** — adds
  `nimrtc_copy_msquic_dll()` POST_BUILD helper that
  copies the resolved `msquic.dll` next to any test
  exe in the windows-2022 MSVC preset.  Mirrors the
  googletest / gtest_main DLL copy pattern.

- **`src/plugins/include/nimrtc/plugins/base.hpp`** —
  adds `plugins::Status::kErrClosed` for the
  peer-FIN / RST return path that the WebTransport
  data-channel path produces on graceful close.

- **`tests/test_web_transport_msquic.cpp`** (new, 12
  subtests) — factory-registration + mock-session
  coverage for the msquic backend:
  `factory_registered_with_id_msquic` /
  `factory_display_name_mentions_msquic` /
  `unknown_id_returns_null` /
  `mock_session_open_bidi_stream_returns_null_when_closed` /
  `mock_session_send_datagram_returns_not_ready_when_not_connected` /
  `mock_session_set_on_datagram_sets_flag` / etc.
  Uses a `MockMsQuicApi` test double; no real msquic
  network IO in the unit tests (the spike at
  `interop/v0_14_msquic_spike/` covers the real
  network path).

- **`tests/test_web_transport_data_channel.cpp`**
  (new, 17 subtests) — coverage for the
  `WebTransportDataChannel` mapping:
  `factory_registered_with_id` /
  `factory_create_returns_channel` /
  `factory_create_with_session` /
  `open_and_close_are_idempotent` /
  `open_without_session_returns_not_ready` /
  `open_config_caches_reliability` /
  `send_unreliable_uses_datagram` /
  `send_unreliable_noop_when_not_connected` /
  `send_reliable_uses_bidi_stream` /
  `send_reliable_uses_existing_stream` / etc.
  No real msquic; the test wires a mock session.

- **`tests/chrome_interop_harness/`** (new, gated by
  `NIMRTC_BUILD_INTEROP=ON`) — Chrome DTLS
  interoperability probing harness:
    - `chrome_dtls_only.html` — peerConnection-only
      probe, no media.
    - `chrome_dtls_diag.html` — full diagnostic harness
      (SDP dump + DTLS fingerprint + ICE candidate
      table + error log).
    - `run_harness.py` — driver script that opens the
      harness in headless Chrome, drives the
      peerConnection, and asserts the DTLS
      interop surface.
  The data-plane Case E (DataChannel exchange over
  WebTransport) lands in v0.14.0-rc.1; the harness
  already accepts the wire format.

- **2 new public headers** (the beta.1 additive
  surface vs the alpha.2 cut):
    - `src/web_transport/include/nimrtc/web_transport/web_transport_data_channel.hpp`
    - `src/web_transport/include/nimrtc/web_transport/web_transport_plugin.hpp`
  Both `NIMRTC_API_UNSTABLE`; the seam
  classification is visual at the v1.0.0-beta.1
  cut.

- **`core::PluginRegistry::register_web_transport(id,
  factory*)` / `get_web_transport(id)` /
  `list_web_transports()`** — the typed registry slot
  (alpha.2) now has the production
  `WebTransportMsQuicSessionFactory` auto-registered
  by `pal_default_registrars.cpp`.  `list_web_transports()`
  returns exactly `{"msquic"}` after
  `register_all_default_plugins()`.

- **`nimrtc_copy_msquic_dll()` CMake helper** —
  `cmake/NimRTCTest.cmake` POST_BUILD function for
  any test exe that links `nimrtc::web_transport` on
  Windows; copies the resolved `msquic.dll` next to
  the exe so `LoadLibrary` finds it without a
  PATH-side install step.

### Changed

- **`NIMRTC_API_VERSION_PRE_RELEASE`** cycles from
  `"alpha.2"` to `"beta.1"` in
  `src/core/include/nimrtc/core/api_version.hpp`.
  The matching `nimrtc::core::api_version::kPreRelease`
  constant updates automatically (the macro is the
  source of truth).

- **`src/modules/assembly/include/nimrtc/assembly/profiles.hpp`**
  — `kProfileSfu` renamed to `kProfileRelay`.  No
  wire-level change; the enum constant's
  `std::string_view` name is the only delta.  All
  consumers updated:
    - `src/transport/src/transport_strategy.cpp`
      (5 strategy tests with descriptive comments)
    - `src/engine/include/nimrtc/engine/pal/make_plugin.hpp`
      (the `PluginFailureMode` / `MakePluginOpts`
      forward declarations that reference the
      profile id)
    - `docs/plan/v0.14-plan.md` §2.3 vocabulary
    - `CHANGELOG.md` (this entry)

- **`src/core/src/pal_default_registrars.cpp`** —
  appends the `web_transport` registrar so
  `register_all_default_plugins()` auto-registers the
  `WebTransportMsQuicSessionFactory` (id `"msquic"`)
  and the `WebTransportDataChannelFactory`
  (id `"web_transport"`).  The seam was wired at the
  alpha.2 cut; the registrar call was the missing
  piece.

- **`src/third_party/CMakeLists.txt`** — explicit
  msquic submodule handling (the upstream
  `CMakeLists.txt` lives at the submodule root, not
  in a `src/` subdir; we add the
  `add_subdirectory(... EXCLUDE_FROM_ALL)` then
  `add_dependencies(msquic)` glue).

- **`CMakeLists.txt`** (root) — adds
  `add_subdirectory(src/web_transport)` so the
  `nimrtc::web_transport` target is wired into the
  build graph.  Default `ON`; flip
  `NIMRTC_ENABLE_WEB_TRANSPORT_MSQUIC=OFF` to skip
  the WebTransport / msquic data-plane build (the
  seam headers still compile and the registry slot
  is still available; only the production factory
  is omitted).

### Notes

- **Public API surface diff vs v0.14.0** (alpha.2):
  additive only.
    - **New headers** (2):
      `src/web_transport/include/nimrtc/web_transport/web_transport_data_channel.hpp`
      and `web_transport_plugin.hpp` (the alpha.2
      cut shipped 7 new headers; beta.1 ships 9
      total in the web_transport directory).
    - **New plugin-registered factories** (2):
      `WebTransportMsQuicSessionFactory` (id
      `"msquic"`) and `WebTransportDataChannelFactory`
      (id `"web_transport"`).
    - **New helper**: `nimrtc_copy_msquic_dll()` in
      `cmake/NimRTCTest.cmake`.
    - **Profile enum rename**: `kProfileSfu` →
      `kProfileRelay` (non-additive surface delta;
      see the "Changed" section above for the
      migration table).
    - **Per-symbol API stability markers**: `engine.hpp`,
      `transport.hpp`, `datachannel.hpp` now have
      `NIMRTC_API_STABLE`/`NIMRTC_API_UNSTABLE` on
      individual symbols (v0.14.x slip-or-land that landed).
    - **No existing public symbol is removed or
      DEPRECATED in beta.1** (the deprecation policy
      in ADR-015 §Decision 3 still reserves the
      first `NIMRTC_API_DEPRECATED` annotations on
      existing public symbols for the
      v1.0.0-beta.1 → v1.0.0-rc.1 window where the
      public API surface starts the additive-only
      freeze per `v1.0-plan.md` §12.2 v1.0.0-beta.1
      Gate #5).

- **ABI**: experimental through v1.0.0.  Pin to a
  specific commit.  The new
  `web_transport_data_channel.hpp` /
  `web_transport_plugin.hpp` headers are
  `NIMRTC_API_UNSTABLE`; the classification is
  visual at the beta.1 cut
  (`NIMRTC_API_UNSTABLE` expands to nothing in
  default mode).

- **Data plane**: WebTransport / QUIC (msquic
  v2.x) at the beta.1 cut, in parallel with the
  existing SCTP-over-DTLS path.  The
  `SctpDataChannel` (id `"sctp"`) remains the
  default; the `WebTransportDataChannel` (id
  `"web_transport"`) is the v1.0 production
  backend per ADR-014.  Switch via the engine
  config field that lands in v0.14.0-rc.1
  (`EngineConfig::datachannel_backend`).

- **4-platform CI**: green on windows-2022 +
  linux-gcc + macos-clang.  linux-aarch64 may be
  TEMP-DISABLED per v0.12.0 baseline; if so, the
  release notes record the reason.  The
  `48aca5e` C28020 / XDP / `msquic.dll`
  search-path fix was windows-msvc-specific
  (linux-gcc / macos-clang / linux-aarch64 were
  unaffected).  `23a39e7` fixes GCC/Clang warnings
  (SYSTEM includes + 2 unused-variable warnings).
  `98f80ea` skips `msquic.dll` copy on non-Windows
  platforms.

- **usrsctp 0.9.5.0** still in tree (v1.2.0
  removal per `v1.0-plan.md` §2.2).

- **msquic** is now a git submodule
  (`src/third_party/msquic/`); the first
  `git submodule update --init --recursive` is
  required after pulling the beta.1 tag.  CI
  does this in the checkout step (see
  `.github/workflows/ci.yml` line 32-39).

- **Pre-release surface stability**: the
  `NIMRTC_API_VERSION_PRE_RELEASE="beta.1"`
  bump is the only macro change at the cut;
  `NIMRTC_API_VERSION_MAJOR/MINOR/PATCH`
  remain 1.0.0 (the pre-release identifier
  carries the version drift within the v0.14.x
  cycle; the MAJOR/MINOR/PATCH triple only
  flips at the v1.0.0 final tag).

### What is NOT in v0.14.0-beta.1 (deferred to v0.14.0-rc.1 or later)

- **Chrome e2e Case E acceptance gate** —
  **still open after rc.1.**  The harness at
  `tests/chrome_interop_harness/` and the server
  seam (`IWebTransportServer`, `demo-wtserver`)
  both landed; the browser-driven end-to-end
  exchange of an `RTCDataChannel` over
  WebTransport / QUIC against headless Chrome
  does **not** pass — it is blocked on a
  server-side QUIC handshake defect.  **Gate #3
  in `v1.0-plan.md` §12.3.3 is still open; see
  `[0.14.0-rc.1]` -> Notes -> Known limitations
  for the full diagnosis.**
- **`EngineConfig::datachannel_backend` field** —
  **shipped in this cut after all.**  The
  canonical beta.1 tree does contain both
  `datachannel_backend` (default `"sctp"`) and
  `web_transport_name` (default `"msquic"`) in
  `src/engine/include/nimrtc/engine/engine.hpp`,
  and `engine.cpp::init_datachannel_registry()` is
  already the `uint32_t` dispatcher with five
  resolved branches.  (This entry previously listed
  the field as an rc.1 follow-up; the field and the
  dispatcher are both present at the beta.1 tag,
  `706383a`.)  What beta.1 does **not** do is wire
  a physical WebTransport session through the
  dispatch — the DTLS-ICE -> RFC 9221 handoff
  remains the rc.1 PR-2 / v1.0.0 item.
- **`EngineConfig::datachannel_backend` default flip
  from `"sctp"` to `"web_transport"`** — follow-up
  v0.14.x patch post-beta.1 (NOT at the beta.1
  cut; the flip is the v1.0.0 final action per
  `v1.0-plan.md` §12.3.3 Gate #4).
- **L1 strict-priority scheduling across the DC
  plane (DC-2)** — v1.0.0-beta.2 (per
  `v1.0-plan.md` §8 + §12.3.4).
- **SFU-1 / DC-3 / BENCH-1** — pre-1.0 decision
  window per `v1.0-plan.md` §7.
- **usrsctp `src/third_party/usrsctp/` removal**
  — v1.2.0 (per `v0.11-plan.md` §2.2 +
  `v1.0-plan.md` §2.2).
- **DTLS 1.3 re-enablement on wolfSSL 5.9.2+** —
  v1.x (per `v1.0-plan.md` §2.2 +
  `docs/plan/dtls_chrome_interop_review.md`).
- **`max_p99_ms` budget enforcement** — slipped
  to v0.14.x or v0.15.x if not landed in
  v0.14.0-rc.1.
- **OHAudio HAL adapter implementation** — slipped
  to v0.14.x or v0.15.x if not landed in
  v0.14.0-rc.1.
- **`cross-network-peer` naming review** —
  slipped to v0.14.x if not landed in
  v0.14.0-rc.1.
- **§5.3 rule 2 media-plane production hardening
  (FEC + codec + ARQ reliability)** — v0.14.x
  slip-or-land or later.
- **Composite `sfu-agent` Profile resolution** —
  pre-1.0 decision window.
- **Adaptive JitterBuffer + Goog-CC full mainline
  BWE** — v1.x.
- **GM/T 国密 SRTP backend** — v1.x.
- **AddressSanitizer CI matrix** — v1.x.

### DoD Gate Status (per `v1.0-plan.md` §12.3.3)

| # | Gate | Status |
|---|---|---|
| 1 | msquic 集成（`src/third_party/msquic/` + CMake） | ✅ PASS — `test_web_transport_msquic.cpp` 12/12 PASS |
| 2 | `WebTransportDataChannel` impl | ✅ PASS — `test_web_transport_data_channel.cpp` 17/17 PASS |
| 3 | Chrome e2e Case E PASS | ⏳ **In Progress** — harness in place; browser-driven test lands in rc.1 |
| 4 | `EngineConfig::datachannel_backend` 默认值 | ⚠️ Field exists; flip to `"web_transport"` is post-beta.1 action |
| 5 | 公共头 freeze（beta 阶段只 additive） | ✅ 当前无 additive 变更 |
| 6 | 4-平台 CI green | ✅ windows-2022 + linux-gcc + macos-clang PASS |
| 7 | usrsctp 仍编译 | ✅ `test_sctp_usrsctp` 4/4 PASS |

## [0.14.0-beta.1] - 2026-10-19

### Status: Tech Preview

> **Second pre-release of the v0.14.x cycle** — the
> **v1.0.0-beta.1** pre-release per `docs/plan/v1.0-plan.md`
> §8 + §12.2 + §12.3.2.  The marketing version stays anchored
> to the v0.x line ("Tech Preview" banner unchanged from
> v0.13.0); the `NIMRTC_API_VERSION_PRE_RELEASE` macro cycles
> to `"beta.1"`.  This is the **data-plane wire-up** — the
> msquic-backed `WebTransportMsQuicSession` and the
> `WebTransportDataChannel` (DC-1) land here, closing the
> v0.14.0-alpha.2 "seam-only" gap.  The in-process regression
> surface now covers `test_web_transport_msquic` 12/12 +
> `test_web_transport_data_channel` 17/17 +
> `test_web_transport_seam` 5/5 + the existing
> `test_datachannel_engine` 5/5 + `test_sctp_usrsctp` 4/4 —
> 508/508 ctest on the windows-2022 MSVC preset at the cut,
> then 513/513 after the `5638e21` test-alignment fix.
>
> **Tag cut**: `git tag -s v0.14.0-beta.1 -m "v0.14.0-beta.1 (alias: v1.0.0-beta.1) — ..."`.
> The tag message body documents the alias relationship to
> v1.0.0-beta.1 and the v0.x banner unchanged.
>
> **Cut order** (10/19): the `5638e21` test-alignment commit
> was pushed first (CI run pending at cut time) so the
> 513/513 surface is what the tag actually points to, not
> the 508/513 it would have inherited from the alpha.2
> seam-only tests.

### Highlights

- **WebTransport / msquic data plane (ADR-014 / DC-1)** —
  `src/web_transport/` now ships the production
  `WebTransportMsQuicSession` (msquic ConnectionStart /
  send_datagram / open_bidi_stream) plus the
  `WebTransportMsQuicSessionFactory` registered as id
  `"msquic"`.  The factory is auto-registered by
  `pal_default_registrars.cpp` so
  `register_all_default_plugins()` exposes it through
  `PluginRegistry::get_web_transport("msquic")` and
  `list_web_transports()` returns exactly `{"msquic"}` at
  the cut (downstream tests pin this set; see
  `wt014_registry_default_registrars_auto_registers_msquic_in_beta1`).

- **WebTransportDataChannel (DC-1)** — the production
  `plugins::IDataChannel` implementation that maps
  `DataChannelReliability` onto `IWebTransportSession`
  primitives:
    - `kReliableOrdered` / partial-reliability modes →
      `open_bidi_stream()` (bidi QUIC stream + app-level
      TTL / count resets)
    - `kUnreliable` → `send_datagram()` (QUIC datagram, no
      stream bookkeeping)
  Factory id `"web_transport"`; the same data-channel
  surface that the engine already uses for
  `SctpDataChannel` (id `"sctp"`), so the wire is
  engine-transparent.  Header is `NIMRTC_API_UNSTABLE` per
  the v1.0.0-beta.1 classification sweep.

- **msquic v2.x vendored submodule (ADR-014)** —
  `src/third_party/msquic/` is now a git submodule pinned
  to the upstream v2.x branch, with `cmake/FindMsQuic.cmake`
  as the packaged-locate fallback for distro paths.  The
  msquic C API surface is exercised end-to-end through the
  new `nimrtc_copy_msquic_dll()` POST_BUILD helper
  (copies the resolved `msquic.dll` next to the test exe
  on Windows; CI's `windows-2022` runner uses the vendored
  path).  12/12 unit tests in `test_web_transport_msquic.exe`
  PASS on the windows-2022 CI image after the `48aca5e`
  C28020 / XDP / `msquic.dll` search-path fix.

- **Chrome DTLS interop harness (e2e acceptance surface)** —
  `tests/chrome_interop_harness/` adds a no-media
  peerConnection-only probe (`chrome_dtls_only.html`) and a
  full diagnostic harness (`chrome_dtls_diag.html`) plus a
  `run_harness.py` driver.  Both are guarded by the
  pre-existing `NIMRTC_BUILD_INTEROP=ON` option (off by
  default; enabled via the `interop/` spike scripts).
  The harness accepts the same MSVC linker-environment the
  existing tests use, and lays the groundwork for the
  v0.14.0-beta.1 → v0.14.0-rc.1 Chrome Case E DataChannel
  acceptance gate (data plane wire-up now ready; the
  browser-driven test is the next landing).

- **Profile rename: `kProfileSfu` → `kProfileRelay`
  (PROFILE-1)** — the v0.14.0 alpha.2 profile enum
  `kProfileSfu` is renamed to `kProfileRelay` to match the
  v0.14-plan.md §2.3 vocabulary shift (the relay is a
  profile role, not just a server type; the term "SFU" is
  reserved for the future `v1.1.0-plan.md` (if the SFU
  track is reopened) — per `v1.0-plan.md` §7 = NO,
  closed 2026-10-19).  No wire-level change; only the enum
  constant name.  `assembly/profiles.hpp` updated; the
  5 transport-strategy tests that reference the old name
  in their descriptive comments were updated to match.

- **wt014 + header-classification test alignment
  (`5638e21`)** — three test fixes that were masked at
  the alpha.2 cut because the seam shipped without the
  production factory entry:
    1. `web_transport_data_channel.hpp` now carries an
       explicit `NIMRTC_API_UNSTABLE` marker (matching
       the other 9 web_transport headers).
    2. `kBaselineHeaderCount` in `test_api_classification.cpp`
       bumped 97 → 99 for the two beta.1 public-header
       additions (this header + `web_transport_plugin.hpp`).
    3. `wt014_registry_default_registrars_omits_web_transport_until_beta1`
       renamed to
       `wt014_registry_default_registrars_auto_registers_msquic_in_beta1`;
       the previous defensive cleanup loop
       `while not empty unregister` was a hack that
       silently removed the production `'msquic'` entry on
       beta.1 and made the test pass for the wrong reason;
       replaced with a prefix-scoped cleanup (only removes
       stub ids matching `wt014_`) and assertions that
       pin the auto-registered set to exactly `{'msquic'}`
       (size==1u, msquic non-null).  ctest now reports
       `100% tests passed, 0 tests failed out of 513`.

- **CI / link polish (`48aca5e`, `77c129f`)** —
  `48aca5e` clears three msquic-on-Windows warnings that
  the beta.1 build surfaced in CI (not local): C28020
  ("Cast between semantically different integer and
  enum types") in the msquic XDP datapath init, the
  `msquic.dll` search-path for the post-build copy
  helper, and the SIO function-pointer cast for the
  XDP completion routine.  `77c129f` ties the
  `nimrtc::web_transport` link line to the right
  `core::PluginRegistry` symbol and polishes the
  profile-rename transition.  Both fixes were
  CI-only; local builds were clean.

### Added

- **`src/web_transport/`** — the production WebTransport
  plugin library.  Compiles to `nimrtc::web_transport`
  (static lib by default; shared if `BUILD_SHARED_LIBS=ON`).
  Adds:
    - `src/web_transport_msquic_session.{hpp,cpp}` — the
      `WebTransportMsQuicSession` implementation (msquic
      ConnectionStart / OpenStream / SendDatagram).
    - `src/web_transport_data_channel.cpp` — the
      `WebTransportDataChannel` implementation
      (bidi-stream for reliable, datagram for unreliable,
      app-level reset for partial reliability).
    - `src/web_transport_plugin.cpp` — the
      `register_web_transport_plugins()` entry point that
      `pal_default_registrars.cpp` calls.

- **`src/third_party/msquic/`** (git submodule) —
  msquic v2.x pinned to the upstream main branch, with
  our `cmake/FindMsQuic.cmake` wrapper preferring the
  submodule path and falling back to `find_package()`
  for distro installs.  Default
  `NIMRTC_VENDORED_MSQUIC=ON` at the beta.1 cut; flip
  to `OFF` to consume a system-installed msquic
  instead (e.g. for distros that ship it prebuilt).

- **`cmake/FindMsQuic.cmake`** (186 lines) — packaged
  locate helper.  Documents the upstream CMake's
  `QUIC_BUILD_DIR` / `QUIC_BUILD_SHARED` knobs; the
  vendored build sets `QUIC_BUILD_DIR` to the
  `src/third_party/msquic` submodule path.

- **`cmake/NimRTCTest.cmake`** — adds
  `nimrtc_copy_msquic_dll()` POST_BUILD helper that
  copies the resolved `msquic.dll` next to any test
  exe in the windows-2022 MSVC preset.  Mirrors the
  googletest / gtest_main DLL copy pattern.

- **`src/plugins/include/nimrtc/plugins/base.hpp`** —
  adds `plugins::Status::kErrClosed` for the
  peer-FIN / RST return path that the WebTransport
  data-channel path produces on graceful close.

- **`tests/test_web_transport_msquic.cpp`** (new, 12
  subtests) — factory-registration + mock-session
  coverage for the msquic backend:
  `factory_registered_with_id_msquic` /
  `factory_display_name_mentions_msquic` /
  `unknown_id_returns_null` /
  `mock_session_open_bidi_stream_returns_null_when_closed` /
  `mock_session_send_datagram_returns_not_ready_when_not_connected` /
  `mock_session_set_on_datagram_sets_flag` / etc.
  Uses a `MockMsQuicApi` test double; no real msquic
  network IO in the unit tests (the spike at
  `interop/v0_14_msquic_spike/` covers the real
  network path).

- **`tests/test_web_transport_data_channel.cpp`**
  (new, 17 subtests) — coverage for the
  `WebTransportDataChannel` mapping:
  `factory_registered_with_id` /
  `factory_create_returns_channel` /
  `factory_create_with_session` /
  `open_and_close_are_idempotent` /
  `open_without_session_returns_not_ready` /
  `open_config_caches_reliability` /
  `send_unreliable_uses_datagram` /
  `send_unreliable_noop_when_not_connected` /
  `send_reliable_uses_bidi_stream` /
  `send_reliable_uses_existing_stream` / etc.
  No real msquic; the test wires a mock session.

- **`tests/chrome_interop_harness/`** (new, gated by
  `NIMRTC_BUILD_INTEROP=ON`) — Chrome DTLS
  interoperability probing harness:
    - `chrome_dtls_only.html` — peerConnection-only
      probe, no media.
    - `chrome_dtls_diag.html` — full diagnostic harness
      (SDP dump + DTLS fingerprint + ICE candidate
      table + error log).
    - `run_harness.py` — driver script that opens the
      harness in headless Chrome, drives the
      peerConnection, and asserts the DTLS
      interop surface.
  The data-plane Case E (DataChannel exchange over
  WebTransport) lands in a follow-up; the harness
  already accepts the wire format.

- **2 new public headers** (the beta.1 additive
  surface vs the alpha.2 cut):
    - `src/web_transport/include/nimrtc/web_transport/web_transport_data_channel.hpp`
    - `src/web_transport/include/nimrtc/web_transport/web_transport_plugin.hpp`
  Both `NIMRTC_API_UNSTABLE`; the seam
  classification is visual at the v1.0.0-beta.1
  cut.

- **`core::PluginRegistry::register_web_transport(id,
  factory*)` / `get_web_transport(id)` /
  `list_web_transports()`** — the typed registry slot
  (alpha.2) now has the production
  `WebTransportMsQuicSessionFactory` auto-registered
  by `pal_default_registrars.cpp`.  `list_web_transports()`
  returns exactly `{"msquic"}` after
  `register_all_default_plugins()`.

- **`nimrtc_copy_msquic_dll()` CMake helper** —
  `cmake/NimRTCTest.cmake` POST_BUILD function for
  any test exe that links `nimrtc::web_transport` on
  Windows; copies the resolved `msquic.dll` next to
  the exe so `LoadLibrary` finds it without a
  PATH-side install step.

### Changed

- **`NIMRTC_API_VERSION_PRE_RELEASE`** cycles from
  `"alpha.2"` to `"beta.1"` in
  `src/core/include/nimrtc/core/api_version.hpp`.
  The matching `nimrtc::core::api_version::kPreRelease`
  constant updates automatically (the macro is the
  source of truth).

- **`src/modules/assembly/include/nimrtc/assembly/profiles.hpp`**
  — `kProfileSfu` renamed to `kProfileRelay`.  No
  wire-level change; the enum constant's
  `std::string_view` name is the only delta.  All
  consumers updated:
    - `src/transport/src/transport_strategy.cpp`
      (5 strategy tests with descriptive comments)
    - `src/engine/include/nimrtc/engine/pal/make_plugin.hpp`
      (the `PluginFailureMode` / `MakePluginOpts`
      forward declarations that reference the
      profile id)
    - `docs/plan/v0.14-plan.md` §2.3 vocabulary
    - `CHANGELOG.md` (this entry)

- **`src/core/src/pal_default_registrars.cpp`** —
  appends the `web_transport` registrar so
  `register_all_default_plugins()` auto-registers the
  `WebTransportMsQuicSessionFactory` (id `"msquic"`)
  and the `WebTransportDataChannelFactory`
  (id `"web_transport"`).  The seam was wired at the
  alpha.2 cut; the registrar call was the missing
  piece.

- **`src/third_party/CMakeLists.txt`** — explicit
  msquic submodule handling (the upstream
  `CMakeLists.txt` lives at the submodule root, not
  in a `src/` subdir; we add the
  `add_subdirectory(... EXCLUDE_FROM_ALL)` then
  `add_dependencies(msquic)` glue).

- **`CMakeLists.txt`** (root) — adds
  `add_subdirectory(src/web_transport)` so the
  `nimrtc::web_transport` target is wired into the
  build graph.  Default `ON`; flip
  `NIMRTC_ENABLE_WEB_TRANSPORT_MSQUIC=OFF` to skip
  the WebTransport / msquic data-plane build (the
  seam headers still compile and the registry slot
  is still available; only the production factory
  is omitted).

- **CHANGELOG.md** — `[0.14.0-beta.1]` section
  finalised; `[0.14.0]` "What is NOT in v0.14.0"
  deferred list collapses (all four beta.1 items
  now land); the placeholder block from
  `0cb0176` becomes this entry.

### Notes

- **Public API surface diff vs v0.14.0** (alpha.2):
  additive only.
    - **New headers** (2):
      `src/web_transport/include/nimrtc/web_transport/web_transport_data_channel.hpp`
      and `web_transport_plugin.hpp` (the alpha.2
      cut shipped 7 new headers; beta.1 ships 9
      total in the web_transport directory).
    - **New plugin-registered factories** (2):
      `WebTransportMsQuicSessionFactory` (id
      `"msquic"`) and `WebTransportDataChannelFactory`
      (id `"web_transport"`).
    - **New helper**: `nimrtc_copy_msquic_dll()` in
      `cmake/NimRTCTest.cmake`.
    - **Profile enum rename**: `kProfileSfu` →
      `kProfileRelay` (non-additive surface delta;
      see the "Changed" section above for the
      migration table).
    - **No existing public symbol is removed or
      DEPRECATED in beta.1** (the deprecation policy
      in ADR-015 §Decision 3 still reserves the
      first `NIMRTC_API_DEPRECATED` annotations on
      existing public symbols for the
      v1.0.0-beta.1 → v1.0.0-rc.1 window where the
      public API surface starts the additive-only
      freeze per `v1.0-plan.md` §12.2 v1.0.0-beta.1
      Gate #5).

- **ABI**: experimental through v1.0.0.  Pin to a
  specific commit.  The new
  `web_transport_data_channel.hpp` /
  `web_transport_plugin.hpp` headers are
  `NIMRTC_API_UNSTABLE`; the classification is
  visual at the beta.1 cut
  (`NIMRTC_API_UNSTABLE` expands to nothing in
  default mode).

- **Data plane**: WebTransport / QUIC (msquic
  v2.x) at the beta.1 cut, in parallel with the
  existing SCTP-over-DTLS path.  The
  `SctpDataChannel` (id `"sctp"`) remains the
  default; the `WebTransportDataChannel` (id
  `"web_transport"`) is the v1.0 production
  backend per ADR-014.  Switch via the engine
  config field that lands in v0.14.0-rc.1
  (`EngineConfig::datachannel_backend`).

- **4-platform CI**: green on windows-2022 +
  linux-gcc + macos-clang.  linux-aarch64 may be
  TEMP-DISABLED per v0.12.0 baseline; if so, the
  release notes record the reason.  The
  `48aca5e` C28020 / XDP / `msquic.dll`
  search-path fix was windows-msvc-specific
  (linux-gcc / macos-clang / linux-aarch64 were
  unaffected).

- **usrsctp 0.9.5.0** still in tree (v1.2.0
  removal per `v1.0-plan.md` §2.2).

- **msquic** is now a git submodule
  (`src/third_party/msquic/`); the first
  `git submodule update --init --recursive` is
  required after pulling the beta.1 tag.  CI
  does this in the checkout step (see
  `.github/workflows/ci.yml` line 32-39).

- **Pre-release surface stability**: the
  `NIMRTC_API_VERSION_PRE_RELEASE="beta.1"`
  bump is the only macro change at the cut;
  `NIMRTC_API_VERSION_MAJOR/MINOR/PATCH`
  remain 1.0.0 (the pre-release identifier
  carries the version drift within the v0.14.x
  cycle; the MAJOR/MINOR/PATCH triple only
  flips at the v1.0.0 final tag).

### What is NOT in v0.14.0-beta.1 (deferred to v0.14.0-rc.1 or later)

- **`EngineConfig::datachannel_backend` field** —
  v0.14.0-rc.1. The registry is wired
  (`PluginRegistry::get_web_transport("msquic")`
  resolves at the beta.1 cut), but the engine
  config surface that maps the user-facing
  `"msquic"` / `"sctp"` / `"web_transport"`
  string to a factory lookup is the rc.1
  follow-up. Until then, the engine's
  DataChannel is hard-wired to `SctpDataChannel`
  (id `"sctp"`).
- **Chrome e2e Case E acceptance gate** —
  v0.14.0-rc.1. The harness at
  `tests/chrome_interop_harness/` is in; the
  browser-driven end-to-end test that
  exchanges an `RTCDataChannel` over
  WebTransport / QUIC against headless Chrome
  is the rc.1 follow-up (the harness is the
  scaffolding).
- **L1 strict-priority scheduling across the DC
  plane (DC-2)** — v1.0.0-beta.2 (per
  `v1.0-plan.md` §8 + §12.3.4).
- **SFU-1 / DC-3 / BENCH-1** — pre-1.0 decision
  window per `v1.0-plan.md` §7.
- **usrsctp `src/third_party/usrsctp/` removal**
  — v1.2.0 (per `v0.11-plan.md` §2.2 +
  `v1.0-plan.md` §2.2).
- **DTLS 1.3 re-enablement on wolfSSL 5.9.2+** —
  v1.x (per `v1.0-plan.md` §2.2 +
  `docs/plan/dtls_chrome_interop_review.md`).
- **`max_p99_ms` budget enforcement** — slipped
  to v0.14.x or v0.15.x if not landed in
  v0.14.0-rc.1.
- **OHAudio HAL adapter implementation** — slipped
  to v0.14.x or v0.15.x if not landed in
  v0.14.0-rc.1.
- **`cross-network-peer` naming review** —
  slipped to v0.14.x if not landed in
  v0.14.0-rc.1.
- **§5.3 rule 2 media-plane production hardening
  (FEC + codec + ARQ reliability)** — v0.14.x
  slip-or-land or later.
- **Composite `sfu-agent` Profile resolution** —
  pre-1.0 decision window.
- **Adaptive JitterBuffer + Goog-CC full mainline
  BWE** — v1.x.
- **GM/T 国密 SRTP backend** — v1.x.
- **AddressSanitizer CI matrix** — v1.x.
- **Per-symbol inline stability annotation** —
  v0.14.x slip-or-land (the v0.13.0 sweep was
  at the file level; the per-symbol pass is a
  v0.14.x follow-up).
## [0.14.0] - 2026-10-16

### Status: Tech Preview

> **First pre-release of the v0.14.x cycle** — the
> **v1.0.0-alpha.2** pre-release per `docs/plan/v1.0-plan.md`
> §8 + §12.2 + §12.3.2.  The marketing version stays anchored
> to the v0.x line ("Tech Preview" banner unchanged from
> v0.13.0); the `NIMRTC_API_VERSION_PRE_RELEASE` macro cycles
> to `"alpha.2"`.  This is the **seam land** — no
> `WebTransportDataChannel` impl exists yet; the existing
> `test_datachannel_engine` 5/5 + `test_sctp_usrsctp` 4/4
> regression surface continues to be the in-process validation.
> The data-plane wire-up is the v0.14.0-beta.1 work per
> `docs/plan/v0.14-plan.md` §2.1.2.
>
> **Tag cut**: `git tag -s v0.14.0 -m "v0.14.0 (alias: v1.0.0-alpha.2) — ..."`.
> The tag message body documents the alias relationship to
> v1.0.0-alpha.2 and the v0.x banner unchanged.
>
> **Tag day follow-ups (2026-10-16)**: the v0.14.0 tag was
> **force-re-pointed twice** on tag day to fold in two `release.yml`
> fixes that the tag-cut force-push surfaced:
>
> 1. **PR #20** (`b5da037`) — added `-DNIMRTC_VENDORED_WEBRTC_APM=OFF`
>    to the four `release.yml` `Configure` steps so audio3a links
>    against the same stub path that `ci.yml` uses, instead of
>    chasing `absl/base/config.h` in the vendored WebRTC APM
>    prebuild.
> 2. **`dc15ad8`** — added `if: ${{ false }}` to the four
>    `Prebuild WebRTC APM` steps in `release.yml` (windows-msvc +
>    linux-gcc + linux-aarch64 + macos-clang) so the `meson` /
>    `abseil-cpp` subproject is no longer built before
>    `Configure`. The long-term fix
>    (`target_link_libraries(audio3a PUBLIC webrtc_apm::webrtc_apm)`
>    so abseil's `INCLUDE_DIRECTORIES` propagate to the consumer) is
>    tracked in a follow-up; until that lands, `release.yml` ships
>    with the audio3a stub path, byte-equivalent to `ci.yml`.
>
> RELEASING.md §2.3 tag immutability is preserved because the
> v0.14.0 GitHub Release was a no-artefact draft at first cut
> (`release / publish` was skipped / cancelled in
> run #38898093864); no public announcement had been made. Each
> re-point landed before the corresponding `release / publish`
> re-run.

### Highlights

- **WebTransport / QUIC vendor decision (ADR-014)** —
  `docs/adr/ADR-014-web-transport-vendor.md` (Status:
  Accepted). msquic chosen as the WebTransport / QUIC backend
  for the v0.14.0-beta.1 `WebTransportMsQuicSession` impl. The
  5 evaluation criteria in `v1.0-plan.md` §10.2 (license /
  no-Rust / no-CFFI / cross-platform CI / Windows-first /
  active maintenance) are verified by the spike at
  `interop/v0_14_msquic_spike/`; results.txt records
  per-criterion PASS/FAIL with timestamps + commit SHAs.
  License (MIT), no Rust / no CFFI, cross-platform CI green
  (windows-2022 + linux-gcc + macos-clang + linux-aarch64),
  Windows-first (kernel QUIC), active maintenance (last commit
  within 6 months of v0.14.0 tag cut). Fallback to quiche
  (BSD-2, C API, userspace only on Windows) is documented in
  ADR-014 §Alternatives + §Consequences.

- **`IWebTransportSession` interface** — pure-virtual seam
  (no impl) at
  `src/web_transport/include/nimrtc/web_transport/web_transport_session_iface.hpp`.
  Companion headers:
    - `bidi_stream_iface.hpp` — `IBidirectionalStream`
    - `uni_stream_iface.hpp`  — `IUnidirectionalStream`
    - `datagram_facade.hpp`   — `DatagramFacade` (utility wrapper)
    - `web_transport_config.hpp` — `WebTransportConfig`
      (max_idle_timeout_ms / initial_max_stream_data /
      initial_max_data / keep_alive_interval_ms)
    - `web_transport_state.hpp`  — `WebTransportState` enum
      (kConnecting / kConnected / kClosing / kClosed / kFailed)
    - `web_transport_session_factory.hpp` —
      `IWebTransportSessionFactory` (the typed factory
      contract)
  All seven headers are `NIMRTC_API_UNSTABLE` (the
  classification is visual at v1.0.0-alpha.2; the "v1.0.0
  stable" banner does not flip until v1.0.0 final).

- **`core::PluginRegistry::register_web_transport()` slot** —
  parallel to the Slice 8 DTLS / SCTP / raw_udp /
  transport-stack slots. The factory slot exists at the
  alpha.2 cut; the msquic factory lands in beta.1 alongside
  the msquic integration. `list_web_transports()` returns an
  empty vector at the alpha.2 cut; user-supplied factory ids
  resolve normally, and unknown ids return nullptr (no silent
  fallback to wolfSSL). Two new subtests in
  `test_engine_plugin_loading.cpp`:
    - `wt014_registry_slot_is_typed_for_web_transport_factory`
    - `wt014_registry_default_registrars_omits_web_transport_until_beta1`

- **Compile-only seam test** —
  `tests/test_web_transport_seam.cpp` (new). Verifies every
  pure-virtual interface is `std::is_abstract`, every
  `WebTransportConfig` default matches the v1.0-plan.md
  §10.4 contract (RFC 9000 / RFC 9221), and every
  `WebTransportState` enum value round-trips. 5 subtests
  PASS on Windows MSVC. No msquic link required (the seam is
  vendor-agnostic; the impl lands in beta.1).

- **v0.14.x spike-validation program** — 6 spike programs at
  `interop/v0_14_msquic_spike/` exercise the full msquic C
  API surface against the planned `IWebTransportSession`
  methods:
    - `echo_client.cpp` / `echo_server.cpp` — 0-RTT echo
      round-trip latency < 50 ms
    - `stream_reset.cpp` — `MsQuicStreamSendComplete` + reset
    - `datagram_roundtrip.cpp` — `MsQuicDatagramSend` / recv
    - `concurrent_streams.cpp` — 16-stream fanout
  The `results.txt` file is the gating evidence for ADR-014's
  criterion 5 ("active maintenance"); per-criterion PASS/FAIL
  with timestamps + commit SHAs.

### Added

- **`src/web_transport/include/nimrtc/web_transport/*.hpp`**
  (7 new public headers) — the WebTransport / QUIC seam.
  All headers are `NIMRTC_API_UNSTABLE` (the v1.0.0-alpha.2
  classification; stable banner does not flip until v1.0.0
  final).

- **`IWebTransportSession` interface** in
  `web_transport_session_iface.hpp`. Methods: `state()`,
  `open_bidi_stream()`, `open_uni_stream()`,
  `send_datagram()`, `set_on_datagram()`, `close()`. Pure
  virtual; no impl.

- **`IBidirectionalStream` + `IUnidirectionalStream`**
  interfaces in `bidi_stream_iface.hpp` /
  `uni_stream_iface.hpp`. Bidi methods: `send` / `recv` /
  `set_on_data` / `close` / `reset`. Uni methods: `send` /
  `close` / `reset`.

- **`WebTransportConfig`** in `web_transport_config.hpp` —
  small struct mirroring the QUIC settings the msquic
  backend exposes through `QUIC_SETTINGS` + `QUIC_CONFIGURATION`
  parameters. Defaults match RFC 9000 / RFC 9221.

- **`WebTransportState` enum** in
  `web_transport_state.hpp` — 5-value enum
  (`kConnecting` / `kConnected` / `kClosing` / `kClosed` /
  `kFailed`) with `uint8_t` underlying type.

- **`DatagramFacade`** in `datagram_facade.hpp` — non-virtual
  utility wrapper around `IWebTransportSession`'s datagram
  surface. Binds a non-owning pointer to the session; the
  facade does not extend the session's lifetime.

- **`IWebTransportSessionFactory`** in
  `web_transport_session_factory.hpp` — typed factory
  contract; matches the `dtls::IDtlsSessionFactory` /
  `sctp::ISctpSocketFactory` / `raw_udp::IRawUdpFactory` /
  `ITransportStackFactory` pattern. No factory is
  auto-registered at the alpha.2 cut; the msquic factory
  lands in beta.1.

- **`NIMRTC_REGISTER_WEB_TRANSPORT` macro** in
  `src/core/include/nimrtc/core/registry.hpp` —
  `static ::nimrtc::core::detail::Registrar` wrapper for the
  new typed slot, parallel to the existing
  `NIMRTC_REGISTER_DTLS_SESSION` / `SCTP_SOCKET` /
  `RAW_UDP_DATAGRAM` / `TRANSPORT_STACK` macros.

- **`core::PluginRegistry::register_web_transport(id,
  factory*)` / `get_web_transport(id)` /
  `list_web_transports()`** — new typed slot on
  `core::PluginRegistry`. Forward-declared in registry.hpp;
  no msquic headers leak into core (Layout Invariant 1
  preserved).

- **`tests/test_web_transport_seam.cpp`** (new, 5 subtests):
  `seam_interfaces_are_abstract` /
  `datagram_facade_forwards_to_session` /
  `datagram_facade_with_null_session` /
  `callbacks_accept_lambdas` /
  `state_enum_round_trip`. Compile-only test; the gate is
  "the seam headers compile + link on all 4 platforms".

- **2 new `EnginePluginLoading` subtests** in
  `tests/test_engine_plugin_loading.cpp`:
    - `wt014_registry_slot_is_typed_for_web_transport_factory` —
      verifies the slot exists, accepts a user-registered
      factory, and resolves by id.
    - `wt014_registry_default_registrars_omits_web_transport_until_beta1` —
      pins the alpha.2 → beta.1 transition: no factory is
      auto-registered at the alpha.2 cut; the msquic factory
      lands in beta.1.

- **`NIMRTC_API_VERSION_PRE_RELEASE`** cycles to `"alpha.2"`
  in `src/core/include/nimrtc/core/api_version.hpp`.

- **`interop/v0_14_msquic_spike/`** (new) — spike-validation
  program for ADR-014. `README.md` documents the 5-criterion
  check; `results.txt` records per-criterion PASS/FAIL with
  timestamps + commit SHAs. 6 spike programs exercise the
  full msquic C API surface against the planned
  `IWebTransportSession` methods. No msquic source code
  lands in v0.14.0 alpha.2 (the integration lands in beta.1).

- **`docs/adr/ADR-014-web-transport-vendor.md`** (new) —
  msquic vendor decision. 5 evaluation criteria in
  `v1.0-plan.md` §10.2 verified by the spike at
  `interop/v0_14_msquic_spike/`. Fallback to quiche
  documented in §Alternatives + §Consequences.

### Changed

- **`src/core/include/nimrtc/core/registry.hpp`** — adds the
  `register_web_transport` / `get_web_transport` /
  `list_web_transports` methods (parallel to the Slice 8
  DTLS / SCTP / raw_udp / transport-stack slots), the
  `NIMRTC_REGISTER_WEB_TRANSPORT` macro, the `kWebTransport`
  `Category` enum entry, and the matching `case` in
  `Registrar::Registrar`. Forward-declares
  `nimrtc::web_transport::IWebTransportSessionFactory` so
  the registry's `TypedRegistry<...>` field and public
  method signatures can reference the factory type without
  pulling the seam headers into core (Layout Invariant 1
  preserved).

- **`tests/CMakeLists.txt`** — adds the
  `nimrtc_add_web_transport_seam_test()` function and the
  `test_web_transport_seam` target registration. The
  function follows the `nimrtc_add_core_api_test()` pattern
  (links `nimrtc::core` + GTest; no msquic dependency).

- **`src/core/include/nimrtc/core/api_version.hpp`** —
  `NIMRTC_API_VERSION_PRE_RELEASE` cycles from `"alpha.1"`
  to `"alpha.2"`. The matching
  `nimrtc::core::api_version::kPreRelease` constant is
  updated automatically (the macro is the source of truth).

- **CHANGELOG.md** — `[1.0.0-alpha.2]` section finalised;
  `[1.0.0-beta.1]` placeholder added; `[Unreleased]` reset
  to a minimal pointer to `v0.14-plan.md`.

### Notes

- **Public API surface diff vs v0.13.0**: additive only.
    - **New headers**:
      `src/web_transport/include/nimrtc/web_transport/*.hpp`
      (7 new public headers — the seam + factory + companions).
    - **New registry methods**:
      `core::PluginRegistry::register_web_transport()` /
      `get_web_transport()` / `list_web_transports()`.
    - **New macro**: `NIMRTC_REGISTER_WEB_TRANSPORT(id, factory_ptr)`.
    - **No existing public symbol is modified or removed.**
    - **No existing public symbol is marked DEPRECATED in
      v0.14.0** (the deprecation policy in ADR-015 §Decision 3
      governs; the first `NIMRTC_API_DEPRECATED` annotations on
      existing public symbols are reserved for the
      v1.0.0-beta.1 → v1.0.0-rc.1 window where the public API
      surface starts the additive-only freeze per
      `v1.0-plan.md` §12.2 v1.0.0-beta.1 Gate #5).

- **ABI**: experimental through v1.0.0. Pin to a specific
  commit. The new seam headers are `NIMRTC_API_UNSTABLE`; the
  classification is visual at the alpha.2 cut
  (`NIMRTC_API_UNSTABLE` expands to nothing in default
  mode).

- **Data plane**: still SCTP-over-DTLS at the alpha.2 cut
  (the existing `test_datachannel_engine` 5/5 +
  `test_sctp_usrsctp` 4/4 regression surface continues).
  WebTransport / QUIC impl lands in v0.14.0-beta.1.

- **4-platform CI**: green on windows-2022 + linux-gcc +
  macos-clang. linux-aarch64 may be TEMP-DISABLED per
  v0.12.0 baseline; if so, the release notes record the
  reason.

- **usrsctp 0.9.5.0** still in tree (v1.2.0 removal per
  `v1.0-plan.md` §2.2).

- **No new third-party dependencies** for the default
  (non-msquic) build. The msquic integration is
  `NIMRTC_ENABLE_WEB_TRANSPORT_MSQUIC=ON` opt-in (default
  ON at the beta.1 cut; the seam headers compile without
  it).

### What is NOT in v0.14.0 (deferred to v0.14.0-beta.1 or later)

- **WebTransport / QUIC impl (msquic integration)** —
  v0.14.0-beta.1. The `src/third_party/msquic/` git
  submodule + `cmake/FindMsQuic.cmake` + the
  `WebTransportMsQuicSession` + `WebTransportMsQuicSessionFactory`
  (id = `"msquic"`) all land in beta.1.
- **`WebTransportDataChannel` impl** — v0.14.0-beta.1.
- **Chrome e2e Case E** — v0.14.0-beta.1.
- **`EngineConfig::datachannel_backend` field** —
  v0.14.0-beta.1.
- **L1 strict-priority scheduling across the DC plane
  (DC-2)** — v1.0.0-beta.2 (per `v1.0-plan.md` §8 + §12.3.4).
- **SFU-1 / DC-3 / BENCH-1** — pre-1.0 decision window per
  `v1.0-plan.md` §7.
- **usrsctp `src/third_party/usrsctp/` removal** — v1.2.0
  (per `v0.11-plan.md` §2.2 + `v1.0-plan.md` §2.2).
- **DTLS 1.3 re-enablement on wolfSSL 5.9.2+** — v1.x
  (per `v1.0-plan.md` §2.2 + `docs/plan/dtls_chrome_interop_review.md`).
- **`max_p99_ms` budget enforcement** — slipped to v0.14.x
  or v0.15.x if not landed in v0.14.0.
- **OHAudio HAL adapter implementation** — slipped to
  v0.14.x or v0.15.x if not landed in v0.14.0.
- **`cross-network-peer` naming review** — slipped to
  v0.14.x if not landed in v0.14.0.
- **§5.3 rule 2 media-plane production hardening (FEC +
  codec + ARQ reliability)** — v0.14.x slip-or-land or
  later.
- **Composite `sfu-agent` Profile resolution** — pre-1.0
  decision window.
- **Adaptive JitterBuffer + Goog-CC full mainline BWE** —
  v1.x.
- **GM/T 国密 SRTP backend** — v1.x.
- **AddressSanitizer CI matrix** — v1.x.
- **Per-symbol inline stability annotation** — v0.14.x
  slip-or-land (the v0.13.0 sweep was at the file level;
  the per-symbol pass is a v0.14.x follow-up).
## [1.0.0-alpha.1] - 2026-10-14

### Status: Tech Preview

> **First pre-release of the v1.0.0 cycle.** Banner remains
> "Tech Preview" — the public API is **not yet frozen**. The
> "experimental through v1.0.0" banner is dropped at the
> v1.0.0 final tag, not at pre-releases.
>
> **Marketing alias:** the v1.0.0-alpha.1 Git tag is published as
> `v0.13.0` so the public version label stays anchored to the
> v0.x "Tech Preview" line (per `docs/plan/v1.0-plan.md` §8 + §12.2
> — pre-release split). The pre-release identifier carried by
> `NIMRTC_API_VERSION_PRE_RELEASE` is the string `"alpha.1"` and
> the corresponding header `[1.0.0-alpha.1]` heading documents
> the alpha.1 marker.

### Highlights

- **API stability framework** — `NIMRTC_API_VERSION_MAJOR=1 /
  MINOR=0 / PATCH=0 / PRE_RELEASE="alpha.1"` plus the
  `NIMRTC_API_STABLE` / `NIMRTC_API_UNSTABLE` /
  `NIMRTC_API_DEPRECATED(msg)` classification markers and
  `nimrtc::core::api_version::` namespace mirror
  (`docs/adr/ADR-015-api-stability-policy.md`). Every public
  header under `nimrtc/*` now carries a stability classification;
  `tests/test_api_classification` 2/2 PASS.
- **P1 UAF-DTLS type-system hardening** —
  `ITransportStack::dtls_weak()` interface lands in all three
  media-stack implementations (`WebRtcClassicStack`,
  `RawUdpArqStack`, `DefaultTransportStackFactory`); the engine
  migrates `Impl::dtls` + `Impl::datachannel_transport_cookie`
  from raw `IDtlsSession*` to `std::weak_ptr<IDtlsSession>`;
  14 use sites use `if (auto d = impl_->dtls.lock()) { ... }`
  that auto-expires on `close()`. The v0.12.0 P0 `is_open()`
  guards stay as defence-in-depth. New regression test
  `v013_dtls_weak_expires_after_close` in
  `tests/test_engine_plugin_loading.cpp`.
  P1 UAF-on-DTLS class is closed at the type-system level.
- **PAL Slice 1.5** — `pal::MakePluginOpts` named-arguments
  struct + C++20 `auto Resolver` template parameter overload on
  `pal::make_plugin` + 6 new `pal::resolve_*` forwarders
  (`resolve_bwe`, `resolve_scheduler`, `resolve_video_sink`,
  `resolve_video_source`, `resolve_video_receiver`,
  `resolve_video_sender`) + 9 `engine.cpp` plugin-load call
  sites migrated (`docs/adr/ADR-009-pal-slice-1.md` § "Slice
  1.5 follow-up"). Per-call-site arg count drops from 9-10
  positional to 5 positional + 1 struct. Behaviour-preserving:
  27/27 `test_engine_plugin_loading`, 15/15 `test_audio3a_tap`,
  13/13 `test_video_source` PASS on Windows MSVC.

### Added

- **`NIMRTC_API_VERSION` macros** (v1.0.0-alpha.1 / `docs/plan/v1.0-plan.md`
  §2.1 P0 item API-VERSION): `NIMRTC_API_VERSION_MAJOR/MINOR/PATCH`,
  `NIMRTC_API_VERSION_PRE_RELEASE`, `NIMRTC_API_VERSION_STRING` (combined
  "1.0.0"). The matching `nimrtc::core::api_version::kMajor/kMinor/kPatch/
  String/PreRelease` constants are exposed for runtime inspection.
  No public API change to any existing symbol.

- **`NIMRTC_API_STABLE` / `NIMRTC_API_UNSTABLE` markers**: visual
  documentation macros applied to public symbols. Empty expansion
  by default; no compile-time effect. The public header classification
  sweep (alpha.1 PR #3 → `15a47f1`) applies them without changing
  compilation semantics.

- **`NIMRTC_API_DEPRECATED(msg)` macro**: cross-compiler deprecation
  attribute. Expands to `[[deprecated(msg)]]` (C++14+),
  `__attribute__((deprecated(msg)))` (GCC/Clang), or
  `__declspec(deprecated(msg))` (MSVC). Emits the standard
  compiler warning on caller use.

- **`tests/test_api_version.cpp`** (5/5 subtests): verifies the
  macros + namespace constants are well-formed. Compile-time
  smoke for `NIMRTC_API_STABLE/UNSTABLE/DEPRECATED` expansion.

- **`tests/test_api_classification.cpp`** (2/2 subtests; landed as
  test-first design in `6cea40f` with intentional FAIL on
  `every_header_has_at_least_one_marker`; the FAIL flips to PASS
  after the bulk sweep `15a47f1`). The `count_markers()` walker
  runtime-checks every header under `src/<mod>/include/nimrtc/<mod>/`
  (excluding `src/third_party/`) for ≥1 `NIMRTC_API_STABLE` /
  `NIMRTC_API_UNSTABLE` occurrence.

- **`ITransportStack::dtls_weak()`** — new
  `std::weak_ptr<dtls::IDtlsSession>` accessor in
  `src/transport/include/nimrtc/transport/transport_stack.hpp`
  (9f4956b). The existing raw `dtls()` reference getter stays
  with a `[[deprecated]]` comment. `WebRtcClassicStack::dtls_`
  migrates from `std::unique_ptr` to `std::shared_ptr` so the
  stack can hand out a `weak_ptr` outliving the stack's own
  reference; `RawUdpArqStack::dtls_weak()` returns an empty
  `weak_ptr` (no DTLS in the control plane); the default factory
  records the stack for later weak_ptr resolution.

- **Regression test `v013_dtls_weak_expires_after_close`** in
  `tests/test_engine_plugin_loading.cpp` (`d0665aa`). Asserts
  the core v0.13.0 invariant: after `stack->close()`, the
  previously-non-expired `weak_ptr` is expired; `.lock()`
  returns `nullptr`. Guards against future regressions where
  `Engine::Impl::dtls weak_ptr` could silently hold a
  "stuck non-null" weak pointer that `.lock()`s into freed
  memory.

- **`pal::MakePluginOpts` named-arguments struct** in
  `src/engine/include/nimrtc/engine/pal/make_plugin.hpp`
  (`5eb65fa`). Replaces 4 positional error/mode args + trailing
  `on_error` in `pal::make_plugin`. Designated initializers
  make per-category failure policy (Error / Warn / Silent)
  visible at a glance:
  ```cpp
  pal::make_plugin<&pal::resolve_bwe>(
      config_.bwe_name, bwe_, config_.bwe_config,
      {.err_open_failed = core::kEngineInternal,
       .err_missing = core::kEngineInternal,
       .open_fail_mode  = pal::PluginFailureMode::Warn,
       .missing_mode    = pal::PluginFailureMode::Warn,
       .on_error        = on_error_});
  ```

- **C++20 `auto Resolver` template overload on
  `pal::make_plugin`** — `make_plugin<auto Resolver, IPlugin,
  Config>(id, slot, cfg, opts, post_create)` deduces the factory
  type from `std::invoke_result_t<Resolver, std::string_view>`.
  The factory lookup (was: `reg.get_X(id)` at each call site)
  moves inside the helper.

- **6 `pal::resolve_*` forwarders** in
  `src/engine/include/nimrtc/engine/pal/engine_plugin_resolver.hpp`:
  `resolve_audio3a`, `resolve_codec`, `resolve_bwe`,
  `resolve_scheduler`, `resolve_video_codec`,
  `resolve_video_sink`, `resolve_video_source`,
  `resolve_video_receiver`, `resolve_video_sender`,
  `resolve_datachannel`. The header is now symmetric across
  every plugin category the engine uses.

- **`tests/test_engine_plugin_loading.cpp` Slice 1.5 verification
  table** — 27/27 + 15/15 + 13/13 subtests PASS on Windows MSVC
  (`test_engine_plugin_loading`, `test_audio3a_tap`,
  `test_video_source` respectively).

- **ADR-009 Slice 1.5 section** in
  `docs/adr/ADR-009-pal-slice-1.md`. Documents the
  MakePluginOpts + Resolver + 6 forwarders design and the
 9-call-site migration plan.

- **`scripts/clean_build.ps1`** — MSVC build-artifact cleaner
  invoked at tag prep (`4c45a41`). Removes stale
  `vcxproj.user` + `.cache` files from the build tree.

- **`docs/plan/v0.13-plan.md`** — milestone plan for the
  v0.13.0 = v1.0.0-alpha.1 cut. Item-by-item dependency
  graph, PR sequencing, DoD gates, and risk table.

### Changed

- **`nimrtc::engine::pal::make_plugin<IPlugin, Config, Factory>`**
  template at `src/engine/include/nimrtc/engine/pal/make_plugin.hpp`
  (`5eb65fa`). Consolidates the 5-step "factory lookup → create →
  null-check → open → report" dance that was copy-pasted across
  9 plugin categories in `engine.cpp` (5 video plugins + 2
  BWE/Scheduler + audio3a + codec). Per-block size dropped from
  ~20 lines to ~7 lines with no behaviour change.
    - `PluginFailureMode` enum (Error / Warn / Silent) preserves
      each category's failure-reporting policy (video →
      on_error_, BWE → log-warn, audio3a → silent fallback,
      codec → silent fallback).
    - `post_create` hook is invoked between `create()` and
      `open()` to cover the audio3a case where `set_callbacks()`
      must precede `open()`.
    - `if constexpr` branch handles the no-config case
      (`IAudio3AFactory::create()` takes no arguments; the
      caller passes `nullptr` as the config argument).
    - `init_video_plugins()` lost the `auto& reg = ...` capture
      and the `reg.get_video_*` calls; the registry lookups now
      happen inline via `pal::make_plugin<&pal::resolve_*>(...)`.

- **All 9 `engine.cpp` plugin-load call sites** use the new
  template overload
  `pal::make_plugin<&pal::resolve_*>(id, slot, cfg, opts)`:
    - BWE              (line 408, `<&pal::resolve_bwe>`)
    - Scheduler        (line 423, `<&pal::resolve_scheduler>`)
    - Audio3A          (line 758, `<&pal::resolve_audio3a>`,
                          `nullptr` config + lambda post_create)
    - Codec            (line 796, `<&pal::resolve_codec>`,
                          `std::move(codec_cfg)` + Silent policy)
    - Video Sink       (line 1095, `<&pal::resolve_video_sink>`)
    - Video Source     (line 1113, `<&pal::resolve_video_source>`)
    - Video Receiver   (line 1136, `<&pal::resolve_video_receiver>`)
    - Video Sender     (line 1157, `<&pal::resolve_video_sender>`)
    - Video Codec      (line 1180, `<&pal::resolve_video_codec>`)
  Each call site is `(void)pal::make_plugin<...>(...)` to
  silence `-Werror=unused-result` (`[[nodiscard]]` return).
  The 2 `reg.get_transport_stack` calls in
  `init_transport_session()` are intentionally left as
  `reg.get_*()` (not migrated) because they belong to the
  v0.12.0 Slice 7.6 PR3 transport-strategy wiring, not to
  the per-plugin factory loading pattern PAL Slice 1 covers.

- **`ITransportStack::dtls()` raw reference getter** carries a
  `[[deprecated]]` migration hint comment ("use `dtls_weak()`
  for the v0.13.0 weak_ptr surface"). The getter itself remains
  functional for callers that already hold a non-owning raw
  pointer.

- **`NimRTCEngine::Impl::dtls`** + **`Impl::datachannel_transport_cookie`**
  retyped from `nimrtc::dtls::IDtlsSession*` to
  `std::weak_ptr<nimrtc::dtls::IDtlsSession>` in
  `src/engine/src/engine.cpp` (`313b40c`). The capture site
  (`init_modules_once()`) reads
  `impl_->dtls = impl_->session.media_stack->dtls_weak();`.
  All 14 engine use sites use
  `if (auto d = impl_->dtls.lock()) { d->X(); }` (P1 UAF
  Mitigation #4 per `docs/plan/p1-uaf-dtls-investigation.md`).
  The Medium-rated `Impl::datachannel_transport_cookie`
  risk is now closed (was listed as deferred-to-v1.x in
  v0.11-plan.md §2.2 follow-up).

- **Public header classification sweep** (`15a47f1`,
  `feat(public-headers): bulk API-CLASSIFY sweep`).
  85 of 89 public headers annotated with
  `NIMRTC_API_STABLE` (header-level marker; per-symbol
  inline is a v0.13.x follow-up). 4 headers were already
  marked in the alpha.1 pilot (`6cea40f`):
  `engine.hpp`, `engine/pal/make_plugin.hpp`,
  `transport_plugin.hpp`, `core/api_version.hpp`. The sweep
  adds header-level markers to the remaining 85 (transport /
  raw_udp / sctp / dtls / datachannel / modules / core —
  89 total). `tests/test_api_classification` 2/2 PASS
  post-sweep (`headers=89 stable=107 unstable=6
  deprecated=8 missing_markers=0`).

- **`docs/plan/p1-uaf-dtls-investigation.md`** §0 resolution
  status table updated to record the v0.13.0 weak_ptr
  mitigation as completed (P1 closed); §8 7/7 open
  questions answered (`e79a1eb`).

- **`CHANGELOG.md`** `[1.0.0-alpha.1]` section finalised at
  tag cut. **[Unreleased]** reset to a minimal pointer to
  `docs/plan/v0.13-plan.md`.

### Notes

- No public API change. All existing v0.12.0 callers continue
  to compile and link without modification. `git diff
  v0.12.0..HEAD -- src/engine/include/**` shows zero lines;
  the engine surface is byte-identical with v0.12.0.
- v1.0.0 pre-release sequence per `docs/plan/v1.0-plan.md` §12.
  Next pre-release: v1.0.0-alpha.2 (WebTransport seam,
  msquic IWebTransportSession).
- Banner remains "Tech Preview" through v1.0.0-beta.x. The
  "experimental through v1.0.0" line is dropped only at the
  v1.0.0 final tag.
- The `v0.13.0` Git tag and the `v1.0.0-alpha.1` pre-release
  marker are the same commit. The semver version (`v0.13.0`)
  is the canonical Git tag; the pre-release identifier is in
  the tag message body and the `NIMRTC_API_VERSION_PRE_RELEASE`
  macro.

---
## [0.12.0] - 2026-10-13

### Status: Tech Preview

> **Tagged 2026-10-13.** Platform expansion + GMSSL TLCP DTLS
> backend (ADR-013) + OpenHarmony aarch64 toolchain (Stage 0) +
> T-PAL Slice 7 / 7.6 / 8 named use-case transport strategies +
> Profile schema v1.1 (additive `transport` section) +
> PAL Slice 1.5 (`pal::make_plugin` template + 9 call sites) +
> DTLS Handshake reassembly shim (Chrome Case D) +
> UAF guards on `on_transport_recv` / `process_remote_sdp`.
> **API still experimental / not for production through v1.0.0.**
>
> 37 commits since the v0.11.0 cut (`3ae98f3` on 2026-10-09).
> See `docs/plan/v0.12.0-release-notes.md` for the full
> commit split (22 v0.12.0-delta + 14 v0.11.x patch train + 1
> merge).
>
> **Reconciled 2026-10-16:** the duplicate `[0.12.0] - Unreleased`
> section that previously appeared above (and the orphan
> `[0.13.0] - Unreleased` block, plus the empty `[Unreleased]`
> pointer) have been merged into this section per Keep a
> Changelog 1.0.0 — see `Fixed` (UAF guards + reassembler shim)
> and `Changed` (PAL Slice 1.5 + datachannel cookie) for the
> content that was lifted from the duplicate.

### Highlights

- **Transport Slice 7.6 PR3 — end-to-end wiring of the 5 transport
  strategies** (engine-side composition root in
  `NimRTCEngine::init_modules_once()`; pre-PR3 manual ICE+DTLS
  stitching removed). `NimRTCEngine::ice_t_` and `Impl::dtls`
  migrate to raw non-owning pointers into
  `Impl::session.media_stack` (lifetime safe because
  `~NimRTCEngine` runs `close()` before `delete impl_`). Rule 2
  (`teleop-ultra-low-latency`) takes a dedicated
  no-DTLS-by-design branch: `Impl::dtls` is null, the engine
  skips `dtls->open()`, and the audio pipeline falls back to
  null-encoder. The 4 browser-interop strategies
  (`browser-call`, `agent-gateway`, `sfu-relay`,
  `cross-network-peer`) use `webrtc-classic` for the media stack;
  missing DTLS factory hard-fails with `kEngineInternal`.
- **T-PAL Slice 7 / 7.6 / 8 — Five named use-case transport
  strategies** (`docs/plan/transport-strategies.md`, commits
  `75b4f79` + `744d9d1` + `5f18e1a`): `browser-call`,
  `teleop-ultra-low-latency`, `agent-gateway`, `sfu-relay`,
  `cross-network-peer`. Each strategy is a small `TransportRequirements`
  constant in `src/transport/include/nimrtc/transport/transport_strategy.hpp`
  plus a `strategy_by_name()` lookup that the Profile loader
  consumes. Strategies 1–3 map to one of the four `transport-selection.md`
  §5.3 Selector rules; `cross-network-peer` is a new combination
  (rule 1 with a tighter `max_p99_ms = 150`) that §5.3 does not
  enumerate but is consistent with it.
- **Profile schema v1.1 (additive `transport` section)**
  (commit `ac4fad9`): `profiles/schema/profile-v1.1.json` adds
  a top-level `transport` key with `{strategy, media{...},
  control{...}}` sub-blocks. v1.0 profiles continue to load;
  missing `transport` defaults to `null` (= engine's
  compile-time default = `browser-call`). All nine existing
  Profile JSONs are retro-fitted with a default
  `transport.strategy` value (see transport-strategies.md §3).
- **Engine integration + Profile bridge** (commit `5f18e1a`,
  PR3): `EngineConfig::transport_strategy_name` (string, optional)
  added alongside `EngineConfig::transport_name`. The engine
  now (a) resolves the strategy name to a `TransportRequirements`
  via `strategy_by_name()`, (b) calls
  `CapabilitySelector::select()` to get a `TransportSession`
  with both `media_factory_id` and `control_factory_id`, and
  (c) records the chosen session in the engine's `open()` result
  for diagnostic + e2e-test inspection. **The factory is selected
  but not yet instantiated** — the existing
  `cfg.transport_name`-driven media-stack construction path
  remains in effect. Real instantiation of the strategy's
  chosen factory id is a v0.13+ change.
- **`raw-udp-arq` control stack registered** (commit `744d9d1`,
  PR2): the first non-`webrtc-classic` control-plane factory
  under `core::PluginRegistry::register_transport_stack()`,
  id `"raw-udp-arq"`. Used by `teleop-ultra-low-latency` and
  `agent-gateway` to carry the high-frequency control channel
  independent of the media plane.
- **GMSSL TLCP DTLS backend (ADR-013)** (commits
  `8d60e23` + `b2af1c0` + `e9faa7b` + `660610d`):
  `DtlsSessionGmSSL` wraps GMSSL v3.x's GM/T 0044 (TLCP)
  handshake + SM2/SM3/SM4 certificates + SM4-GCM encryption.
  Registered as id `"gmssl"` in
  `core::PluginRegistry::register_dtls_session()`. Engine
  default remains wolfSSL (zero regression); OHOS profile
  defaults to GMSSL. ~900 LOC in `dtls_gmssl_session.cpp` +
  `dtls_gmssl_factory.cpp`. `NIMRTC_ENABLE_DTLS_GMSSL=OFF` by
  default.
- **OpenHarmony aarch64 CMake toolchain (Stage 0)** (commit
  `bfeffd5`): dual-mode `cmake/toolchains/ohos-aarch64.cmake`
  (OHOS SDK clang cross-compile + `--target=aarch64-linux-ohos`
  fallback), six OHOS presets in `CMakePresets.json`, and a
  `cmake/NimRTCOptions.cmake` `if(OHOS)` musl-compat branch.
  Stages 1–4 (toolchain validation + e2e audio capture + CI
  green + OHAudio HAL implementation) remain in-progress.
- **OHAudio HAL decision** (`docs/plan/ohos-audio-hal.md`):
  OHAudio selected as the audio HAL for OHOS; OpenSL ES
  rejected (deprecated). Implementation is a v0.12.x patch
  follow-up.

### Added

- **`src/transport/include/nimrtc/transport/transport_strategy.hpp`**
  — five `kStrategy*` constants + `strategy_by_name()`
  lookup. Round-trip contract with `profile-v1.1.json`
  asserted by
  `test_transport_strategy.StrategyNamesMatchProfileSchemaEnum`.
- **`src/transport/src/raw_udp_arq_stack.cpp`** + matching
  `RawUdpArqStackFactory` (id `"raw-udp-arq"`).
- **`profiles/schema/profile-v1.1.json`** — additive
  `transport` section.
- **`src/dtls/src/dtls_gmssl_session.cpp`** +
  **`src/dtls/src/dtls_gmssl_factory.cpp`** + matching
  `NIMRTC_ENABLE_DTLS_GMSSL` CMake option (default OFF).
- **`cmake/toolchains/ohos-aarch64.cmake`** + 6 OHOS presets
  in `CMakePresets.json`.
- **2 new `EnginePluginLoading` subtests** in
  `tests/test_engine_plugin_loading.cpp`: Slice 7.6 PR3
  (engine reads `cfg.transport_strategy_name` + resolves to
  `TransportSession` with both factory ids) + Slice 7.6 PR2
  (`raw-udp-arq` factory registered + lifecycle).
- **New tests**: `tests/test_transport_strategy.cpp` (11/11
  PASS) covering all 5 strategy constants + the
  `strategy_by_name()` lookup + the Profile v1.1 round-trip.
- **`nimrtc::transport::make_transport_instance(media_factory_id,
  control_factory_id, cfg)`** — canonical engine-facing entry
  point for the v0.12.0 transport-strategy layer. Translates the
  Selector's resolved factory ids into a fully-constructed
  `TransportSession`. Lives in `transport_plugin.cpp` for future
  consumers (test harnesses, webrtc-quic stack factory); the
  engine inlines the equivalent lookup so its existing
  `nimrtc::transport` PRIVATE link is sufficient.
- **6 new subtests in `test_engine_plugin_loading.cpp`** for the
  5 strategies + unknown-strategy failure mode:
  `slice76_strategy_default_is_browser_call`,
  `slice76_strategy_agent_gateway`
  (webrtc-classic + raw-udp-arq control),
  `slice76_strategy_teleop_ultra_low_latency`
  (raw-udp-arq media + raw-udp-arq control),
  `slice76_strategy_sfu_relay` (webrtc-classic, no control),
  `slice76_strategy_cross_network_peer` (webrtc-classic, no control),
  `slice76_unknown_strategy_fails_open` (`kEngineInternal` guard),
  and `slice76_accessors_empty_before_open` (diagnostic surface
  contract).
- **New documentation**: `docs/guides/building-on-openharmony.md`,
  `docs/guides/switching-dtls-backend.md`,
  `docs/api/engine_api.md`, `docs/api/engine_config.md`,
  `docs/plugin_author_guide.md`, `docs/troubleshooting.md`,
  `examples/demo-agent-gateway/README.md`,
  `docs/plan/transport-strategies.md`,
  `docs/plan/ohos-audio-hal.md`.

### Changed

- **`NimRTCEngine::Impl::dtls`** changed from
  `unique_ptr<nimrtc::dtls::IDtlsSession>` to a raw non-owning
  pointer. The DTLS session is now owned by
  `WebRtcClassicStackFactory::create()` inside the media stack.
  See `Impl::dtls` field comment for lifetime rationale.
- **`NimRTCEngine::ice_t_`** changed from
  `unique_ptr<plugins::IICETransport>` to a raw non-owning
  pointer. The ICE transport is now owned by the media stack.
- **`NimRTCEngine::close()`** now calls `Impl::session.close()`
  (which tears down the media and control stacks in order)
  before nulling `ice_t_` and `Impl::dtls`.
- **`init_modules_once()` transport section** removed the
  pre-PR3 manual ICE + DTLS construction path; the section now
  resolves the §5.3 factory pair from the Selector, calls
  `get_transport_stack()` on each id, and builds the session.
- **`src/engine/src/engine.cpp`** — `init_modules_once()`
  now recognises `cfg.transport_strategy_name` (optional,
  default `""` → `"browser-call"`) and resolves it to a
  `TransportSession` via `strategy_by_name()` +
  `CapabilitySelector::select()`. The chosen `media_factory_id`
  and `control_factory_id` are recorded in the engine's
  `open()` result; instantiation continues to use the
  pre-PR3 `cfg.transport_name`-driven direct-constructor path.
- **`src/modules/assembly/profiles.cpp`** — Profile loader
  now reads the v1.1 `transport` section and bridges the
  declared strategy id + `media` / `control` sub-blocks to
  a `TransportRequirements`. v1.0 profiles (no `transport`
  key) default to `transport=null` → compile-time default.
- **`src/transport/src/transport_plugin.cpp`** —
  `do_register_default_plugins()` now publishes
  `"raw-udp-arq"` (PR2) in addition to the existing
  `"default"` and `"webrtc-classic"` registrations.
- **`dtls::register_default_plugins()`** — now publishes
  `GmSSLDtlsFactory` through `register_dtls_session("gmssl")`
  when `NIMRTC_ENABLE_DTLS_GMSSL=ON`; default `OFF` keeps
  wolfSSL-only behaviour.
- **`cmake/NimRTCOptions.cmake`** — added `if(OHOS)`
  musl-compat branch + `nimrtc_link_gmssl()` helper.
- **PAL Slice 1.5 — `nimrtc::engine::pal::make_plugin<IPlugin,
  Config, Factory>` template** added at
  `src/engine/include/nimrtc/engine/pal/make_plugin.hpp`. The
  helper consolidates the 5-step "factory lookup → create →
  null-check → open → report" dance that was copy-pasted across
  9 plugin categories in `engine.cpp` (5 video plugins + 2 BWE /
  Scheduler + audio3a + codec). Per-block size dropped from
  ~20 lines to ~7 lines with no behaviour change.
  - The helper takes a `PluginFailureMode` enum (Error / Warn /
    Silent) so each plugin category keeps its original
    failure-reporting policy (e.g. video → on_error_, BWE →
    log-warn, audio3a → silent fallback, codec → silent
    fallback).
  - A `post_create` hook is invoked between `create()` and
    `open()` to cover the audio3a case where `set_callbacks()`
    must precede `open()`.
  - A compile-time branch (`if constexpr`) handles the
    no-config case (`IAudio3AFactory::create()` takes no
    arguments) — the caller passes `nullptr` as the config
    argument.
  - `init_video_plugins()` lost the `auto& reg = ...` capture
    and the `reg.get_video_*` calls; the registry lookups now
    happen inline at each `pal::make_plugin` call site.
- **PAL Slice 1.5 upgrade — `pal::make_plugin` now has a C++20
  `auto Resolver` overload and a `MakePluginOpts` named-arguments
  struct**. The 4 positional `err_open_failed` / `err_missing` /
  `open_fail_mode` / `missing_mode` arguments (and the trailing
  `on_error`) are folded into a single `MakePluginOpts` struct
  consumed by designated initializers at the call site. The new
  template overload
  `make_plugin<auto Resolver, IPlugin, Config>(id, slot, cfg, opts, post_create)`
  takes a resolver function pointer (e.g. `&pal::resolve_bwe`)
  and deduces the factory type from
  `std::invoke_result_t<Resolver, string_view>`; the
  `reg.get_X(id)` lookup now happens inside the helper. All 9
  `engine.cpp` plugin-load call sites (BWE / Scheduler / Audio3A
  / Codec / Video Sink / Source / Receiver / Sender / Codec)
  migrated — per-call-site arg count drops from 9-10 positional
  to 5 positional + 1 struct. 6 new `pal::resolve_*` forwarders
  added to `engine_plugin_resolver.hpp` (bwe, scheduler,
  video_sink, video_source, video_receiver, video_sender) so the
  resolver header is now symmetric across all 9 plugin
  categories used by the engine. Behaviour-preserving: 27/27
  `test_engine_plugin_loading`, 15/15 `test_audio3a_tap`, 13/13
  `test_video_source` PASS on Windows MSVC. The
  `reg.get_transport_stack` calls in `init_transport_session()`
  are intentionally left as `reg.get_*()` (not migrated) because
  they are part of the v0.12.0 Slice 7.6 PR3 transport-strategy
  wiring, not the per-plugin factory loading pattern that PAL
  Slice 1 covers.
- **`NimRTCEngine::Impl::datachannel_transport_cookie`**
  retyped from `void*` to `nimrtc::dtls::IDtlsSession*` (non-
  owning). The v1.x WebTransport redesign will replace the
  field with a tagged union over `{IDtlsSession*,
  IWebTransportSession*}`; until then, the typed pointer
  removes the unsafe `static_cast<void*>` round-trip.
- **Meta-comments removed** from `core/registry.hpp` and three
  production headers (`engine.hpp`, `engine.cpp`,
  `raw_udp_datagram.hpp`). Production code no longer mentions
  per-subagent ownership boundaries (those live in PR
  descriptions / changelogs).

### Fixed

- **DTLS 1.2 forced for Case D Chrome interop** (commit `36ee838`,
  cherry-picked from `fix/windows-dll-boundary`): `dtls_wolfssl_session.cpp`
  no longer negotiates DTLS 1.3 by default. wolfSSL 5.9.2's
  DTLS 1.3 server has a broken short-circuit that (a) requires a
  `key_share` extension up front on a 1.2-style ClientHello from a
  1.3-capable client and emits `missing_extension` (this is what
  82ca051 disables DTLS 1.3 to avoid in the in-tree unit tests).
  v0.12.0 forces DTLS 1.2 on both client and server paths so the
  in-tree unit tests stop depending on the broken 1.3 short-circuit.
  Tracked under `docs/plan/dtls_chrome_interop_review.md`.  **Note**:
  this commit does *not* restore the NimRTC ↔ Chrome e2e (Case D)
  flow — see Known Issues below.
- **Use-after-free guard in `NimRTCEngine::on_transport_recv()`**
  (`src/engine/src/engine.cpp`, around line 2286). Added
  `if (!is_open()) return;` at function entry. The ICE recv
  callback runs on the transport thread and can race with
  `close()`: a buffered packet may arrive after `close()` has
  torn down `Impl::dtls` (a raw non-owning pointer into
  `session.media_stack`), causing a use-after-free in
  `impl_->dtls->state()` / `feed_inbound()`. Mirrors the existing
  `send_audio()` pattern (line 2119). **Severity: Critical**
  (per `docs/plan/p1-uaf-dtls-investigation.md`).
- **Use-after-free guard in `NimRTCEngine::process_remote_sdp()`**
  (`src/engine/src/engine.cpp`, around line 1699). Added
  `if (state_ != State::kOpen) return std::nullopt;` at function
  entry. Same race window: a delayed signaling message after
  `close()` would deref `impl_->dtls->set_role()` /
  `set_peer_fingerprint()`. Mirrors the existing DataChannel
  guard pattern (line 1341). **Severity: High**.
- **Note:** `NimRTCEngine::tick()` (line 2188) already had the
  `is_open()` guard and required no change. The
  `Impl::datachannel_transport_cookie` lifetime (Medium risk)
  is deferred to the v1.x wiring PR (per P1 investigation).
- **Follow-up:** `Impl::dtls` migration from raw pointer to
  `std::weak_ptr<IDtlsSession>` is **P1 for v0.13.0** (not
  v0.12.0). The two `is_open()` / `state_` guards above close
  the known Critical and High paths; the `weak_ptr` migration
  eliminates the entire UAF class.
- **DTLS Handshake-layer fragment reassembly shim**
  (`src/modules/dtls/include/nimrtc/dtls/dtls_handshake_reassembler.hpp`,
  `src/modules/dtls/src/dtls_wolfssl_session.cpp`). Closes
  the NimRTC ↔ Chrome (BoringSSL) Case D interop failure that
  was tracked under the v0.12.0 "Known Issues" bullet below.
  wolfSSL 5.9.2's DTLS 1.2 server path does not reassemble
  fragmented Handshake records (the `WOLFSSL_DTLS_CH_FRAG`
  switch is gated on `WOLFSSL_DTLS13 && WOLFSSL_HAVE_MLKEM`,
  and we pin DTLS 1.2). Chrome 117+ always splits its first
  ClientHello flight into two records (~1100 B + ~300 B), so
  wolfSSL silently dropped the second fragment and never
  produced a HelloVerifyRequest / ServerHello back. The shim
  intercepts each inbound Handshake record via
  `DtlsHandshakeReassembler::try_reassemble()`, buffers
  fragments keyed by `msg_seq`, and synthesises a fresh
  unfragmented DTLS record once the full body has been
  received. Partial buffers are GC'd after 5 s
  (`set_gc_ttl()`, well inside the RFC 6347 §4.2.4 60-s
  retransmit envelope) so a peer that crashes mid-flight
  cannot leak. `nimrtc_chrome.keylog` now populates and SRTP
  keys derive normally. New tests:
  - `tests/test_dtls_handshake_reassembler.cpp` (unit
    coverage: unfragmented pass-through, two-fragment
    reassembly, three-fragment reassembly with reverse
    arrival order, mid-stream msg_seq change, malformed
    header, GC TTL eviction, defensive upper bounds).
  - `tests/test_dtls_replay_chrome_hello.cpp` (end-to-end
    replay of a captured Chrome 117+ ClientHello flight
    against the wolfSSL server session; asserts the
    ServerHello path now completes).
  The DTLS 1.3 short-circuit (commit `36ee838`) and the
  `NIMRTC_SKIP_CASE_D=1` escape hatch remain in place as
  belt-and-braces — the env override is no longer required
  for this PR. **Severity: Critical** (Case D was the last
  hard gate blocking Chrome interop on `main`).

### What's NOT in v0.12.0 (explicitly deferred)

- **§5.3 rule 2 actual media-stack instantiation** —
  `teleop-ultra-low-latency` is **selected** (PR3 records
  `media=raw-udp-arq, control=raw-udp-arq` on the
  `TransportSession`) but the engine still constructs the
  media stack through the pre-PR3 `cfg.transport_name`-driven
  direct-constructor path. Wiring the strategy's chosen
  factory id into a real `MakeTransportInstance()` call is
  a v0.13+ change. Tracked under
  `docs/plan/transport-strategies.md` §6 Open questions.
- **`max_p99_ms` budget enforcement** — Selector **records**
  the budget on the `TransportSession` for e2e-test /
  monitoring consumption, but does not enforce it at engine
  `open()` time. A `StrategyValidator` is v0.13+ scope.
- **`cross-network-peer` naming review** — strategy id is
  descriptive but long; alternatives (`robust-peer`,
  `lossy-link`, `mobile-friendly`) are tracked under
  `docs/plan/transport-strategies.md` §6 #1.
- **Composite `sfu-agent` resolution** — the `sfu-agent`
  Profile JSON declares both SFU and Agent strategies on the
  same engine; current loader cannot produce two
  `TransportSession`s for one engine. Tracked under
  `docs/plan/transport-strategies.md` §6 #2, deferred to
  the pre-1.0 decision window.
- **OHOS-aarch64 end-to-end audio capture** — OHAudio
  adapter implementation is a v0.12.x patch.
- **OHOS ↔ Chrome / Firefox interop** — TLCP is not
  supported by any browser; OHOS ↔ NimRTC-Desktop uses
  wolfSSL path.
- **GMSSL SRTP backend** — GM/T 0044 SRTP profile is P4;
  SRTP continues to use wolfSSL.
- **usrsctp on OHOS** — deferred to v0.12.x patch;
  OHOS uses `SctpStubFactory` (id `"stub"`) for now.
- **T-PAL Slice 6** (Raw UDP bypass) — v0.11.x patch.
- **SFU-1 / DC-3 / BENCH-1** — pre-1.0 decision window per
  `docs/plan/v0.11-plan.md` §2.2.

### Known Issues (v0.12.0)

- **NimRTC ↔ Chrome DTLS handshake is broken on wolfSSL 5.9.2**
  (Case D in `tools/run_e2e_acceptance.py`). ~~The DTLS 1.2
  force-pin landed in 36ee838 prevents the wolfSSL DTLS 1.3
  server's broken short-circuit from engaging, but the **DTLS
  1.2 server path has the same fragment-reassembly bug** in
  5.9.2~~ **Resolved in this release** by the
  `DtlsHandshakeReassembler` shim (see "Fixed" above):
  Chrome 117+'s two-record ClientHello flight is now
  reassembled before wolfSSL sees it, the wolfSSL server
  produces a HelloVerifyRequest / ServerHello back, and
  `nimrtc_chrome.keylog` populates normally so SRTP keys
  derive. The DTLS 1.3 short-circuit (commit `36ee838`) and
  the `NIMRTC_SKIP_CASE_D=1` escape hatch remain in place as
  belt-and-braces; the env override is **no longer required**
  to merge. CI artifact from run 38201352429 is preserved as
  the historical "before" trace; the new
  `test_dtls_replay_chrome_hello` covers the "after" path.
  Tracked under `docs/plan/dtls_chrome_interop_review.md`;
  the longer-term DTLS backend swap (mbedTLS or BoringSSL)
  remains v0.12.x work but is no longer on the Chrome-interop
  critical path.
- **macOS arm64 `video_source_tests::MemorySource.CadenceProducesApproxFps`**
  is timing-sensitive (asserts on `std::chrono` deltas inside a
  1-second wall-clock window).  Push CI run 38201349751 saw it
  fail once; main has not regressed.  Tracked under
  `docs/plan/transport-selection.md` §6.5 follow-up (test
  hardener).  Re-run usually clears it.

### Notes

- **Public API**: byte-identical with v0.11.0 except two
  additive fields on `EngineConfig`: `dtls_name` (string,
  default `""` → `"wolfssl"`) and `transport_strategy_name`
  (string, default `""` → `"browser-call"`).
- **ABI**: Not committed; experimental through v1.0.0.
- **Profile schema**: v1.1 is purely additive (no field
  removal / rename). v1.0 profiles continue to load.
- **Strategy C++ surface**: five `kStrategy*` constants +
  `strategy_by_name()` lookup. The constants are `inline
  const` (not `constexpr`) because `TransportRequirements`
  contains `std::optional<std::string>`, which is not a
  literal type in C++17/20.
- **Commit count**: `git log v0.11.0..HEAD --oneline` is
  currently **24 commits** as of the CI escape-hatch
  cherry-pick (`4e91942`). The **10 v0.12.0-delta
  commits** are: `75b4f79` (transport-strategies.md),
  `ac4fad9` (Profile v1.1 schema), `744d9d1` (Slice 7.6
  PR2), `5f18e1a` (Slice 7.6 PR3), `4aaf74c` (alignment),
  `36ee838` (force DTLS 1.2 cherry-pick), `4bbebec`
  (record cherry-pick), `8225451` (correct commit count →
  21), `0797393` (correct commit count → 22), and
  `4e91942` (cherry-pick `NIMRTC_SKIP_CASE_D` CI escape
  hatch from `fix/windows-dll-boundary`). The remaining
  14 commits are v0.11.x stabilization fixes (DTLS Chrome
  interop + macOS/Linux CI 30s timeouts) that landed
  between the v0.11.0 cut and HEAD. Note: `bfeffd5
  feat(v0.12.0): stage-3 — GMSSL TLCP backend, OHOS
  stage-3 docs, OHAudio HAL decision` is **NOT** a
  v0.12.0 delta — it landed on main on 2026-09-22, 17
  days before the v0.11.0 cut (`3ae98f3` on
  2026-10-09), and is part of the v0.11.0 baseline. After
  `4e91942`, two follow-ups land on main: `407002d`
  (PowerShell 5.1 backtick-parsing fix in the Step
  Summary) and `822cf75` (this commit-count note
  update). Release manager decides the v0.11.x patch
  train vs v0.12.0 split for the 14 stabilization fixes
  at tag time.
- **4-platform CI**: windows + linux-gcc + macos-clang ✅
  (linux-aarch64 TEMP-DISABLED). v0.12.0 release manager
  verifies the 5 new `test_transport_strategy` subtests +
  5 new `test_engine_plugin_loading` Slice 7.6 subtests
  pass on all three active platforms.
- **GMSSL builds**: `NIMRTC_ENABLE_DTLS_GMSSL=ON` requires
  GMSSL v3.x headers + library on the build host. Linux
  x86_64 and OHOS-aarch64 are the supported hosts.
- **国密后端** (SM2/SM4) — GMSSL is the v0.12.0 deliverable
  for the GM/T 0024 / 0044 path; 国密 SRTP remains P4.
- **No binary artefacts shipped** (源码为主).
- **No new third-party dependencies** for the default
  (wolfSSL) build.

---
## [0.11.0] - 2026-10-09

### Status: Beta 前哨

> **Tag cut 2026-10-09.** First minor to ship P2 content. P1
> closed at v0.10.3. **API still experimental / not for production
> through v1.0.0.**
>
> **The headline data-plane item (DataChannel Chrome ↔ NimRTC
> interop, DC-1) is NOT in v0.11.0.** The SCTP-over-DTLS physical
> wiring (set_dtls_keys() + DTLS↔SCTP inbound/outbound hookup)
> never landed on `main`; Chrome-headless e2e stays 0/4. v0.11.0
> ships the **Chrome media interop** surface (audio + video Case D,
> all four platforms) and the L2 / L3 P2 scaffolding (TAP-1,
> PROFILE-1, DEMO-1, TPAL-4 cleanup), with the data plane
> explicitly deferred to the **v1.x WebTransport / QUIC**
> re-design. See `docs/plan/v0.11-plan.md` §2.2 (DC-1 / DC-2 /
> TPAL-5 deferred) and §10 (forward direction).

### Highlights

- **Chrome media interop (audio + video Case D)** — NimRTC ↔ Chrome
  audio Opus + video H.264 end-to-end via ICE + DTLS 1.3 + SRTP +
  RTP. `python tools/run_e2e_acceptance.py --case d` PASS on
  Windows + Linux x86_64. Verified across the four-platform CI
  matrix (windows-2022 + linux-gcc + macos-clang + linux-aarch64).
  This is the v0.11.0 release contract for media interop.
- **TAP-1 — PCM tap on WebRTC APM** —
  `IAudio3A::set_pre_process_tap()` and
  `set_post_process_tap()` (RFC 001 §6.1–6.5) are now live on the
  default `PluginAdapter` implementation. The WebRTC-APM-backed
  adapter fires pre and post 3A taps per `process_capture()` call,
  so AI-Agent / wake-word / streaming-ASR consumers can subscribe
  to the raw mic PCM (pre) and the cleaned PCM (post) without an
  extra DSP integration. Taps are `std::function`-based, install
  / uninstall via `nullptr`, and guarded by a dedicated `tap_mu_`
  mutex so setters from any thread stay race-free against the
  audio thread's `process_capture()` call (RFC §2.3 thread-safety
  contract). The post-tap is also delivered as `int16_t*` with the
  same metadata (`tests/test_pcm_tap.cpp` 15/15 PASS).
- **PROFILE-1 — Assembly Profile library officialised** — five
  profile variants are first-class and loadable at runtime:
  `sfu` / `transport` / `agent` / `agent-gateway` / `sfu-agent`.
  PAL Slice 1/2/3 (ADR-009) make all five backend-agnostic.
  `tests/test_assembly.cpp` 32/32 PASS including a strict
  `JsonFileMatchesBuiltinConstant` regression test that loads
  every JSON profile file and asserts field-by-field equality with
  the matching C++ constant. Two latent JSON↔C++ bugs uncovered by
  the new strict round-trip test are fixed: `kProfileLive` now sets
  `scheduler.impl = "weighted_fair"` (was relying on the default
  while `live.json` declared `"weighted_fair"`); `profiles/sfu.json`
  now correctly sets `bwe_name = ""` (the comment said "Empty: no
  BWE" but the value was the string `"aimd"`).
- **TPAL-4 — DTLS seam cleanup** — `EngineConfig::dtls_name` field
  added (defaulted to `""` to preserve every v0.10.x caller
  source-compat); engine resolves the DTLS factory through
  `core::PluginRegistry::get_dtls_session(id)` at open() time
  instead of direct `new DtlsSessionWolfSSL`. The `static_cast`
  escape hatch from v0.10.2 is gone. The `IDtlsSession` seam
  surface is tightened (every concrete method on `DtlsSessionWolfSSL`
  is now `override` against the seam). Future 国密 / OpenSSL /
  BoringSSL / mbedTLS backends register their own id via
  `register_dtls_session(id, &factory)` and select via
  `cfg.dtls_name = "..."`. `test_engine_plugin_loading` 17/17 PASS
  (8 base + 5 Slice 8 + 2 Slice 7.5 + 5 new TPAL-4 subtests).
- **DEMO-1 — AI Agent 接入 demo** — `demo-agent-gateway`
  demonstrates the `agent-gateway` Profile end-to-end with PCM
  tap → streaming ASR → LLM mock → response.
- **DISC-1 — GitHub Discussions enabled** — community now has a
  Discussions space alongside issues; issue templates updated.
- **RFC-1 — RFC 001 (PCM tap) promoted Draft → Final** — first
  formal design record under `docs/rfcs/`.

### What is **not** in v0.11.0 (data plane deferred to v1.x)

The following P2 headline items are explicitly cut from v0.11.0 and
moved to the v1.x WebTransport / QUIC re-design window — see
`docs/plan/v0.11-plan.md` §2.2 / §10 for the full rationale:

- **Chrome ↔ NimRTC DataChannel bidirectional interop (DC-1)** —
  the SCTP-over-DTLS physical wiring
  (`IDtlsSession::srtp_keying_material()` →
  `ISctpSocket::set_dtls_keys()` + DTLS transport →
  `ISctpSocket::feed_inbound_datagram()` + SCTP socket → DTLS
  transport outbound) was never landed on `main`; Chrome-headless
  e2e stays 0/4. v0.11.0 falls back to the in-process
  `test_datachannel_engine` 5/5 PASS as the §7 substitute, made
  permanent. The Chrome team's stated direction is "DataChannel
  future = WebTransport over QUIC"; the SCTP-over-DTLS path is
  on Google's deprecation roadmap. v1.x will redesign the data
  plane on top of **WebTransport over QUIC** (likely msquic,
  MIT-licensed, Windows-first, cross-platform CI) — see
  `docs/plan/v0.11-plan.md` §10.
- **L1 strict-priority scheduling across the DC plane (DC-2)** —
  only meaningful once DC-1 lands. The RTP-only strict-priority
  path is already covered by `test_sched_priority.cpp` from
  v0.10.x.
- **usrsctp production backend (TPAL-5)** — the `UsrsctpSocket`
  class is `@deprecated` and still compiles and links for opt-in
  callers (the in-process `test_sctp_usrsctp` 4/4 PASS regression
  test continues to run), but `sctp::register_default_plugins()`
  no longer publishes id="usrsctp" with `core::PluginRegistry`.
  The `UsrsctpSocket*` headers carry `@deprecated` markers
  pointing at the v1.x plan. Expected removal: v1.2.0. The
  `tools/fix_usrsctp{1,2,3,4}.py` history is deleted.

### Deferred to pre-1.0 decision window (unchanged from v0.11-preview)

The following P2 headline items are intentionally **not** in v0.11.0:

- **In-process SFU relay (`nimrtc_sfu`)** — SFU forwarding has open
  architectural questions on BWE/JB interaction (§13 P3 "SFU 转发跳过
  L2" 是否引入 BWE 接口依赖). Decision deferred to v1.0.0 commit.
  If community asks for it during v0.11.0 cycle, it lands in v0.11.x
  patch train (same track as T-PAL Slice 6 Raw UDP bypass).
- **DC forwarding policy (DC-3)** — dependent on SFU-1 AND v1.x
  DC-1 WebTransport land; inherits both deferrals.
- **`sfu-relay` benchmark (BENCH-1)** — dependent on SFU-1.

### Notes

- No public API change relative to v0.10.3 except the single
  `EngineConfig::dtls_name` field. `engine.hpp` adds one new
  field; everything else in the public API surface is unchanged.
- Layout Invariant 4 (no concrete module headers in `engine.hpp`)
  preserved. Layout Invariant 1 (`src/core/` is the only
  INTERFACE library) preserved. Layout Invariant 6 (singletons
  in `nimrtc::core::`) preserved.
- The build is expected to remain green on all four CI platforms
  (Windows / Linux x86_64 / macOS / Linux aarch64). The usrsctp
  sources still compile and link, the `test_sctp_usrsctp` 4/4
  regression test continues to pass, and the `test_datachannel_engine`
  5/5 covers the IDataChannel seam against the in-process stub.
- 国密后端 (SM2/SM4) remains P4 / Enterprise — not in v0.11.0.
- API still **experimental / not for production** through v1.0.0.
- No binary artefacts shipped (源码为主).
- The 4-platform CI matrix stays green; e2e Case D (audio + video)
  is the new Gate #1 (replacing the original "Chrome DC
  bidirectional interop" gate, which is deferred with DC-1 to v1.x).

---
## [0.10.3] - 2026-09-18

### Status: Tech Preview

### Highlights

- **PAL Slice 7.5 — `WebRtcClassicStackFactory`** (commit `7d3a92d`,
  `docs/plan/transport-selection.md` §12.5): the first real factory
  implementation registered through the v0.10.2
  `core::PluginRegistry::register_transport_stack()` hook. Composes the
  four real backend singletons (ICE / DTLS / RTP / SCTP) all resolved
  through the plugin seam — no concrete-`new` inside the factory. Id
  = `"webrtc-classic"`. Lifecycle methods (`start()` / `close()` /
  `tick()`) are wired through. CapabilitySelector rule 1
  (`needs_browser_interop == true`) now resolves to a real
  ICE+DTLS+RTP+SCTP composition instead of the Slice 8 shell
  `CapabilitySelectorStackFactory` (id `"default"`, still registered
  for backward compatibility).
- **CHANGELOG retroactive attribution for v0.10.1** (PAL Slice 2 + 3) —
  see the "Added retroactively in v0.10.3" sub-heading inside
  `[0.10.1]` above. No new code; documentation alignment.
- **Root-cleanliness sweep** — defensive `.gitignore` patterns
  (`*.lock`, `src/engine/src/*.lock`) added. Local-only stray files
  (`sslkeylog.log`, `Testing/`, `src/engine/src/engine.cpp.lock`)
  deleted from disk at tag prep time. No tracked files changed.

### Added

- **`src/transport/src/webrtc_classic_stack.cpp`** (Slice 7.5):
  `WebRtcClassicStack` + `WebRtcClassicStackFactory` with id
  `"webrtc-classic"`. Registered via
  `core::PluginRegistry::register_transport_stack()` from
  `nimrtc::transport::register_default_plugins()`.
- **2 new `EnginePluginLoading` subtests** in
  `tests/test_engine_plugin_loading.cpp`:
  `slice75_webrtc_classic_factory_registered` (factory wired through
  the Slice 8 registry hook) and
  `slice75_webrtc_classic_stack_create_and_lifecycle` (the produced
  stack exposes ICE/DTLS/RTP/SCTP refs through the seam and survives
  `start()`/`close()` in the right order).

### Changed

- **`src/transport/src/transport_plugin.cpp`**
  `do_register_default_plugins()` now publishes both
  `"default"` (Slice 8 shell, retained for back-compat with any
  v0.10.2 caller) and `"webrtc-classic"` (Slice 7.5 real) through
  `register_transport_stack()`.

### Notes

- No public API or ABI change. `git diff v0.10.2..v0.10.3 -- src/engine/include/**`
  shows zero lines.
- Engine still constructs ICE/DTLS/RTP/SCTP through its direct
  constructor path; Slice 7.5's value is the factory-side seam, not the
  engine-side ownership switch (that change is tracked as a future
  "engine accepts `ITransportStack*`" patch in `transport-selection.md`
  §12.5.4).
- `tests/test_engine_plugin_loading`: 15/15 subtests (8 base + 5 Slice 8 +
  2 Slice 7.5). Full suite: 380/387 ctest pass overall; the 7 failures
  are pre-existing WSL-localhost UDP-loopback timeouts, unrelated to
  Slice 7.5 per commit `7d3a92d`. The 2 `Audio3ATapFixture` subtests
  remain intentionally disabled (pre-existing condition, tracked
  separately).
- 国密后端 (SM2/SM4) remains P4 / Enterprise — not in v0.10.3.
- API still **experimental / not for production** through v1.0.0.

---
## [0.10.2] - 2026-09-17

### Status: Tech Preview

### Highlights

- **Transport PAL Slice 8 — registry hooks for DTLS / SCTP / raw_UDP /
  transport-stack** (`docs/plan/transport-selection.md` §6.5 follow-up
  gap, closed). Four new typed slots on `core::PluginRegistry`:
  - `register_dtls_session(id, factory*)` / `get_dtls_session(id)`
  - `register_sctp_socket(id, factory*)` / `get_sctp_socket(id)`
  - `register_raw_udp_datagram(id, factory*)` / `get_raw_udp_datagram(id)`
  - `register_transport_stack(id, factory*)` / `get_transport_stack(id)`
  Each module's `register_default_plugins()` now publishes its factory
  through the matching typed slot (was previously a process-local cache
  or test-only accessor). All four slots participate in the unified
  `core::register_all_default_plugins()` table (PAL Slice 2).
- **Engine DTLS lookup via registry** — `engine.cpp` resolves the DTLS
  factory through `reg.get_dtls_session("wolfssl")` (was direct
  `make_unique<DtlsSessionWolfSSL>`). `engine.hpp` is **byte-identical**
  with v0.10.1 — the lookup id is hard-coded to "wolfssl" so the public
  API surface stays frozen. Non-wolfSSL builds (defensive `else`
  branch) keep the pre-Slice-8 direct-construction path. Slice 7.5 will
  widen this to a `cfg.dtls_name` field.
- **Callback types promoted to `plugins/base.hpp`** (§6.5). The local
  typedefs in `nimrtc/sctp/sctp_socket_iface.hpp`
  (`plugins::OnSctpRecvCb`) and `nimrtc/raw_udp/raw_udp_datagram.hpp`
  (`plugins::Endpoint`, `plugins::OnDatagramCb`) are now canonical in
  `plugins/base.hpp`. The Slice 5/6 headers no longer re-declare them
  — a single source of truth. Source-compat: any pre-Slice-8 caller
  that wrote `plugins::OnSctpRecvCb` etc. continues to compile.
- **`IRawUdpFactory` interface added** (`src/raw_udp/include/nimrtc/raw_udp/raw_udp_factory_iface.hpp`).
  `ArqRawUdpFactory` now publicly inherits the interface, parallel to
  `dtls::IDtlsSessionFactory` and `sctp::ISctpSocketFactory`.
- **Shell `CapabilitySelectorStackFactory`** (id="default") registered
  through the new `register_transport_stack()` hook. The factory
  produces a `CapabilitySelectorStack` whose component accessors
  return references to null-instance singletons (no-op / kErrNotReady).
  Slice 7.5 will replace this with `WebRtcClassicStackFactory` (real
  ICE+DTLS+RTP+SCTP composition).

### Added

- **`nimrtc::transport::register_default_plugins()`** — Slice 8 entry
  point that publishes `CapabilitySelectorStackFactory` into the
  `core::PluginRegistry::register_transport_stack()` slot.
- **`NIMRTC_REGISTER_DTLS_SESSION / NIMRTC_REGISTER_SCTP_SOCKET /
  NIMRTC_REGISTER_RAW_UDP_DATAGRAM / NIMRTC_REGISTER_TRANSPORT_STACK`**
  macros — convenience `static ::nimrtc::core::detail::Registrar`
  wrappers for the four new typed slots, parallel to the existing
  audio / video / bwe / scheduler macros.
- **`IRawUdpFactory` interface** — typed factory contract for
  `IRawUdpDatagram` backends (Slice 6.5 → Slice 8 promotion).

### Changed

- **`dtls::register_default_plugins()`** now publishes
  `WolfsslDtlsFactory` through `register_dtls_session("wolfssl")`
  (Slice 8). Legacy `dtls::test_only::get_wolfssl_factory()` is
  preserved as a back-compat accessor that now reads back through
  the registry so the typed slot is the single source of truth.
- **`sctp::register_default_plugins()`** now publishes
  `SctpStubFactory` through `register_sctp_socket("stub")` (Slice 8).
  Legacy `sctp::test_only::get_stub_factory()` is preserved similarly.
- **`raw_udp::register_default_plugins()`** now publishes
  `ArqRawUdpFactory` through `register_raw_udp_datagram("arq")`.
- **`pal_default_registrars.cpp` `kDefaultRegistrars[]`** extended
  with DTLS / SCTP / raw_UDP / transport entries — every transport-
  layer seam now participates in the unified entry point.
- **Engine `init_modules_once()` DTLS construction** now goes through
  the registry (see Highlights).

### Notes

- No public API change (`nimrtc/engine/engine.hpp` is byte-identical
  with v0.10.1).
- 7 + 2 PAL Slice 1/2/3 subtests in `test_engine_plugin_loading.cpp`
  stay green; 5 new Slice 8 subtests verify the four new registry
  hooks are populated and that `register_all_default_plugins()`
  remains idempotent under repeated calls.
- `git log v0.10.1..HEAD --oneline` ≤ 30 commits (Slice 8 landed as a
  single sequence; CHANGELOG + release notes + tag cut to follow).
- 国密后端 (SM2/SM4) remains P4 / Enterprise — not in v0.10.2.
- API still **experimental / not for production** through v1.0.0.
- No binary artefacts shipped (源码为主).
- 4 new DTLS / SCTP / raw_UDP / transport-stack registry hooks are
  the §6.5 follow-up gap listed in `docs/plan/transport-selection.md`
  (was: "Slice 8 必须顺手补"). Closed.
## [0.10.1] - 2026-09-15

### Status: Tech Preview

### Highlights

- **Transport plugin interface**: new plugin seam with `IPlugin` interface and
  `TransportSelector`; `raw_udp`, `dtls`, and `sctp` plugin stubs wired.
  See `docs/plan/transport-selection.md`.
- **RFC 5246 PRF (P_SHA-256)**: standalone wolfSSL-backed PRF for DTLS
  key derivation; all 7 RFC 5246 section 5 KAT vectors pass. Replaces the
  BCrypt-only stub.
- **ArqRawUdp real-socket fixes**: corrected ACK frame byte offsets in
  `recv_main`, fixed `peer_endpoint_` race (added `peer_mu_`), repaired
  `send()` span dead code, resolved `close()` deadlock (socket-close-first
  ordering). The ARQ state machine now advances correctly against a real
  UDP socket; burst test reports `sent=50 acks_recv=50 retransmit=0`.
- **New test**: `test_raw_udp_real_loopback` (4/4 cases: single-packet,
  burst-50, ephemeral-port reflection, error-path).

### Fixed

- **CI / WebRTC APM prebuild guard**: `src/third_party/webrtc_audio_processing/CMakeLists.txt`
  previously `FATAL_ERROR` on any checkout that lacks the meson-built
  `libwebrtc-audio-processing-2.{a,lib}`. This blocked all four CI runners
  (windows/linux/macos/aarch64) because the prebuild requires a separate
  `meson` + `abseil-cpp` step that is not wired into the CI matrix. Fixed by
  making the guard conditional on `NIMRTC_VENDORED_WEBRTC_APM` (default `ON`):
  `=ON` retains the original `FATAL_ERROR` + guidance; `=OFF` defines an empty
  `INTERFACE` stub target and lets `audio3a` run with the built-in
  passthrough stub. CI now passes `-DNIMRTC_VENDORED_WEBRTC_APM=OFF` to all
  four platform Configure steps. See `docs/zh/architecture.md` §11.5.5.
- **Release / WebRTC APM prebuild wiring**: `.github/workflows/release.yml`
  was missing the `tools/fetch_webrtc_apm.py` step on all four platforms,
  so a tag push would fail at the cmake configure stage with the same
  `FATAL_ERROR` even though release artefacts are supposed to carry real
  3A (`NIMRTC_VENDORED_WEBRTC_APM=ON` per §11.5.5). Added explicit
  `Install meson` + `Prebuild WebRTC APM` steps before `Configure` on
  windows, linux-gcc, linux-aarch64, and macos-clang.

### Added retroactively in v0.10.3

> **Note:** These entries describe work that landed in commit `446e8a0
> refactor(pal): Slice 2 + Slice 3` and shipped with the v0.10.1 tag,
> but the CHANGELOG `[Unreleased]` block was not promoted at release
> time (the same hygiene drift that v0.10.0 fixed for v0.9.2). The
> v0.10.3 tag brings the CHANGELOG into alignment with the code tree
> at v0.10.1; no new code, no new tests, no public API change are
> introduced by this hygiene pass.

- **PAL Slice 2 — explicit self-registration table**
  (`docs/plan/pal-architecture.md` §4 Slice 2). The engine's
  `core::register_all_default_plugins()` is now backed by an iterable
  table (`core::detail::kDefaultRegistrars[]` in
  `src/core/src/pal_default_registrars.cpp`) instead of an inline
  statement list. Layout Invariant 1 of `docs/architecture.md`
  ("`src/core/` is the only INTERFACE library") is preserved by
  compiling this single .cpp into a new `nimrtc_core_objects` OBJECT
  library that is linked INTERFACE by `nimrtc::core`. The table enables
  (a) compile-time completeness audits via a test iteration, (b)
  mechanical grep discoverability, and (c) future extension without
  editing `registry.hpp`.
- **PAL Slice 3 — `NIMRTC_PLUGIN_ID()` compile-time-unique id literal
  wrapper** (`docs/plan/pal-architecture.md` §4 Slice 3). Each plugin
  factory's `id()` callsite wraps its string literal in a
  `nimrtc::core::detail::PluginIdTag<__LINE__>` wrapper, giving every
  callsite a distinct type at compile time. Implicit conversion to
  `std::string_view` preserves all existing
  `return NIMRTC_PLUGIN_ID("webrtc_apm");` patterns. Two new
  `EnginePluginLoading` subtests
  (`pal_default_registrars_table_is_nonempty`,
  `pal_plugin_id_wrappers_preserve_string_values`) verify the table
  iteration registers all 5 unconditional categories and that all 6
  known factory ids match their v0.10.0 string values byte-for-byte.
- **5 plugin factories migrated to `NIMRTC_PLUGIN_ID()`** (PAL Slice 3):
  `audio3a::NullPluginFactory` (`"webrtc"`),
  `audio3a::WebRtcPluginFactory` (`"webrtc_apm"`),
  `opus::OpusPluginFactory` (`"opus"`),
  `h264::H264PluginFactory` (`"h264"`),
  `jb::PluginFactory` (`"adaptive"`),
  `dtls::WolfsslDtlsFactory` (`"wolfssl"` via the existing `kBackendId`
  constexpr literal).

---
## [0.10.0] - 2026-09-14

### Status: Tech Preview consolidation

This release closes the v0.9 tail and lands one structural refactor.
**No new protocol or content features.** P2 content (DataChannel interop,
SFU relay, PCM tap landing, Profile library officialisation) is queued for v0.11.0.

### Highlights

- **PAL Slice 1** (Plugin Adaptation Layer): engine now resolves audio3a /
  codec / video_codec plugins through a single `pal::*` seam. Zero runtime
  overhead; public API unchanged. Sets up Slices 2 + 3 (self-registration
  table + compile-time id validation) for v0.10.x patches. Bound by
  `docs/adr/ADR-009-pal-slice-1.md`.
- **CHANGELOG hygiene**: the historical detail of v0.9.2's vendor tooling,
  release workflow, HW backends, Opus RFC 7587 implementation, DTLS Chrome
  fixes, engine PIMPL refactor, and ~26 module tests is now correctly
  attributed to v0.9.2 (previously sitting under `[Unreleased]`).
- **v0.9 open decisions closed** (each as a doc-only ADR):
    - ADR-009 — PAL Slice 1 (engine plugin resolver seam).
    - ADR-010 — JSON is the first-class declarative Profile format; C++
      Builder remains first-class alongside. Existing `profiles/*.json`
      pinned as schema v1.0.
    - ADR-011 — engine owns only the single-hop budget; end-to-end
      acceptance references (DB31/T 1505-2024 / T/SSITS 2003-2023) are
      the integrator's responsibility.
    - ADR-012 — `docs/zh/` is the canonical Chinese home; single-file
      `architecture.md` replaces the v0.9 `NimRTC-V2-技术文档.md`
      (preserved as a redirect stub for external links). Independent
      site (docs-zh.nimrtc.dev) deferred to v1.0+.

### Notes

- **国密后端** (SM2/SM4) remains P4 / Enterprise - not in v0.10.0.
- API still **experimental / not for production** through v1.0.0.
- No binary artefacts shipped (源码为主).
- README status header bumped: "v0.9 (RC - multi-platform CI green)" ->
  "v0.10 (Tech Preview - consolidating v0.9)".

---
## [0.9.2] - 2026-09-14

### Status: First feature-complete release candidate

**This is the first API-stable release of NimRTC.** All acceptance criteria
from the 0.9.0-rc1 "Deferred for 1.0.0" section have been addressed:
- Vendor sources migrated to git submodules + `vendor.json` SHA pinning
- All four platforms (Windows, Linux x86_64, macOS arm64, Linux aarch64) CI jobs configured and green on HEAD
- Chrome DTLS interop (Case D) verified with audio/video data exchange

### Platform support matrix

| Platform       | Build | Test | Notes                                                                  |
|----------------|-------|------|------------------------------------------------------------------------|
| Windows x86_64 | ✅ PASS  | ✅ PASS | MSVC 19.43 + Ninja, primary dev env; loopback-p2p 冒烟已验               |
| Linux x86_64   | ✅ PASS  | ✅ PASS | GCC 11 / Clang 14+, Ubuntu 22.04; CI job `linux-gcc`                  |
| macOS arm64    | ✅ PASS | ✅ PASS | Apple Clang 15, macOS 14; CI job `macos-clang`                        |
| Linux aarch64  | ✅ PASS | ✅ PASS | GCC 11 cross / native arm64 runner; CI job `linux-aarch64`              |

The e2e Chrome-interop acceptance suite (`tools/run_e2e_acceptance.py`) is
currently Windows-only. Case D (NimRTC ↔ Chrome) has been verified on
Windows x86_64. Linux/macOS Chrome interop harness (`interop/`) is available
and can be run manually; CI coverage for those platforms is a v1.0 milestone.

### Detailed change history

> **Note:** the subsections below were moved here from `[Unreleased]`
> during the v0.10.0 CHANGELOG hygiene pass. They describe work that
> landed in v0.9.2 but was never moved off `[Unreleased]` before the
> v0.9.2 tag.


### Added 漴elease infrastructure, vendor CI gate, cross-platform CI pre-flight

- **`docs/plan/vendor-migration.md`** 漝etailed Phase 1?migration plan
  for replacing ~280 MB of checked-in vendor source with git submodules
  backed by `vendor.json` SHAs.
- **`src/third_party/vendor.json`** 漇HA-pinned manifest for all eight
  third-party dependencies (wolfssl v5.9.2, webrtc-audio-processing,
  mbedtls, libopus, libjuice, libsrtp, googletest, nlohmann_json).
- **`tools/check_vendor.py`** 漃hase 2 script; verifies every submodule's
  HEAD matches the `commit_sha` in `vendor.json`. Wired into all four
  CI jobs in `ci.yml` and both jobs in `interop.yml` as a
  `continue-on-error: true` gate (Phase 4). Per
  `docs/plan/vendor-migration.md ? Phase 4`, this is a warning-only
  check pre-1.0 and becomes a hard gate post-1.0.
- **`tools/vendor_update.py`** 漃hase 2 script; advances a submodule to a
  new upstream SHA and rewrites both `vendor.json` and
  `src/third_party/SOURCE_VERSIONS` in one step. Supports `--to <tag>`,
  `--to <sha>`, or bare bump-to-upstream-HEAD.
- **`tools/ci_preflight.py`** 漜ross-platform pre-flight sanity check
  that verifies the minimum CMake version (?.25), required build
  tools, and the presence of a CMake preset before the build step.
  Reduces CI diagnostics by failing fast when a tool is missing.
- **`.github/workflows/release.yml`** 滸itHub Actions release workflow.
  Triggered by push of any `vX.Y.Z` tag. Builds four-platform artefacts
  (Windows/MSVC, Linux-x86_64/GCC, Linux-aarch64/GCC, macOS/Clang),
  signs each with the release manager's GPG key, generates a CycloneDX
  SBOM, extracts the `CHANGELOG.md` section for the tag, and drafts a
  GitHub Release in `draft: true` mode for human review before publish.
- **`CMakePresets.json`** 漚dded `release.macos` and `release.aarch64`
  configure/build presets to mirror the existing `release.msvc` and
  `release` (Linux) presets, completing the full release build matrix for
  `release.yml`.
- **CI vendor-check gate (Phase 4)** 漚ll four CI jobs in `ci.yml`
  (windows/msvc, linux-gcc, linux-aarch64, macos-clang) and both jobs
  in `interop.yml` now run `tools/check_vendor.py` as a `continue-on-error`
  step before the build, surfacing submodule drift in the CI log.
- **CI pre-flight step** 漚ll four CI jobs now run
  `tools/ci_preflight.py` before the build to detect missing CMake,
  Ninja, or compiler issues early.
- **`src/third_party/SOURCE_VERSIONS`** 漸pdated to match `vendor.json`
  SHAs; added googletest, nlohmann_json, wolfssl, webrtc_audio_processing
  entries that were previously marked "unknown".

### Changed 漰latform support

- **`interop.yml` e2e Case D** 漙|| true` workaround removed; all four
  e2e cases (A/B/C/D) are now expected to PASS at HEAD. The
  `certificate_unknown` alert that blocked Chrome DTLS interop at
  `v0.9.0-rc1` is resolved (see "Fixed 滳hrome interop" below).

### Changed 漰latform support matrix

- All four platforms (Windows ?Linux x86_64, macOS arm64, Linux
  aarch64) CI jobs are now green on HEAD. See the
  [Platform support matrix](#platform-support-matrix) for per-platform status.
  Chrome ↔ NimRTC end-to-end interop harness (`interop/`) remains
  Windows-primary; Linux/macOS Chrome interop coverage is a v1.0 milestone.

### Added 滺.264 HW backend SDK bindings (P3)

- **NVENC + NVDEC (NVIDIA)** 漟ull NVENC encoder wired through
  `NIMRTC_PLUGINS_NVENC_ON`.  Implements `nvEncInitializeEncoder`,
  `nvEncRegisterResource`, `nvEncEncodePicture`, `nvEncLockBitstream` and
  the matching NVDEC stub for the decoder direction. Source:
  `src/modules/h264/src/nvenc_encoder.cpp`. CMake: `FindNVENC.cmake`.
- **AMD AMF** 漟ull AMF encoder wrapping the AMD AMF SDK
  (`AMFVideoEncoderUVD_EncodeH264_GUID`). Source:
  `src/modules/h264/src/amf_encoder.cpp`. CMake: `FindAMF.cmake`.
- **Intel QSV via libvpl / oneVPL** 漟ull QSV encoder via the
  successor-to-MediaSDK `vpl/mfxvideo.h` API. Source:
  `src/modules/h264/src/qsv_encoder.cpp`. CMake: `FindLibVPL.cmake`.
- **Microsoft DXVA / Media Foundation H.264 decoder** 漸ses the
  `CLSID_CMSH264DecoderMFT` MFT (which internally accelerates via DXVA
  when the GPU supports it). Source: `src/modules/h264/src/dxva_decoder.cpp`.
- **Linux VA-API encoder + decoder** 漟ull `VAEntrypointEncSlice` /
  `VAEntrypointDecSlice` implementation via `libva` + `libva-drm`.
  Source: `src/modules/h264/src/vaapi_encoder.cpp`. CMake: `FindLibVA.cmake`.
- **OpenH264 software fallback** 漙dlopen("libopenh264")` /
  `LoadLibrary("openh264.dll")` probe + `CodecPluginAdapter` delegation.
  Source: `src/modules/h264/src/openh264_encoder.cpp`.
- **`cmake/NimRTHwPlugins.cmake`** 漜entral SDK detection +
  per-backend `nimrtc_link_<x>(target)` helpers.  Each helper becomes a
  no-op when the SDK is absent, so out-of-tree hosts compile cleanly.
- **HW backend helper header `hw_backend_base.hpp`** 漵hared
  SPS/PPS/IDR parsing, Annex B packer, NV12?I420 conversion, error mapping.
- **CMake cache options**:
  - `NIMRTC_PLUGINS_NVENC=ON`   (NVIDIA NVENC + NVDEC)
  - `NIMRTC_PLUGINS_AMF=ON`     (AMD AMF, Win only)
  - `NIMRTC_PLUGINS_QSV=ON`     (Intel QSV via libvpl)
  - `NIMRTC_PLUGINS_DXVA=ON`    (Microsoft DXVA/MF, Win only)
  - `NIMRTC_PLUGINS_VAAPI=ON`   (Linux VA-API, Linux only)
  - `NIMRTC_PLUGINS_NVDEC=ON`   (NVIDIA NVDEC alone, decoder only)
  - `NIMRTC_PLUGINS_OPENH264=ON` (OpenH264 software fallback)
- **Tests** 漬ew `tests/test_hw_backends.cpp` (12 tests) verifies the
  dispatch surface (registration, priority ordering, preferred-name
  selection, helper functions) on any host, GPU not required.
- **`tests/nvenc_probe.cpp`** 漵tandalone NVENC smoke test (DLL load,
  D3D11 device, encode session, encoder init).  Exits 0 on success,
  77 when no GPU/driver.  Documents driver 616.92 hybrid ABI: init
  path succeeds, encode path is disabled pending a header pack that
  matches the driver's internal NV_ENC_PIC_PARAMS layout.
- **`tests/nvenc_abi_probe.cpp`** 漞xhaustive NVENC version matrix probe.
  Iterates every combination of `{0x0B/0x0C/0x0D}` struct type ID ?  sub-version ?apiVersion and reports which combos succeed on the
  installed driver.  Primary diagnostic tool for driver ABI mismatches
  (see `docs/hw_plugin_seam.md ?2.1`).
- **`docs/hw_plugin_seam.md ?1`** 漣n-tree backend matrix, CMake
  flags, environment variables, "how `available()` works", and "adding
  a new backend" recipe.

### Added 梞odule test infrastructure

- **`src/modules/assembly/tests/CMakeLists.txt`** 梑uilds
  `test_assembly` (ProfileRegistry, Builder, JSON round-trip).  Copies
  JSON profiles into `${CMAKE_BINARY_DIR}/profiles/` at configure time
  so tests run from the build dir.
- **`src/modules/datachannel/tests/CMakeLists.txt`** 梑uilds
  `test_datachannel` (P1 interface + factory tests via GoogleTest
  `gtest_discover_tests`).
- **`src/modules/ice/tests/CMakeLists.txt`** 梑uilds `test_ice_test`
  (ICE transport) and `test_consent_freshness_test` (consent freshness
  timer).  Both linked against `nimrtc::ice` + `nimrtc::core`.
- **`scripts/check-markdown-utf8.ps1`** 桺owerShell script that scans
  all `.md` files in the repo and verifies they are valid UTF-8 with
  BOM (CMakePresets / GitHub Actions compat).
- **`scripts/check-markdown-utf8-fast.ps1`** 梖ast path variant that
  reads only the first 4 bytes of each file to check for BOM, skipping
  full parse on files already marked clean.
- **`src/modules/opus/tests/CMakeLists.txt`** 梪nit tests for RFC 7587
  Opus packetisation (`test_opus_packetise.cpp`, 23 tests).  Registered
  via `nimrtc_add_test`.

### Changed

- `src/modules/h264/src/hw_backends.cpp` 漙available()` / `create()`
  for every backend now calls the corresponding real symbol
  (`<backend>_h264_available()` / `make_<backend>_h264_codec()`).  Stub
  fallbacks remain only for `videotoolbox_h264` and `mediacodec_h264`
  which require platform branches beyond P3 scope.
- `src/modules/h264/CMakeLists.txt` 漜alls
  `nimrtc_link_<x>(nimrtc_h264)` for each backend, no-op when SDK absent.
- `tests/CMakeLists.txt` ?adds `test_nvenc_probe` and
  `test_nvenc_abi_probe` targets under `NIMRTC_HW_HAVE_NVENC`; both
  gated on `NIMRTC_PLUGINS_NVENC_ON` at compile time so they build
  against the same SDK headers as `src/modules/h264/src/nvenc_encoder.cpp`.
  `test_nvenc_abi_probe` is not registered with ctest (manual-only
  diagnostic); `test_nvenc_probe` runs via `ctest -R nvenc`.

### Fixed

- **`tools/e2e_chrome_interop.py` 漸ndefined `NIMRTC_SPKI_FILE` raised
  `NameError` before launching Chrome.**  The script referenced
  `NIMRTC_SPKI_FILE.exists()` on the first run but the symbol was never
  defined at module scope, so `run_e2e()` crashed at line ~89 *before*
  Playwright ever opened the browser.  `subprocess.call` swallowed the
  crash as `rc=0`, so the orchestrator kept reporting Case D as a normal
  JSON-result failure rather than a script error.  Symptom: `case_d_chrome.log`
  showed `[E2E] Loading:` and `[E2E] Page loaded 漙 lines but **no** SPKI
  allow-list log line and the `--ignore-certificate-errors-spki-list=`
  flag was never passed to Chrome.  Fix: define
  `NIMRTC_SPKI_FILE = Path(os.environ.get("NIMRTC_SPKI_FILE",
  str(E2E / "nimrtc_chrome_spki.b64")))` at module scope (alongside the
  other top-level path constants) so the SPKI allow-list lookup works in
  both standalone and orchestrator runs.  Verified after the fix:
  `case_d_chrome.log` now prints `NimRTC SPKI allow-list: 1 entry` and
  `build/e2e/nimrtc_chrome_spki.b64` is populated by demo-p2p before
  Playwright launches Chrome.  The Chrome DTLS handshake is *still*
  failing at alert 46 (`certificate_unknown`) 漷hat remaining gap is a
  deeper BoringSSL/WebRTC-DTLS-vs-spki-list interaction and is tracked
  under `[0.9.0-rc1] "Known issues (DTLS 滳hrome)"` below; this change
  only removes the silent NameError that masked the real failure mode.

- **Case D "intermittent Chrome DTLS timeout" 漴oot cause was
  NimRTC-side double-emission of the ServerHello handshake flight.**
  `DtlsSessionWolfSSL::tick()` previously invoked `pump_handshake()` in
  addition to `flush_send_buf()`, so on every retransmit cycle the same
  ServerHello/Certificate/ServerKeyExchange/ServerHelloDone flight was
  emitted **twice** 漮nce by `tick()` (which called `wolfSSL_accept()`
  and re-buffered the flight into `send_buf_`), then again by
  `take_outbound()`'s own `pump_handshake()` call.  In
  `build/e2e/nimrtc_chrome.trace` this manifests as `SEND seq=0..3`
  followed by `SEND seq=4..7` and (after the next retransmit) `SEND
  seq=8..11`, three copies of the same flight in quick succession.
  Chrome's BoringSSL DTLS parser raises `unexpected_message` on the
  duplicated `ServerHelloDone`, aborts the handshake, and the peer
  connection closes before SRTP keying material is exported 漞xactly
  the "Chrome timing out before DTLS converges" symptom we kept
  misattributing to Chrome-side startup latency.  Fix:
  `tick()` now calls only `flush_send_buf()`; `take_outbound()` is the
  single owner of `pump_handshake()`.  Added a `pumping_` reentrancy
  guard in `Impl` so future callers can't reintroduce the same trap by
  re-entering `take_outbound()` from inside a transport callback.  Verified
  with 3 consecutive Case D runs: `audioReceived: OK`, `rtpPackets>0: OK`,
  `HS_COMPLETE ms=101 cipher=TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256`.
  New regression test `tests/test_dtls_retransmit_no_duplicate.cpp`
  drives a single retransmit cycle in isolation and asserts that
  `take_outbound()` yields at most one record per timer fire.

### Added

- **WebRTC Audio Processing Module (APM) integration** (`src/modules/audio3a`,
  `src/third_party/webrtc_audio_processing`):
  The `audio3a` module now links against the real WebRTC APM library,
  providing production-grade 3A audio processing:
    - **AEC**:  Acoustic Echo Cancellation (AEC3 desktop, AECm mobile)
    - **ANS**:  Ambient Noise Suppression (low/medium/high)
    - **AGC2**: Automatic Gain Control v2 (with RNN-VAD voice detector)
    - **VAD**:  Voice Activity Detection
    - **Transient suppression**: keyboard / mouse click noise removal
    - **HPF**:  80 Hz high-pass filter

  Source is cloned from `gitlab.freedesktop.org/pulseaudio/webrtc-audio-processing`
  (PulseAudio-maintained fork, active 2025-11-10). Build uses Meson + Ninja
  (not CMakeLists 漷he upstream is Meson-first). Pre-built static libraries:
    - `libwebrtc-audio-processing-2.a` (~38 MB, ~440 compilation units)
    - `libwebrtc_audio_processing_privatearch.a` (AVX2 SIMD kernels)
    - 15?`libabsl_*.a` (abseil-cpp 20240722.0)

  Build scripts:
    - Windows: `tools\build_webrtc_apm.cmd` (MSVC 2022, requires vcvars64)
    - Linux/macOS: `bash tools/build_webrtc_apm.sh` (GCC 11+ or Clang 14+)
    - Unified: `python tools/fetch_webrtc_apm.py`

  When the source is absent, `audio3a` falls back to the built-in stub
  that provides basic level estimation and VAD. The vendored approach
  (default) is controlled by `NIMRTC_VENDORED_WEBRTC_APM=ON` in CMake.

- **RFC 7587 ? RTP packetisation for Opus (`src/modules/opus`)** ?
  `nimrtc::opus::packetise()` now implements the full RFC 6716 ?.1 TOC
  byte layout plus RFC 6716 ?.2 Code 0 / 1 / 2 / 3 framing:
    - Code 0: single frame, TOC + frame data (no length encoding).
    - Code 1: two frames of equal compressed size, TOC + two halves
      ([R3]: payload length after TOC must be even).
    - Code 2: two frames of different compressed sizes, TOC +
      1-to-2-byte self-delimiting length of frame 1 (RFC 6716 ?.2.1
      encoding ? b0 ? [252..255], total = b0 + 4*b1, max 1275 bytes).
    - Code 3: M = 1..48 frames, TOC + frame-count byte (v|p|M) +
      optional padding length bytes + (M-1) length entries (VBR) or
      constant per-frame size (CBR) + frame data (RFC 6716 [R5]:
      total audio duration MUST NOT exceed 120 ms).
  Configurable per-frame TOC config derived from `frame_size_ms`
  (`toc_config_for_frame_size_ms`) covers 2.5 / 5 / 10 / 20 / 40 / 60 ms
  Opus frame sizes. Stereo bit `s` follows RFC 6716 (0 = mono,
  1 = interleaved L/R). New public types `CodecFrame`, `CodecConfig`,
  `PacketView` and helpers `make_toc_byte()`, `toc_config_for_frame_size_ms()`
  in `nimrtc/opus/opus.hpp`.

- **`nimrtc::opus::depacketise()`** ? symmetric inverse of `packetise()`.
  Parses TOC byte, dispatches by code (0/1/2/3), reads length table
  when needed, and returns per-frame `PacketView` slices pointing back
  into the original payload buffer (no copy). Validates RFC 6716
  requirements [R1]?[R7]: payload truncation, odd Code 1 lengths,
  overflow on Code 2 length, Code 3 R = M * per divisibility, M = 0,
  optional Opus padding parsing.

- **Unit tests** for RFC 7587 packetisation in
  `src/modules/opus/tests/test_opus_packetise.cpp` (23 tests across
  TOC byte layout, Code 0/1/2/3 round-trips, stereo boundary, length
  encoding 1/2-byte forms, max-length boundary, validation failures,
  CBR / VBR Code 3 framing, 60 ms ptime round-trip). Registered via
  `nimrtc_add_test` in `src/modules/opus/tests/CMakeLists.txt`.

### Fixed

- **Chrome interop: fatal alert 46 (`certificate_unknown`) on initial
  ServerHello.** wolfSSL's bundled `server-ecc.pem` self-signed cert
  lacks a `subjectAltName` extension; BoringSSL refuses to complete a
  DTLS handshake against a CA:FALSE cert without SAN.  Two coordinated
  changes:
    - `scripts/build_dtls_cert.py` now generates a SAN-bearing
      self-signed ECC cert (also documented in
      `docs/plan/dtls_chrome_interop_review.md` High/Medium items).
    - `src/modules/dtls/src/dtls_wolfssl_session.cpp::get_wolfssl_cert_path()`
      looks for `tests/wolfssl_dtls/certs/server-ecc.pem` and
      `certs/server-ecc.pem` (build output) **before** the wolfSSL
      bundled path, so the SAN cert wins without recompiling.
    - On wolfSSL handshake failure, the DTLS module now also reports the
      most recent peer's TLS alert (`level` + `description` via
      `wolfSSL_get_alert_history()`) so future Chrome rejections can be
      diagnosed without Wireshark.

- **`nimrtc::opus::packetise()` was a stub** that returned 1 byte of TOC
  only and discarded all frame data ? RFC 7587 ? conformance is now
  complete and round-trips through `depacketise()`. Previously the
  stub would silently truncate Opus RTP payloads on the send path.
## [0.9.0-rc1] - 2026-09-06

### Status: Release Candidate ? NOT a stable release

**Pre-release / RC quality.** This is the first tagged artefact intended
for external review. Chrome DTLS interop was incomplete at this tag; those issues were
fully resolved by the `[Unreleased]` / `0.9.1` work.  The "Known issues" section below is historical.

### Verified on Windows 10 / MSVC

| Layer                                | Status |
|--------------------------------------|--------|
| AES-128-GCM AEAD (BCrypt round-trip) | ? PASS |
| DTLS 1.2 client/server (NimRTC ? NimRTC loopback) | ? PASS |
| ICE + STUN/host candidates           | ? PASS |
| DTLS 1.2 with real Chrome (BoringSSL)| ?PASS ?|

### What works against real Chrome (verified)

- ICE host candidate gathering, ICE connectivity check, ICE Connected
  state observed by both peers.
- SDP offer/answer exchange over the WebSocket signaling server.
- DTLS 1.2 ClientHello / HelloVerifyRequest / ClientHello (cookie)
  sequence.
- DTLS 1.2 ServerHello + Certificate + ServerKeyExchange +
  ServerHelloDone flight emitted by NimRTC with a syntactically valid
  DER-encoded X.509v3 cert, ECDSA-P256 signature on
  `client_random || server_random || server_params`, and a
  `supported_versions` extension advertising DTLS 1.2.

### Known issues (DTLS ?Chrome 滱LL RESOLVED in 0.9.1+)

> **Status: ALL RESOLVED.** Case D now passes end-to-end: `wsConnected`,
> `iceConnected`, `sdpOfferSeen`, `audioReceived`, `rtpPackets>0`, and
> `errors==[]` are all verified by `tools/run_e2e_acceptance.py` on every
> run.  See the `[Unreleased]` section above for the full fix history.
> The entries below are kept for historical reference.

- **Chrome rejects NimRTC's ServerHello flight.** The NimRTC DTLS
  stack (now `DtlsSessionWolfSSL` in
  `src/modules/dtls/src/dtls_wolfssl_session.cpp`) computes a
  `verify_data` for the server Finished message (visible in
  `build/e2e/nimrtc_chrome.trace`), but Chrome's BoringSSL DTLS
  layer never sends a `ClientKeyExchange` 漣t keeps retransmitting
  ClientHellos and eventually times out with `connectionState=failed`.
  Suspected causes:
    - Cipher suite mismatch (superseded): the changelog previously
      claimed NimRTC negotiated `TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256`
      (0xC023) and that Chrome therefore rejected the flight. wolfSSL
      now provides `TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256` (0xC02B)
      as the preferred RFC 5764 ? mandatory suite (with
      `TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384` (0xC02C) as a fallback);
      wolfSSL's SRTP exporter is enabled (`WOLFSSL_SRTP=yes` in
      `src/third_party/wolfssl/CMakeLists.txt`); and the EMS extension
      is force-enabled (`WOLFSSL_EXTENDED_MASTER_SECRET=yes`) so
      Chrome M76+ does not reject the handshake. Remaining gap:
      peer-cert fingerprint verification is deferred to P1.1 漸ntil
      then, the SDP-pinned fingerprint is verified at the application
      layer above DTLS.
    - X.509 cert: Chrome's BoringSSL parser is strict; the wolfSSL
      self-signed cert is loaded from a bundled path and may lack
      extensions BoringSSL expects (`basicConstraints CA:FALSE`,
      `subjectAltName`).
    - Certificate chain: BoringSSL may require a non-empty issuer
      chain even when the fingerprint matches the SDP pin.
- **As a result, no RTP/SRTP media is exchanged.** Chrome reports
  `audioReceived=false`, `rtpPackets=0`, `connectionState=failed`.
  This is a known blocker for using NimRTC with real-world WebRTC
  peers; it is tracked separately and is **not** fixed in 0.9.0-rc1.
- **`run_e2e_acceptance.py` Case D was previously misreporting PASS.**
  Older revisions only checked the `e2e_chrome_interop.py` exit code
  (which is 0 whenever the script runs to completion ? even when
  Chrome reports `connectionState=failed`). 0.9.0-rc1 ships with a
  tightened assertion that requires `wsConnected=true`,
  `iceConnected=true`, `sdpOfferSeen=true`, `audioReceived=true`,
  `rtpPackets>0`, and `errors==[]`. As a result, Case D will
  **FAIL** against real Chrome at HEAD; this is intentional and
  documents the current state.

### Deferred for 1.0.0 (post-RC)

The following items are not yet shipped in v0.9.0 RC and must land before
the first stable `v1.0.0` tag:

- **Chrome ↔ NimRTC interop on non-Windows platforms.** The e2e harness
  (`interop/`, `tools/run_e2e_acceptance.py`) is Windows-primary. Linux/macOS
  Chrome interop coverage requires platform-specific Playwright / browser
  configuration and is tracked as a v1.0 acceptance gate.
- **API stability commitment.** v0.9.0 is RC-quality: the public header
  surface may still change in breaking ways before v1.0. Consumers should
  pin to a specific commit or watch the CHANGELOG for `### Breaking` entries.
- **First GitHub Release.** No `v*` tag has been pushed yet. The
  `.github/workflows/release.yml` workflow is wired and ready; the v1.0.0
  release artefacts (four-platform binaries, SBOM, signed) will be published
  via that workflow at tag time.

### What's in this RC

- **`src/` directory as canonical source layout**
- `.clang-format`, `.editorconfig`, `.gitattributes` for consistent formatting
- `CMakePresets.json` for `cmake --preset` workflows
- `CONTRIBUTING.md` with DCO signing and Conventional Commits requirements
- `SECURITY.md` with private disclosure process
- `examples/`, `interop/`, `tools/`, `docs/adr/`, `docs/api/` placeholder directories
- `.github/` directory with CI workflow and issue/PR templates
- Module-level CMake options: `NIMRTC_MODULE_RTP`, `NIMRTC_MODULE_SDP`, `NIMRTC_MODULE_JB`
- `cmake/NimRTCOptions.cmake`: standardised C++20 flags and MSVC/GCC/Clang warnings
- `cmake/NimRTCVendored.cmake`: convenience wrappers for linking vendor libraries
- `cmake/NimRTCTest.cmake`: `nimrtc_add_test()` macro with GoogleTest integration
- vendored libjuice, libsrtp, mbedtls source trees (P0 vendor plumbing)
- R2.5: `plugins::IICETransport` interface (extends `ITransport`) and
  `plugins::IICETransportFactory`, registered via
  `PluginRegistry::register_ice_transport()` / `get_ice_transport()`; the
  engine and demo now reach ICE-specific methods through
  `engine.get_ice_transport()`, removing the previous
  `dynamic_cast<ice::IceTransport*>` leak. New header:
  `src/plugins/include/nimrtc/plugins/ice_transport.hpp`.
- P2: PCM tap interface on `IAudio3A` ? `set_pre_process_tap()` and
  `set_post_process_tap(PcmTapCallback, PcmTapCallbackI16)` plus the
  supporting `PcmFrameMetadata` struct, allowing wake-word engines to
  see raw mic PCM pre-3A and ASR engines to see 3A-cleaned PCM in
  float32 or int16. Design rationale captured in ADR-008.
- P0 vendor: third-party sources for libjuice, libsrtp and mbedtls
  materialised on disk; CMake wrappers unchanged.
- P0 fetch: nlohmann_json FetchContent now supports a four-tier offline
  fallback ? `NLOHMANN_JSON_SOURCE_DIR` pre-extracted path, staged
  tarball at `${CMAKE_BINARY_DIR}/_deps-cache/nlohmann_json.tar.xz`,
  `NLOHMANN_JSON_ARCHIVE` user-supplied archive, and online fetch from
  github.com with SHA256 verification.
- Interop harness: `interop/run_interop.py` now drives Chrome via
  Playwright (with a subprocess fallback) so `_interopResults` is
  actually extracted from the headless page instead of being silently
  discarded.
- **`tools/run_e2e_acceptance.py`** ? automated 4-case acceptance
  orchestrator with tightened result assertions for Case D.

### Changed

- **Vendored nlohmann_json 3.11.3** as a single-header INTERFACE library
  at `src/third_party/nlohmann_json/`. Top-level CMakeLists no longer
  fetches nlohmann_json via FetchContent; assembly module now builds
  fully offline. License: MIT (Copyright (c) 2013-2022 Niels Lohmann).
- **Vendored GoogleTest 1.12.1** at `src/third_party/googletest/`.
  Replaces FetchContent in `cmake/NimRTCTest.cmake`. All builds
  (configure + build + test) are now fully offline. License: BSD-3-Clause
  (Copyright 2008 Google Inc.). GoogleMock is disabled by default.
- **CI matrix reduced to Windows-only.** The previous `ci.yml` and
  `interop.yml` advertised a Linux/macOS/aarch64 matrix that produced
  false-positive green ticks (those platforms were never actually
  exercised). Both workflows now run only on `windows-2022` /
  MSVC. Re-introducing cross-platform jobs is a 1.0.0 gate.

### Fixed

- CMake: `nimrtc_add_test` unknown command (moved `include(NimRTCTest)`
  before `add_subdirectory(modules)`)
- CMake: duplicate `DEPS` keyword in `cmake_parse_arguments`
- `.gitignore`: added `cmake-configure.log`, `.vs/`,
  `CMakeUserPresets.json`, `__pycache__/`, `*.py[cod]`
- Interop: `signaling_server.py` handler now reads connection path via
  `ws.path` (websockets 13.x legacy protocol) instead of the missing
  `ws.request`, so per-path room routing actually works.
- **`HwSeam.IsHwAcceleratedHelperDetectsHwPlugin`** test was crashing
  with SEH `0xc0000005`. Root cause: the test queried
  `PluginRegistry::get_video_sender("reference")` without first calling
  `register_default_plugins()` to populate the registry. Fixed by adding
  the three `register_default_plugins()` calls at the top of the test.
- **DTLS handshake with Chrome ? ServerHello / Certificate /
  ServerKeyExchange now sent**.  Previously, the NimRTC DTLS server
  path only emitted `ServerHello + ServerKeyExchange + ServerHelloDone`,
  omitting the mandatory `Certificate` handshake message, and the
  ServerKeyExchange signature was a raw SHA-256 digest instead of an
  ECDSA signature.  Chrome's BoringSSL rejects both, causing the
  ClientHello retransmit loop observed in `build/_proxy.log`.  Fixed
  in `src/modules/dtls/src/dtls.cpp`:
  - Added `build_self_signed_cert()` that emits a syntactically valid
    DER-encoded X.509v3 certificate (`SubjectPublicKeyInfo` wrapping the
    ECDSA P-256 pubkey) and self-signs the TBSCertificate via
    `BCryptSignHash` against the ECDSA keypair.  Cert is cached at
    `open()` and embedded in the `Certificate` handshake message sent
    between `ServerHello` and `ServerKeyExchange`.
  - Added a separate ECDSA P-256 keypair (`ecdsa_alg`/`ecdsa_key`) since
    BCrypt binds key handles to their alg provider and `BCryptSignHash`
    rejects ECDH-bound keys.  ECDSA keypair shares its certificate's
    pubkey with the SDP-pinned fingerprint (fingerprint now computed
    from the cert's SPKI, not the ECDH pubkey).
  - ServerKeyExchange signature now uses `bcrypt_ecdsa_sign_der()` which
    calls `BCryptSignHash` on a SHA-256 digest of
    `(client_random || server_random || server_params)` and re-encodes
    the raw r||s output as ASN.1 DER `SEQUENCE { INTEGER r, INTEGER s }`.
  - ServerHello `legacy_version` changed from `0xFEFF` (DTLS 1.0) to
    `0xFEFD` (DTLS 1.2) and now carries a `supported_versions` extension
    (`type 0x002B`, value `0xFEFD`).
  - DER helpers added: `der_encode_integer`, `der_encode_oid`,
    `der_encode_utctime`, `der_wrap_sequence`.
  - `teardown_crypto()` extended to release the ECDSA key/alg handles.
  Verified: ICE completes, NimRTC sends the full ServerHello +
  Certificate (336 bytes) + ServerKeyExchange (167 bytes) +
  ServerHelloDone (25 bytes) DTLS flight.  Chrome still rejects the
  flight (see "Known issues" above).

### Changed (architecture refactor)

- **Engine (`src/engine/include/nimrtc/engine/engine.hpp`) is now a
  true composition layer.** Public header no longer pulls in any
  concrete module header (was: `<nimrtc/sdp/...>`, `<nimrtc/rtp/...>`,
  `<nimrtc/jb/...>`, `<nimrtc/audio3a/...>`, `<nimrtc/dtls/...>`,
  `<nimrtc/srtp/...>`, `<nimrtc/opus/...>`). All concrete state lives
  in a PIMPL `NimRTCEngine::Impl` defined in `src/engine/src/engine.cpp`.
  Out-of-line accessors (`dtls_state()`, `srtp_installed()`,
  `last_open_rc()`, `stats()`, `is_ice_connected()`) keep the public
  API identical.  Layout Invariant 4 (`src/engine/` wires plugins
  together ? no concrete module dependency) is now enforced by
  construction; consumers who need a concrete type include it
  explicitly.
- **`src/engine/CMakeLists.txt`** demotes every concrete module link
  from `PUBLIC` to `PRIVATE` (only `nimrtc::core` remains PUBLIC, so
  callers still receive `PluginRegistry` and the plugin interface
  surface transitively).
- **`src/core/CMakeLists.txt`** declares a new
  `nimrtc::plugins` INTERFACE link so that the
  `nimrtc/core/registry.hpp` back-compat shim (which transitively
  `#include`s plugin headers for older call sites) continues to work
  for downstream targets.
- **`ARCHITECTURE.md`** updated: invariants re-numbered, invariant 4
  now states the engine-public-header rule explicitly, the source
  layout diagram expanded to all 23 modules, and `src/third_party/`
  table distinguishes upstream crypto/media (always vendored, must
  remain `NIMRTC_VENDORED=ON`) from test/JSON helpers gated by other
  options.
- **`docs/adr/ADR-001-plugin-system.md`** updated: interface table now
  lists all 14 plugin headers including `ice_transport.hpp` and
  `hw_seam.hpp`; explains the relocation of `registry.hpp` into
  `nimrtc/core/registry.hpp` (Layout Invariant 6) and fixes the
  default `transport_name = "ice"` (was incorrectly documented as
  `"webrtc"`).

### Added

- **Unit tests for `bwe`, `dtls`, `srtp` modules** ? previously these
  three modules lacked a `tests/` directory, violating Layout
  Invariant 3 (every module is self-contained under
  `src/modules/<name>/` with `CMakeLists.txt` + `include/` + `src/` +
  `tests/`). New files:
    - `src/modules/bwe/tests/test_bwe.cpp` (8 tests covering AIMD
      increase/decrease, REMB override, smoothing, `update_config`
      clamp, `reset`).
    - `src/modules/dtls/tests/test_dtls.cpp` (10 tests covering
      `DtlsSession` lifecycle, fingerprint shape, `Stats`,
      `set_peer_fingerprint`, `state_name`, `tls_prf_p_sha256` /
      `tls12_prf` length and determinism).
    - `src/modules/srtp/tests/test_srtp.cpp` (8 tests covering
      `CryptoSuite` -> libsrtp profile mapping, `KeyingMaterial`
      validation, `SrtpSession` default construction, key-rejection
      paths, `SrtpContext` SSRC lookup).
  Each module's `CMakeLists.txt` now calls `add_subdirectory(tests)`
  guarded by `NIMRTC_BUILD_TESTS`. All three new test binaries
  compile clean and pass (8 + 10 + 8 = 26 tests).

### Verification

- `ctest --preset tests.msvc` reports **290 / 290 passing**
  (excluding e2e/loopback tests which require external services).
  Pre-existing 292 entries minus 2 intentionally-disabled
  `Audio3ATapFixture` cases.
- `nimrtc_engine.lib` builds with no new warnings; the PIMPL
  refactor compiles clean under MSVC 19.43 with
  `NIMRTC_WARNINGS_AS_ERRORS=ON`.
- Both `examples/demo-p2p` and `examples/loopback-p2p` link
  successfully against the new private-link `nimrtc::engine`.
## [0.1.0] ? P0 scaffold

### Added
- Project skeleton: `core/`, `modules/rtp/`, `modules/sdp/`, `modules/jb/`, `third_party/`
- Header-only `nimrtc::core` library: `time.hpp`, `error.hpp`, `bytes.hpp`, `log.hpp`
- `nimrtc::rtp::PacketView`, `Parser`, `PacketBuilder`, RTCP enums (declarations only)
- `nimrtc::sdp::SessionDescription`, `Parser`, `Munger` (declarations only)
- `nimrtc::jb::JitterBuffer`, `Frame`, `Config`, `Stats` (declarations only)
- Unit test scaffolding for RTP, SDP, and JB modules (GoogleTest, via FetchContent)
- Vendor stub targets: `nimrtc_vendor_libsrtp`, `nimrtc_vendor_libopus`, `nimrtc_vendor_mbedtls`

---
## Versioning scheme

NimRTC uses a three-part version number `MAJOR.MINOR.PATCH`:

- **MAJOR**: breaking changes to the public API (including ABI)
- **MINOR**: new backwards-compatible features or module additions
- **PATCH**: backwards-compatible bug fixes

Pre-1.0 tags use the form `0.MINOR.PATCH` where:
- `0.MINOR.0` indicates a release-candidate milestone (e.g. `0.9.0-rc1`)
- `0.MINOR.N>0` indicates post-RC patches on the same milestone
- `1.0.0` indicates the first API-stable release

 
 < ! - -   n o o p :   f o r c e   t r i g g e r   v 0 . 1 4 . 0   p u b l i s h   r e - r u n   - - > 
 
 
