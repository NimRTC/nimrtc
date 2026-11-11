# Session Capability Plan — Renegotiation, Transceiver, DTMF, RTCP XR/TMMBR

> **What this document is.** The executable counterpart to the session-layer
> capability-gap table raised for the v1.1.0 review. The table listed nine
> session/signalling capabilities that `libwebrtc` has and NimRTC v1.1.0
> does not. Items 3 and 4 (two-way codec negotiation, payload-type
> demultiplexing) were implemented as **C9** and **C11** of the video-pipeline
> plan; item 8 (the full `getStats` surface) was split out into
> `docs/plan/stats-surface-plan.md`. This document covers the four remaining
> items — **5, 6, 7 and 9** — with the same shape as the sibling plans:
> the *verified current state*, the *work items*, the *acceptance criteria*,
> and the *release placement*.
>
> | | |
> |---|---|
> | Plan opened | 2026-11-10 |
> | Baseline | `main` @ post-v1.1 consolidation |
> | Scope class | Session / signalling — session lifecycle, m-line semantics, RTP payload semantics, RTCP feedback surface. No transport rewrite. |
> | Status | **Gap analysis complete; work items written; version placement owner-decided (§4) — the whole plan lands in `v1.2.0`. Tracks N, T and D have landed; in Track X, X1 (XR block parsing + the `IXrReport` seam), X2 (XR block construction + the engine's outbound XR report), X3 (TMMBR / TMMBN round-trip), X4 (the engine's TMMBR follows the live BWE target), X5 (the inbound TMMBR clamps the egress rate and is answered with a TMMBN), X5b (the SDP wiring) and X6 (the documentation) have all landed.** |
> | Owner | BDFL |
> | Related | `docs/plan/video-pipeline-v2-plan.md`（**C9 / C11** 已落地，v1.2.0）、`docs/plan/stats-surface-plan.md`（会话能力第 8 项，O1 / M2）、`docs/plan/datachannel-capability-plan.md`、`docs/plan/v1.1-plan.md` §5.6（版本阶梯）、ADR-015 (API stability) |

---

## 1. The gap table, verified against the tree

The table below is the input table with every cell re-checked against the
source. **One cell does not survive the check** — item 5 (Renegotiation) is
*not* a blank slate: the `session` module already ships a Track/Stream/Session
data model with the right operations. What is missing is the *engine
wiring* and the *SDP-level* consequence. That correction is in §1.2.1.

| # | Capability | libwebrtc | NimRTC v1.1.0 (verified) | Verdict |
|---|---|---|---|---|
| 5 | Renegotiation（增删 track / 改方向） | ✅ | **数据模型已有，引擎未接线** | **corrected**（§1.2.1） |
| 6 | Transceiver / Unified Plan | ✅ | ❌ 无 transceiver 概念，SDP 只有固定 m-line | confirmed（§1.2.2） |
| 7 | DTMF（RFC 4733） | ✅ | ❌ 无实现，`telephone-event` 只作为"不可解码的负载类型"被丢弃 | confirmed（§1.2.3） |
| 9 | RTCP XR / TMMBR | ✅ | ❌ PT 207 只被识别，未解析；TMMBR 只有 SDP 侧的 `disable_tmmbr` 开关 | confirmed（§1.2.4） |

### 1.1 Why these four were not treated as "code missing"

Each of the four touches the *session semantics* rather than a single
protocol handler, so the naive reading ("grep for the feature name, find
nothing, therefore write it from scratch") would have been wrong in at
least one case, and would have produced duplicate work in another. The
rule applied here was: **the capability counts as present only if the
engine can produce and consume it end-to-end**, not if a type or a flag
exists somewhere.

### 1.2 Evidence per item

#### 1.2.1 Item 5 — Renegotiation: the data model exists, the wiring does not

**What exists.** `src/modules/session/` is a complete, self-contained
module (L3-conformant: own `CMakeLists.txt`, `include/`, `src/`, `tests/`)
that models exactly the objects a renegotiation needs:

| Object | API | File |
|---|---|---|
| `Track` | `id()`, `type()`, `direction()` / `set_direction()`, `ssrc()` / `set_ssrc()`, `bundled()` / `set_bundled()` | `src/modules/session/include/nimrtc/session/track.hpp` |
| `Stream` | `add_track()`, `remove_track()`, `tracks()`, `track_by_id()` | `src/modules/session/include/nimrtc/session/session.hpp` |
| `Session` | `add_stream()`, `remove_stream()`, `streams()`, `bundle_tracks()`, `bundled_track_ids()`, `set_rtcp_mux()` / `rtcp_mux()` | same |

`TrackDirection` covers all four RFC 4566 states (`kSendRecv`, `kSendOnly`,
`kRecvOnly`, `kInactive`) — verified in
`src/modules/session/tests/test_session.cpp` (`Track.DefaultDirectionIsSendRecv`,
`Track.AllDirections`, `Track.DirectionRoundTrip`, `Stream.AddTrack`,
`Stream.RemoveTrack`).

**What is missing.** The module is **not reachable from the engine**:

- `src/engine/` contains **no include of `nimrtc/session/**` — verified by
  grepping every `src/engine/**` header and source: zero hits.
- `src/engine/CMakeLists.txt` does **not** link `nimrtc::session` — the
  `target_link_libraries(nimrtc_engine …)` list contains `nimrtc::opus`,
  `nimrtc::audio_rx`, `nimrtc::net_feedback`, `nimrtc::video_source`,
  `nimrtc::video_sink`, `nimrtc::video_pipeline`, `nimrtc::h264` and no
  session target.
- The only consumers of `nimrtc/session/**` in the whole tree are the
  module's own three source files and its own test file.

So `Stream::add_track()` / `remove_track()` / `Track::set_direction()` are
**reachable only from a host that links `nimrtc_session` directly and drives
it by hand** — they do not change what the engine puts on the wire.

**And the engine's SDP has no renegotiation story at all.**
`NimRTCEngine::create_offer()` (`src/engine/src/engine_signaling.cpp:43`)
builds a **fixed** media list: an audio m-section with `mid = "0"`
(line 70), a video m-section with `mid = "1"` (line 239), and optionally a
`m=application` line (line 395). The public API
(`src/engine/include/nimrtc/engine/engine.hpp`) exposes exactly two
signalling entry points — `create_offer()` and `process_remote_sdp()` —
and **no** `addTrack` / `removeTrack` / `setDirection` / `renegotiate`
equivalent, and no "negotiation needed" signal.

**What the SDP layer *can* already express.** The `sdp` module is not the
blocker: `sdp::Direction` (`src/modules/sdp/include/nimrtc/sdp/session_description.hpp:43`)
already has all four states, the parser accepts the media-level direction
attribute (`src/modules/sdp/src/parser.cpp:380-386`), and the munger emits
it (`src/modules/sdp/src/munger.cpp:126-128`). `a=mid:` and
`a=group:BUNDLE` are parsed and generated too (`session_description.hpp:72`
/ `:139`, `parser.cpp:419` and `:449-453`, `munger.cpp:122-124` and
`:291-296`). The engine already emits `sdp.bundle_mids = {"0", "1"}`
(`engine_signaling.cpp:317`).

**Verdict.** The *data model* is done and tested; what is missing is (i) an
engine-level API that mutates the session and triggers a new offer, and
(ii) the media-list construction that turns a track set into m-lines. This
is an **integration** item, not a from-scratch implementation item — which
is why it carries a different risk profile from items 6/7/9.

#### 1.2.2 Item 6 — Transceiver / Unified Plan: absent, but the SDP primitives are half-there

- **No transceiver concept anywhere.** Grepping `src/` for `transceiver` /
  `Transceiver` yields exactly **one** hit, and it is a *comment* in
  `src/modules/sdp/tests/test_sdp.cpp:509` ("audio transceiver"). There is
  no `RtpTransceiver` type, no `addTransceiver()` API, and no
  `a=mid:`-keyed transceiver registry.
- **The Unified Plan's distinguishing feature — many m-lines, each with its
  own mid and direction — is not reachable.** The engine emits a fixed
  two-m-line offer (audio `mid="0"`, video `mid="1"`). A peer offering
  three or more media sections is parsed, but the engine has no notion of a
  per-mid object with its own lifecycle, so the extra m-lines cannot be
  renegotiated, re-ordered, or stopped independently.
- **The `Direction` primitive exists** (§1.2.1) and is parsed/emitted
  correctly, so the *lowest* layer of Unified Plan — per-m-section
  direction — is ready. What is missing is the *object* that owns a
  mid ↔ direction ↔ sender/receiver pairing, i.e. the transceiver.

**Verdict.** Confirmed missing. The work is a new abstraction, but it can
build on the existing `Direction` / `mid` / BUNDLE primitives rather than
re-inventing them.

#### 1.2.3 Item 7 — DTMF (RFC 4733): absent, and the current behaviour is a deliberate drop

- **There is no telephone-event codec, no RFC 4733 encoder or decoder, and
  no DTMF API on the engine.** Grepping `src/` for `telephone-event`
  returns **five** hits, and every one of them is in a *rejection* path:
  - `src/engine/src/audio_routing.hpp:16` and `:68` — the doc comment
    explaining that a payload type other than the negotiated one is
    "a telephone-event, a RED stream, a codec we do not implement";
  - `src/engine/src/engine_impl.hpp:731`, `engine_signaling.cpp:560` and
    `engine_timeline.cpp:249` — the same statement repeated at the three
    places where defect F-8 was fixed.
- **The drop is correct behaviour, not an accident.** The audio routing
  table (`src/engine/src/audio_routing.hpp`) only routes the two mandatory
  WebRTC codecs; anything else — including a telephone-event payload type —
  produces **no route**, and the engine counts it as
  `kAudioPayloadTypeUnknown`. That is the right thing to do for a codec
  NimRTC cannot decode.
- **The SDP side has no telephone-event declaration either.** The engine's
  offer builder (`engine_signaling.cpp:82-86` for audio, `:251-256` for
  video) emits exactly one `a=rtpmap:` per m-section, for the negotiated
  codec. There is no `a=rtpmap:126 telephone-event/8000` line and no
  `a=fmtp:126 0-16` line (RFC 4733 §2.3.2).

**Verdict.** Confirmed missing. Note that the *sender* and the *receiver*
are separate sub-items: an RFC 4733 receiver needs the telephone-event
stream decoded into events, and an RFC 4733 sender needs to transmit
in-band events. Neither exists. The plumbing to *ignore* telephone-event
packets already exists and is tested, so this item adds a new path rather
than changing an existing one.

#### 1.2.4 Item 9 — RTCP XR / TMMBR: absent; the identifier exists, the payload does not

**XR (RFC 3611).** The payload type **is** recognised:
`RtcpBlockKind::kExtendedReport` with the comment "PT 207 — XR"
(`src/modules/rtp/include/nimrtc/rtp/rtcp.hpp:100`), and the demux maps
`kPtXr = 207` to it (`src/modules/rtp/src/rtcp.cpp:29` and `:67`). But
recognition is the *whole* of the implementation:

- There is **no XR block parser and no XR builder** in `src/modules/rtp/`.
  The RTPFB parser handles NACK and transport-cc; the PSFB parser handles
  PLI / FIR / SLI / REMB. The XR-specific block types — Receiver Reference
  Time, DLRR, Statistics Summary — have no representation at all, and
  neither have the rest of RFC 3611 section 6.2's registry (Loss RLE,
  Duplicate RLE, Packet Receipt Times, VoIP Metrics).
- The generic `demux_rtcp()` walk returns the XR sub-packet as an opaque
  `RtcpBlock` with `kind == kExtendedReport` and the raw bytes in
  `packet`. Nothing downstream consumes it.

**TMMBR (RFC 5104, RTPFB FMT=3).** The only occurrence of the string
"TMMBR" in `src/` is a **comment listing the FMT values**
(`src/modules/rtp/include/nimrtc/rtp/packet.hpp:367`:
"NACK (1), TMMBR/TMMBN (3), TSTR (4), TSTN (5), PLI (6), SLI (7), FIR (8)").
There is no TMMBR structure, no builder, no parser, and no FMT=3 handling
anywhere. Additionally, `plugins::SdpCodecConfig::disable_tmmbr`
(`src/plugins/include/nimrtc/plugins/sdp.hpp:93`) exists as an
*munging* option, but grepping `src/engine/**` for `tmmbr` returns **zero**
hits — the engine never sets it and never acts on it, so the flag is
currently inert from the engine's point of view.

**Verdict.** Confirmed missing. The XR *demux* recognition is a small
head start, but the payload layer is entirely absent.

---

## 2. Work items

Landing-site markers follow `video-pipeline-v2-plan.md`: `P` = `src/plugins/`,
`M` = `src/modules/<name>/`, `E` = `src/engine/`, `T` = `tests/`, `D` = `docs/`.

### Track N — Renegotiation (item 5)

| ID | Item | Landing | Acceptance |
|---|---|---|---|
| **N1** | **引擎接线 `session` 模块**：引擎持有 `session::Session`（或等价的轨道集合），并暴露宿主可调用的轨道增删与改向 API。落点仍在 `src/engine/`，但**通过 `src/plugins/` 声明的接口访问 `src/modules/session/`，不直接 include 具体模块头**（遵守 L4）。 | `P` + `E` + `M` | 引擎能新增一条轨道并反映到下一次 offer；`src/engine/` 中不出现 `#include <nimrtc/session/…>` |
| **N2** | **协商触发信号**：宿主增删轨道后，引擎能回答"是否需要重新协商"，并能产出与当前轨道集合一致的新 offer。重新协商的 SDP 往返复用现有的 `create_offer()` / `process_remote_sdp()`，不引入第二套信令路径。 | `E` | 增删轨道后 `needs_negotiation()` 为真，且新 offer 的 m-line 集合与轨道集合一致；不增删时为假 |
| **N3** | **m-line 与轨道集合的映射**：轨道增删要能映射为 m-line 的增删，并保持 `a=mid:` 的稳定性与 `a=group:BUNDLE` 的正确性（新增轨道的 mid 不得与既有 mid 冲突）。 | `E` | 增删轨道后 offer 的 mid 集合与既有 mid 无冲突；BUNDLE 组只包含实际存在的 mid |
| **N4** | **轨道方向到 m-line 方向的映射**：`Track::set_direction()` 的四种取值必须落到 `a=sendonly` / `a=recvonly` / `a=inactive` / `a=sendrecv`（`sdp::Direction` 已支持），且方向变更要走协商而非静默生效。 | `E` + `M` | 改方向后，新 offer 的 m-line 携带对应的方向属性，且在协商完成前旧方向仍在生效 |
| **N5** | **停用轨道的行为定义**：被移除的轨道，其 RTP 流如何停止、是否发 `a=mid:` 为空的 m-line 还是整条 m-line 消失、接收侧如何丢弃残留包。这一条规格里没写，**需要 owner 决策**（见 §4 的 OD-A）。 | `D` → `E` | 行为在 `docs/api/engine_api.md` 中写明，且有一个测试固定该行为 |

### Track T — Transceiver / Unified Plan (item 6)

| ID | Item | Landing | Acceptance |
|---|---|---|---|
| **T1** | **transceiver 数据模型**：在 `src/modules/session/` 内引入 transceiver 抽象（mid ↔ 方向 ↔ 发送/接收轨道对），`src/plugins/` 声明宿主可见的接口。这一项**依赖 N1** 的接线，否则仍是无消费者的类型。 | `P` + `M` | 一个 transceiver 能绑定 mid、方向与一对轨道；与 N1 的轨道集合共存，不产生两套真相 |
| **T2** | **多 m-line 的 offer/answer**：引擎能生成并接受多于一条音视频 m-line 的 offer，每个 m-line 有自己的 mid、方向与负载类型。 | `E` | 三条 m-line 的 offer 能被正确解析并生成对应 answer；每条 m-line 的 mid 唯一 |
| **T3** | **按 mid 的收发分离**：`a=mid:` 与 `a=group:BUNDLE` 的组合下，发送与接收可以指向不同的 m-line（Unified Plan 的核心语义），而不仅仅是"同一条 m-line 既发又收"。 | `E` + `M` | 一个 mid 为发送、另一个 mid 为接收的 offer 能正确协商；两方向的包走各自 mid 的负载类型 |
| **T4** | **停用 / 重启 transceiver**：transceiver 的 `stop()` / `restart()` 语义与 N4 的方向语义衔接，且不破坏 BUNDLE 组。 | `M` | 停用一个 transceiver 后 offer 中对应的 m-line 变为 `inactive`（或按 OD-A 的决策消失），BUNDLE 组保持合法 |

### Track D — DTMF (item 7)

| ID | Item | Landing | Acceptance |
|---|---|---|---|
| **D1** | **RFC 4733 接收**：解析 telephone-event 流（PT 由协商决定，通常 126），把事件还原成 (event code, duration, end-of-event) 序列并交给宿主。**不引入新插件类型**——复用现有的音频载荷路由面，在其旁边加一条 DTMF 出口。 | `P` + `M` + `E` | 一段合法的 telephone-event 载荷能被还原成有序事件序列；非 RFC 4733 的载荷被安全忽略 |
| **D2** | **RFC 4733 发送**：宿主能把 DTMF 事件编码成 telephone-event 载荷发出，含 RFC 4733 要求的重复包与结束包时序。 | `P` + `M` + `E` | 发出一个按键后，对端能收到该事件码与时长；结束包按 RFC 4733 §2.5 的时序出现 |
| **D3** | **SDP 声明**：offer / answer 中出现 `a=rtpmap:<pt> telephone-event/8000` 与 `a=fmtp:<pt> 0-16`（RFC 4733 §2.3.2），且该负载类型**不进入**普通音频解码路径（与 §1.2.3 现有的丢弃路径一致，不产生回归）。 | `E` | offer 携带 telephone-event 声明；对端发来的 telephone-event 包**不**被当作音频解码，计数口径与 F-8 修复后一致 |
| **D4** | **与 F-8 修复的边界**：`kAudioPayloadTypeUnknown` 的计数口径必须明确——telephone-event 在**未协商 DTMF** 时仍计为 unknown，在**已协商 DTMF** 时不再计入 unknown。 | `E` + `D` | 两种协商状态下，unknown 计数的行为不同且各有测试固定 |

> **D1 / D2 / D3 / D4 已落地（v1.2.0，2026-11-11）。** 验证矩阵：
>
> | 工作项 | 落地内容 | 验证 |
> |---|---|---|
> | **D1** | `plugins::IDtmfCodec` 接缝 + `src/modules/dtmf/` 实现（RFC 4733 编解码） | `src/modules/dtmf/tests/test_dtmf_codec.cpp` |
> | **D2** | 发送路径：事件重复包与结束包由 `tick()` 按 RFC 4733 §2.5.1.2 的节奏发出 | `src/modules/dtmf/tests/test_dtmf_codec.cpp`（线格式）+ `tests/test_engine_dtmf.cpp`（引擎面） |
> | **D3** | SDP 声明：offer / answer 携带 `a=rtpmap:<pt> telephone-event/8000` 与 `a=fmtp:<pt> 0-16`，payload type 进入 m-line 的格式表；answer 回带**协商后**的事件码（对端声明 ∩ 本端实现），不是盲回显 | `tests/test_engine_dtmf.cpp`（`ADefaultEngineOffersNoTelephoneEvent`、`...DeclaresTheTelephoneEventExactlyOnce`、`TheAnswerEchoesTheTelephoneEventDeclaration`、`TheAnswerCarriesTheNegotiatedEventCodes`） |
> | **D4** | 计数口径：已协商的 telephone-event 不计入 `kAudioPayloadTypeUnknown`，未协商的仍计入；malformed 单独计数 | `tests/test_engine_dtmf.cpp`（`ANegotiatedTelephoneEventIsNotCountedAsUnknownAudio`、`AnUnnegotiatedTelephoneEventIsCountedAsUnknownAudio`、`AMalformedTelephoneEventIsCountedSeparately`） |
>
> 公开面：`NimRTCEngine::negotiated_dtmf_payload_type()` 与 `Stats` 的 `dtmf_packets_sent` / `dtmf_packets_received` / `dtmf_packets_malformed` / `dtmf_events_delivered` / `dtmf_payload_type`。文档见 `docs/api/engine_api.md` §8.5。

### Track X — RTCP XR / TMMBR (item 9)

| ID | Item | Landing | Acceptance |
|---|---|---|---|
| **X1** | **XR 块解析**：为 `src/modules/rtp/` 增加 XR 块的解析，覆盖 RFC 3611 §6.2 登记的块类型中真正有 QoE / 网络统计价值的三类：**Receiver Reference Time（BT=4）**、**DLRR（BT=5）** 与 **Statistics Summary（BT=6）**。RFC 3611 中**不存在**名为 "Receiver Estimated Quality" 的块类型——那是早期草案的编号，按草案编号发出的报告无法与合规对端互通，因此本计划一律以 RFC 的实际编号为准。解析器落在模块内，引擎通过插件接口消费（遵守 L4）。 | `M` + `P` | 一段构造的 XR 块能被解析出块类型与字段；未知块类型被安全跳过而非报错 |
| **X2** | **XR 块构造**：引擎能生成 Receiver Reference Time、DLRR 与 Statistics Summary 报告并发出。 | `M` + `E` | 引擎能发出可被对端解析的 XR 报告；DLRR 的子报告时间戳正确 |
| **X3** | **TMMBR 构造与解析**：实现 RTPFB FMT=3（TMMBR）与 FMT=4（TMMBN）的载荷，处理 RFC 5104 §4 的 ssrc / bitrate 对列表。 | `M` | 一个带多个 (ssrc, bitrate) 对的 TMMBR 能被往返构造与解析，结果一致 |
| **X4** | **TMMBR 与码率控制器的接线**：引擎发出的 TMMBR 必须反映**当前实际生效的码率**，与 BWE 的估计一致——否则对端会按一个我们并未遵守的码率调整。 | `E` | 引擎发出的 TMMBR 携带的码率与 BWE 当前的 `target_bitrate_bps` 一致；带宽下降后发出的 TMMBR 随之下降 |
| **X5** | **`disable_tmmbr` 开关接通**：把 `plugins::SdpCodecConfig::disable_tmmbr`（目前是死字段）接到引擎的 munging / 发送路径上，使宿主能真正关闭 TMMBR。同时新增 `EngineConfig::tmmbr_enabled`（默认 `false`）——**TMMBR 是显式 opt-in**：`tmmbr_enabled=false` 时引擎一条 TMMBR 都不发，`disable_tmmbr` 覆盖 `tmmbr_enabled`（两者同设为真 → 完全关闭）。入站方向**完整执行**：解析对端 TMMBR → 回 TMMBN + 把本端发送码率上限压到对端给的值（**编码器、pacing、调度器预算三处都压**，见 OD-E 决定 3）。 | `E` + `M` | 设置 `disable_tmmbr` 后引擎不再发出 TMMBR；未开启 `tmmbr_enabled` 时行为不变（一条都不发）；入站 TMMBR 会收紧 `peer_tmmbr_limit_bps()` 并触发 TMMBN；入站 TMMBR 会把**编码器**码率压到限额以下（**包括压到 `audio_min_bitrate_bps` 以下**） |
| **X6** | **文档口径**：`docs/api/engine_api.md` 中对 RTCP 反馈面的描述要写明支持哪些 PT 与哪些块类型，避免宿主误以为 XR 已被完整支持。 | `D` | 文档中的 RTCP 支持矩阵与代码一致 |

> **X1 已落地（v1.2.0，2026-11-11）。** 验证矩阵：
>
> | 工作项 | 落地内容 | 验证 |
> |---|---|---|
> | **X1** | `plugins::IXrReport` 接缝（`src/plugins/include/nimrtc/plugins/xr_report.hpp`）+ `src/modules/rtp/` 的 RFC 3611 解析与构造（`rtp/rtcp_xr.hpp`）+ 模块侧适配器（`rtp/xr_report_plugin.hpp`）；引擎通过注册表的 XR 槽位解析 consumer，把入站 RTCP 的 PT 207 子包路由给它 | `src/modules/rtp/tests/test_rtcp_xr.cpp`（线格式）+ `src/modules/rtp/tests/test_xr_report_plugin.cpp`（接缝）+ `tests/test_engine_xr_report.cpp`（引擎面） |
>
> 覆盖的块类型：**BT=4**（Receiver Reference Time）、**BT=5**（DLRR）、**BT=6**（Statistics Summary）。其余 RFC 3611 §6.2 登记的块类型（Loss RLE BT=1、Duplicate RLE BT=2、Packet Receipt Times BT=3、VoIP Metrics BT=7）**已识别但未解析**——它们在块遍历中按长度安全跳过，不是错误。
>
> 公开面：`EngineConfig::xr_report_name`（默认 `"nimrtc_xr"`）、`NimRTCEngine::xr_report()`（宿主读 `snapshot()` 的解码结果）、`Stats::xr_report_blocks`（路由计数器）。
>
> **编号口径的更正**：本计划最初写的是 "Receiver Estimated Quality（BT=1）与 DLRR（BT=6）"。RFC 3611 §6.2 中**不存在**名为 "Receiver Estimated Quality" 的块类型——那是早期草案的编号，草案里 BT=1 是 "Receiver Estimated Quality"、BT=6 是 DLRR；RFC 的实际登记编号是 BT=1 = Loss RLE、BT=4 = Receiver Reference Time、BT=5 = DLRR、BT=6 = Statistics Summary。按草案编号发出的报告无法与合规对端互通，故实现与文档一律以 RFC 编号为准（OD-C 的决定同样据此更新）。

> **X2 / X3 / X4 / X5 / X5b / X6 已落地（v1.2.0，2026-11-11）。** 验证矩阵：
>
> | 工作项 | 落地内容 | 验证 |
> |---|---|---|
> | **X2** | XR 块的构造（`rtp::build_xr` / `build_xr_receiver_reference_time` / `build_xr_dlrr` / `build_xr_statistics_summary`）；引擎的 `maybe_send_extended_report()` 按 RTCP 间隔发出 BT=4 / BT=5 / BT=6 | `src/modules/rtp/tests/test_rtcp_xr.cpp`（线格式）+ `tests/test_engine_xr_report.cpp`（引擎面）+ `tests/test_engine_xr_tmmbr.cpp`（**线上字节**：RRTR 存在、DLRR 的 LRR / DLRR 正确、Statistics Summary 只带对端点名的字段） |
> | **X3** | TMMBR / TMMBN 的构造与解析（RTPFB FMT=3 / FMT=4，RFC 5104 §4.2）；FCI 条目的 (ssrc, bitrate) 列表 | `src/modules/rtp/tests/test_rtcp_xr.cpp` |
> | **X4** | 引擎的 TMMBR 携带 `bwe_->estimate().target_bitrate_bps`——即**当前实际生效**的码率，与 BWE 一致；码率变化时在下一个 RTCP 槽位重发，未变化时每 5 个间隔刷新一次 | `tests/test_engine_tmmbr.cpp` + `tests/test_engine_xr_tmmbr.cpp`（线上 TMMBR 的 FCI 码率 == BWE 目标码率） |
> | **X5** | `plugins::SdpCodecConfig::disable_tmmbr` 的死字段被引擎侧 `EngineConfig::disable_tmmbr` 接管；入站 TMMBR → `peer_tmmbr_limit_bps()` 收紧调度器预算、pacing 速率**与音频编码器**，并回 TMMBN 确认 bounding set（RFC 5104 §4.2.2.2）；对端限额**覆盖** `audio_min_bitrate_bps` 本地下限（OD-E 决定 3，peer-wins），且即使 `audio_bitrate_adaptive = false` 也生效 | `tests/test_engine_tmmbr.cpp` + `tests/test_engine_xr_tmmbr.cpp`（TMMBN 的 owner SSRC 与"一批 TMMBR 只回一条 TMMBN"）+ `tests/test_engine_audio_bitrate.cpp`（**编码器被压到限额以下 / 压到本地下限以下 / 钉死编码器时仍然生效 / 最紧限额胜出**） |
> | **X5b** | SDP 接线：offer 携带 `a=rtcp-fb:<pt> ccm tmmbr`（RFC 5104 §7.2）与 `a=rtcp-xr:rcvr-rtt=all stat-summary=loss,jitt`（RFC 3611 §5.1）；answer 回带协商后的子集；`sdp::MediaDescription` 新增 `rtcp_xr` / `rtcp_xr_present` 字段与解析器 / munger 支持 | `tests/test_engine_tmmbr.cpp` + `src/modules/sdp/tests/test_sdp.cpp` |
> | **X6** | 文档口径：`docs/api/engine_api.md` §8.6 / §8.7 / §8.8（XR 块矩阵、TMMBR 语义、完整 RTCP PT 支持矩阵）+ `docs/api/engine_config.md` §2.4a / §2.4b | 文档与代码逐条对照 |
>
> 公开面（新增 / 落地）：`EngineConfig::rtcp_xr_enabled`、`EngineConfig::tmmbr_enabled`、`EngineConfig::disable_tmmbr`、`NimRTCEngine::xr_reports_sent_count()`、`peer_tmmbr_limit_bps()`、`last_tmmbr_bps()`、`tmmbr_sent_count()`、`tmmbr_received_count()`。
>
> **协商的对称性**：XR 与 TMMBR 的发送都以**对端的描述**为准（RFC 3611 §5.1「participants SHOULD NOT send XR blocks other than the ones indicated by the parameters」；RFC 5104 §7.2「only the joint subset of CCM parameters from the offer and answer may be used」）。规则在 offer 与 answer 两个方向上对称：对端描述里的 `stat-summary` 永远是**对我们要发的块**的请求，`ccm tmmbr` 永远是**我们要发的消息**的授权。
>
> **OD-E 的三个决定（owner 已决，2026-11-11）**：
>
> 1. **默认发送还是默认不发 → 默认不发（opt-in）。** 新增 `EngineConfig::tmmbr_enabled`（默认 `false`），宿主不显式开启时引擎一条 TMMBR 都不发。这条决定推翻了本计划初稿 X5 验收栏的「未设置时行为不变」——那句话原本指的是「`disable_tmmbr` 未设置时」的行为，而 TMMBR 本身的默认发送策略在本轮才定下来。`disable_tmmbr` 仍然保留，且**覆盖** `tmmbr_enabled`：两者同时为真 → TMMBR 完全关闭（连 SDP 的 `a=rtcp-fb:<pt> ccm tmmbr` 也不带）。
> 2. **收到对端 TMMBR 时要不要真的执行 → 完整执行。** 引擎完整实现入向路径：解析对端 TMMBR（RFC 5104 §4.2.1.1）→ 按 §4.2.2.2 批量回一条 TMMBN → 把本端的发送预算与 pacing 速率压到 `peer_tmmbr_limit_bps()`（调度器侧与 pacing 侧都取「BWE 估计」与「对端限额」的较小值）。入向路径**不需要**配置开关：对端发来的 TMMBR 总是被遵守——发 TMMBN 就是一份承诺，承诺了就要兑现。
> 3. **对端限额与本地下限冲突时谁赢 → 对端赢（peer-wins）。** `EngineConfig::audio_min_bitrate_bps`（默认 8000）存在的目的是让**拥塞塌陷**不至于饿死音频流，它保护的是本端流。而对端的 TMMBR 限额是另一类约束：引擎回 TMMBN（RFC 5104 §4.2.2.2）等于确认了这个 bounding set，**回一条 TMMBN 就是一份承诺**。一份「少发」的承诺不是发送方可以悄悄突破的，所以对端限额**覆盖**本地下限——当对端要的比本地下限还低时，编码器允许降到本地下限**以下**。
>
>    该决定的落地范围是**三处**（缺一则承诺不成立）：
>
>    | 位置 | 落地内容 |
>    |---|---|
>    | `maybe_update_audio_bitrate()` | 编码器上限 = `min(audio_bitrate_ceiling_bps_, peer_limit)`；下限 = `min(audio_min_bitrate_bps, peer_limit)`；**有效估计** = `min(BWE 估计, peer_limit)` |
>    | `update_pacing_rates()` | pacing 的音频 class 速率同样被 `peer_limit` 夹住（编码器降速是限速的，pacer 不能在过渡期放行旧速率） |
>    | `audio_bitrate_adaptive = false` | 该开关只表示「不因**估计**移动编码器」，**不豁免**线上已经许下的承诺——限额依然生效 |
>
>    另有一条实现细节：一旦编码器当前速率已经**超过**对端限额（即承诺已被破坏），降速**不再走**「每步至多 -50 %」的限速，而是**一步到位**降到限额——限速降速会让承诺在接下来的若干秒里持续被违反。

---

## 3. 依赖关系

```
N1 (引擎接线 session 模块)
  ├── N2 (协商触发) ── N3 (m-line 映射) ── N4 (方向映射)
  └── T1 (transceiver 模型) ── T2 (多 m-line) ── T3 (收发分离) ── T4 (停用/重启)

D1 (DTMF 接收) ── D2 (DTMF 发送) ── D3 (SDP 声明) ── D4 (计数口径)

X1 (XR 解析) ── X2 (XR 构造)          X3 (TMMBR 往返) ── X4 (接线码率) ── X5 (开关)
```

- **N1 是关键路径**。没有它，T1 的 transceiver 就是一个没有消费者的类型
  （这正是 `src/modules/session/` 今天的处境，见 §1.2.1）。
- **D 组完全独立**，不依赖 N 组也不依赖 X 组，可以并行。
- **X 组独立**，但 X4 依赖码率控制器的当前估计值可读——该能力在
  `docs/plan/video-pipeline-v2-plan.md` 的 B 轨（B5）落地后才稳定。

---

## 4. 里程碑与待决事项

**版本落位（owner 决定，2026-11-10）：本计划的全部工作项落在 `v1.2.0`。**

| 里程碑 | 工作项 | 理由 |
|---|---|---|
| **v1.2.0** | N1, N2, N3, N4, N5 | N 组是**接线**而非新建：数据模型已就位（§1.2.1），接上刚落地的 C9/C11 协商结构（`VideoNegotiationResult`）即可，留在 v1.2.0 与视频 B 轨同批落地 |
| **v1.2.0** | T1, T2, T3, T4 | Transceiver / Unified Plan 依赖 N1；两者同属 v1.2.0 时，T1 在 N1 落地后即可开工，不必跨版本等待 |
| **v1.2.0** | D1, D2, D3, D4 | DTMF 是纯增量路径：不碰现有音频解码，只加一条出口；RFC 4733 互操作面窄，验证成本低 |
| **v1.2.0** | X1, X2, X3, X4, X5 | XR / TMMBR 是协议侧增量。**X4 的前置（码率控制器）在同一版本内满足**——`video-pipeline-v2-plan.md` 的 B 轨（B5 统一编码参数计算）已排在 v1.2.0，因此 X4 不必再推一版 |
| **v1.2.0** | X6 | 文档口径随代码同版本更新 |

**这条压缩的代价，owner 需要知情：** v1.2.0 现在同时装着视频 B 轨（F / P / B）、音频 P0+P1（WP-0 … WP-11）、C9 / C11、usrsctp 移除、SM4-GCM SRTP，以及本计划的 N / T / D / X 四组。v1.2.0 的验收窗口会很紧；若中途发现 T 组（Unified Plan 的 Chrome 互操作验证）吃不下工作窗口，可执行项是把 **T 组**整体推到 v1.3.0——N / D / X 三组是独立轨道，砍掉 T 组不影响其余三组的落地。

### 待 owner 决策（规格里没写的行为，不自己选）

| ID | 决策点 | 选项 | 影响 |
|---|---|---|---|
| **OD-A** | **轨道被移除时，SDP 上怎么表现？** | (a) 整条 m-line 从 offer 中消失；(b) 保留 m-line 但标为 `inactive`；(c) 保留 m-line 但端口置 0 | 决定 N3/N5 与 T4 的实现形状。libwebrtc 在 Unified Plan 下倾向 (b)/(c)，但 NimRTC 是否要跟需要 owner 定 |
| **OD-B** | **DTMF 是否随引擎默认开启？** | (a) 默认关闭，宿主显式开；(b) 默认开启 | 影响 SDP 默认形状与 §2 的默认流量 |
| **OD-C** | **XR 要覆盖哪些块类型？** | (a) 只做 BT=4 + BT=5 + BT=6（Receiver Reference Time / DLRR / Statistics Summary，X1 建议的最小集）；(b) 覆盖全部 RFC 3611 块类型 | 影响 X1/X2 的工作量与 X6 的支持矩阵 |

### 已决策（owner，2026-11-10）

三个决策点全部落定，实现按下列语义执行。

| ID | **决定** | 落地含义 |
|---|---|---|
| **OD-A** | **(b) 保留 m-line 但标为 `inactive`** | 轨道被移除时，m-line 仍留在 offer / answer 中，方向属性写 `inactive`；mid **不回收**，BUNDLE 组保持合法（组内 mid 仍然存在）。N3 / N5 / T4 按此实现。**理由**：mid 稳定性是 Unified Plan 的核心不变式——回收 mid 会让对端把一条已建立的 m-line 误认为"新增的"，触发一次不必要的重协商；`inactive` 在语义上恰好表达"这条轨道本轮不收发"。 |
| **OD-B** | **(a) 默认关闭，宿主显式开启** | `EngineConfig` 新增 `dtmf_enabled`（默认 `false`）。默认情况下 offer **不**携带 `a=rtpmap:<pt> telephone-event/8000`，telephone-event 负载类型仍走 §1.2.3 的既有丢弃路径（计入 `kAudioPayloadTypeUnknown`）。宿主显式开启后才声明、才解码。**理由**：DTMF 是可选能力，默认开启会让每一份 offer 都多出一条属性，而绝大多数会话用不到它。 |
| **OD-C** | **(a) 只做 BT=4 + BT=5 + BT=6** | XR 实现覆盖 Receiver Reference Time（BT=4）、DLRR（BT=5）与 Statistics Summary（BT=6）；其余 RFC 3611 块类型（Loss RLE BT=1、Duplicate RLE BT=2、Packet Receipt Times BT=3、VoIP Metrics BT=7）在解析时**安全跳过**而非报错。X6 的支持矩阵只列这三类。**理由**：这三类是 QoE / 网络统计侧真正会收到的；其余块类型目前无人消费，做了也只是死代码。**注**：本决策最初写的是 "BT=1 + BT=6"，那用的是早期草案的编号（"Receiver Estimated Quality"）——RFC 3611 §6.2 的实际登记编号是 BT=1 = Loss RLE、BT=5 = DLRR、BT=6 = Statistics Summary，**不存在**名为 "Receiver Estimated Quality" 的块类型；按草案编号发出的报告无法与合规对端互通，故此处以 RFC 编号为准。 |

> **OD-A 的推论**：N5 的"停用轨道"与 N4 的"改方向"在 SDP 上**同形**——两者都表现为 m-line 的方向属性变为 `inactive`。区别在语义层：改方向是这条轨道下一轮可能恢复，停用是这条轨道已被移除。实现上由"轨道是否仍在 session 的轨道集合中"区分。

### 决策带来的验收口径变化

| 工作项 | 原验收 | 决策后的验收 |
|---|---|---|
| **N3** | 增删轨道后 mid 集合与既有 mid 无冲突 | **不变**，并追加：被移除轨道的 mid **仍在** mid 集合中（OD-A） |
| **N5** | 行为在 `docs/api/engine_api.md` 中写明，且有一个测试固定该行为 | **具体化**：被移除轨道的 m-line 保留、方向为 `inactive`、mid 不回收；该行为在 `docs/api/engine_api.md` 写明并由测试固定 |
| **T4** | 停用后 m-line 变为 `inactive`（或按 OD-A 消失） | **具体化**：停用后 m-line **保留**且方向为 `inactive`（OD-A 选定 (b)，"或消失"的分支不再适用） |
| **D3** | offer 携带 telephone-event 声明 | **条件化**：仅当 `EngineConfig::dtmf_enabled == true` 时 offer 才携带（OD-B 选定默认关闭） |
| **D4** | 两种协商状态下 unknown 计数行为不同 | **不变**，且默认态固定为"未协商 DTMF" |
| **X1 / X2** | 覆盖 BT=1 与 BT=6 | **具体化**：覆盖 BT=4（Receiver Reference Time）、BT=5（DLRR）与 BT=6（Statistics Summary）——原验收里的 "BT=1" 指的是早期草案的编号，RFC 3611 的实际编号见 OD-C |
| **X6** | 支持矩阵与代码一致 | **具体化**：矩阵中 XR 一栏只列 BT=4 / BT=5 / BT=6，其余块类型标注为"已识别但未解析" |

---

## 5. 明确不做的事

| 不做 | 理由 |
|---|---|
| 在 `src/engine/` 中直接 `#include <nimrtc/session/…>` | 违反 L4（引擎只做插件编排，不包含具体模块头）。N1 必须走 `src/plugins/` 的接口 |
| 把 transceiver 做成 SDP 层的语法糖 | transceiver 是**会话对象**（有生命周期、可停可重启），不是 SDP 属性。放进 `sdp::MediaDescription` 会把生命周期语义塞进一个纯数据模块 |
| 复刻 libwebrtc 的 `RTCPeerConnection` 对象模型 | NimRTC 是 C++ 库，宿主不是浏览器。本计划的目标是**能力对齐**，不是 API 逐项复刻 |
| 为 DTMF 新建一个编解码插件类型 | telephone-event 不是音频，混进 `IAudioDecoder` 会污染现有音频路径。走 D 组的独立出口 |
| 在 X4 之前发出 TMMBR | 发出一个我们并不遵守的码率比不发出更糟——见 X4 的验收标准 |
