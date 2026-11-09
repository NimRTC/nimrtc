# Plan: getStats 完整面（会话能力第 8 项单独立项）

| | |
|---|---|
| Status | **open — 待排期** |
| Plan opened | 2026-11-09 |
| Baseline | `main` @ post-v1.1 consolidation |
| Scope class | 观测面（observability）— 纯增量统计字段与查询面，不改 wire format |
| Related | `docs/plan/video-pipeline-v2-plan.md` 的 **O1**（逐流可观测面，落在 v1.2.0）、**R1**（`video_stats` 接入引擎，落在 v1.2.0）、`docs/plan/audio-pipeline-dev-plan.md` 的 **WP-8 / B-13**（音频统计计数器，v1.2.0）、`docs/plan/session-capability-plan.md`（会话能力第 5/6/7/9 项） |

---

## 1. 为什么单独立项

会话与信令能力对比表里，getStats 是 9 项中唯一**已有部分实现、但没有完整验收口径**的一项：它既不是"完全没有"，也不是"完整"。表格里旧备注写的"只有 video_stats"已经不成立，而"完整的 getStats 面"到底包含什么、验收标准是什么，仓库里此前没有任何文档定义过。

O1（在 `video-pipeline-v2-plan.md`）定义的是**逐流可观测面**：实际码率、目标码率、当前阶梯档位、pacing 速率、队列深度、关键帧率、编解码错误、冻结计数、A/V skew，全部可从 engine 查询。它是**面向视频质量闭环的内部遥测**，不是 libwebrtc 那种"给宿主看的标准统计报告"。

本计划单独立项的是后者：**一个宿主可以查询的、有明确字段口径的、跨音视频统一形状的统计面**。两者关系见 §5。

---

## 2. 现状核实（2026-11-09，代码为证）

### 2.1 引擎已有的统计面

`NimRTCEngine::stats()`（声明 `src/engine/include/nimrtc/engine/engine.hpp:1385-1435`，实现 `src/engine/src/engine_media.cpp:1328`）返回的 `Stats` 是一个**扁平计数器结构体**，字段分四组：

| 组 | 字段 | 来源 |
|---|---|---|
| SRTP | `srtp_drops` | `impl_->srtp_stats_drop` |
| 视频发送/接收 | `video_frames_sent` / `video_packets_sent` / `video_bytes_sent` / `video_frames_received` / `video_nack_seqs_queued` / `video_keyframes_q` / `video_feedback_applied` / `video_keyframes_applied` | v1.1 Track V（`docs/plan/v1.1-plan.md` 的 V1） |
| 音频发送 | `audio_frames_sent` / `audio_bytes_sent` / `audio_encode_errors` / `audio_samples_in` / `audio_samples_pending` | 音频管线 WP-5 |
| 音频接收 | `audio_packets_sent` / `audio_frames_received` / `audio_samples_received` / `audio_concealed_frames` / `audio_fec_recovered_frames` / `audio_packets_lost` / `audio_decode_errors` / `audio_jitter_ms` / `audio_playout_delay_ms` | 音频管线 WP-8（spec B-13） |
| 自适应码率 | `audio_encoder_bitrate_bps` | BWE 链路 |

结论：**引擎层已经有视频和音频两套累计计数器**，不再是"只有 video"。

### 2.2 但 `video_stats` 模块根本没接进引擎

`src/modules/video_stats/` 存在，接口完整（`VideoStreamStats` 含帧计数、码率、冻结计数、QP、时间戳边界，`IVideoStatsSink` 含推接口与 `snapshot()` / `reset()`），并有单测。

但是：

```
Grep "video_stats|VideoStreamStats|IVideoStatsSink"  →  src/engine/  零命中
```

`src/engine/` 里既不引用这个模块，也不引用它的类型。**`video_stats` 提供了冻结检测、关键帧间隔、QP 统计、丢帧归因（JB / decoder 两类）这些 `Engine::Stats` 完全没有的字段，但引擎拿不到。**

`video-pipeline-v2-plan.md` 的 R1 正是这件事（"`video_stats` wired into the engine"），里程碑 v1.2.0，状态未完成。

### 2.3 插件层已有的统计面（散落、无法统一查询）

各插件自己的 `stats()` 已经在位：

| 插件 | 接口 | 提供的字段 |
|---|---|---|
| RTP | `plugins/rtp.hpp:152` | `packets_in` / `bytes_in` / `packets_out` / `bytes_out` / `parse_errors` / `last_seq` / `last_recv_us` |
| 抖动缓冲 | `plugins/jb.hpp:95` | `buffer_level_ms` / `target_level_ms` / `frames_out` / `frames_dropped` / `packets_received` / `jitter_ms` |
| 音频接收 | `plugins/audio_receiver.hpp:158` | `packets_received` / `bytes_received` / `frames_played_out` / `frames_concealed` / `frames_fec_recovered` / `packets_lost` / `decode_errors` / `current_delay_ms` / `jitter_ms` |
| 音频解码 | `plugins/audio_decoder.hpp:140` | `frames_decoded` / `bytes_decoded` / `plc_frames` / `fec_recovered_frames` / `decode_errors` |
| 调度器 | `plugins/scheduler.hpp:103` | `queued_packets` / `dropped_packets` / `sent_packets` / `sent_bytes` / `target_bitrate_bps` |
| BWE | `plugins/bwe.hpp:154` | `BweEstimate`：`target_bitrate_bps` / `pacing_bitrate_bps` / `timestamp` / `reason` |
| 视频编解码 | `plugins/video_codec.hpp:144` | `VideoCodecStats` |
| 视频 sink | `plugins/video_sink.hpp:87` | `VideoSinkStats` |
| 3A | `plugins/audio3a.hpp:164` | `audio3a::Stats` |

问题不在于数据缺失，而在于：

1. **没有统一的聚合入口**。宿主要拿"当前会话的完整画面"，得自己逐个 accessor 取（`bwe()` / `scheduler()` / `video_source()` / `video_sink()` / …），而且要自己知道每个插件的 stats 里有什么。
2. **字段口径不统一**。同一概念在不同插件里名字不同：`jb::Stats::frames_dropped` vs `video_stats` 的 `frames_dropped_jb` / `frames_dropped_decoder`（后者区分了丢弃原因，前者不区分）；`audio_receiver::Stats::jitter_ms` 与 `Engine::Stats::audio_jitter_ms` 同名同义但来源不同。
3. **无时间戳**。所有统计都是自 `open()` 起的累计值，宿主无法知道"这个数是什么时候的"，也无法算出区间速率（except 少数自带 EMA 的字段）。
4. **无标准形状**。libwebrtc 的 `RTCStatsReport` 是一组带 `type` / `id` / `timestamp` 的条目（`inbound-rtp` / `outbound-rtp` / `candidate-pair` / `codec` / …）。NimRTC 是一个扁平结构体，宿主拿不到"每个流一条记录"的形状，多流场景下无法区分。

### 2.4 缺失的统计维度

| 维度 | 现状 |
|---|---|
| ICE candidate-pair（RTT、选中对、可用对） | 无统计接口 |
| RTCP 反馈收发计数（PLI / NACK / REMB / transport-cc） | `video_feedback_applied` 只有一个总数，不区分类型 |
| 丢包率 / 抖动（视频侧） | 无。音频侧有 `audio_packets_lost` / `audio_jitter_ms` |
| 远端反馈（RTT / fraction lost） | 无。引擎**发送** RR（`engine_rtcp.cpp:240` 的 `RrBuilder`），但不消费对端 RR 里的 RTT |
| 编解码 / 渲染耗时 | 无 |
| 端到端延迟 | 无（ADR-017 的 timeline 提供了捕获时间戳的换算基础，但没接到统计面上） |

---

## 3. 工作项

沿用 `video-pipeline-v2-plan.md` 的编号与表格格式。落点标记：`E` = 改 engine，`M` = 新增/改模块，`X` = 工具/测试侧。

### Track S — 统计面统一

| ID | Item | Landing | Acceptance |
|---|---|---|---|
| **S1** | **统计快照加时间戳与区间语义**：`Engine::Stats` 增加 `snapshot_time_us`（engine 单调原点，与 `tick()` 的 `now_us` 同源），并新增一个 `StatsDelta` 或等价机制，让宿主能算出区间速率，而不只是自 `open()` 起的累计值。字段**加法式**扩展，不改动已有字段含义。 | `E` | 两次 `stats()` 之间的差值等于两次调用之间实际发生的增量；对 `snapshot_time_us` 的断言能区分"同一时刻"与"不同时刻" |
| **S2** | **插件统计统一聚合入口**：新增一个聚合查询面，把 §2.3 表格里的插件统计一次性收进一份快照，宿主不必逐个 accessor 取。字段名冲突按"最具体者胜出"处理（如 `frames_dropped` 拆为 `frames_dropped_jb` / `frames_dropped_decoder`），并在文档里给出**字段 → 来源插件 → 含义**的三列表。 | `E` + `M`（若需新增插件侧聚合接口，放 `src/plugins/` 的接口声明，实现落在模块侧） | 一次查询返回 §2.3 表格中所有插件的统计字段；字段来源表在 `docs/api/engine_api.md` 中与代码一致 |
| **S3** | **`video_stats` 接入引擎（R1 的执行）**：引擎创建 `IVideoStatsSink` 实现，喂入帧到达 / 解码 / 渲染 / 丢弃 / RTCP 反馈 / QP 样本，`snapshot()` 的结果并入统计面。这是冻结计数、关键帧间隔、QP、按原因区分的丢帧的唯一来源。 | `M`（模块已存在）+ `E`（引擎侧接线） | 引擎统计面能给出 `freeze_count` / `freeze_total_ms` / `keyframe_interval_avg_ms` / `qp_sum` / `qp_count`；注入一次帧丢失后 `frames_dropped_*` 相应增长且原因区分正确 |
| **S4** | **区分 RTCP 反馈类型**：把 `video_feedback_applied` 这一个总数拆成按类型的计数（PLI / FIR / NACK / REMB / transport-cc），让宿主能判断"对端到底在要什么"。 | `E`（计数点已存在，`engine_rtcp.cpp`） | 注入一次 PLI 后，PLI 计数 +1 而 NACK 计数不变；未收到的反馈类型为 0 |

### Track R — 报告形状与传输层维度

| ID | Item | Landing | Acceptance |
|---|---|---|---|
| **S5** | **流级记录形状**：为多流场景提供"每个流一条记录"的查询形状（每条记录含流标识、方向、上述字段），而不是一个全局扁平结构体。libwebrtc 的 `RTCStatsReport` 是参照物，但**不追求 wire 兼容**——NimRTC 是 C++ 库，不做 W3C 形状的逐字段复刻。 | `E` | 两路流同时存在时，查询能分别返回各自的计数，不会混在一个总数里 |
| **S6** | **ICE candidate-pair 统计**：暴露选中候选对的地址 / 协议 / RTT，以及候选对的可用性状态。RTT 取自引擎已有的 DTLS / SRTP / RTCP 反馈面，**不新增探针**。 | `E` + `M`（若 ICE 模块侧无该接口，在 `src/plugins/` 声明、在模块侧实现） | 选中对可查询到地址与 RTT；未选中的候选对标记为不可用 |
| **S7** | **对端反馈消费**：消费对端 RR 里的 RTT 与 fraction lost，纳入统计面。这是引擎当前完全没读的一类信息（只发不收）。 | `M`（`src/modules/rtp` 已有 `parse_rr`）+ `E`（接线） | 收到带 RTT 的 RR 后，统计面能给出该 RTT 值与 fraction lost |
| **S8** | **视频侧丢包与抖动**：音频侧已有 `audio_packets_lost` / `audio_jitter_ms`，视频侧补齐对等字段。 | `E` | 视频接收侧在丢包与抖动变化时，统计面数值相应变化 |

### Track D — 文档与口径

| ID | Item | Landing | Acceptance |
|---|---|---|---|
| **D1** | **`docs/api/engine_api.md` 的 Stats 段落重写**：当前该文档展示的 `struct Stats` 只有一个 `srtp_drops` 字段，与代码相差 30 余个字段，必须更新为真实字段清单，并按 §2.1 的四组分节说明。 | `D` | 文档中列出的字段与 `engine.hpp` 中的字段一一对应，无遗漏无多余 |
| **D2** | **对外能力表口径修正**：任何描述"getStats 只有 video_stats"的表述改为准确描述——引擎已有视频与音频两套累计计数器，但缺标准报告形状、缺时间戳、缺流级记录、缺 `video_stats` 模块的冻结 / QP / 按原因丢帧数据，且尚未暴露 ICE / RTT 维度。 | `D` | 文档中不再出现与代码不符的"只有 video_stats"表述 |
| **D3** | **字段口径表**：为统计面每个字段写明"单位 / 累计还是瞬时 / 来源 / 何时更新"，避免宿主把 EMA 字段当累计值用。 | `D` | 每个公开统计字段在口径表中有对应一行 |

---

## 4. 里程碑建议

| 里程碑 | 工作项 | 理由 |
|---|---|---|
| **v1.2.0** | S1, S2, S3, S4, D1, D2, D3 | S3 是 R1 的执行、已属 v1.2.0；S1/S2/S4 与 D 组是纯增量、低风险。S3 的接线依赖 O1 的遥测字段落地，二者同属 v1.2.0 可避免跨版本返工 |
| **v1.3.0** | S5, S7, S8 | 流级记录形状（S5）需要 S1/S2 的快照基础设施稳定后再做，否则要改两次；S7 / S8 是协议侧增量 |
| **v1.4.0** | S6 | candidate-pair 统计涉及 ICE 模块接口扩展，跨版本改动成本更高 |

**与 `session-capability-plan.md`（会话能力第 5/6/7/9 项）的依赖关系**：本计划与该计划**无双向依赖**。Renegotiation（第 5 项）会改变流的增删方式，届时 S5 的流级记录形状需要支持动态增删的流——因此**S5 排在 v1.3.0 而非 v1.2.0**，且实现时应预留流记录可增删的结构，而不是固定数组。DTMF（第 7 项）不产生统计面需求。Transceiver（第 6 项）会引入 transceiver 级方向概念，S5 的"方向"字段取值应与之对齐——但由于 S5 排在 v1.3.0、Transceiver 排期更晚，S5 实现时只需保证字段可扩展，不必现在等待。

---

## 5. 与 O1 的边界

| | O1（`video-pipeline-v2-plan.md`，v1.2.0） | 本计划 |
|---|---|---|
| 目的 | 服务视频质量闭环的**内部控制决策**（码率控制器需要知道当前档位、pacing、冻结情况） | 服务**宿主观测**（宿主要知道会话此刻健康吗） |
| 形状 | 逐流可观测面，偏视频 | 跨音视频统一，含 ICE / RTT / 传输层维度 |
| 消费方 | 引擎内部的码率控制器与 benchmark 工具（O2 / O5） | 宿主应用、运维监控 |
| 重叠 | S3（`video_stats` 接入引擎）是两者的公共前置 | O1 依赖它拿冻结计数 |

**结论**：O1 是本计划的**子集与前置**，不重复立项。O1 已定义并已排期 v1.2.0 的字段不再在本计划里重写；本计划只补 O1 未覆盖的部分（音频侧、视频侧丢包/抖动、ICE candidate-pair、RTT、流级形状、时间戳、字段口径）。

---

## 6. 明确不做的事

| 不做 | 理由 |
|---|---|
| 复刻 W3C `RTCStatsReport` 的 wire 格式 / JSON schema | NimRTC 是 C++ 库，宿主不是浏览器；逐字段复刻的收益低于其维护成本。参照其**形状**（条目化、带 timestamp）即可 |
| 把统计面做成可订阅的推送式事件流 | 拉取式快照已能满足观测需求；推送式会引入新的线程模型与生命周期约束（`engine_api.md` §15 的线程模型是本计划的风险面） |
| 用统计面驱动自适应决策 | 决策输入属于 O1 / BWE 的职责。统计面是**只读观测**，不得成为控制回路的一环，否则可观测性与控制耦合 |
| 把 `video_stats` 模块合并进引擎 | 违反 L4（engine 只编排）。模块留在 `src/modules/video_stats/`，引擎只接线 |
