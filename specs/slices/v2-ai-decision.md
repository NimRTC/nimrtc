# Slice: v2-ai-decision

## 目的
fork `examples/demo-agent-gateway` 示例出 `examples/demo-agent-realtime/`，
把「PCM tap → VAD → mock LLM → 决策 JSON → 带帧标签的控面消息」这条链跑通，
并把 M1 §5.2 的 `ai_decision` JSON 格式与 §5.3 的 `partial` / `barge_in` 字段定死。

这是 M1 的核心片：M2 的「处刑台」与 M3 的「语音 Agent」都建立在
「AI 决策能带帧标签地发出去」这件事之上。本片先把格式与链路做成，
真模型留到 M3（§8 明确「M1 不接真 ASR/LLM/TTS」）。

## 范围
- examples/demo-agent-realtime/**
- examples/CMakeLists.txt
- tests/acceptance/v2-ai-decision/acceptance.test.cjs

## 需求
1. `examples/demo-agent-realtime/` 是**原 gateway 示例的 fork**：
   保留合成 48kHz PCM + 能量 VAD（`mock_asr`），**不改动**原 gateway 示例的行为。
2. ASR 与 LLM 是**可插拔回调**（`IASR` / `ILLM` 形态的函数对象），
   M1 用 mock 实现，M3 换成真模型时不动调用点。
3. 决策消息按 §5.2 的 JSON 格式产出，字段齐备：
   `type` / `frame_seq` / `capture_ts_us` / `action` / `reason` / `age_us_at_decision`。
4. 控制通道标签为 `agent-control`，且是 **framed** 模式；
   两端 `framing` 必须一致（framed 与 raw 不互通）。
5. §5.3 的 `partial` 与 `barge_in` 两种消息在本片就有**字段定义与解析入口**，
   M1 不要求真的产生语音，但解析路径可跑通（喂进去能解出结构）。
6. 决策时能拿到「当前帧」的 `frame_seq` 与 `capture_ts_us`；
   拿不到时 `frame_seq` / `capture_ts_us` 如实置空，不编造。
7. 控制台按 Timeline 瀑布打屏：每条决策打一行（时间戳 + 帧号 + 动作 + age）。
8. 在 examples 的 CMake 中登记新子目录，并让新子目录自带一份 CMake 描述文件。

## 验收标准
1. `examples/demo-agent-realtime/` 存在且**原 gateway 示例未被改动**；验证方式：静态检查新目录存在，且原示例文件内容与版本分支上的基线一致
2. 决策消息的 JSON 含 §5.2 全部六个字段；验证方式：单元测试构造一次决策，断言序列化后的 JSON 依次含 `type`、`frame_seq`、`capture_ts_us`、`action`、`reason`、`age_us_at_decision`
3. `type` 取值为 `ai_decision`；验证方式：单元测试断言该字段的取值
4. `partial` 与 `barge_in` 两种消息可被解析成结构体；验证方式：单元测试各喂一条 §5.3 样例 JSON，断言解析成功且 `ref_frame` / `capture_ts_us` 取值正确
5. 数据通道标签为 `agent-control` 且 framing 为 framed；验证方式：静态检查创建通道处的标签字面量与 framing 参数
6. 决策时拿不到帧信息时，`frame_seq` 与 `capture_ts_us` 为空而不是伪造值；验证方式：单元测试在 provider 返回空的情形下断言这两个字段为空
7. 本片不引入真 ASR / LLM / TTS 依赖（不新增第三方依赖）；验证方式：静态检查新增文件未引入外部模型 SDK 头文件或链接
8. `examples/demo-agent-realtime/` 已被 CMake 构建系统收录；验证方式：静态检查 `examples/CMakeLists.txt` 含该子目录，且 `examples/demo-agent-realtime/CMakeLists.txt` 存在
9. 原 gateway 示例的源文件未被改动；验证方式：静态检查该文件里没有 `demo-agent-realtime` 相关改动（即 fork 是复制而非原地修改）

## 风险
1. `create_data_channel` 在引擎里是**延迟创建**的（调用方负责建，默认不自动建）→ 决策消息可能在通道就绪前发出；应对：通道未就绪时按「决策先算、消息后发」排队或直接丢弃并计数，不阻塞决策循环
2. Tap 是 10ms 一帧，直接调 LLM 会烧 token；应对：按 100ms 攒批（§3 坑 4 明确预警过）
3. `framed` 与 `raw` 两端不一致时通道静默不通 → 表现为「决策发出去了但对面收不到」；应对：建通道时两端都显式声明 framed，并在验收里断言这个参数
4. `capture_ts_us` 是对端时钟重建值，与本地 Unix 时间可能有偏差；应对：本片只负责如实透传，不做跨端时钟对齐（那是 ADR-017 tracker 的事）
