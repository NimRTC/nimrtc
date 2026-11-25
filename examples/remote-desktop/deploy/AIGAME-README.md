# AI 游戏 Demo — 部署包（测延迟用）

给**操作端（viewer）**的机器用。整个目录拷过去解压就能跑，
不需要装 Visual Studio，也不需要装 "VC++ redistributable"。

这个包和普通远程桌面包的区别：**设备端采集的是合成的打地鼠游戏画面**
（`--game-source`），不是真实屏幕 —— 所以它测的是"远程玩游戏"的延迟，
而不是远程看文档的延迟。操作端每 5 秒打一行**延迟数字**。

## 目录内容

| 文件 | 说明 |
|---|---|
| `rd_device.bat` | **设备端用这个启动**。它采集合成游戏画面 |
| `rd_viewer.bat` | **操作端用这个启动**。它渲染画面并把鼠标键盘送回去 |
| `rd_device.exe` / `rd_viewer.exe` | 两端的媒体程序（由 `client.py` 驱动） |
| `demo-agent-realtime.exe` | AI 决策引擎（切片 v2-ai-decision） |
| `demo-agent-teammate.exe` | 本地小模型 AI 队友（切片 v4-ai-teammate） |
| `demo_agent_self_test.bat` | AI 决策引擎的验收自检，退出码 0 = 全过 |
| `demo_teammate_self_test.bat` | AI 队友的验收自检，退出码 0 = 全过 |
| `msquic.dll` | **必需**。`rd_viewer.exe` 硬依赖它，缺了就是 `0xc000007b` 闪退 |
| `crt/` | MSVC 运行时（`msvcp140.dll` 等），`.bat` 会把它加进 DLL 搜索路径 |
| `client.py` + `wsproto.py` + `signalling.py` | 信令 + 媒体桥接的 Python harness（纯标准库） |

> **AI 决策二进制不接在媒体链路里。** 决策引擎是自包含的纯算术模型，
> 远程游戏会话走的是媒体链路（采集 → 编码 → RTP → SRTP → 解码 → 渲染 →
> 输入回传）。这个包把两者都带上，是为了让你**并排**跑：一边是 AI 决策
> 自己的延迟（`demo_agent_self_test.bat`），一边是链路本身的延迟
> （viewer 打的 `latency_ms` 行）。

## 怎么跑

### 设备机（跑游戏的那台）

1. 开一个命令行，**在解压后的目录里**，起信令服务：

   ```bat
   python signalling.py --host 0.0.0.0 --port 8765
   ```

   > `--host 0.0.0.0` 必须加，否则只监听 `127.0.0.1`，操作端连不上。

2. 另开一个命令行，起设备端。它会打印一个 **6 位配对码**：

   ```bat
   rd_device.bat --url ws://127.0.0.1:8765 --duration 3600 --backend hardware --no-stun
   ```

   > `rd_device.bat` **已经带了 `--game-source`**（合成打地鼠画面）。
   > 想改帧率加 `--game-fps 30`，想改分辨率加 `--width 1280 --height 720`。

3. **要让操作端真的能操控游戏，设备端必须加 `--inject`。**
   不加的话输入事件会被安全地丢掉（你看得见画面，点不动）。

   ```bat
   rd_device.bat --url ws://127.0.0.1:8765 --duration 3600 --backend hardware --no-stun --inject
   ```

### 操作机（玩的那台）

```bat
rd_viewer.bat --url ws://<设备机IP>:8765 --code 186534 ^
              --duration 3600 --backend hardware --no-stun ^
              --sink d3d11
```

| 参数 | 作用 |
|---|---|
| `--url ws://<设备机IP>:8765` | 信令服务地址。**不能用 `127.0.0.1`** |
| `--code <6位数字>` | 设备端打印的配对码 |
| `--sink d3d11` | 开窗口显示画面。不加就是 headless（只跑不显示） |
| `--backend hardware` | 硬件编解码。机器没有硬件解码器就改成 `software` |
| `--no-stun` | 纯局域网模式，不要去连公网 STUN |

## 延迟数字怎么看

操作端每 5 秒打两行。第二行就是你要的：

```
[rd_viewer] frames=422 decoded=422 rendered=422 dropped=0 events_sent=193 denied=0 beats=87 ctrl_out=280
[rd_viewer] latency_ms p50=42.3 p95=88.1 input_to_photon_p50=61.5 presented=422 freeze_count=0
```

| 字段 | 含义 | 怎么读 |
|---|---|---|
| `p50` | 端到端延迟中位数（渲染时刻 − 采集时刻） | **"画面有多旧"**。这是"卡不卡"的主数字 |
| `p95` | 端到端延迟 95 分位 | 抖动有多大。`p95` 比 `p50` 大很多 = 网络不稳 |
| `input_to_photon_p50` | 按键 → 画面响应的中位延迟 | **"我点了多久才动"**。比 `p50` 多一个来回 |
| `presented` | 已经显示出来的帧数 | 在涨 = 画面正常 |
| `freeze_count` | 卡住不动（冻帧）的次数 | 涨 = 画面卡了 |

`-` 表示**还没测到**，不是 0（第一次出 IDR 之前没有采集时间戳，
采样还没有）。`p50` 稳定在几十毫秒 = 手感正常；上三位数 = 明显延迟。

**想测得更准**：跑够 30 秒以上再看（百分位需要样本），关掉别的占带宽的
程序，用 `--no-stun` 走纯局域网。

## AI 决策自检（可选）

在**任何**一台机器上跑（它们是自包含的，不需要联网）：

```bat
demo_agent_self_test.bat
demo_teammate_self_test.bat
```

退出码 0 = 全部验收标准通过（决策延迟、准确率、策略切换、日志追踪）。

## 前置条件

- 目标机器要装 **Python 3**（`python --version` 能出就行）
- 两台机器在**同一网段**，或者网络允许 UDP 直连
- 防火墙要放行 UDP（见下面的排查表）

### 防火墙（两台机器都要做）

ICE 用的是**临时端口**，所以不能只放行固定端口。两条命令，管理员权限的
cmd 里跑：

```bat
netsh advfirewall firewall add rule name="nimrtc-signalling-tcp" dir=in action=allow protocol=TCP localport=8765
netsh advfirewall firewall add rule name="nimrtc-udp-in" dir=in action=allow protocol=UDP
```

第一条是信令（TCP 8765），第二条放行所有入站 UDP —— ICE 的临时端口是
随机的，没有别的办法精确放行。**UDP 出站默认就是通的**，不用加规则。

如果两台机器之间还隔着 Wi-Fi 路由器，确认它没有开 "AP 隔离 / client
isolation"，那个开关会让两台机器互相看不见。

## 排查

| 现象 | 原因 |
|---|---|
| `0xc000007b` 闪退 | 缺 `msquic.dll` 或缺 MSVC 运行时。用 `rd_viewer.bat` 而不是 exe |
| `unknown argument '--url'` | 直接跑了 exe。必须用 `.bat`（`--url/--code` 是 Python 层的参数） |
| `pairing_invalid` | 配对码被用过了。**看设备端窗口里的最新那个码** |
| 画面有了但点不动 | 设备端没加 `--inject` |
| `latency_ms p50=-` | 还没到第一帧 IDR，或采集时间戳没带上。等几秒再看 |
| `dropped` 在涨 | 解码器丢帧。确认设备端是 `--backend hardware`，且第一次出的是 IDR |
| 一直 `connecting` | UDP 不通，或 AP 开了 client isolation |
| `frames=0` | 媒体面没通，检查 UDP 端口 |

详细说明见 `docs/guides/lan-test-runbook.md`。
