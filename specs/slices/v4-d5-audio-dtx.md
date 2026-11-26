```markdown
# Slice: v4-d5-audio-dtx

## 目的
让接收侧能区分「发送方抑制了这一帧（DTX 静音）」与「这一帧丢了」，
以此正确驱动 PLC（丢包遮蔽）：静音帧**不**触发遮蔽，真丢的帧**才**触发。
DTX 默认关闭，可按流单独切换。

## 现状核实（写规格时的树状态，version/v3 @ a2bb408c）

- **已在**：`audio_rx/src/receiver.cpp:166-169` 读协商结果 `peer_dtx_enabled` 并写进 `fmt.use_dtx`；
  `receiver.cpp:311-312` 已有 `f.dtx_silence` 分支（注释写明「D5：dtx_silence 是对方抑制了这帧」）；
  Opus 侧 `stats.dtx_frames` 已在统计（`opus.cpp:281`）。
- **缺**：① 静音帧路径与遮蔽统计之间的**判据**没有钉死（静音帧会不会被计进 `frames_concealed` 没有单测）；
  ② 「真丢帧」路径没有被区分出来（丢包时没有任何 DTX 相关的状态可读）；
  ③ **按流开关**没有（现在只有协商结果，没有宿主级开关）。

## 范围
要改的（这些是 agent 会去改的文件）：
- `src/modules/audio_rx/src/receiver.cpp`
- `src/modules/audio_rx/src/receiver.hpp`（或同模块的配置结构）
- `src/modules/audio_rx/include/nimrtc/audio_rx/**`
- `src/modules/opus/src/opus.cpp`（只加「静音帧与帧数的对账」所需的最小暴露）
- `src/modules/audio_rx/tests/test_audio_rx.cpp`
- `tests/acceptance/v4-d5-audio-dtx/acceptance.test.cjs`

只读参考（**不得修改**，agent 只许读）：
- 只读 `src/modules/opus/` —— 现有 DTX 抑制与帧识别（本片只改上面的 `opus.cpp`）

## 需求
1. 静音帧（`dtx_silence`）不计入 `frames_concealed`；
2. 真丢帧（序号有缺口 / 到达超时）计一次遮蔽，且可从计数或回调看出「这是丢帧不是静音」；
3. DTX 默认关闭，可按流单独切换（同一进程里两条流一个开一个关，互不影响）。

## 验收标准
1. 协商了 DTX 的对端发静音帧 → 接收端按静音处理，**不**触发遮蔽（`frames_concealed` 不增加）；
   验证方式：单测 feed 若干静音帧后断言 `frames_concealed == 0`。
2. 真丢 DTX 帧 → 遮蔽计数增加；验证方式：单测制造序号缺口，断言 `frames_concealed` 恰好增加缺口数。
3. DTX 默认关闭，可按流单独切换；验证方式：单测断言未配置时 DTX 不生效；两条流一开一关，各自结果独立。

## 风险
1. 静音帧被误判为丢帧 → 遮蔽统计虚高；应对：两条路径各一条单测，钉死判据。
2. 默认关闭的改动影响现有通话；应对：默认值不变（关），已有测试全绿作为回归。
```
