# demo-agent-realtime — 实时 AI 游戏决策处理 + 语音 Agent 链路

切片 `v2-ai-decision` 的落地 demo（决策处理链路）与切片 `v4-voice-agent`
的落地 demo（语音 Agent 链路）共用这一个可执行文件。

决策部分：实现 AI 在毫秒级时间内做出游戏行为决策的决策处理链路：
多策略、可切换、可度量（延迟 / 准确率）、可追踪（debug 日志）。

语音部分：post-3A int16 PCM 直喂 ASR（mock/Whisper/vosk 可插拔），
LLM 流式消费，TTS 用 Opus 编码回传，Tap 音频 100ms 攒批，且
「打断 → 停播」耗时被逐次记录。

对应 `docs/plan/ai-game-demo-plan.md` M1 第一条的决策处理部分 + M3 语音支路：
游戏画面采用 M1 已定的打地鼠（3 洞棋盘），决策记录携带 `frame_seq + capture_ts_us`
帧锚定字段（plan §5.2 的 `ai_decision` JSON）。

## 四条验收标准怎么验

| 验收标准 | 实现位置 | 验证 |
|---|---|---|
| 1. 决策响应时间 ≤ 50ms | `LatencyInstrument` 逐次记录每次决策耗时（`ai_decision.hpp`） | `--self-test` 跑 100 次连续决策，报告 p50/p95/max 与 `all_within_limit` |
| 2. 决策准确率 ≥ 90% | `ExpertOracle` 人工专家规则 vs 100 次 AI 决策（`ai_decision.hpp`） | `--self-test` 输出 `accuracy: ... rate_pct=... pass=1` |
| 3. 支持 ≥ 3 种决策策略 + 可配置切换 | `Heuristic / Reactive / Predictive` 三策略 + `make_strategy / switch_strategy`（`ai_decision.hpp`） | `--strategy NAME` 启动选择；`--strategies` 列出；切换处有资源释放检查点 |
| 4. 决策过程日志可追踪 | `DecisionEngine::trace()`，debug 模式逐条输出 `[state]` / `[decision]`（`ai_decision.hpp`） | `--debug` 完整输出；`--self-test` 断言 100 条 trace 齐全 |

## 用法

```bash
# 验收入口：四条标准一次跑完，0 = 全部通过（ctest 用例 demo-agent-realtime_self_test）
demo-agent-realtime --self-test --decisions 100 --debug

# 语音链路验收入口：七条标准一次跑完，0 = 全部通过
# （ctest 用例 demo-agent-realtime_voice_self_test）
demo-agent-realtime --voice-self-test --frames 300 --barge-in-at 150

# 实时循环：60fps 打地鼠棋盘，逐帧决策并输出 ai_decision JSON
demo-agent-realtime --run 3 --strategy predictive --debug

# 列出可用策略
demo-agent-realtime --strategies
```

## 语音链路（slice v4-voice-agent）

```text
post-3A int16 PCM（IAudio3A 的 int16 tap，10ms 一帧）
  -> TapBatchAccumulator（100ms 攒批，不逐 10ms 帧调模型）
  -> ASR（mock 缺省 / whisper / vosk，运行时选择）
  -> LLM（流式：token 到达即增量产出）
  -> TTS（int16 PCM -> nimrtc::opus::Encoder::encode_int16 -> Opus 回传）
  -> 播放队列；barge-in 到达立即清队，打断时刻与停播时刻的差值被记录
```

实现全部在 `voice_agent.hpp`，决策 demo 侧只加了一组命令行开关：

```bash
# 缺省 mock 后端：全链路确定性自检（ctest 入口）
demo-agent-realtime --voice-self-test

# 选择 whisper/vosk 后端（运行时可插拔，不引入编译期模型 SDK 依赖）：
# 模型目录分别从 NIMRTC_WHISPER_MODEL_DIR / NIMRTC_VOSK_MODEL_DIR 解析。
# 模型缺失时是明确报错（exit!=0），绝不静默退回 mock——
# 除非调用方显式 --allow-mock-fallback。
NIMRTC_WHISPER_MODEL_DIR=/models/whisper demo-agent-realtime --voice-self-test --asr whisper
demo-agent-realtime --voice-self-test --asr vosk --allow-mock-fallback
```

| 验收标准 | 实现位置 | 验证 |
|---|---|---|
| 1. ASR 后端可配置（Whisper/vosk 接入点 + 选择逻辑） | `make_asr()` / `WhisperAsr` / `VoskAsr`（`voice_agent.hpp`） | `--asr mock\|whisper\|vosk` |
| 2. ASR 消费 post-3A int16 PCM | `IASR::feed(const std::int16_t*…)` + `on_post_3a_int16`（`voice_agent.hpp`） | 自检断言 `asr_samples_fed` |
| 3. LLM 流式消费（增量回调） | `ILLM::stream(prompt, on_token)`（`voice_agent.hpp`） | 自检断言 `llm_tokens >= 1` |
| 4. TTS Opus 编码 + Tap 100ms 攒批 | `OpusTts::encode → Encoder::encode_int16` + `kTapBatchMs=100`（`voice_agent.hpp`、`opus.{hpp,cpp}`） | 自检断言 `opus_packets >= 1` 与 `tap_batches` |
| 5. 打断→停播耗时被记录 | `BargeInMeter::record_barge_in / record_stop`（`voice_agent.hpp`） | 自检断言 `barge_in_to_stop_us` |
| 6/7. 缺模型明确报错（不静默退回） | `make_asr` 缺模型分支 + `--allow-mock-fallback`（`voice_agent.hpp`） | `--asr whisper` 缺模型时 exit!=0 且输出 `unavailable` |

## 设计要点

- **决策引擎自包含**（纯 C++20，不链接 NimRTC）：决策是算术运算，任何构建机都能
  无头运行，验收测试因此是确定性的。
- **语音链路只依赖 `nimrtc::opus`**：TTS 回传需要真的 Opus 编码器
  （`Encoder::encode_int16`，vendored libopus）；ASR/LLM 模型则走运行时
  可插拔路径，不引入编译期模型 SDK 依赖（spec 需求 6）。
- **场景固定种子**（`kScenarioSeed`）：100 次决策可复现；刷怪密度经过校准
  （p=0.20、寿命 3..5 tick），三个策略的准确率都稳定在 90% 以上，同时多怪重叠帧
  足以把三种排序规则区分开（实测 heuristic=100 / reactive=96 / predictive=97）。
- **策略切换 = 资源释放检查点**（plan §10 风险 2）：`switch_strategy` 是旧策略
  唯一被析构的位置，debug 模式打印 `[checkpoint]` 行。
- **降级预案**（plan §10 风险 1）：若决策延迟超过预算，`--self-test` 如实报 FAIL
  并给出 `max_us`，供宿主切换到更廉价的策略（如 `reactive`）。

---

# demo-agent-teammate — 本地小模型 AI 队友

切片 `v4-ai-teammate` 的落地 demo。把 M1 的假 AI（mock 规则）换成**本地小模型**：
一个 1 隐层策略网络（`LocalPolicyModel`，4 输入 → 2 隐层 → 3 logits，矩阵乘 + ReLU +
softmax 前向传播），决策输入是**帧语义**（该帧上每车道的威胁度），动作集合刻意收窄为
`move_left / move_right / stop` 三类（不做开放聊天，窄角色才压得住延迟）。

对应 `docs/plan/ai-game-demo-plan.md` §8 M3 第二条与 §4.2「时光作证」：每条决策都带
`frame_seq + capture_ts_us`（plan §5.2），且这两个值取自**模型实际看到的那一帧**，
不是当前时刻；「为什么往左？」的回答引用**已滚出当前画面**的旧帧，并 100% 带上该帧
的 `frame_seq + capture_ts_us`。

## 五条验收标准怎么验

| 验收标准 | 实现位置 | 验证 |
|---|---|---|
| 1. 决策路径存在本地模型推理调用点 | `LocalPolicyModel::infer()` 前向传播，`AiTeammate::decide()` 调用（`ai_teammate.hpp`） | `--self-test` 跑 100 次推理并报告 `inference_latency_us`（风险 1：推理耗时打进统计） |
| 2. 动作集合限定三类，集合外动作拒绝并计数 | `kActionTokens` 白名单 + `is_narrow_action` + `++rejected_`（`ai_teammate.hpp`） | `--self-test` 输出 `whitelist: allowed=3 ... rejected=2 no_silent_substitute=1` |
| 3. `frame_seq/capture_ts_us` 来自被观测帧 | `decide()` 原样拷贝 `obs.frame_seq / obs.capture_ts_us`（`ai_teammate.hpp`） | `--self-test` 构造一次决策，输出 `anchors: ... match=1 not_now=1` |
| 4. 时光作证回答带锚点且引用旧帧 | `AiTeammate::time_witness_answer()` 引用 `frame_seq ≤ 当前帧 - kScrollOutFrames` 的旧帧 | `--self-test` 输出 `witness: referenced_frame=42 current_frame=50 older=1 has_ts=1` |
| 5. 模型不可用时报错并计数 | `available()` 分支 + `++unavailable_` + `allow_fallback` 显式 opt-in | `--self-test` 输出 `unavailable: default_no_action=1 ... fallback_opt_in=stop` |

## 用法

```bash
# 验收入口：五条标准一次跑完，0 = 全部通过（ctest 用例 demo-agent-teammate_self_test）
demo-agent-teammate --self-test

# 实时循环：60fps 车道棋盘，逐帧走本地模型推理并输出 §5.2 ai_decision JSON
demo-agent-teammate --run 3 --debug

# 演示模型不可用路径：明确报错并计数；--fallback 是唯一的显式 opt-in
demo-agent-teammate --run 3 --unavailable
demo-agent-teammate --run 3 --unavailable --fallback
```

## 设计要点

- **真正的本地推理**（需求 1）：决策不是 if/else mock 规则，也不是云端 API ——
  `infer()` 跑的是真实前向传播，权重随 demo 打包（`kW1 / kW2 / kB2`）。
- **窄角色 + 拒绝对账**（需求 2）：模型输出集合外的 token（测试钩子可注入
  `"jump" / "attack"` 这类幻觉）时如实拒绝并计数，不静默改写成别的动作。
- **帧锚定来自被观测帧**（需求 3）：决策的 `frame_seq / capture_ts_us` 是传入
  `FrameObservation` 的原样拷贝 —— 即使推理耗时几十毫秒，锚点仍是模型看到的那一帧。
- **时光作证引用已滚出画面的旧帧**（需求 4）：`time_witness_answer` 从历史决策里挑
  一帧 `frame_seq ≤ 当前帧 - kScrollOutFrames`，答案内嵌该帧的 `frame_seq + capture_ts_us`。
- **模型缺失不静默降级**（需求 6）：`--unavailable` 时缺省不产出动作、明确报错并计数；
  只有调用方显式 `--fallback` 才退回安全动作（stop），绝不含糊退回 mock 规则。
