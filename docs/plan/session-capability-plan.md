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
> | Status | **Gap analysis complete; work items written; version placement partially owner-decided (§4)** |
> | Owner | BDFL |
> | Related | `docs/plan/video-pipeline-v2-plan.md`（**C9 / C11** 已落地，v1.2.0）、`docs/plan/stats-surface-plan.md`（会话能力第 8 项，O1 / M2）、`docs/plan/datachannel-capability-plan.md`、`docs/plan/v1.1-plan.md`、ADR-015 (API stability) |

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
  PLI / FIR / SLI / REMB. The XR-specific block types — Receiver Estimated
  Quality, DLRR, Duplicate Arrival Notification, Statistics Summary,
  Virtual Time, IDMS — have no representation at all.
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

### Track X — RTCP XR / TMMBR (item 9)

| ID | Item | Landing | Acceptance |
|---|---|---|---|
| **X1** | **XR 块解析**：为 `src/modules/rtp/` 增加 XR 块的解析，至少覆盖 Receiver Estimated Quality（BT=1）与 DLRR（BT=6）两类——它们是 QoE 侧真正会收到的。解析器落在模块内，引擎通过插件接口消费（遵守 L4）。 | `M` + `P` | 一段构造的 XR 块能被解析出块类型与字段；未知块类型被安全跳过而非报错 |
| **X2** | **XR 块构造**：引擎能生成 Receiver Estimated Quality 与 DLRR 报告并发出。 | `M` + `E` | 引擎能发出可被对端解析的 XR 报告；DLRR 的子报告时间戳正确 |
| **X3** | **TMMBR 构造与解析**：实现 RTPFB FMT=3（TMMBR）与 FMT=4（TMMBN）的载荷，处理 RFC 5104 §4 的 ssrc / bitrate 对列表。 | `M` | 一个带多个 (ssrc, bitrate) 对的 TMMBR 能被往返构造与解析，结果一致 |
| **X4** | **TMMBR 与码率控制器的接线**：引擎发出的 TMMBR 必须反映**当前实际生效的码率**，与 BWE 的估计一致——否则对端会按一个我们并未遵守的码率调整。 | `E` | 引擎发出的 TMMBR 携带的码率与 BWE 当前的 `target_bitrate_bps` 一致；带宽下降后发出的 TMMBR 随之下降 |
| **X5** | **`disable_tmmbr` 开关接通**：把 `plugins::SdpCodecConfig::disable_tmmbr`（目前是死字段）接到引擎的 munging / 发送路径上，使宿主能真正关闭 TMMBR。 | `E` + `M` | 设置该开关后引擎不再发出 TMMBR；未设置时行为不变 |
| **X6** | **文档口径**：`docs/api/engine_api.md` 中对 RTCP 反馈面的描述要写明支持哪些 PT 与哪些块类型，避免宿主误以为 XR 已被完整支持。 | `D` | 文档中的 RTCP 支持矩阵与代码一致 |

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

## 4. 里程碑建议与待决事项

| 里程碑 | 工作项 | 理由 |
|---|---|---|
| **v1.3.0** | N1, N2, N3, N4, N5 | N 组是**接线**而非新建，风险低于 T 组，但需要 SDP 与引擎两条线同时动；排在 C9/C11 之后可以复用刚落地的协商结构（`VideoNegotiationResult` 那一层） |
| **v1.4.0** | T1, T2, T3, T4 | Transceiver / Unified Plan 是一次 SDP 语义的改变，涉及与 Chrome 的互操作验证，需要独立的验证窗口 |
| **v1.3.0** | D1, D2, D3, D4 | DTMF 是纯增量路径：不碰现有音频解码，只加一条出口；且 RFC 4733 的互操作面窄，验证成本低 |
| **v1.4.0** | X1, X2, X3, X5 | XR / TMMBR 是协议侧增量，不影响现有路径 |
| **v1.5.0** | X4 | TMMBR 接线依赖 B 轨的码率控制器稳定后再做，否则要返工 |

### 待 owner 决策（规格里没写的行为，不自己选）

| ID | 决策点 | 选项 | 影响 |
|---|---|---|---|
| **OD-A** | **轨道被移除时，SDP 上怎么表现？** | (a) 整条 m-line 从 offer 中消失；(b) 保留 m-line 但标为 `inactive`；(c) 保留 m-line 但端口置 0 | 决定 N3/N5 与 T4 的实现形状。libwebrtc 在 Unified Plan 下倾向 (b)/(c)，但 NimRTC 是否要跟需要 owner 定 |
| **OD-B** | **DTMF 是否随引擎默认开启？** | (a) 默认关闭，宿主显式开；(b) 默认开启 | 影响 SDP 默认形状与 §2 的默认流量 |
| **OD-C** | **XR 要覆盖哪些块类型？** | (a) 只做 BT=1 + BT=6（N1 建议的最小集）；(b) 覆盖全部 RFC 3611 块类型 | 影响 X1/X2 的工作量与 X6 的支持矩阵 |

---

## 5. 明确不做的事

| 不做 | 理由 |
|---|---|
| 在 `src/engine/` 中直接 `#include <nimrtc/session/…>` | 违反 L4（引擎只做插件编排，不包含具体模块头）。N1 必须走 `src/plugins/` 的接口 |
| 把 transceiver 做成 SDP 层的语法糖 | transceiver 是**会话对象**（有生命周期、可停可重启），不是 SDP 属性。放进 `sdp::MediaDescription` 会把生命周期语义塞进一个纯数据模块 |
| 复刻 libwebrtc 的 `RTCPeerConnection` 对象模型 | NimRTC 是 C++ 库，宿主不是浏览器。本计划的目标是**能力对齐**，不是 API 逐项复刻 |
| 为 DTMF 新建一个编解码插件类型 | telephone-event 不是音频，混进 `IAudioDecoder` 会污染现有音频路径。走 D 组的独立出口 |
| 在 X4 之前发出 TMMBR | 发出一个我们并不遵守的码率比不发出更糟——见 X4 的验收标准 |
