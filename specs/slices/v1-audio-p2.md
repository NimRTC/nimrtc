# Slice: v1-audio-p2

## 目的
让 Opus 的 DTX（不连续发送）信号从**编码侧**传到**接收侧的遮蔽逻辑**，使接收端能区分「发送方主动抑制了这一帧（静音）」与「这一帧真的丢了」。

今天的状态（已核实）：
- 编码侧：`EncoderConfig::dtx_enabled` → `OPUS_SET_DTX`（模块内 `src/modules/opus/src/opus.cpp`）已经接上 libopus。但 `Encoder::stats().dtx_frames` 只在 `encode_frame(force_dtx=true)` 这条**测试专用**路径上递增，真实编码路径不记。
- 线路形态（已核实 libopus 源码）：DTX 静音帧在 RTP 载荷里是**只有 TOC 的 1 字节 Opus 包**（RFC 6716 §3.4）。第三方库只读 `opus_encoder.c` 里 silk DTX 时 `nBytes==0` → 写 `data[-1] = gen_toc(...)` → 返回 1 字节；第三方库只读 `opus_decoder.c` 收到 `len<=1` 时走 PLC/静音路径。
- 接收侧：`audio_rx` 从不提及 DTX —— 它只看 RTP 时间戳：时间戳跳过去了就 `conceal()`（遮蔽）。于是**DTX 静音帧会被当成丢帧**，触发不该有的遮蔽，`frames_concealed` 被虚增。
- 协商状态：`plugins::AudioDecoderConfig::use_dtx` 字段**声明了但从未被任何代码读取**（全仓库搜索确认）。引擎侧也没有把对端的 `usedtx=1` 传下来。

D5 就是这个**耦合**：DTX 信号传到遮蔽逻辑能看见的地方。

D5-1：DTX **默认关**（`EngineConfig::audio_use_dtx` 默认已是 `false`），可按流切换 —— 保持现状，不要改这个默认值。

## 设计

### 数据流（DTX 信号从哪来、到哪去）

```
发送方                                  接收方
──────                                  ──────
EngineConfig::audio_use_dtx (默认 false)
  → codec_cfg.dtx_enabled
  → OPUS_SET_DTX(1)
  → libopus silk 编码器
  → 静音时产出 1 字节 Opus 包（只有 TOC）
  → packetise() 原样带上（RFC 7587 code 0）
  → RTP 载荷
                                    RTP 载荷
  → depacketise() → PacketView{size == 0}  ← DTX 的结构性判据
  → AudioDecoderPlugin::decode_packet()
  → AudioDecodedFrame.dtx_silence = true   ← 新字段
  → audio_rx::Pipeline::consume()
  → 不调 conceal()，frames_concealed 不动  ← D5 的落点
```

### 三个设计决定

**决定一：判据用「线路结构」，不用「PCM 内容」。**
DTX 帧在 RFC 6716 §3.4 里的形态是**只有 TOC 的 1 字节 Opus 包**。`depacketise()` 对 code 0 且 `payload_len == 1` 会产出一个 `size == 0` 的 `PacketView`。这就是判据：帧体长度为 0 ⇒ 这是一帧 DTX 抑制。
不用「解出来的 PCM 是否全零」：那是**间接猜测**，会把「真的录到了静音」误判成 DTX，也会把「DTX 帧解出来的不是全零」（libopus 的 PLC 路径会合成过渡噪声）误判成普通帧。

**决定二：DTX 判据在 FEC 恢复之前生效。**
模块内 `src/modules/opus/src/audio_decoder_plugin.cpp` 的解码循环里，FEC 恢复尝试在 `i == 0 && gap` 时先跑。DTX 帧不携带 FEC 冗余，所以若 FEC 先跑，DTX 帧会被判成「无 FEC 可恢复」→ 走 `decode_errors` 或触发遮蔽。两条路都是错的。
所以顺序固定为：**先判 DTX（是就直接产出静音帧并跳过 FEC），再判 FEC**。这个顺序要在代码里注释写明——它不是风格问题，颠倒它会让 DTX 静音变成丢帧。

**决定三：DTX 是「发送方的抑制」，不是「接收方的丢包」。**
两者在 RTP 层都表现为「时间戳跳了一格」，但语义相反：
- DTX 抑制：发送方**故意**不发这一帧（对端用 1 字节包占位，或直接不发）。这**不是**丢包，不该被遮蔽。
- 真丢包：帧在路上消失了。这**应该**被遮蔽。
两者的区分依据不是 RTP 层（不可区分），而是**帧里带的结构性标记**。所以「收到一个带 DTX 标记的帧」= 发送方抑制 = 按静音处理；「时间戳跳了且没有对应帧」= 真丢包 = 遮蔽。

### 接口变更（都只追加，不改既有字段）

| 位置 | 变更 |
|---|---|
| `plugins::AudioDecodedFrame` | 追加 `bool dtx_silence = false` |
| `plugins::AudioReceiverConfig` | 追加 `bool peer_dtx_enabled = false` |
| `plugins::AudioDecoderConfig` | `use_dtx` 字段已存在但从未被读取——**消费它或删掉它**，二选一，不许留摆设 |

`plugins::AudioDecodedFrame` / `AudioReceiverConfig` 是 `NIMRTC_API_STABLE` 公开头，新字段一律**追加在末尾并给默认值**，不改既有字段的顺序与语义。

### 引擎侧的协商链

`usedtx` 已经是 `OpusFmtpParams` 的一个字段，但注释写着「parsed but not acted on」，`build_opus_fmtp` 与 `build_negotiated_opus_fmtp` 都跳过它。本片要把它接上：

- `build_opus_fmtp`：加 `if (p.usedtx) add("usedtx", ...)`（offer 侧声明）。
- `build_negotiated_opus_fmtp`：`out.usedtx = peer.usedtx`（answer 侧回显）——**并同步改掉「减去 NimRTC 不支持的（`usedtx`…）」那条注释**，否则文档与行为相反。
- `src/engine/src/engine_signaling.cpp`：解析结果已存进 `impl_->audio_state.negotiated_opus_fmtp`，不需要新解析。
- `src/engine/src/engine_media.cpp`：构造 `rx_cfg` 时把 `negotiated_opus_fmtp.usedtx` 传进 `rx_cfg.peer_dtx_enabled`。

`usedtx` 是 `std::optional<bool>`，三态要分清：对端发 `usedtx=1` → 回显 1；对端发 `usedtx=0` → 回显 0；对端没发 → 不回显。

### 验证策略

- **单元测试（opus 模块）**：造一个 1 字节的 Opus 包，喂进模块内 `AudioDecoderPlugin::decode_packet()`，断言产出帧的 `dtx_silence == true`。
- **单元测试（audio_rx 模块）**：把一段 DTX 静音帧喂进流水线，断言 `frames_concealed == 0`（验收标准 2）；再制造一次真实丢帧，断言 `frames_concealed == 1`（验收标准 3，负控制）。**两条必须同时存在**，否则「DTX 不遮蔽」可能只是把遮蔽整个关了。
- **单元测试（engine 模块）**：模块内 `build_opus_fmtp` / `build_negotiated_opus_fmtp` 的 `usedtx` 三态回显规则。
- **验收测试**：`tests/acceptance/v1-audio-p2/acceptance.test.cjs`，每条都是真断言，不留 `test.todo`。

## 范围
- src/modules/opus/**
- src/modules/audio_rx/**
- src/plugins/include/nimrtc/plugins/audio_decoder.hpp
- src/plugins/include/nimrtc/plugins/audio_receiver.hpp
- src/engine/src/audio_routing.hpp
- src/engine/src/engine_signaling.cpp
- src/engine/src/engine_media.cpp
- src/engine/src/engine_audio_state.hpp
- tests/acceptance/v1-audio-p2/acceptance.test.cjs

**为什么必须动 `src/engine/`**（已核实）：验收场景是「开启 DTX 的**发送方**与协商了 DTX 的**对端**之间」。按 RFC 6716 §8，answer 不回显 `usedtx` 时对端理解为「对端不支持 DTX」，于是根本不会发 DTX 帧——DTX 链路从源头就不成立。所以 `usedtx` 的解析（已有）、answer 回显（缺）、传到接收器（缺）必须在本片一起做完，否则验收标准第 4 条与第 2 条都无法在真实链路上成立。

## 需求
- D5：实现 DTX 耦合，让 DTX 信号传到遮蔽逻辑能看见的地方，使接收端能区分「发送方抑制了这一帧」和「这一帧丢了」
- D5-1：DTX 默认关，可按流切换（该默认值改变每通电话「听起来」的效果，不应作为适配项）

## 验收标准
1. **DTX 帧在解码侧被识别为「静音」而非「普通帧」**：解码后的帧能区分「这是 DTX 抑制出来的静音帧」与「这是普通解码帧」。验证方式：`plugins::AudioDecodedFrame` 上新增一个布尔字段承载这个标记，模块内 `src/modules/opus/src/audio_decoder_plugin.cpp` 在解码时正确置位。判据必须是**结构性的**——DTX 帧的线路形态是 1 字节包（只有 TOC，无帧体），`depacketise()` 之后该帧的 `size == 0`。**不允许**用「解码出来的 PCM 是否全零」这种间接猜测：那会把真正的静音 PCM 和 DTX 抑制混为一谈。
2. **DTX 静音不触发遮蔽**：`src/modules/audio_rx/` 的接收流水线在收到一个 DTX 标记的帧时，按**静音**处理，`frames_concealed` **不递增**。验证方式：单元测试构造一段连续的 DTX 静音帧喂进流水线，断言 `stats.frames_concealed` 保持为 0。
3. **真正丢掉的帧仍然触发遮蔽**（负控制）：不是 DTX 标记的帧如果真的没到（时间戳跳跃且无对应帧），`frames_concealed` **必须递增**。验证方式：单元测试制造一次真实的帧丢失，断言 `frames_concealed` 从 0 变成 1。**这条是第 2 条的对照**——两条同时成立，才说明「DTX 静音不被遮蔽」不是把遮蔽功能整个关掉了。
4. **DTX 协商状态被真正消费**：接收侧知道对端是否协商了 DTX（`usedtx=1`），且这个状态**真的影响到行为**。今天 `plugins::AudioDecoderConfig::use_dtx` 这个字段声明了但**从未被任何代码读取**——本片要么消费它，要么（如果判据不成立）删掉它并说明理由，不允许留着一个「看起来支持」的字段。验证方式：静态检查该字段在 `src/modules/` 下有实际读取点，或已不存在。
5. **`usedtx` 的 SDP 协商链打通**（本片新增，因为范围扩到了 `src/engine/`）：
   - 发送方在自己的 offer 里**声明** `usedtx=1`（仅当主机开了 DTX 时声明）；
   - 收到对方 offer 后，answer **回显** `usedtx=1`（仅当对方声明了且本端支持）；
   - 协商结果**传到接收器**（`AudioReceiverConfig` 上的一个 DTX 开关）。
   验证方式：对 `src/engine/src/audio_routing.hpp` 的 `build_opus_fmtp` / `build_negotiated_opus_fmtp` 做单元测试——对方声明 `usedtx=1` 时 answer 的 fmtp 含 `usedtx=1`；对方没声明时 answer **不含** `usedtx`（RFC 6716 §8：answer 只回显实际协商到的参数；不回显 ≠ 声明不支持，但对端必须能区分「协商到 DTX」与「没协商到 DTX」）。
6. **D5-1：DTX 默认关**：默认配置下 DTX 不生效。验证方式：断言 `EngineConfig` 的 `audio_use_dtx` 默认值为 `false`，且未显式开启时接收侧不会走 DTX 分支。
7. **验收测试文件是真实断言**：`tests/acceptance/v1-audio-p2/acceptance.test.cjs` 里每条用例都是可执行的断言，不允许残留 `test.todo`。验证方式：运行 `node --test tests/acceptance/v1-audio-p2/acceptance.test.cjs` 全部通过。
8. **不破坏既有行为**：既有的 opus 编解码测试、audio_rx 流水线测试、引擎音频媒体路径全部仍然通过。验证方式：`ctest --test-dir build --output-on-failure -C Debug` 无回归。

## 风险
- **误判 DTX 会造成「该遮蔽的不遮蔽」**（丢帧被当成静音，音频里出现杂音而不是补帧）——这是本片最危险的失败模式。对策：DTX 判据必须基于**线路结构**（1 字节包 / 帧体长度为 0），不能基于「PCM 是否静音」；并在验收标准第 3 条里用负控制锁住「真丢帧仍然遮蔽」。
- **DTX 与 FEC 的交互**：Opus 的 DTX 帧不携带 FEC 冗余。若 FEC 恢复逻辑先于 DTX 判据生效，可能把 DTX 帧误判为「无 FEC 可恢复」而触发遮蔽。对策：DTX 判据在 FEC 恢复**之前**生效，或两者互斥且顺序固定；在实现时明确注释这个顺序。
- **跨模块接口变更**：`plugins::AudioDecodedFrame` / `AudioReceiverConfig` 是 `NIMRTC_API_STABLE` 的公开头文件。对策：新增字段必须**追加**在结构体末尾并给默认值，不改已有字段的顺序与语义；`docs/api/public_headers.manifest` 若因此变化，需一并更新（本片不改动 `engine.hpp` 的既有字段，不改动 `CHANGELOG.md`）。
- **SDP 回显的语义容易搞错**：`build_negotiated_opus_fmtp` 现在的注释明说「回显对端发的参数，减去 NimRTC 不支持的（`usedtx`、`sprop-maxcapturesize`）」。本片要把 `usedtx` 从「不支持」变成「支持」，**这条注释和 `build_opus_fmtp` 的 `if (p.usedtx)` 分支都要一起改**——漏改注释会让文档与行为相反。风险在于 `usedtx` 是 `std::optional<bool>`：对端发 `usedtx=0` 与「不发 usedtx」是两件事，回显规则必须区分（对端发 0 → 不回显或回显 0；对端发 1 且本端支持 → 回显 1）。
- **`engine.hpp` 是 STABLE 公开头**：本片**不改** `engine.hpp` 的既有字段。`audio_use_dtx` 已存在且默认 `false`（D5-1 已满足），所以不需要动它——这既省掉一次破坏性契约变更，也让本片的风险面小一圈。
- **1 字节包在现有代码路径上的表现未验证**：模块内 `src/modules/opus/src/opus.cpp` 的 `depacketise()` 对 code 0 且 `payload_len == 1` 会产出 `size == 0` 的 view；模块内 `src/modules/opus/src/codec_plugin.cpp` 的 `CodecPluginAdapter::decode()` 与模块内 `src/modules/opus/src/audio_decoder_plugin.cpp` 的 `decode_packet()` 都会把 `size == 0` 的帧送进 libopus。libopus 对 `len<=1` 会走 PLC 路径返回静音样本，但**具体返回值与 `AudioDecodedFrame` 的产出方式本片必须实测确认**，不能假设。如果实测发现 1 字节包在现有路径上被丢弃（`decode_packet` 返回 0 → 流水线计入 `decode_errors` 而非产出一帧），那么实现必须先修好这条路径，否则 DTX 帧到不了遮蔽逻辑。这一点是本片**第一个要验证的事实**，实现时应先写一个最小的验证程序确认行为，再动手改结构。
