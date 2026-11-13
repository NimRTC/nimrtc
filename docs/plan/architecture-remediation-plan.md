# NimRTC 架构缺陷彻底整改计划

> 状态：**Phase 0 / 1 / 2 / 3 已完成**（决策已拍板，见 §0.0）；Phase 4（结构重构）进行中（§4.1 已落地），Phase 5（文档与 ADR 同步）待执行
> 制定日期：2026-11-12
> 基线：`split/6-h265-av1` @ `origin/split/6-h265-av1`（ahead 1）
> 范围：插件化契约完整性、性能热路径、结构分层、文档与代码一致性
>
> **本计划的判定标准只有一个：用户能否真正替换任意模块。**
> 每一条任务都必须能回答"它如何让一个第三方实现被引擎实际使用"。
>
> ## 进度快照（2026-11-13）
>
> | Phase | 内容 | 状态 | 关键产物 |
> |---|---|---|---|
> | 0 | 基线与注册表一致性守卫 | ✅ | `tools/benchmark_hotpath.py`、`tests/bench_engine_hotpath.cpp`、注册表契约守卫 |
> | 1 | 正确性（A1-A12） | ✅ | transport 空指针契约、注册表键生命周期、视频 RTP 统一、分辨率可配置、调度器锁外 I/O、S-family 流控 |
> | 2 | 插件契约补全（B1-B10、Q3/Q4） | ✅ | `ISrtpSessionBackend` 接线、`CapabilitySelector` 接线、配置项 / 宏名显式化、契约测试 |
> | 3 | 性能（C1-C7） | ✅ | 帧缓冲池化、GPU 零拷贝路径、逐包分配消除、`hotpath_zero_allocs` 与音频分配门禁 |
> | 4 | 结构重构（D1-D6、Q5） | 🔄 | §4.1 已完成：五个 seam 目录并入 `src/modules/`（include 路径与 target 名均不变）；余下 D1 引擎拆分、D2 死模块处置、D5/D6 |
> | 5 | 文档与 ADR 同步 | ⬜ | ADR-015 / ADR-017 与本次整改的同步 |
>
> Phase 3 的验收（§3.5）现状：
>
> - `send_video()` 1080p30 的 per-call 堆分配次数 = 0 —— ✅ 门禁
>   `hotpath_zero_allocs`（实测 `0.000000`，1920x1080 / 1280x720 / 854x480）
> - 音频路径 per-packet 堆分配次数 = 0 —— ✅ 门禁
>   `tests/test_engine_audio_alloc.cpp`（分配次数不随包数增长）
> - `tick()` p99 耗时相比基线下降 ≥ 30 % —— ✅ 基线 0.3 us → 0.2 us（-33 %）
> - 调度器在满载下的 `drain_with()` 不阻塞 `enqueue()` —— ✅ 基准第三段
>   （生产者 p99 即锁竞争足迹），并由 Phase 1.6 的锁外 I/O 修复保证
>
> Phase 4 §4.1（目录统一，Q5）现状：
>
> - `src/{dtls,raw_udp,sctp,transport,web_transport}/` 全部并入
>   `src/modules/` —— ✅ 旧目录已不存在
> - include 路径不变 —— ✅ `engine_impl.hpp` 的 include 行与 `main` 逐字节相同
> - CMake target 名不变 —— ✅ `nimrtc::{dtls,dtls_seam,sctp,raw_udp,transport,web_transport}`
> - DTLS 双实现归一 —— ✅ seam 头 + wolfSSL / GmSSL 两个后端同在
>   `src/modules/dtls/`
> - 全量 `ctest` —— ✅ 1411 / 1411（Windows MSVC Release）

---

## 0.0 已拍板的决策（2026-11-12）

| # | 决策 | 结论 | 影响 |
|---|---|---|---|
| Q1 | `select_next_packet` 保留还是删除？ | **保留并补齐流控**（不删除） | 见 §0.1 —— 该 API 是 v1.0.0 已冻结的 DoD 交付物，删除会破坏已发布契约；且远程桌面 M4 多流需要它 |
| Q2 | SDP / RTP 是否做成可替换？ | **先不替换** | 删除假配置项 `rtp_name` / `sdp_name`，诚实标注不可替换；接口暂时保留但标注 deprecated |
| Q3 | `ISrtpSessionBackend` 接线还是删除？ | **接线** | SRTP 后端可替换；新增 `EngineConfig::srtp_backend_name` + 注册表槽位 |
| Q4 | `CapabilitySelector` 接线还是删除？ | **接线** | 让 Selector 真正决定 media/control stack，兑现 `transport-strategies.md` |
| Q5 | 目录统一是否要做？ | **做，一次做彻底** | 五个 seam 目录并入 `src/modules/`，**无过渡期**，同一次提交内完成全部路径迁移 |

### 0.1 Q1 补充调查：为什么当初这样实现，删除的负面影响是什么

**用户的疑问**：Q1 的负面影响的确认，以及"为什么之前要这样实现"。

**为什么当初这样实现（历史背景）**：

`select_next_packet` 属于 **DC-2**（"L1 strict-priority scheduling across the DC plane"），由 `v0.11.0` 引入（commit `9016a39` / `d5f97fa`），并作为 **v1.0.0-beta.2 的 DoD 交付物**落地（`docs/plan/v1.0-plan.md:67,116,520,563`）。

设计意图是**正确的**：`drain()` / `drain_with()` 用的是 **P-family（每包优先级）**，只有 5 级（`kControl > kAudio > kVideoKeyframe > kVideo > kBestEffort`）；而 DC-2 想要的是**按流**区分优先级（一个 DC 流是 High，另一个是 Low），这在 P-family 的 5 级里表达不了 —— 一个 DC 包无论属于哪个流，都会被打成同一个 `plugins::Priority`。所以专门引入了 **S-family（每流优先级）**，`select_next_packet()` 是它的排空入口。

**这个设计动机是对的，实现也是完整的**：
- `set_stream_priority()` 支持**运行时迁移**：改优先级时把已排队的包从旧桶 splice 到新桶，保持原顺序（`scheduler.cpp:843-854`）
- 三桶严格优先级：High 桶排空后才动 Normal/Low
- 有 ~20 个专门测试（`tests/test_sched_priority_dc.cpp`）

**当初为什么"看起来没用"**：`docs/plan/datachannel-capability-plan.md:100-105` 记录了原因 —— DC-2 的**跨平面**保证依赖 DC 与媒体共用一条连接，但 v1.2.0 把 SCTP 后端退役、数据面改成独立的 WebTransport/QUIC 连接后，两条连接各有独立的拥塞控制，**跨连接优先级在架构上就不可达**了。`sched.hpp:60-64` 的 scope note 自己写得很清楚：

> Today the honest statement is: **the scheduler arbitrates within a connection, and the data plane does not use it at all** — the per-stream API has no caller in the data path.

**删除的负面影响（逐条）**：

1. **破坏 v1.0.0 已冻结的公开 API**。`sched::StreamPriority`、`set_stream_priority()`、`select_next_packet()`、`enqueue_for_stream_owned()` 全部带 `NIMRTC_API_STABLE` 标记（`sched.hpp:139,225,261`）。按 ADR-015 与 `api_version.hpp` 的"v1.0.x 公开头冻结承诺"，删除属于**破坏性变更**，需要 MINOR 版本 + deprecation 周期，而不是在整改计划里顺手删掉。

2. **丢掉一个已经写好且有测试的通用能力**。多流场景（远程桌面 M4 / `remote-desktop-plan.md` H1-H4）明确要求"两个视频流并发、独立 SSRC、独立 codec"。H4 要求 per-stream 统计，**每流的调度优先级是同一族需求**。将来接多流时，S-family 是现成的落点；删掉就要重写。

3. **删不掉的隐性依赖**。`set_stream_priority()` 实现了"改优先级时原子迁移已排队包"这个非平凡语义（`scheduler.cpp:843-854` 的双趟 splice）。重写这个逻辑的成本高于补齐流控。

4. **C ABI 会跟着一起消失**。`sched_create` / `sched_set_stream_priority` / `sched_enqueue_stream_owned` / `sched_select_next_packet` 五个 C 包装（`sched.hpp:281-320`）只服务 S-family 路径。删 S-family 就得删 C ABI，而 C ABI 存在的意义是**给 C 语言 / 嵌入式宿主用的稳定入口** —— 删它会削弱嵌入式场景的能力（这恰恰是 NimRTC 的目标场景）。

**结论：Q1 改为"保留 + 补齐流控"**。

**保留后的具体动作（原 A9 的替代方案）**：

| 动作 | 说明 |
|---|---|
| **A9-a** | `drain_stream_bucket()`（`scheduler.cpp:936-979`）加入与 `drain_with` 相同的 **BWE credit 检查**（`bwe_limit_active_` / `credit_milli_bytes_`） |
| **A9-b** | `drain_stream_bucket()` 加入 **pacing 消耗**（`pacing::TokenBucket`），与 `drain_with` 的分桶 pacing 对齐 |
| **A9-c** | `select_next_packet()` 从**具体类独有**提升到 **`plugins::IScheduler` 接口**（`plugins/scheduler.hpp`）—— 否则第三方 scheduler 实现无法提供 S-family 能力，接口是不完整的 |
| **A9-d** | 修正 `sched.hpp` 的 scope note：删掉"数据面完全不用它"的现状描述，改为说明 **S-family 的适用边界是"同一连接内的流间仲裁"**，并补一句"多流（M4）场景下它是 per-stream 优先级的落点" |
| **A9-e** | 在 `docs/plan/datachannel-capability-plan.md` 的 DC-E 条目里，把"per-stream API 无调用方"的状态更新为"S-family 已可用且已补齐流控；DC 数据面因跨连接架构暂不消费" —— **保留能力、修正描述**，而不是删能力 |
| **A9-f** | 保留 C ABI 五个入口（不删 D5 的 sched 部分），并在 `sched.hpp` 注明其用途是给 C 宿主的 S-family 入口 |

**注意**：`select_next_packet` 提升到 `IScheduler` 接口（A9-c）是**接口的加法**（additive），不破坏现有实现 —— 但需要给 `IScheduler` 加一个**带默认实现的虚函数**，否则现有实现类会编译失败。这与 ADR-015 的"加法式变更 = MINOR bump"一致。

**D5（删无用 C ABI）的修正**：C ABI **不删**，但需要核实每个入口是否真被外部使用；若某个入口确实无调用方且无外部承诺，可单独删除并记录。

---

## 0. 缺陷总清单（本计划覆盖的全部问题）

### A 类 — 正确性缺陷（会导致崩溃 / 静默降级）

| ID | 缺陷 | 位置 | 后果 |
|---|---|---|---|
| A1 | `StackConfig::rtp_id` 默认 `"nim"`，RTP 模块实际注册 `"webrtc"` | `src/transport/include/nimrtc/transport/transport_stack.hpp:85` | RTP 组件为 null，`stack->rtp()` 解引用空指针 |
| A2 | `StackConfig::sctp_id` 默认 `"usrsctp"`，SCTP 模块实际只注册 `"stub"` | `src/transport/include/nimrtc/transport/transport_stack.hpp:86` | SCTP 组件为 null |
| A3 | 工厂解析失败时 `return stack`（半成品）而非 nullptr | `src/transport/src/webrtc_classic_stack.cpp:239,253,274,290` | 引擎的空检查形同虚设，缺陷被推迟到解引用时爆炸 |
| A4 | 视频 RTP 路径手工拼 12 字节头，硬编码 "no extensions"，无 abs-send-time | `src/engine/src/engine_media.cpp:702-717` | 视频流缺失 ADR-017 时间线扩展，BWE 对视频失效 |
| A5 | 视频路径与音频路径 RTP 构造逻辑重复且已漂移 | `engine_media.cpp:702` vs `engine_media.cpp:1040` | 任何 RTP 特性修复必须改两处 |
| A6 | 视频解码缓冲分辨率硬编码 640×480 | `src/engine/src/engine_media.cpp:810-811` | 非 640×480 流解码尺寸错误 / 潜在越界 |
| A7 | 视频编码器配置硬编码 640×480 / 30fps | `src/engine/src/engine_media.cpp:537-538, 572-573` | 无法配置分辨率 |
| A8 | `Scheduler::drain_with()` 持锁跨越 `ice_t_->send()` 网络 I/O | `src/modules/sched/src/scheduler.cpp:415` + `src/engine/src/engine_transport.cpp:924-930` | UDP 写阻塞会冻结全部媒体与控制流 |
| A9 | `select_next_packet` / `drain_stream_bucket` 完全绕过 BWE credit 与 pacing | `src/modules/sched/src/scheduler.cpp:936-979` | 走 stream 优先级的流不受拥塞控制约束 || A10 | `EngineConfig` 混合 `std::string_view`（12 个）与 `std::string`（10 个） | `src/engine/include/nimrtc/engine/engine.hpp:156-858` | `string_view` 字段绑定临时 `std::string` → 悬垂指针 |
| A11 | `TypedRegistry` 存储 `std::string_view` 作为键 | `src/core/include/nimrtc/core/registry.hpp:237` | 第三方用临时 `std::string` 注册 → 键悬垂 |
| A12 | `registry.hpp` 顶部文档示例 `cfg.transport_name = "my_udp"` 暗示可替换，但 `transport_name` 实际只用于 ICE | `src/core/include/nimrtc/core/registry.hpp:33-38` | 误导用户 |

### B 类 — 插件契约缺失（用户"换不了模块"）

| ID | 缺陷 | 位置 |
|---|---|---|
| B1 | `EngineConfig::rtp_name` / `sdp_name` 引用次数 = 0，纯装饰 | `engine.hpp:159,162` |
| B2 | `EngineConfig` 缺 `sctp_socket_name` / `raw_udp_name` / `transport_stack_name` | `engine.hpp` 全文 |
| B3 | `EngineConfig::video_capture_name` 不存在，但 `video_capture` 槽位已注册 | `engine.hpp` / `registry.hpp:500` |
| B4 | 文档写 `NIMRTC_REGISTER_VIDEO_RX` / `_TX`，实际是 `_RECEIVER` / `_SENDER` | `docs/plugin_author_guide.md:184-185` |
| B5 | 文档写 `transport_stack_name` / `raw_udp_name` / `sctp_socket_name` 三个不存在的配置项 | `docs/plugin_author_guide.md:188-192` |
| B6 | `codec_family_from_plugin_id()` 靠解析 ID 字符串推断 codec 家族，契约未显式化 | `src/engine/src/video_codec_config.hpp:115-133` |
| B7 | `ISrtpSessionBackend` seam 有接口有实现，但引擎从不使用，注册表无槽位 | `src/plugins/include/nimrtc/plugins/srtp_backend.hpp:33` |
| B8 | `ISDP` / `IRTP` 接口注册但引擎完全绕过 | `engine_impl.hpp:423-424`、`engine_media.cpp:1040,1336` |
| B9 | `CapabilitySelector` 永远运行在 "rule-only mode"，返回空 session 后被 `(void)session` 丢弃 | `src/engine/src/engine_transport.cpp:297-352` |
| B10 | `docs/plugin_author_guide.md` §3 Step 4 承诺 "no silent fallback"，实际多处静默降级 | 文档 vs A1-A3 |

### C 类 — 性能缺陷

| ID | 缺陷 | 位置 | 量级 |
|---|---|---|---|
| C1 | `stage_source_frame` 每帧 `make_shared` 一次 + 全帧三平面拷贝 | `engine_media.cpp:91-162` | 1080p30 ≈ 93 MB/s + 每帧 3 MB 分配 |
| C2 | NV12 → I420 去交织每帧一次 | `engine_media.cpp:135-144` | 额外一遍内存带宽 |
| C3 | `GpuBuffer` / `GpuBufferPool` / 零拷贝 handle 已实现但引擎从不使用 | `src/modules/video_frame/include/nimrtc/video_frame/frame.hpp:423,559` | 硬件编码路径断裂 |
| C4 | `PacketBuilder::build()` 每音频包一次堆分配 | `src/modules/rtp/src/builder.cpp:174-176` | 50 Hz |
| C5 | `SrtpSession::unprotect_rtp()` 每包一次 `std::vector` 分配 | `src/modules/srtp/src/srtp.cpp:307` | 每 RTP 包 |
| C6 | `demux_rtcp()` 每 compound 返回 `std::vector<RtcpBlock>` | `src/modules/rtp/src/rtcp.cpp:79` | 每 RTCP 包 |
| C7 | 视频 RTP 包 `rtp_pkt.insert()` 两次拼接 | `engine_media.cpp:727-729` | 每视频包 |

### D 类 — 结构缺陷

| ID | 缺陷 | 位置 |
|---|---|---|
| D1 | 引擎是上帝对象：`engine_impl.hpp` 1109 行 + 15 个 `engine_*.cpp` 共 ~7000 行 | `src/engine/` |
| D2 | 死模块：`video_stats` / `video_sdp_attributes` / `assembly` / `jb_strategy` / `nvcodec` / `video_rtcp_feedback` 引擎从不加载 | `src/modules/` |
| D3 | DTLS 双实现（`src/dtls/` seam+GmSSL vs `src/modules/dtls/` wolfSSL），文档专门写一节解释权威 | `ARCHITECTURE.md:96-108` |
| D4 | 五个 seam 家族目录在 `src/` 顶层而非 `src/modules/` 下 | `src/dtls/ raw_udp/ sctp/ transport/ web_transport/` |
| D5 | `sched` 模块暴露 C ABI（`sched_create` 等），仅测试使用 | `src/modules/sched/include/nimrtc/sched/sched.hpp:285` |
| D6 | 引擎私有实现直接 include 具体模块头，违反 L4 精神 | `engine_impl.hpp:86-122` |

---

## 1. 执行原则

1. **每个 Phase 独立可合入、独立可验证**，不做大爆炸式重构。
2. **每个 Phase 结束必须通过 `ctest` 全绿 + 新增的针对性测试**。
3. **先修"会崩的"，再修"看不见的"，最后修"不优雅的"**。
4. **禁止在 Phase 1-3 引入新的公共 API 破坏性变更**；若必须，按 ADR-015 的 deprecation 流程走。
5. **每条任务都要更新 `docs/plugin_author_guide.md`** —— 这是用户唯一的契约文档。

---

## 2. Phase 0 — 建立基线与守卫网（防止修复过程中引入回归）

**目标**：在动任何代码之前，先建立"能自动发现这类缺陷"的测试。

### 0.1 注册表一致性契约测试（新增 `tests/test_registry_contract.cpp`）

遍历注册表**每一个**槽位，对每个已注册 factory 断言：

- `factory->id() == 注册时的 id`（字符串一致）
- `factory->id()` 非空
- `factory->create()` 不返回 nullptr（对无 config 的族）
- `factory->display_name()` 非空

**这条测试会立刻抓出 A1（`rtp_id="nim"` vs 实际 `"webrtc"`）**——因为 `WebRtcClassicStackFactory` 用 `cfg.rtp_id` 去查，而 `cfg.rtp_id` 的默认值与注册表里的键不匹配。

同时新增一个**跨模块 ID 一致性测试**：为每个 seam 建立"期望 ID 常量表"，断言 `StackConfig` 的默认值、`EngineConfig` 的默认值、模块 `id()` 返回值三者一致。

| 期望 ID | 注册方 | 消费方默认 |
|---|---|---|
| `webrtc` | `rtp_plugin.cpp:254` | `StackConfig::rtp_id` |
| `webrtc` | `sdp_plugin.cpp:302` | `StackConfig::sdp_id`（新） |
| `stub` / `usrsctp` | `sctp_stub_factory.hpp:51` | `StackConfig::sctp_id` |
| `ice` | `ice.cpp:1144` | `StackConfig::ice_id` |
| `wolfssl` | `dtls_wolfssl_factory.cpp:87` | `StackConfig::dtls_id` |
| `webrtc-classic` | `webrtc_classic_stack.cpp:314` | `EngineConfig`（新） |

### 0.2 文档一致性自动检查（新增 `tools/check_doc_contract.py`）

解析 `docs/plugin_author_guide.md` §4 表格，校验：
- 表格中每个 "Engine config knob" 必须在 `engine.hpp` 中**真实存在**
- 表格中每个 "Factory macro" 必须在 `registry.hpp` 中**真实定义**

挂到 CI（`.github/workflows/ci.yml`）。**这条检查会立刻抓出 B4、B5。**

### 0.3 性能基线

在 `tools/benchmark_latency.py` 基础上补三个基准点，作为 Phase 3 的验收依据：
- `send_video()` 1080p30 的 CPU 占用与 per-call 分配次数
- `tick()` 的 p99 耗时
- 调度器满载时的 `drain_with()` 耗时（区分"锁内"与"锁外"）

### 0.4 阶段验收
- 新增 2 个测试目标全绿
- 现有 `ctest` 全绿（记录基线通过数）
- CI 增加文档一致性检查

---

## 3. Phase 1 — P0 正确性修复（A 类，共 12 项）

> 目标：消除崩溃隐患与静默降级。**本 Phase 不新增任何功能。**

### 1.1 修 A1/A2：ID 错配

**改动文件**：`src/transport/include/nimrtc/transport/transport_stack.hpp:85-88`

```
rtp_id   : "nim"      → "webrtc"
sctp_id  : "usrsctp"  → "stub"
```

并在 `StackConfig` 上方补注释，说明每个 ID 对应哪个模块的 `id()`。

### 1.2 修 A3：工厂失败必须返回 nullptr

**改动文件**：`src/transport/src/webrtc_classic_stack.cpp`

四处 `return stack;` 全部改为 `return nullptr;`（行 239 / 253 / 274 / 290）。同时补一条：SCTP 缺失目前只是 WARN，但 `sctp()` 访问器返回 `*sctp_` 同样会崩 —— 改为 ERROR + nullptr。

**配套**：`src/transport/include/nimrtc/transport/transport_stack.hpp` 的 `ITransportStack` 文档补充"任一必需组件解析失败时 `create()` 必须返回 nullptr"的契约。

**配套测试**：`tests/test_transport_stack_contract.cpp` 新增 —— 注册一个不存在的 `rtp_id`，断言 `create()` 返回 nullptr。

### 1.3 修 A4/A5：视频 RTP 改用 `rtp::PacketBuilder`

**改动文件**：`src/engine/src/engine_media.cpp:695-730`

删除手工 `hdr[12]` 拼装，改为与音频路径同构的 `rtp::PacketBuilder`：

- `set_ssrc` / `set_seq` / `set_timestamp` / `set_payload_type` / `set_marker`
- 在 `config_.timeline_enabled && impl_->local_abs_send_time_id != 0` 时 `set_abs_send_time(...)` —— **补上视频缺失的 ADR-017 扩展**

**配套测试**：
- `tests/test_engine_timeline.cpp` 增加"视频包也带 abs-send-time"的断言
- 新增单测断言视频 RTP 包的 X 标志位与扩展元素存在

### 1.4 修 A6/A7：分辨率硬编码

**改动文件**：`src/engine/src/engine_media.cpp`

- 新增 `EngineConfig::video_width` / `video_height` / `video_fps`（默认 640/480/30，保持现有行为不变）
- 行 537-538、572-573 的 `cfg.width = 640; cfg.height = 480;` 改为读配置
- 行 810-811 的 `const std::uint32_t w = enc.payload.size() > 0 ? 640 : 0; const std::uint32_t h = 480;` 改为从 codec 实际输出尺寸推导

**关键**：解码尺寸必须来自 codec 的实际输出（`IVideoCodec::decode` 的输出信息），不能靠猜。需要先确认 `EncodedVideoFrame`/`VideoFrame` 是否携带尺寸字段；若无，在 `plugins::VideoFrame` 上**新增 `width`/`height` 字段**（这是 additive，符合 ADR-015）。

### 1.5 修 A8：调度器锁内网络 I/O

**改动文件**：`src/modules/sched/src/scheduler.cpp` + `src/engine/src/engine_transport.cpp`

当前形态：
```cpp
std::lock_guard<std::mutex> lock(impl_->mu_);   // scheduler.cpp:415
... ice_t_->send(bv, {});                          // engine_transport.cpp:928
```

**改法**：把 `drain_with` 拆成两段语义：
1. `drain_with` 内部在**锁内**把待发包搬进一个本地 `std::vector`（含 buffer 所有权转移）
2. **锁外**调用 `cb`

为不破坏 `IScheduler` 接口，新增一个重载：
```cpp
virtual int drain_with(int max_packets, DrainCallback cb) noexcept = 0;
```
保持签名不变，但在实现里改成"锁内收集 + 锁外回调"。

**注意**：`DrainCallback` 的 veto 语义（`scheduler.cpp:952-958` 把包放回队首）必须保留 —— 收集阶段要把 veto 的包重新入队。

**配套测试**：新增"回调内阻塞时 `enqueue()` 不被阻塞"的并发测试（用 `std::promise` / `std::condition_variable` 控制）。

### 1.6 修 A9：`select_next_packet` 补齐流控（**保留，不删除**）

> **决策已定**：见 §0.1。`select_next_packet` 是 v1.0.0 已冻结的 `NIMRTC_API_STABLE` 公开 API，且是远程桌面 M4 多流场景的 per-stream 优先级落点。**保留并补齐流控**，不删除。

**改动文件**：`src/modules/sched/src/scheduler.cpp:936-979`

| 子项 | 动作 |
|---|---|
| **A9-a** | `drain_stream_bucket()` 加入 BWE credit 检查 —— 与 `drain_with` 相同的 `bwe_limit_active_` / `credit_milli_bytes_` 逻辑（参考 `scheduler.cpp:444-455, 481-490, 542-550`） |
| **A9-b** | `drain_stream_bucket()` 加入 pacing 消耗 —— `pacing::TokenBucket` 按包的 paced class 扣减（参考 `scheduler.cpp:470, 505-510`） |
| **A9-c** | `select_next_packet()` 提升到 `plugins::IScheduler` 接口（`src/plugins/include/nimrtc/plugins/scheduler.hpp`）—— **带默认实现**（默认返回 0），保证现有实现类不破坏。接口补全后，第三方 scheduler 实现才能提供 S-family 能力 |
| **A9-d** | 修正 `sched.hpp:60-64` 的 scope note —— 删掉"数据面完全不用它"的现状断言，改为说明 **S-family 的适用边界是"同一连接内的流间仲裁"**，并补"多流（M4）场景下它是 per-stream 优先级的落点" |
| **A9-e** | `docs/plan/datachannel-capability-plan.md:100-105` 的 DC-E 条目更新 —— 状态从"per-stream API 无调用方"改为"S-family 已可用且已补齐流控；DC 数据面因跨连接架构暂不消费" |
| **A9-f** | 保留 C ABI 五个入口（`sched.hpp:281-320`），在头文件注明它们是**给 C 宿主的 S-family 入口**，不是遗留物 |

**关键实现细节**：

- **credit 的归属**：S-family 与 P-family 是两个独立队列族。credit 是**会话级**的（`impl_->credit_milli_bytes_`），所以 S-family 消费 credit 时必须与 P-family **共用同一个池** —— 否则总发送速率会翻倍。实现上：`drain_stream_bucket` 内直接读写 `impl_->credit_milli_bytes_`，与 `drain_with` 用同一个字段。
- **pacing 的归属**：`class_buckets_` 是按 `PacedClass` 分的，S-family 的包需要知道自己属于哪个 class。当前 `PacketRecord` 里有 `priority` 字段 —— 需要确认 S-family 路径如何设置它。**若无法确定 paced class，S-family 的包应归入 `kBestEffort` class**（最保守）。
- **veto 语义保留**：`drain_stream_bucket` 现有的"veto → 放回队首"（`scheduler.cpp:952-958`）必须保留，且放回时**不扣 credit**（与 `drain_with` 的 `scheduler.cpp:546-550` 语义一致）。
- **时间信用**：`drain_with` 的 credit 是按 **实测 wall-clock 间隔**发放的（`scheduler.cpp:428-439`）。`select_next_packet` 是**独立调用**，没有"上一次排空"的参照 —— 需要为 S-family 单独维护一个 `last_drain_time_` + `has_last_drain_`，否则第一次调用会拿到一个巨大的默认 credit。

**配套测试**：
- 新增：S-family 在 BWE 限额生效时**不能**超过 credit 上限
- 新增：S-family 的包**确实**消耗 pacing token（对比 drain 前后的 token 余量）
- 新增：`select_next_packet` 的 veto 不扣 credit
- 新增：`IScheduler` 接口新增虚函数后，现有 `Scheduler` 实现的默认行为正确

**这一步不需要用户决策了**（§0.1 已定）。

### 1.7 修 A10：`EngineConfig` 字段类型统一

**改动文件**：`src/engine/include/nimrtc/engine/engine.hpp`

12 个 `std::string_view` 字段全部改为 `std::string`：
`transport_name`(156) / `rtp_name`(159) / `sdp_name`(162) / `jb_name`(165) / `audio3a_name`(174) / `codec_name`(177) / `video_source_name`(291) / `video_sink_name`(294) / `video_receiver_name`(297) / `video_sender_name`(300) / `transport_strategy_name`(678) / `audio_decoder_name`(702) / `audio_receiver_name`(710) / `session_name`(829) / `dtmf_codec_name`(858)

**影响面**：这些字段被传给 `pal::make_plugin(plugin_id: std::string_view, ...)`，改成 `std::string` 后仍可隐式转换，无需改调用点。但**有些地方取 `std::string(config_.jb_name)`**（`engine_media.cpp:329`）会变成冗余拷贝，可简化。

**注意**：`std::string` 字段的 `EngineConfig` 拷贝成本上升（15 个 string）。可接受 —— `EngineConfig` 只在构造时拷贝一次。

**替代方案**（若要保 ABI）：保留 `string_view` 但加 `std::string` 影子存储 —— 不推荐，太复杂。用 `std::string` 更简单安全。

### 1.8 修 A11：注册表键的生命周期

**改动文件**：`src/core/include/nimrtc/core/registry.hpp:237`

```cpp
std::vector<std::pair<std::string_view, const T*>> entries_;
```
改为
```cpp
std::vector<std::pair<std::string, const T*>> entries_;
```

`register_one` / `get` / `unregister_one` / `list_ids` 的签名保持 `std::string_view` 入参，但**存储时拷贝成 `std::string`**。

`list_ids()` 返回类型 `std::vector<std::string_view>` 会因此失效 —— 改为返回 `std::vector<std::string>`。需检查所有调用方（`registry.hpp` 内的 `list_*` 包装 + `tests/test_engine_plugin_loading.cpp`）。

**这是对第三方插件作者最重要的修复**：目前 `register_video_codec(std::string("myid"), &f)` 会让键悬垂。

### 1.9 修 A12 + B10：文档示例更正

**改动文件**：`src/core/include/nimrtc/core/registry.hpp:33-38`

删除 `cfg.rtp_name = "webrtc";` 这类暗示 RTP 可替换的示例（除非 Phase 2 决定让 RTP 可替换）。

### 1.10 Phase 1 验收标准

- `ctest` 全绿
- 新增测试：transport stack nullptr 契约、注册表键生命周期、abs-send-time 视频覆盖、调度器锁外 I/O
- ASAN/UBSAN 跑通 `tests/`（`tools/sanitizer_gate.py`）
- 视频分辨率可配置，且 1920×1080 端到端可跑

---

## 4. Phase 2 — 插件契约补全（B 类，共 10 项）

> 目标：**兑现"用户可替换任意模块"的承诺**。

### 2.1 决策已定：SDP / RTP **先不替换**

> **决策（2026-11-12）**：Q2 = 先不替换。执行**方案 A**，但**不删除接口**（见下方"细化"）。

**方案 A（执行）**：保持不可替换，但诚实标注
- 删除 `EngineConfig::rtp_name` / `sdp_name`（它们是假承诺，引用次数 = 0）
- **保留** `plugins::IRTP` / `plugins::ISDP` 接口与注册表槽位，但标注 `@deprecated` + 明确注释"引擎不使用此 seam"
  - **为什么不删接口**：它们是 `NIMRTC_API_STABLE` 公开头的一部分（`plugins/rtp.hpp:123,168`、`plugins/sdp.hpp:131,171`），按 ADR-015 删除属于破坏性变更，需要独立的版本周期 —— 整改计划里顺手删公开 API 是危险的
  - **为什么标注 deprecated**：让新用户不会误以为可以替换，同时保留未来重新启用的可能
- 在 `plugin_author_guide.md` §4 表格中移除 RTP / SDP 两行的"可替换"标记，改为单独一节"**目前不可替换的模块（及原因）**"

**不可替换的技术理由（写进文档）**：
1. **热路径零虚拟调用**：RTP 打包/解析在每包路径上，SDP 解析在每次协商时；引擎走具体类可让编译器内联
2. **接口无法表达引擎实际需求**：当前 `ISDP::parse/serialize/mung/is_compatible/pick_codec` 与引擎用法不匹配 —— 引擎实际用的是 `sdp::Munger` 的细粒度 API（`munger.cpp` 的逐属性改写）。要真正走接口，需要重新设计 `ISDP`，属于独立的架构工作，不在本次整改范围
3. **用户价值低**：几乎不会有第三方实现自己的 SDP 解析器或 RTP 打包器

**后续路径**：若将来确有需求（例如国密场景需改 RTP 扩展、或需 SDP 的特殊变体），再单独立项做接口重新设计 + 引擎迁移。**本次只保证"不撒谎"**。

**"不撒谎"的验收标准**：删除 `rtp_name` / `sdp_name` 后，`docs/plugin_author_guide.md` §4 表格里**不再有任何一行暗示它们可替换**；Phase 0.2 的文档一致性检查通过。

**连带影响（必须一起处理）**：
- `src/core/include/nimrtc/core/registry.hpp:33-38` 的文件头示例里有 `cfg.rtp_name = "webrtc";` —— 一并删除（= A12）
- `src/plugins/include/nimrtc/plugins/rtp.hpp` / `sdp.hpp` 的 `IRTPFactory` / `ISDPFactory` 文档注释要补"当前引擎不使用此 seam"
- `src/core/src/pal_default_registrars.cpp:127,129` 的 `rtp::register_default_plugins` / `sdp::register_default_plugins` **保留**（注册表槽位还在），但注释说明"注册但不消费"

### 2.2 修 B2/B3：补齐缺失的 `EngineConfig` 开关

**改动文件**：`src/engine/include/nimrtc/engine/engine.hpp`

新增：
```cpp
/** SCTP socket factory id. Default = "stub". */
std::string sctp_socket_name = "stub";

/** Raw-UDP datagram factory id. Empty = disabled. */
std::string raw_udp_name;

/** Transport-stack factory id. Default = "webrtc-classic". */
std::string transport_stack_name = "webrtc-classic";

/** Video capture device factory id. */
std::string video_capture_name;
```

**接线**：
- `sctp_socket_name` → `StackConfig::sctp_id`（`engine_transport.cpp:386` 附近）
- `raw_udp_name` → `StackConfig::raw_id`
- `transport_stack_name` → `init_transport_strategy()` 的 fallback（`engine_transport.cpp:336`）替代硬编码 `"webrtc-classic"`
- `video_capture_name` → `engine_capture.cpp` 的视频采集路径

### 2.3 修 B4/B5：文档与代码对齐

**改动文件**：`docs/plugin_author_guide.md`

- §4 表格：`NIMRTC_REGISTER_VIDEO_RX` → `NIMRTC_REGISTER_VIDEO_RECEIVER`；`_TX` → `_SENDER`
- §4 表格：补 `sctp_socket_name` / `raw_udp_name` / `transport_stack_name` 三行（Phase 2.2 落地后即真实存在）
- §3 Step 4：把 "no silent fallback" 改为准确描述 —— 明确"哪些族是硬失败（`open()` 返回非 0）、哪些族是软降级（打日志后回退到内置实现）"，并给出一张表
- §4 表格：按 2.1 的决策增删 RTP / SDP 两行
- 新增一节"**当前可替换性矩阵**"，逐族列出：能否替换 / 配置项名 / 失败模式 / 是否有内置回退

### 2.4 修 B6：codec 家族契约显式化

**改动文件**：`src/plugins/include/nimrtc/plugins/video_codec.hpp` + `src/engine/src/video_codec_config.hpp`

**改法**：在 `IVideoCodec` 接口上新增虚函数：
```cpp
/** Codec family — authoritative. The engine uses this (not the plugin id)
 *  to derive the SDP a=rtpmap encoding name and the payload type. */
virtual VideoCodecKind kind() const noexcept = 0;   // 已有
```
`kind()` 已存在且被引擎用（`engine_media.cpp:596`）。真正的问题是 `codec_family_from_plugin_id` 在 codec 尚未实例化时先被调用（行 568）。

**改法**：调整顺序 —— 先用 `config_.video_codec_name` 解析出候选 family 用于**查找**，实例化后**一律以 `video_codec_->kind()` 为准**。当 `kind()` 返回 `kUnknown` 且 ID 解析也失败时，**明确报错**（`kEngineInvalidParam`），而不是静默用 unknown 继续。

**同时**在 `IVideoCodec` 文档中写明："`id()` 必须以 `h264` / `h265` / `hevc` / `av1` 开头，或实现 `kind()` 返回具体家族。"

### 2.5 修 B7：`ISrtpSessionBackend` —— **接线**（Q3 决策已定）

> **决策（2026-11-12）**：Q3 = 接线。SRTP 后端可替换。

**改动文件**：`src/plugins/include/nimrtc/plugins/srtp_backend.hpp`、`src/modules/srtp/`、`src/core/include/nimrtc/core/registry.hpp`、`src/engine/include/nimrtc/engine/engine.hpp`

**为什么值得接线**：SRTP 是用户**确实可能想换**的 —— 头文件注释里已经提到 SM4-GCM 的国密需求（`srtp_backend.hpp:6`），而 `SrtpContext` 直接调 libsrtp 的 C API（`srtp.cpp:260,308,331,351`），完全没有替换点。这是"假 seam"的典型：接口、实现、工厂都在，但主路径不经过。

**执行步骤**：

| 步 | 动作 | 文件 |
|---|---|---|
| 1 | `SrtpContext` 内部改为持有 `std::unique_ptr<plugins::ISrtpSessionBackend>`，把 `srtp_protect` / `srtp_unprotect` / `srtp_protect_rtcp` / `srtp_unprotect_rtcp` 的调用**全部**转到接口方法 | `src/modules/srtp/src/srtp.cpp` |
| 2 | 新增注册表槽位 `srtp_backend_`：`register_srtp_backend(id, factory)` / `get_srtp_backend(id)` / `list_srtp_backends()` + `NIMRTC_REGISTER_SRTP_BACKEND` 宏 + `Registrar::Category::kSrtpBackend` | `src/core/include/nimrtc/core/registry.hpp`、`src/core/src/plugin_registry.cpp` |
| 3 | `EngineConfig` 新增 `std::string srtp_backend_name = "libsrtp";` | `src/engine/include/nimrtc/engine/engine.hpp` |
| 4 | 引擎在 SRTP 安装时（`engine_transport.cpp` 的 `install_srtp` 附近）通过 `pal::resolve_srtp_backend(config_.srtp_backend_name)` 解析后端 | `src/engine/src/engine_transport.cpp` |
| 5 | `LibsrtpBackendFactory` 的注册 ID 改为 `"libsrtp"`，注册进新槽位 | `src/modules/srtp/src/libsrtp_backend.cpp`、`src/core/src/pal_default_registrars.cpp` |
| 6 | `pal::resolve_srtp_backend` 前向器 | `src/engine/include/nimrtc/engine/pal/engine_plugin_resolver.hpp` |

**接口能力要求**：`ISrtpSessionBackend` 需要能表达 `SrtpContext` 现在用到的全部能力 —— 创建会话（带 policy）、protect/unprotect RTP 与 RTCP、导出 key。当前接口（`srtp_backend.hpp:33-66`）已经覆盖，**但 `SrtpContext` 里的 `SrtpSession` 是按 ssrc + 方向分开的多个会话**（`srtp.cpp:491` 的 `get_session(ssrc, outgoing)`）—— 需要确认接口能表达"按 ssrc 的多会话"模型。**若接口是单会话模型，需要扩展接口**（additive）。

**影响面提示**：这一步会改动 SRTP 的核心路径，**必须配 SRTP 回归测试**：
- `src/modules/srtp/tests/test_srtp.cpp` 现有用例必须全绿
- `tests/test_rtp_srtp_arq_e2e.cpp`、`tests/test_srtp_over_dtls_over_arq_udp.cpp` 必须全绿
- 新增：注册一个 mock SRTP backend，断言引擎确实使用了它

### 2.6 修 B9：`CapabilitySelector` —— **接线**（Q4 决策已定）

> **决策（2026-11-12）**：Q4 = 接线。让 Selector 真正决定 media/control stack。

**改动文件**：`src/engine/src/engine_transport.cpp:297-352`、`src/transport/src/transport_selector.cpp`、`src/transport/include/nimrtc/transport/transport_selector.hpp`

**为什么值得接线**：
- `docs/plan/transport-strategies.md` 的 §5.3 规则已经写好（规则 1/1b/2/3/4），是一套完整的**能力协商**设计
- 远程桌面 RD-5 / G2 明确要求"控制面有自己的拥塞控制域" —— 需要 media/control 两个独立 stack，这正是 Selector 的职责
- 当前状态是**最坏的中间态**：Selector 跑在无 resolver 的 "rule-only mode"，返回空 session，被 `(void)session` 丢弃，媒体栈硬编码回退 `"webrtc-classic"`（`engine_transport.cpp:316-352`）。规格文档描述的分层协商在运行时不存在

**执行步骤**：

| 步 | 动作 | 文件 |
|---|---|---|
| 1 | `CapabilitySelector` 接受 resolver 依赖（工厂查找回调），能真正构造 `TransportSession` 而非返回空壳 | `src/transport/src/transport_selector.cpp` |
| 2 | 引擎构造 Selector 时**注入 resolver**（走 `PluginRegistry::get_transport_stack`），不再用无参 `CapabilitySelector selector;` | `src/engine/src/engine_transport.cpp:302` |
| 3 | 移除 `(void)session;` 与硬编码 fallback —— `session.media_stack` / `session.control_stack` 直接使用 | `engine_transport.cpp:342-352` |
| 4 | `trace.media_factory_id` 为空时（规则 3 / `webrtc-quic` 未实现）→ **明确报错** `kEngineInvalidParam`，而不是静默回退 | `engine_transport.cpp:330` |
| 5 | `EngineConfig::transport_stack_name` 作为**用户显式覆盖**（优先级高于 Selector），给用户一个逃生阀 | `engine_transport.cpp` |

**风险与控制**：这是 Phase 2 里风险最高的一项 —— 改错了会让所有连接建立不起来。

**控制措施**：
- 保留 `docs/plan/transport-strategies.md` §5.3 的规则表作为实现的**验收基准**（`src/transport/tests/test_transport_strategy.cpp` 已有对应测试，扩展它）
- Step 3-4 单独成一次提交，方便二分定位
- 必须跑通：`tests/test_dtls_over_udp_loopback.cpp`、`tests/test_dtls_over_arq_udp.cpp`、`tests/test_rtp_srtp_arq_e2e.cpp`、`tests/test_srtp_over_dtls_over_arq_udp.cpp`

**依赖提示**：Q4 接线依赖 `EngineConfig::transport_stack_name`（2.2）与 `raw_udp_name` / `sctp_socket_name` —— 必须先做 2.2。

### 2.7 Phase 2 验收标准

- `EngineConfig` 每个 `*_name` 字段都有**至少一处**真实读取（新增测试断言：反射式遍历 `EngineConfig`，每个名字字段被 `src/engine/` 引用）
- `docs/plugin_author_guide.md` 通过 Phase 0.2 的自动检查
- 新增测试：每个可替换族，用一个 mock factory 走通"注册 → EngineConfig 指定 → open() → 插件被实际调用"的完整链路

---

## 5. Phase 3 — 性能修复（C 类，共 7 项）

> 目标：消除热路径上的分配与拷贝，落地零拷贝路径。
> **验收依据：Phase 0.3 建立的基准。**

### 3.1 修 C1/C2：`stage_source_frame` 缓冲池化

**改动文件**：`src/engine/src/engine_media.cpp:91-162`、`src/engine/src/engine_impl.hpp`

**方案**：在 `Impl` 中加一个 `video_frame::VideoFrameBuffer` 池（复用 `src/modules/video_frame/` 已有的池化设施 —— 需先确认 `VideoFrameBuffer` 是否已支持外部提供存储）。

- `stage_source_frame` 改为：从池中取缓冲 → 拷贝/转换 → 返回
- 编码完成后归还

**但注意生命周期**：`raw.buffer` 是 `shared_ptr<VideoFrameBuffer>`，会传给 `video_codec_->encode()`，且 `VideoFrame` 的 `buffer` 字段语义是"跨 encode() 保持有效"。因此归还时机取决于 codec 是否持有引用 → **用 `shared_ptr` 自定义 deleter 实现池化**（归还时归还缓冲，而非销毁）。

**兼容性**：`video_codec_->encode(raw, buf, cap, encoded)` 的接口要求 codec 不持有 `raw.buffer` 超过调用期 —— 需在 `IVideoCodec` 文档中确认。

### 3.2 修 C3：打通零拷贝 / GPU 路径

**改动文件**：`src/engine/src/engine_media.cpp:1204-1230`、`src/engine/include/nimrtc/engine/engine.hpp`

**改动**：
- `send_video()` 中，若源帧带有 GPU handle（`VideoSourceFrame` 已有 GPU 相关字段，需确认），**跳过 `stage_source_frame`**，直接把 handle 传给 codec
- `EngineConfig` 新增 `video_zero_copy` 开关（默认 off，保持现有行为）

**前置**：`VideoSourceFrame` 需要能携带 GPU handle。需先读 `plugins/video_source.hpp` 确认现状（`frame.hpp:626-662` 已有 `VideoFrameBuffer` 的 GPU 描述，但 `VideoSourceFrame` 是否支持需确认）。

**若 `VideoSourceFrame` 不支持 GPU handle** → 需要在 `plugins::video_source.hpp` 上新增字段（additive），并同步 `IVideoSink` 侧。

### 3.3 修 C4/C5/C6/C7：逐包分配消除

| 项 | 文件 | 改法 |
|---|---|---|
| C4 | `src/modules/rtp/src/builder.cpp:174` | `build()` 增加"写入调用方提供的 buffer"的重载；音频路径复用 `impl_` 内的常驻 buffer |
| C5 | `src/modules/srtp/src/srtp.cpp:307` | `unprotect_rtp` 复用 `impl_->work_buf_`（`protect_rtp` 已经这么做了，`unprotect` 没有） |
| C6 | `src/modules/rtp/src/rtcp.cpp:79` | `demux_rtcp` 增加"写入调用方 vector"的重载；引擎侧复用常驻 vector |
| C7 | `src/engine/src/engine_media.cpp:727` | 视频路径改用 `rtp::PacketBuilder`（Phase 1.3 已做）后自动消除 |

### 3.4 调度器两条排空路径的流控统一

> **决策修正**：1.6 改为"保留 `select_next_packet`"，因此**不删除** `drain_stream_bucket`。本项改为"两条路径共享同一套流控"。

**改动文件**：`src/modules/sched/src/scheduler.cpp`

**目标**：P-family（`drain_with`）与 S-family（`select_next_packet`）**共用同一个 credit 池与同一套 pacing 记账**，避免总发送速率翻倍。

| 动作 | 说明 |
|---|---|
| 抽出共用的 credit 授予逻辑 | 把 `drain_with` 里的"按实测 wall-clock 间隔发放 credit"（`scheduler.cpp:428-455`）抽成一个私有方法 `grant_credit_locked(elapsed_us)`，两条路径共用 |
| S-family 独立的时间基准 | `select_next_packet` 是独立调用，没有"上次排空"的参照 → 在 `Impl` 里加 `stream_last_drain_time_` + `stream_has_last_drain_`，首次调用用 `drain_interval_ms` 作种子（与 `drain_with` 的做法一致） |
| credit 共享 | 两条路径读写**同一个** `impl_->credit_milli_bytes_`，保证会话总速率上限不被绕过 |
| pacing 记账统一 | 抽出 `consume_pacing_locked(class, cost)` 私有方法，两条路径共用 |
| 统计分离 | 保留 `stream_sent_total_` / `stream_dropped_total_`（`scheduler.cpp:296-298`）独立统计，但 credit / pacing 的消耗计入**会话级**统计 |

**验收**：
- 新增测试：P-family 与 S-family 混合排空时，**总发送字节数不超过** BWE 限额
- 新增测试：S-family 排空不消耗超出其应得的 credit

### 3.5 Phase 3 验收标准

- `send_video()` 1080p30 的 per-call 堆分配次数 = 0
- 音频路径 per-packet 堆分配次数 = 0
- `tick()` p99 耗时相比基线下降 ≥ 30%
- 调度器在满载下的 `drain_with()` 不阻塞 `enqueue()`

---

## 6. Phase 4 — 结构重构（D 类，共 6 项）

### 4.1 修 D3/D4：目录与 DTLS 统一（Q5 决策：一次做彻底，**无过渡期**）

> **决策（2026-11-12）**：Q5 = 统一目录，**一次做彻底**。不做 include 路径过渡期，**在同一次提交内完成全部路径迁移**。

**目标结构**：

```
src/modules/
├── dtls/                    ← 统一后的 DTLS（含 seam + 两个后端）
│   ├── include/nimrtc/dtls/          ← seam 头（IDtlsSession / IDtlsSessionFactory / 类型）
│   ├── src/                          ← GmSSL 后端
│   ├── backends/wolfssl/             ← wolfSSL 后端（原 src/modules/dtls/ 的内容）
│   └── tests/
├── raw_udp/                 ← 原 src/raw_udp/
├── sctp/                    ← 原 src/sctp/
├── transport/               ← 原 src/transport/
└── web_transport/           ← 原 src/web_transport/
```

**关键点：include 路径 `nimrtc/dtls/...` 保持不变。**

这是"彻底"与"破坏性"的分界线 —— **目录搬家 ≠ include 路径变更**：
- `git mv src/dtls src/modules/dtls_seam_tmp` → 合并内容 → 落到 `src/modules/dtls/`
- include 路径仍是 `#include <nimrtc/dtls/dtls_session_iface.hpp>`，**下游用户的 include 不需要改**
- 只有 **CMake target 名**和 **include 目录的安装路径**可能需要调整

**具体步骤**：

| 步 | 动作 | 备注 |
|---|---|---|
| 1 | 把 `src/dtls/` 的 seam 头（`include/nimrtc/dtls/`）与 GmSSL 后端（`src/`）迁入 `src/modules/dtls/` | `git mv` 保留历史 |
| 2 | 把 `src/modules/dtls/`（现有 wolfSSL 实现）迁入 `src/modules/dtls/backends/wolfssl/` | `git mv` |
| 3 | 合并两个 `CMakeLists.txt`，产物 target 名保持 `nimrtc_dtls_seam` / `nimrtc_dtls`（避免破坏下游链接） | **target 名是下游的 ABI/链接契约，不能改** |
| 4 | `src/raw_udp/` → `src/modules/raw_udp/` | |
| 5 | `src/sctp/` → `src/modules/sctp/` | |
| 6 | `src/transport/` → `src/modules/transport/` | |
| 7 | `src/web_transport/` → `src/modules/web_transport/` | |
| 8 | 根 `CMakeLists.txt:271-275` 的 5 个 `add_subdirectory(src/xxx)` 删除，改由 `src/modules/CMakeLists.txt` 统一 `add_subdirectory` | 注意**顺序**：seam 必须在依赖它的模块之前 |
| 9 | 全量校验 include 路径未变 | 见下方"验证" |

**为什么可以"彻底"而不需要过渡期**：include 路径（`nimrtc/dtls/...`、`nimrtc/sctp/...` 等）**保持不变**，只有**磁盘位置**变了。下游用户 `#include <nimrtc/dtls/...>` 的代码**一行都不用改**。真正会受影响的是：
- 直接引用 CMake target 名的人 → **target 名保持不变**即可
- 直接引用源码路径的人（如 `add_subdirectory(nimrtc/src/dtls)`）→ 属于非公开用法，可在 CHANGELOG 中声明

**验证**（本次改动的核心验收）：
```powershell
# 1. include 路径未变（应输出 0 个差异）
git grep -n "nimrtc/dtls/\|nimrtc/sctp/\|nimrtc/raw_udp/\|nimrtc/transport/\|nimrtc/web_transport/" -- ':!docs' | ...
# 2. 无残留旧目录
Test-Path src/dtls, src/sctp, src/raw_udp, src/transport, src/web_transport   # 期望全 False
# 3. CMake target 名未变
#    对比改动前后的 target 列表
```
**include 站点数**（改动前的基线，用于比对）：`nimrtc/dtls/` 85 处、`nimrtc/raw_udp/` 41 处、`nimrtc/sctp/` 41 处、`nimrtc/transport/` 35 处、`nimrtc/web_transport/` 75 处 —— **改动后这些数字应完全不变**。

**同步更新**：`ARCHITECTURE.md` 的目录树（`:46-82`）+ 删除"seam-family 目录"整段辩护（`:84-108`）+ Layout Invariant 3 的措辞。

**风险点**：
- **CMake 循环依赖**：`src/core` 的 `pal_default_registrars.cpp` 引用各模块的 `register_default_plugins`，而模块又依赖 `core`。搬目录后 `src/modules/CMakeLists.txt` 的 `add_subdirectory` 顺序会变化 → 需确认 `nimrtc_core_objects` 的 OBJECT 库机制仍然成立（`src/core/CMakeLists.txt:26-47`）
- **可选模块的 `EXISTS` 守卫**：`src/modules/CMakeLists.txt:185-227` 有 `if(EXISTS ...)` 形式的守卫，搬家后路径要同步

**执行方式**：建议拆成 **2 次提交**（虽然用户要求"一次做彻底"，但 git 历史上分 2 步更安全）：
- 提交 A：5 个目录搬进 `src/modules/`（include 路径不变，target 名不变）
- 提交 B：DTLS 内部整合（seam + GmSSL + wolfSSL 归到一个模块目录）

两次之间每步都跑全量 `ctest`。

#### 4.1 执行结果（2026-11-13）

| 验收项 | 结果 |
|---|---|
| 旧目录 `src/{dtls,raw_udp,sctp,transport,web_transport}/` | ✅ 全部不存在 |
| include 路径未变 | ✅ `engine_impl.hpp` 的 include 行与 `main` 逐字节相同；`nimrtc/<family>/...` 拼写保留 |
| CMake target 名未变 | ✅ `nimrtc::{dtls,dtls_seam,sctp,raw_udp,transport,web_transport}` 与 `nimrtc_*` 归档名 |
| DTLS 归一（D3） | ✅ `src/modules/dtls/{include,src,backends/gmssl,tests}/` —— seam 头 + wolfSSL + GmSSL 同处一个模块 |
| `ARCHITECTURE.md` 同步 | ✅ 目录树更新；"seam-family 目录"整段辩护删除 |
| 全量 `ctest` | ✅ 1411 / 1411（Windows MSVC Release） |

最终形态：

```
src/modules/
├── dtls/                    ← 统一的 DTLS 模块
│   ├── include/nimrtc/dtls/ ← seam 头（IDtlsSession / IDtlsSessionFactory / 类型）
│   ├── src/                 ← wolfSSL 会话实现
│   ├── backends/gmssl/      ← GmSSL 后端
│   └── tests/
├── raw_udp/                 ← 原 src/raw_udp/
├── sctp/                    ← 原 src/sctp/
├── transport/               ← 原 src/transport/
└── web_transport/           ← 原 src/web_transport/
```

**剩余风险已排除**：
- `nimrtc_core_objects` 的 OBJECT 库机制未受影响 —— `src/core/CMakeLists.txt` 只是把五处
  `${CMAKE_SOURCE_DIR}/src/<family>/include` 改成 `src/modules/<family>/include`，
  依赖方向不变（core → 模块头文件，模块 → `nimrtc::core`），无环。
- `if(EXISTS ...)` 守卫：五个 seam 目录是**无条件** `add_subdirectory`（它们不是可选模块），
  守卫只用于 `h265` / `av1` / `video_*` 等可选模块，路径未受影响。

### 4.2 修 D2：死模块处置

| 模块 | 处置 | 理由 |
|---|---|---|
| `video_stats` | **接线**或**删除** | 引擎已有内联统计；`video_stats` 模块是完整实现但没接。若保留则必须接线 |
| `video_sdp_attributes` | **接线**到 SDP 生成路径 | `docs/plan/remote-desktop-plan.md` 的 H.265/AV1 fmtp 需要它 |
| `assembly` | **接线**到 `transport_strategy.hpp` 的 profile 解析 | 已被引用但未真正消费 |
| `jb_strategy` | **接线**到 `jb` / `video_jb` 的策略选择 | 已被引用 |
| `nvcodec` | **保留待接线**（硬件编解码合成层） | 与 `h264`/`h265`/`av1` 的 `hw_backends.hpp` 集成中 |
| `video_rtcp_feedback` | **接线**到 `video_pipeline` | 已被引用 |

**原则**：要么接线，要么删除。**"有完整实现 + 有测试 + 引擎不加载"是最坏状态** —— 它制造"功能已具备"的错觉。

**优先级**：`video_sdp_attributes` 与 `video_stats` 优先（当前分支正在做 H.265/AV1，远程桌面计划需要）。

### 4.3 修 D1：引擎拆分

**目标**：`engine_impl.hpp` 从 1109 行降到 < 400 行。

**拆分方案**（按关注点提取协作者类，引擎只做编排）：

| 新类 | 职责 | 从哪来 |
|---|---|---|
| `VideoPipeline` | video source/codec/sender/receiver/sink 的编排 + 生命周期 | `engine_media.cpp` 的 Phase 1-2 |
| `AudioPipeline` | audio3a/codec/receiver/JB 的编排 | `engine_media.cpp` + `engine_audio_frames.cpp` |
| `RtpPacketizer` | 音视频 RTP 打包（统一走 `rtp::PacketBuilder`） | `engine_media.cpp:695-730` + `:1030-1090` |
| `RtcpProcessor` | RTCP 解析 / SR / RR / 反馈 | `engine_rtcp.cpp` |
| `TimelineTracker` | ADR-017 时间线 | `engine_timeline.cpp` |
| `TransportManager` | ICE/DTLS/SRTP/调度器编排 | `engine_transport.cpp` |

**约束**：新类全部放 `src/engine/src/` 内部（不新增公共 API），`Impl` 只持有它们的成员。

**风险控制**：分 6 次独立 PR，每次搬一个协作者，每次跑全量测试。

### 4.4 修 D5：C ABI **保留**（Q1 决策的连带修正）

> **决策修正**：1.6 改为"保留 `select_next_packet`"后，S-family 的 C ABI 入口**不再删除**。

**改动文件**：`src/modules/sched/include/nimrtc/sched/sched.hpp:281-320`

`sched_create` / `sched_destroy` / `sched_set_stream_priority` / `sched_get_stream_priority` / `sched_enqueue_stream_owned` / `sched_select_next_packet` **全部保留**。

**理由**：C ABI 是给 **C 语言 / 嵌入式宿主**的稳定入口 —— 而嵌入式正是 NimRTC 的目标场景。S-family 是目前唯一能表达"按流优先级"的公开能力，删掉它会削弱嵌入式用户的能力。

**补充动作**：
- 在 `sched.hpp` 的 C ABI 段落加注释，说明这五个入口是 **S-family 的 C 宿主入口**，不是遗留物
- **逐个核实**每个 C ABI 入口是否有外部使用者（`grep -rn "sched_" src/ tests/ examples/`）；若某个入口确实零调用且从未对外承诺，可单独删除并记录 —— 但 `select_next_packet` 相关的一律保留
- 确认 C ABI 的 `DrainCallback` 与 C++ 回调的 ABI 兼容性（`plugins/scheduler.hpp:198`）

### 4.5 修 D6：引擎与具体模块的耦合

**目标**：`engine_impl.hpp` 不再 include 具体模块头。

**当前 include**（`engine_impl.hpp:86-122`）：
```cpp
#include <nimrtc/sdp/session_description.hpp>
#include <nimrtc/rtp/packet.hpp>
#include <nimrtc/rtp/rtcp.hpp>
#include <nimrtc/rtp/timeline.hpp>
#include <nimrtc/audio3a/audio3a.hpp>
#include <nimrtc/h264/codec_plugin.hpp>
#include <nimrtc/dtls/dtls.hpp>
#include <nimrtc/srtp/srtp.hpp>
```

**改法**：把每处具体类型用法替换为接口用法：
- `sdp::Parser` / `sdp::Munger` → **Q2 已定走方案 A（先不替换）** → 这两行**保留**，并在 `engine_impl.hpp` 顶部加注释标注为"已记录的 L4 例外：RTP/SDP"
- `rtp::PacketBuilder` → 同上，保留并标注例外；但把 `rtp::` 的用法**集中到 1 个文件**（`src/engine/src/rtp_packetizer.cpp`，见 4.3），避免散落
- `dtls::IDtlsSession` → 已经是 seam 接口（`src/dtls/include/nimrtc/dtls/dtls_session_iface.hpp`），OK
- `srtp::SrtpContext` → **Q3 已定接线**（2.5）→ 改为 `plugins::ISrtpSessionBackend` 接口
- `audio3a::` → 查具体用法

**现实约束**：`AGENTS.md` 的 L4 说"`src/engine/` 只做插件编排，不直接包含任何具体模块头文件"。**完全达成需要 2.1 选方案 B**（SDP/RTP 走接口）。Q2 已定走方案 A，因此只能达成部分 —— 需要在 `AGENTS.md` / `ARCHITECTURE.md` 中显式记录"引擎内部允许依赖 RTP/SDP 具体实现"这一例外，并说明理由（热路径零虚拟调用 + 接口无法表达引擎实际所需的细粒度能力）。

**执行**：**修改 L4 约束本身**，明确豁免 RTP/SDP，把"不 include 具体模块头"的适用范围收窄到"可替换的那些模块"。这比强行抽象更诚实。L4 改写后的措辞草案：

> **L4** `src/engine/` 只做插件编排，不直接包含任何具体模块头文件。
> **L4 例外（ADR-021）**：`src/engine/` 允许直接使用 `rtp::` 与 `sdp::` 的具体实现 —— 这两个模块**不可替换**（见 ADR-021）。其 `rtp::` 用法必须集中在 `src/engine/src/rtp_packetizer.cpp` 一个文件内。

**注意**：DTLS 目录合并后，`src/dtls/` → `src/modules/dtls/`，`engine_impl.hpp` 的 `#include <nimrtc/dtls/dtls.hpp>` **include 路径不变**（4.1 的关键设计），这一行不需要改。

### 4.6 修 B7/B9 的最终形态 + Phase 4 验收

- `ctest` 全绿
- `engine_impl.hpp` 行数 < 400
- 无死模块（每个模块或被引擎加载，或被删除）
- `ARCHITECTURE.md` 的 Layout Invariant 4 描述与代码一致

---

## 7. Phase 5 — 文档与 ADR 同步

### 7.1 必改文档

| 文件 | 改动 |
|---|---|
| `docs/plugin_author_guide.md` | §3 Step 4 的"no silent fallback"改为准确描述；§4 表格全量对齐；新增"当前可替换性矩阵"；新增"如何报告你的插件 ID"章节 |
| `docs/api/engine_config.md` | 补齐新增/变更的配置项；补 `std::string_view` → `std::string` 的迁移说明 |
| `ARCHITECTURE.md` | Layout Invariant 4 收窄（RTP/SDP 豁免）；去掉 DTLS"权威在哪"的辩护段；更新目录树 |
| `docs/hw_plugin_seam.md` | 同步 `IVideoCodec::kind()` 契约与 GPU handle 传递方式 |
| `docs/adr/ADR-001-plugin-system.md` | 补充"插件 ID 命名契约"与"哪些族可替换"的决策 |
| `docs/guides/switching-dtls-backend.md` | 补充新增的契约测试要求 |
| `docs/troubleshooting.md` | 新增"配置了插件但没生效"的排查条目 |
| `AGENTS.md` | 视 4.5 的决策更新 L4 |

### 7.2 新增 ADR

- `ADR-019-plugin-id-naming-contract.md` —— 插件 ID 的命名规则与校验方式
- `ADR-020-engineconfig-string-ownership.md` —— 配置字段的字符串所有权与生命周期
- `ADR-021-sdp-rtp-not-pluggable.md` —— SDP/RTP 为何不走插件接口（若 2.1 选 A）

### 7.3 版本策略

按 ADR-015：
- Phase 1 的 `EngineConfig` 字段类型变更（`string_view` → `string`）**是 ABI 破坏性的** → 需要 MINOR 版本号提升 + 迁移说明
- Phase 2 的新增配置项（`srtp_backend_name` / `video_capture_name` / `sctp_socket_name` / `raw_udp_name` / `transport_stack_name`）是 additive → 无破坏
- Phase 2 删除 `EngineConfig::rtp_name` / `sdp_name` —— 这是**字段移除**，属于破坏性 → 与 Phase 1 的类型变更**合并到同一个 MINOR 版本的迁移说明**里
- Phase 1.6 的 `IScheduler` 新增虚函数（**带默认实现**）→ additive，不破坏现有实现 → MINOR
- Phase 4.1 的目录搬家：**include 路径与 CMake target 名均不变** → **对下游用户零破坏**，不需要版本过渡；仅在 CHANGELOG 声明"源码目录位置变化，非公开用法受影响"
- Phase 4.5 的 L4 约束改写 → 文档变更，无 API 影响

---

## 8. 已决策的问题（2026-11-12 全部拍板）

| # | 问题 | **决策** | 落地章节 |
|---|---|---|---|
| Q1 | `select_next_packet` 保留还是删除？ | **保留 + 补齐流控**（不删除） | §0.1 / §1.6 / §3.4 / §4.4 |
| Q2 | SDP / RTP 是否做成可替换？ | **先不替换** —— 删假配置项，接口标 deprecated | §2.1 / §4.5 / §7.2 |
| Q3 | `ISrtpSessionBackend` 接线还是删除？ | **接线** —— SRTP 后端可替换 | §2.5 / §4.5 |
| Q4 | `CapabilitySelector` 接线还是删除？ | **接线** —— Selector 真正决定 media/control stack | §2.6 |
| Q5 | 目录统一是否要做？ | **做，一次做彻底** —— 5 个 seam 目录并入 `src/modules/`，**include 路径与 target 名不变** | §4.1 |

**决策变更对工作量的影响**：
- Q1 从"删除"改为"保留 + 补齐" → **Phase 1/3 各增约 1 天**（S-family 流控实现 + 双路径共享记账）
- Q3 接线 → Phase 2 增约 3 天（注册表槽位 + 引擎接线 + 回归测试）
- Q4 接线 → Phase 2 增约 2 天（风险最高的一项）
- Q5 "彻底做" + include 路径不变 → Phase 4 总量不变，但**风险大幅下降**（无 include 破坏，无需转发头）

**合计估算**：56-86 天（较原估 +4 天）

---

## 9. 工作量估算与排期

| Phase | 内容 | 估算 | 依赖 |
|---|---|---|---|
| 0 | 基线 + 守卫测试 | 3-5 天 | — |
| 1 | P0 正确性（12 项，含 A9 补齐） | 9-13 天 | Phase 0 |
| 2 | 插件契约（10 项，含 Q3/Q4 接线） | 13-17 天 | Phase 1；Q2/Q3/Q4 已定 |
| 3 | 性能（7 项，含双路径流控统一） | 11-16 天 | Phase 1（3.1/3.2 需 Phase 2 的接口结论） |
| 4 | 结构（6 项，含目录彻底统一） | 20-30 天 | Phase 2-3 |
| 5 | 文档 + ADR | 3-5 天 | Phase 1-4 |
| **合计** | | **59-86 天** | |

**建议排期**：Phase 0 + 1 作为一个 PR（"P0 正确性修复"，约 2 周）；Phase 2 拆 3 个 PR（2.1/2.2/2.3+2.4 一组、2.5 SRTP 一组、2.6 Selector 一组）；Phase 3 拆 2-3 个 PR；Phase 4 拆 6+ 个 PR（4.1 拆 A/B 两个提交）。

**关键路径**：Phase 0 → Phase 1 → Phase 2.2（配置项）→ Phase 2.6（Selector 接线）→ Phase 4.1（目录）→ Phase 4.3（引擎拆分）。

**最先可启动的工作**（不依赖任何待定决策）：
- Phase 0：基线 + 注册表一致性守卫
- Phase 1.1（A1-A3 transport 空指针契约）、Phase 1.4/1.5（视频 RTP 统一）、Phase 1.7/1.8（字符串生命周期）、Phase 1.3（A6/A7 分辨率）

---

## 10. 每条任务的完成定义（DoD）

任何一项任务算"完成"必须满足：

1. **有针对性测试**，且该测试在修复前失败、修复后通过
2. **无回归**：全量 `ctest` 通过
3. **ASAN/UBSAN 干净**（`tools/sanitizer_gate.py`）
4. **文档同步**：若涉及公共 API / 配置项 / 插件契约，`docs/` 与 ADR 已更新
5. **提交规范**：`Conventional Commits` + `Signed-off-by:`，英文提交信息
6. **编码规范**：`.clang-format`（LLVM style, 100 列）
7. **根目录清洁**：不产生临时文件（`.cursor/rules/root-cleanliness.mdc`）

---

## 11. 附录：完整文件改动清单

### Phase 1
```
src/transport/include/nimrtc/transport/transport_stack.hpp      (rtp_id, sctp_id)
src/transport/src/webrtc_classic_stack.cpp                      (4× return nullptr)
src/engine/src/engine_media.cpp                                  (RTP builder, 分辨率)
src/engine/include/nimrtc/engine/engine.hpp                      (string_view→string, video_width/height/fps)
src/engine/src/engine_impl.hpp                                   (video decode 尺寸字段)
src/modules/sched/src/scheduler.cpp                              (锁外 I/O, S-family 流控)
src/modules/sched/include/nimrtc/sched/sched.hpp                 (scope note 修正)
src/plugins/include/nimrtc/plugins/scheduler.hpp                 (IScheduler 新增虚函数, 带默认实现)
src/core/include/nimrtc/core/registry.hpp                        (键改 std::string)
src/core/src/plugin_registry.cpp                                 (list_* 返回类型)
src/plugins/include/nimrtc/plugins/video_codec.hpp               (kind() 契约)
tests/test_registry_contract.cpp                                 (新增)
tests/test_transport_stack_contract.cpp                          (新增)
tests/test_sched_stream_flowcontrol.cpp                          (新增, S-family 流控)
```

### Phase 2
```
src/engine/include/nimrtc/engine/engine.hpp                      (新增 5 个配置项, 删 rtp_name/sdp_name)
src/engine/src/engine_transport.cpp                              (接线新配置项, Selector resolver)
src/engine/src/engine_capture.cpp                                 (video_capture 接线)
src/engine/src/video_codec_config.hpp                             (family 解析顺序)
src/plugins/include/nimrtc/plugins/video_codec.hpp                (契约文档)
src/plugins/include/nimrtc/plugins/srtp_backend.hpp               (接线)
src/modules/srtp/src/srtp.cpp, libsrtp_backend.cpp                (SrtpContext 走接口)
src/core/include/nimrtc/core/registry.hpp                         (srtp 槽位 + 宏)
src/core/src/plugin_registry.cpp, pal_default_registrars.cpp      (槽位注册)
src/transport/src/transport_selector.cpp                          (resolver)
docs/plugin_author_guide.md                                       (全量对齐)
tools/check_doc_contract.py                                       (新增)
```

### Phase 3
```
src/engine/src/engine_media.cpp                                  (池化, 零拷贝)
src/engine/src/engine_impl.hpp                                    (缓冲池成员)
src/engine/include/nimrtc/engine/engine.hpp                       (video_zero_copy)
src/plugins/include/nimrtc/plugins/video_source.hpp               (GPU handle 字段)
src/modules/rtp/src/builder.cpp                                   (build 重载)
src/modules/rtp/src/rtcp.cpp                                      (demux 重载)
src/modules/srtp/src/srtp.cpp                                      (unprotect 复用 buffer)
src/modules/sched/src/scheduler.cpp                               (双路径共享 credit/pacing)
src/modules/video_frame/include/nimrtc/video_frame/frame.hpp       (池化支持)
```

### Phase 4
```
src/dtls/ → src/modules/dtls/                                     (git mv, include 路径不变)
src/modules/dtls/ → src/modules/dtls/backends/wolfssl/            (DTLS 内部整合)
src/raw_udp/ → src/modules/raw_udp/                               (include 路径不变)
src/sctp/ → src/modules/sctp/                                     (include 路径不变)
src/transport/ → src/modules/transport/                           (include 路径不变)
src/web_transport/ → src/modules/web_transport/                   (include 路径不变)
src/engine/src/engine_impl.hpp                                    (拆分后 < 400 行)
src/engine/src/video_pipeline_helper.cpp                          (新增)
src/engine/src/audio_pipeline_helper.cpp                          (新增)
src/engine/src/rtp_packetizer.cpp                                 (新增)
src/engine/src/rtcp_processor.cpp                                 (新增)
src/modules/video_stats/                                         (接线或删除)
src/modules/video_sdp_attributes/                                (接线)
src/modules/assembly/, jb_strategy/                               (接线)
src/core/CMakeLists.txt, src/engine/CMakeLists.txt, src/modules/CMakeLists.txt
ARCHITECTURE.md, AGENTS.md
```

### Phase 5
```
docs/plugin_author_guide.md
docs/api/engine_config.md
docs/api/engine_api.md
docs/hw_plugin_seam.md
docs/troubleshooting.md
docs/adr/ADR-019-plugin-id-naming-contract.md      (新增)
docs/adr/ADR-020-engineconfig-string-ownership.md  (新增)
docs/adr/ADR-021-sdp-rtp-not-pluggable.md          (新增, 条件)
docs/adr/ADR-001-plugin-system.md                   (补充)
ARCHITECTURE.md
AGENTS.md
```
