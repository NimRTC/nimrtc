# Slice: v2-qoe-feed

## 目的
把 `QoeCollector` 的**手动投喂**接进远控 demo 的真实链路：viewer 侧在帧呈现时投
`on_frame_presented`、在输入事件发出时投 `on_input_event_sent`、在「反映该输入的帧
呈现时」投 `on_frame_presented_for_input`；device 侧把 `age_us` 与帧锚定信息打进统计行。

这是 M1 验收里「看板出现 `age_us`」的**度量侧** ——
`v2-input-binder` 负责把 `ref_frame` 反查成采集时刻（数据来源），
本片负责让这些数字真的进入 QoE 采集器并被统计行读出来（度量通路）。

对应 `docs/plan/ai-game-demo-plan.md` §8 M1 第三条，以及 §6「看板字段」。

## 范围
- src/modules/rd_qoe/include/nimrtc/rd_qoe/qoe_metrics.hpp
- src/modules/rd_qoe/src/qoe_metrics.cpp
- src/modules/rd_qoe/tests/test_qoe_metrics.cpp
- examples/remote-desktop/rd_viewer.cpp
- examples/remote-desktop/rd_device.cpp
- tests/acceptance/v2-qoe-feed/acceptance.test.cjs

## 需求
1. viewer 侧在每一帧呈现时调用 `on_frame_presented`，`FrameObservation` 里的
   `capture_ts_us` 与 `render_ts_us` 都是真实采集/呈现时刻，不填 0 充数。
2. viewer 侧在输入事件**被控面接受**（而非构造时）投 `on_input_event_sent`，
   保证测到的 input-to-photon 含排队与线上时延。
3. viewer 侧在「反映某个已发输入事件的帧」呈现时投 `on_frame_presented_for_input`，
   与 `on_input_event_sent` 配对，产出 `input_to_photon_p50/p95`。
4. device 侧统计行输出 `age_us` 与帧锚定字段（`ref_frame` id ↔ `capture_ts_us`）。
5. 采集器不重复计数：反映输入的帧**不**额外算作一次呈现帧（否则帧率被灌水）。
6. 缺帧时（如 `capture_ts_us` 拿不到）不投喂该帧，而不是投喂一个假的 0 时刻。

## 验收标准
1. `on_frame_presented` 投喂后快照里的 `frames_presented` 增加 1；验证方式：单元测试投喂一次后断言快照计数为 1
2. `on_input_event_sent` 与 `on_frame_presented_for_input` 配对后产出非零 `input_to_photon_p50_us`；验证方式：单元测试投喂一对（间隔 30000us）后断言 p50 落在 30000us 附近
3. 未发过的 `event_id` 投 `on_frame_presented_for_input` 不产生延迟样本；验证方式：单元测试只投 `on_frame_presented_for_input` 后断言 `input_to_photon_p50_us` 仍为 0
4. 反映输入的帧不重复计入呈现帧数；验证方式：单元测试投一次 `on_frame_presented` 加一次 `on_frame_presented_for_input` 后断言 `frames_presented` 为 1
5. device 统计行含 `age_us` 字段；验证方式：静态检查统计格式串包含 `age_us`
6. `capture_ts_us` 为 0 或缺值的帧不进入延迟样本；验证方式：单元测试投喂一个 `capture_ts_us=0` 的观测后断言 `latency_p50_us` 仍为 0

## 风险
1. 手动投喂漏了某一路 → 延迟指标偏小（而不是偏大），看起来「更快」；应对：投喂点与帧呈现/输入发送的代码路径一一对应，验收里对每一路都做静态检查
2. 时刻来源不统一（单调钟 vs Unix epoch）混用 → 延迟算出来是天文数字；应对：同一路投喂的 `capture_ts_us` 与 `render_ts_us` 必须来自同一个时钟域
3. 投喂发生在渲染线程之外 → 竞态；应对：采集器本身是线程安全的，调用点不需要额外加锁，但**不要**把渲染回调里再套一层异步
