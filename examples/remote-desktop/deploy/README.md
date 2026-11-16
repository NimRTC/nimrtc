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

   > **屏幕不能锁！** Windows 一旦锁屏（或屏保启动），WGC 采集会停在
   > 1 帧不动，DXGI 会直接报 `E_ACCESSDENIED`。保持会话处于已登录且
   > 桌面可见的状态（把屏保和自动锁定都关掉）。

   > **想用键盘鼠标控制设备端，必须加 `--inject`。** 默认不开，是为了
   > 不让测试脚本意外操纵你本机的桌面。开了之后操作端发的鼠标/键盘
   > 事件会被真实注入到设备机的系统里。

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

### 设备端可选项

| 参数 | 作用 |
|---|---|
| `--inject` | **允许把操作端发来的鼠标/键盘事件真实注入设备机系统**。默认关闭 |
| `--capture windows-dxgi` | 换成 DXGI 桌面复制采集（默认是 WGC） |
| `--capture windows-wgc` | 显式指定 WGC 采集 |
| `--backend software` | 软件编解码（没有 GPU / 硬件编解码器时用） |
| `--codec h264` / `av1` / `h265` | 指定视频编码 |

### 已在本机验证过的组合

下面每一条都在本机跑通了完整链路（信令 → 配对 → ICE → 采集 → 编码 → 传输 → 解码 → 渲染）：

| 设备端 | 操作端 |
|---|---|
| `--backend hardware`（WGC） | `--backend hardware --sink d3d11` |
| `--backend software`（WGC） | `--backend software` |
| `--backend hardware --capture windows-dxgi` | `--backend hardware --sink d3d11` |

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
| 设备端 `capture` 不涨 / 卡在 1 | **屏幕被锁了**。关掉屏保和自动锁定，保持会话已登录 |
| 设备端报 `plugin open failed` | 看后面附带的 DXGI 原因：`E_ACCESSDENIED` = 锁屏，`NOT_CURRENTLY_AVAILABLE` = 别的程序占着桌面复制 |
| 设备端 `injected=0` | 没加 `--inject`，输入事件被安全地丢掉了 |
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
