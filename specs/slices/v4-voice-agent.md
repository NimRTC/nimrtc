# Slice: v4-voice-agent

## 目的
把 M1 的 mock ASR/LLM 换成**真模型**：post-3A int16 直喂 Whisper/vosk，
LLM 走流式，TTS 用 Opus 回传（Tap 100ms 攒批），并让「打断 → 停播」这条链路可量化。

这是 M3 的语音支路。模型是**可插拔实现**（`v2-ai-decision` 已经把 `IASR` / `ILLM`
定义成回调），本片只负责提供真模型实现 + Opus TTS + 攒批策略。

对应 `docs/plan/ai-game-demo-plan.md` §8 M3 第一条，以及 §4.1 链路图里的语音支路。

## 范围
- examples/demo-agent-realtime/**
- src/modules/opus/include/nimrtc/opus/opus.hpp
- src/modules/opus/src/opus.cpp
- tests/acceptance/v4-voice-agent/acceptance.test.cjs

## 需求
1. ASR 后端可插拔：至少支持 Whisper 与 vosk 两种接入方式，
   由配置/命令行选择，缺省仍是 M1 的 mock 实现（不引入真模型依赖时 demo 仍能跑）。
2. ASR 输入是 **post-3A 的 int16 PCM**（不是 pre-3A 原始麦克风数据）。
3. LLM 走**流式**：决策在 token 到达过程中就能增量产出，不等整段生成完。
4. TTS 用 Opus 编码回传；Tap 的音频按 **100ms 攒批**再送模型（不逐 10ms 帧调）。
5. 打断（barge-in）到停播的**全链路可量化**：记录「打断指令时刻」与「停播帧时刻」的差值。
6. 不引入编译期强依赖的模型 SDK：模型接入走**运行时**可插拔路径，
   缺模型时明确报错而不是静默退回 mock（除非调用方显式要求退回）。

## 验收标准
1. ASR 后端可由配置项选择，且存在 Whisper 与 vosk 两个接入点；验证方式：静态检查源码含两个后端的实现与选择逻辑
2. ASR 消费的音频是 int16 PCM；验证方式：静态检查音频缓冲的类型与采样格式
3. LLM 以流式方式被消费（增量回调而非一次性拿整段）；验证方式：静态检查存在增量回调点
4. TTS 走 Opus 编码器；验证方式：静态检查存在 Opus 编码器调用
5. Tap 音频按 100ms 攒批后送模型；验证方式：静态检查存在 100ms 批大小常量与攒批逻辑
6. 打断到停播的耗时被记录下来（不是只做功能不做度量）；验证方式：静态检查存在该差值的记录点
7. 缺模型运行时给出明确错误；验证方式：静态检查缺模型分支的错误输出

## 风险
1. Whisper/vosk 是重依赖 → 构建体积与部署复杂度上升；应对：不在编译期链接，做成运行时可选接入，demo 默认路径不依赖它
2. 真模型推理耗时超过 Tap 周期 → 决策滞后；应对：攒批 + 流式（需求 3、4），并把推理耗时打进统计
3. TTS 与 barge-in 同时发声 → 声音叠加；应对：barge-in 到达时立即停掉上一段 TTS 的播放队列
4. 3A（webrtc_apm）处理后的 int16 与模型期望的格式（采样率/声道/位深）不一致 → 识别全错；应对：接入前显式做格式声明与转换
