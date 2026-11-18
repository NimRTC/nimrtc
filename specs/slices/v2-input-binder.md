# Slice: v2-input-binder

## 目的
把「输入事件 ↔ 视频帧」的绑定能力接进远控 demo 的真实链路：viewer 侧给输入事件盖章
（`RefFrameBinder` + `EncodeOptions::include_ref_frame`），device 侧把随事件送来的
`ref_frame` id 反查成采集时刻与帧龄（`ref_frame_to_capture_ts` /
`data_message_frame_age_us`），并把结果打到统计行上。

这是 M1 验收里「输入事件带 `ref_frame`，看板出现 `age_us`」的后半截 ——
盖章那一半（viewer 侧）已有，本片补的是**消费那一半**（device 侧反查与展示），
以及缺失的 `include_ref_frame` 开关接线。

## 范围
- src/modules/rd_input/include/nimrtc/rd_input/ref_frame_binder.hpp
- src/modules/rd_input/src/ref_frame_binder.cpp
- src/modules/rd_input/src/event_codec.cpp
- src/modules/rd_input/tests/test_rd_input_g5_g6.cpp
- examples/remote-desktop/rd_device.cpp
- examples/remote-desktop/rd_viewer.cpp
- tests/acceptance/v2-input-binder/acceptance.test.cjs

## 需求
1. `rd_device` 解码输入事件后，能用事件携带的 `ref_frame` id 反查该帧的采集时刻
   （`ref_frame_to_capture_ts`）与帧龄（`data_message_frame_age_us`）。
2. 无帧时 provider 返回 `nullopt` 的语义在链路上成立：事件不盖章、flag 位保持 clear，
   不报错、不静默填 0。
3. `rd_viewer` 的 `--bind-ref-frame` 开关打开时，`EncodeOptions::include_ref_frame` 被置真，
   且盖章用的 provider 指向 `engine.last_displayed_ref_frame_id()`。
4. device 侧把 `age_us`（帧龄）与 `ref_frame` id 打进既有统计行，供看板与验收读取。
5. 本片不改动 `event_codec` 的线上编码格式：不开 `include_ref_frame` 时字节流与改动前完全一致。

## 验收标准
1. `rd_device` 对带 `ref_frame` 的输入事件能反查出非空采集时刻，且帧龄为正；验证方式：单元测试构造带 `ref_frame` 的编码事件，经 `rd_input` 解码后断言 `ref_frame` id 与原值一致
2. `rd_device` 对不带 `ref_frame` 的事件不盖章，解码结果的 `ref_frame` 为空；验证方式：单元测试用默认 `EncodeOptions`（`include_ref_frame=false`）编码后解码，断言 `ref_frame` 无值
3. `rd_viewer` 在 `--bind-ref-frame` 打开时把 `EncodeOptions::include_ref_frame` 置真；验证方式：静态检查该开关分支存在赋值语句
4. `rd_device` 的统计输出行里包含 `age_us` 字段；验证方式：静态检查统计格式串包含 `age_us`
5. 不开 `include_ref_frame` 时编码字节与改动前一致；验证方式：单元测试对同一输入事件分别用 `include_ref_frame=false` 编码，断言字节长度等于开关关闭时的基线长度

## 风险
1. `ref_frame` id 是 per-SSRC 递增的 `frame_seq`，device 与 viewer 两侧的 SSRC 视角可能不同 → 反查可能落空；应对：反查落空时如实报告「帧未知」，不猜测采集时刻
2. 帧表是 1024 条的环形缓冲，超期的 id 查不到 → `age_us` 无值；应对：与上一条同样如实报告，不回退到「当前时刻」
3. `include_ref_frame` 打开会给每个输入事件多加字节 → 输入路带宽与延迟轻微上升；应对：保持默认关闭，只在需要锚定的 demo 场景显式打开
