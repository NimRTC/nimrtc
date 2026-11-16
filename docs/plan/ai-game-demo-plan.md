# AI 游戏 Demo 规划 — AI 队友同屏双打（延迟处刑台 + 时光作证）

> Status: Draft v0.1（2026-11-16）
> Owner: NimRTC Demo 组
> 基于主干能力盘点：引擎 + 模块侧 90% 已落地，剩余为外围 Demo 胶水 + 真模型 + 前端 + 部署。
> 必读前置：`docs/guides/lan-test-runbook.md`、`examples/remote-desktop/deploy/README.md`、
> `examples/remote-desktop/README.md`、`docs/profiles.md`、`docs/adr/ADR-017-cross-end-timeline.md`、
> `examples/demo-agent-gateway/README.md`。

---

## 0. 转交必读（先看这两条，再写一行码）

> **① 一律从源码构建，不用下载包。** 地基（Timeline / ref_frame / rd_qoe / 新 Profile）
> 都在主干未发布部分，已发布的 `v1.0.2` 包里没有。构建必须带
> `-DNIMRTC_PLUGINS_NVENC=ON -DNIMRTC_PLUGINS_NVDEC=ON -DNIMRTC_PLUGINS_OPENH264=ON`
>（§9）；多 build 树并存时用 `rd_lan.py --binary` pin 显式路径，防拿错旧二进制（§10）。
> **② M1 先冻结帧标签协议，再写码。** §5 的 JSON（`ai_decision / partial / barge_in`）
> 是后续所有工作（游戏源、AI、瀑布、裁判机）的契约，格式先定死再开工。

---

## 1. 一句话定位

> **AI 队友同屏双打，延迟处刑台 + 时光作证**：30 秒内，观众亲手打断一次 AI，再让 AI 点中一个已经滚走的按钮。

- 前 10 秒抓人：打断比快，可感知的低延迟（`<150ms` vs `>500ms`，看尸体不解释 p99）。
- 后 20 秒立住：时光点击，证明 Timeline 不是图，是能力（`frame_seq <-> capture_ts <-> age_us` 全链路打屏）。
- 全程可复现：`tools/rd_lan.py` / `lan-test-runbook` 那套 harness 就是裁判机。

不做“能聊天的 NPC”，做“**和你玩同一帧游戏**”的 AI 队友。这是只有同时拥有
低延迟传输 + 跨端时间 + 帧锚定的人能做的事，三样恰好全在 NimRTC 手里。
2026 年 LLM NPC 常态是 2.6~15s 响应（377 篇 mapping 研究结论），语音 Agent
人人都有（OpenAI Realtime / LiveKit Agents / Pipecat），时光锚定只有我们有
（GameLab 用 QR 码想做而没产品化，我们 `ref_frame` 原生就有）。

---

## 2. 目标与非目标

### 2.1 目标（P0）

1. 单机闭环可跑：合成游戏画面 + 假 AI 逻辑，输入带 `ref_frame`，Timeline 瀑布打屏。
2. 双机/局域网可跑：`rd_device / rd_viewer` 改出游戏源，`decoded>0 && rendered>0 && beats增长`。
3. 处刑台可演示：同屏双路（NimRTC vs 300ms 缓冲对照），观众喊停，两边同时停，差值可读。
4. 时光作证可演示：高速滚动画面下，AI 决策注明 `frame_seq + capture_ts_us`，后台全链路可查。
5. 裁判机可复用：verdict 沿用远控三数 + `input-to-photon p50/p95 + AI决策帧age`。

### 2.2 非目标（明确不做）

- 不做 `cloudgame` 官方 Profile（长期候选，未进 shipped 集合，不等它）。
- 不做从零 SFU（2026-10-19 BDFL 已关闭 SFU-1/DC-3/BENCH-1，不 reopen）。
- 不做浏览器原生 `RTCPeerConnection.createDataChannel` 互通（当前 Case E 是
  `new WebTransport(url)`，DTLS-SCTP 未接线，不要碰）。
- 不做 `kDataChannel` 控面（`engine.hpp` 原话 `not wired by the engine`，走默认
  `kDtlsAppData` / `kRawUdp`）。
- M1 不接真 ASR/LLM/TTS（用 mock + 可插拔接口占位，M3 才接）。

---

## 3. 技术地基盘点（别的 Agent 不用重做）

| 能力 | 主干状态 | 位置/证据 | Demo 用法 |
|---|---|---|---|
| pre/post-3A PCM Tap | ✅ v0.11.0 落完 | `examples/demo-agent-gateway/demo-agent-gateway.cpp: set_pre_process_tap / set_post_process_tap`（float + int16 双路） | fork 出 `demo-agent-realtime`，M1 保留合成 48kHz PCM + 能量 VAD，M3 把 `mock_asr()` 换真模型 |
| 跨端 Timeline ADR-017 | ✅ 落完 | `EngineConfig::timeline_enabled / sr_period_ms`，`last_capture_ts_us() / capture_ts_us_for_ssrc() / last_one_way_delay_us()`，SR + abs-send-time + 拟合斜率 + skew | 打开 `timeline_enabled=true, sr_period_ms=1000`，瀑布直接读 |
| ref_frame 时光锚定 | ✅ 两半落完 | 引擎半 `engine_timeline.cpp / engine_render_state.cpp / engine_control.cpp`：`record_video_ref_frame / ref_frame_to_capture_ts / data_message_frame_age_us / last_displayed_ref_frame_id / displayed_frame_for_ref_frame / last_displayed_frame` 全 STABLE；模块半 `src/modules/rd_input/{ref_frame_binder.hpp, event_codec.cpp}`（flag 位，不加点零字节开销） | 一行接线：`RefFrameBinder{[&engine]{ return engine.last_displayed_ref_frame_id(); }}` |
| 屏/控双平面 | ✅ 推荐路径落完 | `control_channel_transport=kDtlsAppData` 默认（注释明说比 DC 路延迟低）；`kRawUdp` 独立拥塞域；`rd_input` 编解码 + 死人看门狗 + `control_channel_stats{heartbeats}` | 控指令走默认路，别碰 `kDataChannel` |
| 数据通道底座 DC-1 | ✅ PASS E1–E6 | `datachannel_backend="web_transport"` 默认；`examples/demo-wtserver` + `interop/chrome/test_chrome_webtransport.html`；`kFramed/kRaw + priority + max_retransmits/TTL` 全在 `engine.hpp` | Demo 协议用 `framed` 发 partial JSON |
| 视频采集/编码/渲染/码控 | ✅ 落完 | 采集 `synthetic/wgc/dxgi/screencapturekit/pipewire/x11`；渲染 `headless/pinned/d3d11/metal/egl`；`IVideoEncoder/Decoder + auto/hardware/software`；OpenH264/dav1d 真软核；`NVENC 3.06ms/总7.34ms`（`docs/benchmarks/capture_encode_windows_x86_64.json`） | 游戏画面当 `VideoSource` 灌入 |
| 裁判机 rd_qoe + benchmarks | ✅ 落完 | `src/modules/rd_qoe/qoe_metrics.hpp: QoeCollector{freeze/latency_p50-99/input_to_photon/sharpness}`（**被投喂的**，`on_frame_presented / on_input_event_sent / on_frame_presented_for_input` 宿主手动推）；`tools/benchmark_{latency,dc,weaknet,capture_encode,remote_desktop}.py` | verdict 直接复用 |
| Profile/信令/部署包/双机手册 | ✅ 落完 | `agent/agent-gateway/agent-low-latency/teleop/remote-desktop` 10 JSON + C++ 常量回环测试；`examples/remote-desktop/signalling.py` 纯标库 + `deploy/README.md` 绿色包 + `lan-test-runbook.md (2026-11-16 实测 decoded=25 rendered=25)` | 信令加 WSS/鉴权，`.bat` 包一层即玩 |

**四个坑（写法上绕开，不用等引擎）：**

1. `teleop control_weight=20` 不是跨连接抢占。`sched.hpp` Scope note 写死：
   DC-2 只在同一 `Scheduler` 内排序。默认后端下媒体（DTLS-SRTP/ICE/UDP）和
   数据（WebTransport/QUIC）是两条独立连接、各自拥塞控制。文案写“控指令走
   `kDtlsAppData/raw-udp-arq` 同一对 ICE 端口，与视频同命”，别写“wire 上抢占视频”。
2. `QoeCollector` 是被投喂的（头文件原话 `is fed, it does not pull`）。Demo 里手动推。
3. 公网兜底缺 TURN server：参考服务只发 `username:timestamp+HMAC` credential，
   server 默认没跑。公网 Demo 自搭 coturn 填 `add_turn_server()`。
4. Tap 10ms 一帧，调 LLM 前按 100ms 攒批（`demo-agent-gateway/README.md §3.4` 已预警 token 费用）。

---

## 4. Demo 形态设计

### 4.1 链路

```text
服务端（托管机，RTX 3060+）：
  轻量2D游戏（M1 打地鼠，M2 再加躲弹幕，60fps 合成画面起步）
    -> NVENC H.264 -> RTP/SRTP -> 客户端渲染（d3d11 / Chrome）
  AI 决策（M1 假逻辑，M3 小模型 4B 量级本地跑）
    -> 带 ref_frame 标签 --raw-udp-arq / kDtlsAppData--> 服务端执行

玩家输入 --\
            +--> 带 ref_frame 标签 --> 服务端
AI决策   --/    （注明“我看了 frame_seq=42 才按的左移”）

Timeline 瀑布实时打屏：
采集 -> 编码 -> 网络（one_way_delay / skew / slope_fitted）
 -> 解码 -> 渲染（displayed ref_frame） -> AI决策 -> 输入回传（input-to-photon）

语音（可选 M3）：
Mic -> pre-3A Tap（VAD） -> webrtc_apm -> post-3A int16 -> ASR
 -> DC（framed）发 partial -> LLM -> TTS（Opus）回传
```

对照组（处刑台）：同屏双路，左边 300ms 缓冲“云端 NPC”，右边 NimRTC
（`agent-low-latency`：20ms JB + AGC 关 + Opus FEC 关）。

### 4.2 两大名场面（验收标准）

1. **延迟处刑**：观众喊“左/停”，两边同时停。
   - PASS：NimRTC 侧 `打断帧 capture_ts -> TTS停播帧` 差值 `<150ms`，对照组 `>500ms`。
   - 观众看到的是尸体（撞墙 vs 急停），不是数字。
2. **时光作证**：游戏高速滚动时问 AI“为什么往左？”
   - PASS：AI 回答包含 `frame 42（xx:xx:xx.xxx 采集） + 该帧语义（右侧有怪）`，
     后台 `frame_seq <-> capture_ts <-> age_us` 全链路可查，且被引用的帧确实
     已经滚出当前画面。

### 4.3 Profile 选择

- 主路：`agent-low-latency`（JB 20ms + AGC 关 + Opus FEC 关）+ `timeline_enabled=true`。
- 控面：`remote-desktop` 的屏/控分离思想（视频 SRTP，输入独立 `raw-udp-arq`），
  smoothness 优先 ladder + 720p 地板保可读。
- 严格优先级讲 `teleop` 的故事，但文案按 §3 坑 1 收敛。

---

## 5. 帧标签协议（M1 先定死）

### 5.1 输入事件绑定

- 复用 `rd_input::InputEvent + RefFrameBinder + event_codec`，`EncodeOptions.include_ref_frame=true`。
- 发送侧：`binder.stamp(event)`，provider 为
  `[&engine]{ return engine.last_displayed_ref_frame_id(); }`。
- 无帧时 provider 返回 `nullopt`，事件不盖章（flag 位保持 clear，合法，不报错）。
- 接收侧：`displayed_frame_for_ref_frame(id)` 反查，
  `ref_frame_to_capture_ts(id) / data_message_frame_age_us(id)` 换算采集时刻与 age。

### 5.2 AI 决策标注（JSON over framed control，M1 格式冻结）

```json
{
  "type": "ai_decision",
  "frame_seq": 42,
  "capture_ts_us": 1710000000000000,
  "action": "move_left",
  "reason": "frame 42 右侧有怪",
  "age_us_at_decision": 38000
}
```

- `frame_seq` 即 `ref_frame id`（`= plugins::VideoSourceFrame::frame_seq`，per-SSRC 递增）。
- `capture_ts_us` 为 ADR-017 Unix epoch 微秒（peer 时钟重建值，可与本地 Unix 时间直接减）。
- `age_us_at_decision` 为决策时刻 `now - capture_ts`，看板直接显示，处刑台用它排名。

### 5.3 partial / barge-in（语音可选路，M1 预留字段）

```json
{ "type": "partial", "text": "往左", "capture_ts_us": 1710000000000000, "ref_frame": 42 }
{ "type": "barge_in", "at_capture_ts_us": 1710000000000000, "ref_frame": 43 }
```

- 通道：`create_data_channel("agent-control", framed, priority=0)` 发 `barge_in`；
  datagram 发心跳/能量条。两端 `framing` 必须一致（framed vs raw 不互通）。

---

## 6. 看板字段（verdict 体系，M1 冻结）

沿用远控三数 + 扩展：`decoded / rendered / dropped + beats + input-to-photon p50/p95 + AI决策帧age`。

| 分组 | 字段 | 来源 |
|---|---|---|
| 媒体面 | `decoded / rendered / dropped / presented_fps / freeze_count` | viewer 统计 + `QoeCollector::snapshot()` |
| 控面 | `beats / ctrl_in / ctrl_out / heartbeats_sent/received` | `control_channel_stats()` |
| 跨端时间 | `capture_ts_us / one_way_delay_us / skew_us / slope_fitted` | ADR-017 tracker（`last_*` 系列） |
| 帧锚定 | `frame_seq <-> capture_ts / age_us / displayed ref_frame` | `record/ref_frame_to_capture/data_message_frame_age/displayed_frame_for_ref_frame` |
| 视频成本 | `capture/convert/encode/packetize ms` | `benchmark_capture_encode` 引擎内打点 |
| 底座对照 | `p50/p99 / jitter / JB target / BWE bps / pps` | `bench_latency/dc/weaknet` + `jb_strategy` |

viewer 每 5 秒一行统计沿用远控格式（`frames/decoded/rendered/dropped/events_sent/beats/ctrl_out`），Demo 加挂
`age_us / one_way_delay_us / input_to_photon` 三列。非零退出码 = 失败并转储
`build/lan_device.log + build/lan_viewer.log`（沿用 `rd_lan.py` 规矩）。

---

## 7. 部署架构（你托管，给客户端）

```text
GPU托管机（NAT 后，重活全在这）：游戏 + NVENC + AI，主动向外连信令
          │
公网小鸡（2C4G，无 GPU，有公网 IP）：signalling WSS:8765 + coturn UDP:3478（备胎）
          │
用户客户端：viewer 绿色包（rd_viewer.exe + msquic.dll + crt/ + client.py）
            或 Chrome WebTransport 页，输入房间码即玩
```

- **媒体默认 P2P**：`host > srflx > relay` 自动选路，host 能通绝不用 relay。
  信令归你，媒体归 P2P，TURN 只买保险。中转多一跳 +20~50ms，处刑台一测现形。
- NAT 后托管三选一：
  - A（推荐 Demo 期）：Tailscale/WireGuard 组虚拟局域网，拿 `100.x` 跑
    `--no-stun --url ws://100.x:8765`，退化成内网跑法，10 分钟通。
  - B（产品态）：STUN 打洞 + TURN 备胎。`signalling_core.py` 发 credential，
    对接 `add_turn_server()`。控指令走 `kDtlsAppData/raw-udp-arq`（与媒体同一对
    ICE 端口，P2P 通它就通），别走 `WebTransport` C/S 路。
  - C（无公网 IP）：frp/cloudflared 映射 8765 做信令，媒体照常打洞。
- 绿色包沿用 `examples/remote-desktop/deploy/` 结构（`.bat + msquic.dll + crt/ + client.py + wsproto.py`，
  纯标库无 pip）。Demo 期先托管，验证完再升级：WSS + 鉴权、配对码换房间码、TURN 地址写死。

### 7.1 硬件（1080p30 单路）

| 位置 | 最低 | 推荐 | 说明 |
|---|---|---|---|
| 服务端 GPU | GTX1660 / 核显+OpenH264 | **RTX 3060+** | NVENC 3.06ms/帧、总 7.34ms（预算≤8ms）；OpenH264 27ms 仅 fallback |
| 服务端 CPU/内存 | 4核/8GB | 6核12线程/16GB | 参考机 12400F，1080p 捕获 257fps |
| AI 推理 | 同机小模型 4B 量级 | 独立 GPU 或同卡分区 | 窄角色+小模型才能压住延迟 |
| 客户端 | 核显即可 | 有 NVDEC 最好 | 无硬解走 `--backend software`；Python3 + 几十 MB 包（静态库 12.55MB），或 Chrome 零安装 |

并发按路数线性加；观战路可用 `relay`（跳过 L2 省编码钱）。

### 7.2 网络

| 场景 | 要求 | 状态 |
|---|---|---|
| 同局域网 | 同网段、关 AP 隔离、UDP 放行 | ✅ `--no-stun` 即用 |
| 公网 P2P | STUN 取 srflx，打洞 | 需配 STUN + `add_turn_server` 备胎 |
| 打洞失败 | TURN 中转（coturn 自搭） | 参考服务只发 credential，server 自搭 |
| 弱网演示 | 丢包 5% + 抖动 100ms，用 `benchmark_weaknet` shaper 复现 | ✅ 当“恶劣模式”开关 |
| 带宽 | 1080p 一路 2~8Mbps（ladder 自适应），720p 地板 | ✅ smoothness 优先 |

延迟预算：传输 p50 ~10us、input-to-photon p50 ~56ms；云游戏 FPS 整链路 80ms、
慢节奏 100~150ms，局域网能过，公网看 TURN 绕路。

---

## 8. 里程碑与交付物

### M1 单机闭环（1-2 天）— 先做这个

- [ ] fork `demo-agent-gateway.cpp` → `demo-agent-realtime.cpp`：保留合成 48kHz PCM +
      能量 VAD，把 mock LLM 换可插拔 `IASR/ILLM` 回调，加
      `create_data_channel("agent-control", framed)` 发 §5.2/5.3 JSON，控制台打 Timeline。
- [ ] fork `rd_device/viewer` → 游戏源：合成 2D 画面当 `VideoSource` 灌入，
      输入路径接 `RefFrameBinder`（§5.1 一行接线）。
- [ ] `QoeCollector` 手动投喂接线（`on_frame_presented / on_input_event_sent / on_frame_presented_for_input`）。
- [ ] 验收：单机 `decoded>0 && rendered>0`，输入事件带 `ref_frame`，看板出现 `age_us`。

### M2 双机 + Chrome（3-5 天）

- [ ] 复用 `signalling.py + rd_lan.py` harness（`--bind` 演练跨网形）。
- [ ] viewer 用 `interop/chrome/test_chrome_webtransport.html` 改出瀑布页
     （`new WebTransport(url)`，不是 `createDataChannel`）。
- [ ] 处刑台双路 + 弱网开关（`benchmark_weaknet` 5% 丢包 + 100ms 抖动档）。
- [ ] 验收：`decoded/rendered/beats` 三数 verdict + `input-to-photon p50/p95` + `AI决策帧age`，
      处刑差值 `<150ms vs >500ms` 可读，时光引用帧确实已滚出画面。

### M3 接真模型（按需）

- [ ] post-3A int16 直喂 Whisper/vosk，LLM 流式，TTS Opus 回传（Tap 100ms 攒批）。
- [ ] AI 决策走本地小模型（窄角色：只会“左右移/停”，别做开放聊天）。
- [ ] 验收：打断到停播全链路可量化，时光回答 100% 带 `frame_seq + capture_ts`。

---

## 9. 构建与运行（给别的 Agent 的命令）

```bash
# 构建（含硬件编解码，不静默 fallback）
cmake -S . -B build/msvc-full -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DNIMRTC_PLUGINS_NVENC=ON \
  -DNIMRTC_PLUGINS_NVDEC=ON \
  -DNIMRTC_PLUGINS_OPENH264=ON
cmake --build build/msvc-full

# 单机 harness（先跑通这个）
python tools/rd_lan.py --media rd --duration 30 --backend hardware --no-stun

# 双机：设备机起信令 + device，操作机起 viewer（见 lan-test-runbook §4）
python examples/remote-desktop/signalling.py --host 0.0.0.0 --port 8765
python examples/remote-desktop/client.py --role device --url ws://<device-lan-ip>:8765 --media rd --duration 600 --backend hardware --no-stun
python examples/remote-desktop/client.py --role viewer --url ws://<device-lan-ip>:8765 --code <6位> --media rd --duration 600 --backend hardware --no-stun

#  verdict：viewer 报告 decoded>0 && rendered>0；beats 增长 = 控面活
```

防火墙（两机）：放行 TCP 8765 + 全量入站 UDP；确认 AP 无 client isolation。
`--backend hardware` 无卡即硬失败（不静默降级），无卡改 `--backend software`。

---

## 10. 风险与对策

| 风险 | 对策 |
|---|---|
| 对照组 300ms 缓冲被质疑“人造” | 对照组参数公开（JB/缓冲配置打屏），观众可亲手改大改小；NimRTC 侧同样允许加压 |
| 公网打洞失败 Demo 翻车 | 展会/内测默认走方案 A（Tailscale 虚拟局域网）；公网场提前一天测 B，打不通切 TURN |
| 时光回答被当“背台词” | 被引用帧的 `capture_ts + 裁剪缩略图` 同屏打出，观众可肉眼核对已滚走 |
| 模型延迟盖过传输优势 | M3 只用小模型窄角色 + 本地跑；大模型只走文字事后解说，不进决策环 |
| 主干未发布（v1.0.2 包里没有新能力） | Demo 一律从源码构建，不用下载包；`--binary`  pin 显式路径，防多 build 树拿错旧二进制（`rd_lan.py --binary`） |

---

## 11. 待确认

1. 托管机出向联网是否通（NAT 后出向一般通，确认一下 A/B 都能走）？
2. 有无公网小鸡（2C4G 即可，无需 GPU）跑 `signalling WSS + coturn`？
3. 观众画像（投资人/客户 vs 开发者）→ 钩子优先级：前者先做时光点击，后者先做 Kill 台 + 瀑布。
4. 游戏题材：**已定 — M1 打地鼠，M2 再加躲弹幕**。打地鼠的点击语义天然配
   `ref_frame`（“你点的就是你当时看到的那一帧”），时光作证一句话讲清；
   躲弹幕的连续决策更能体现 `age_us` 排名，留到 M2 做处刑台加压。

---

## 12. 文件索引（别的 Agent 按图索骥）

- 引擎面：`src/engine/include/nimrtc/engine/engine.hpp`（`timeline_enabled / ref_frame_* / displayed_* / control_channel_* / datachannel_*`）
- 输入绑定：`src/modules/rd_input/include/nimrtc/rd_input/{ref_frame_binder.hpp,input_event.hpp,event_codec.hpp,control_channel.hpp}`
- QoE：`src/modules/rd_qoe/include/nimrtc/rd_qoe/qoe_metrics.hpp`
- 采集/渲染/码控：`src/modules/video_capture / video_sink / video_bitrate_control / nvcodec / h264`
- 调度：`src/modules/sched`（读 Scope note，别按跨连接抢占写文案）
- Profile：`profiles/{agent,agent-low-latency,agent-gateway,teleop,remote-desktop}.json` + `docs/profiles.md`
- 信令/部署：`examples/remote-desktop/{signalling.py,signalling_core.py,client.py,wsproto.py,deploy/README.md,README.md}`
- 网页：`interop/chrome/test_chrome_webtransport.html` + `examples/demo-wtserver/demo-wtserver.cpp`
- 语音起点：`examples/demo-agent-gateway/{demo-agent-gateway.cpp,README.md}`
- 手册/基准：`docs/guides/lan-test-runbook.md`、`docs/guides/remote-desktop.md`、
  `tools/{rd_lan.py,benchmark_latency.py,benchmark_dc.py,benchmark_weaknet.py,benchmark_capture_encode.py,benchmark_remote_desktop.py}`、
  `docs/benchmarks/{capture_encode_windows_x86_64,remote_desktop_weaknet_windows_x86_64}.json`
