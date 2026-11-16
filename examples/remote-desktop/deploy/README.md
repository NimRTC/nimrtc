# 远程桌面 — 部署包

给**操作端（viewer）**的机器用。整个目录拷过去解压就能跑，
不需要装 Visual Studio，也不需要装 "VC++ redistributable"。

## 目录内容

| 文件 | 说明 |
|---|---|
| `rd_viewer.bat` | **操作端用这个启动**。它是入口脚本 |
| `rd_device.bat` | **设备端用这个启动** |
| `rd_viewer.exe` / `rd_device.exe` | 两端的媒体程序（由 `client.py` 驱动） |
| `msquic.dll` | **必需**。`rd_viewer.exe` 硬依赖它，缺了就是 `0xc000007b` 闪退 |
| `crt/` | MSVC 运行时（`msvcp140.dll` 等），`.bat` 会把它加进 DLL 搜索路径 |
| `client.py` + `wsproto.py` | 信令 + 媒体桥接的 Python harness（纯标准库，无第三方依赖） |

## 怎么跑

### 设备机（被控的那台）

1. 开一个命令行，**在解压后的目录里**，起信令服务（信令只在设备机上跑）：

   ```bat
   python signalling.py --host 0.0.0.0 --port 8765
   ```

   > `signalling.py` 就在这个包里（和 `client.py` 放在一起）。
   > `--host 0.0.0.0` 必须加，否则只监听 `127.0.0.1`，操作端连不上。
   > 如果这台机器没有 Python，从源码树跑也行：
   > `python D:\MyOpen\NimRTC\examples\remote-desktop\signalling.py --host 0.0.0.0 --port 8765`

2. 另开一个命令行，起设备端。它会打印一个 **6 位配对码**：

   ```bat
   rd_device.bat --url ws://127.0.0.1:8765 --duration 3600 --backend hardware --no-stun
   ```

   > 设备端默认就是 `--media rd`，不用手写这个参数。

### 操作机

```bat
rd_viewer.bat --url ws://172.16.33.44:8765 --code 186534 ^
               --duration 3600 --backend hardware --no-stun ^
               --sink d3d11
```

| 参数 | 作用 |
|---|---|
| `--url ws://<设备机IP>:8765` | 信令服务地址。**不能用 `127.0.0.1`** |
| `--code <6位数字>` | 设备端打印的配对码 |
| `--sink d3d11` | 开窗口显示画面。不加就是 headless（只跑不显示） |
| `--backend hardware` | 硬件解码。机器没有硬件解码器就改成 `software` |
| `--no-stun` | 纯局域网模式，不要去连公网 STUN |

## 配对码怎么用

配对码是**一次性**的：谁用掉它，谁就是当前的操作端。

- 操作端关掉窗口 / Ctrl+C 之后，**设备端会自动打印一个新的配对码**，
  拿新的那个再连就行。
- 如果提示 `pairing_invalid`，说明你用的码已经用过了或者过期了 ——
  看设备端窗口里最新打印的那个码。
- 设备端的窗口**不要关**：它关了就没人发新码了。

## 前置条件

- 目标机器要装 **Python 3**（`python --version` 能出就行）
- 两台机器在**同一网段**，或者网络允许 UDP 直连
- 防火墙要放行 UDP（见下面的排查表）

## 排查

| 现象 | 原因 |
|---|---|
| `0xc000007b` 闪退 | 缺 `msquic.dll` 或缺 MSVC 运行时。用 `rd_viewer.bat` 而不是 exe |
| `unknown argument '--url'` | 直接跑了 exe。必须用 `.bat`（`--url/--code` 是 Python 层的参数） |
| `pairing_invalid` | 配对码被用过了。**看设备端窗口里的最新那个码** |
| 连上了但黑屏/无画面 | 设备端还没出 IDR。看设备端日志有没有 `armed the video encoder for an IDR` |
| 一直 `connecting` | UDP 不通，或 AP 开了 client isolation |
| `frames=0` | 媒体面没通，检查 UDP 端口 |

## 怎么看有没有成功

viewer 端每 5 秒打一行统计：

```
[rd_viewer] frames=422 decoded=422 rendered=422 dropped=0 events_sent=193 denied=0 beats=87 ctrl_out=280
```

`decoded` 在涨 = 画面正常。`decoded=0` = 媒体面没通。

设备端每 5 秒打一行：

```
[rd_device] capture=1941 pumped=1298 ctrl_in=164 ctrl_sent=0 injected=0 denied=0 refused=0
```

`capture` 在涨 = 采集正常。`pumped` 在涨 = 编码器在出帧。

详细说明见 `docs/guides/lan-test-runbook.md`。
