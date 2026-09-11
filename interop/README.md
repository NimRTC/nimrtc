# interop/ — Chrome ↔ NimRTC Interop Test Suite

Browser and third-party interop test fixtures, scripts, and runners.

## 目录结构

```
interop/
├── README.md                   ← 本文件
├── signaling/
│   ├── README.md               ← 信令协议说明
│   └── signaling_server.py     ← Python WebSocket 信令服务器（SDP 交换）
├── chrome/
│   └── test_chrome_opus.html   ← Chrome headless 测试页面（WebRTC API）
├── fixtures/
│   ├── chrome_opus_offer.sdp         ← Chrome 生成的 Opus SDP offer 样本
│   └── chrome_opus_offer.expected.md ← NimRTC 解析该 SDP 的期望结果
├── docker/
│   ├── Dockerfile              ← 完整的 NimRTC + Chrome 测试容器镜像
│   └── docker-compose.yml      ← 一键启动 signaling + NimRTC + Chrome 容器
├── run_interop.py              ← Python 测试运行器（所有测试用例）
└── .github/
    └── workflows/
        └── interop.yml         ← CI 自动化 workflow
```

## 测试用例

| 测试 | 验证内容 | 依赖 |
|------|---------|------|
| `sdp_exchange` | `demo-p2p` 生成 RFC 8829 兼容的 Opus SDP offer | 仅需二进制 |
| `loopback_ice` | 两个 `NimRTCEngine` 实例在 127.0.0.1 上完成 ICE 握手 | `loopback-p2p` |
| `chrome_opus_interop` | Chrome (headless) ↔ NimRTC via 信令服务器，验证 ICE + RTP | 信令服务器 + Chrome |

## 快速开始

### 1. 启动信令服务器

```bash
# 安装依赖
pip install websockets

# 启动（默认 ws://localhost:8765）
python interop/signaling/signaling_server.py --port 8765
```

### 2. 运行测试

```bash
# 安装依赖
pip install websockets

# 运行所有测试
python interop/run_interop.py

# 运行单个测试
python interop/run_interop.py --test sdp_exchange

# 指定 NimRTC 二进制路径
python interop/run_interop.py --nimrtc-binary ./build/examples/demo-p2p/demo-p2p
```

### 3. 手动验证（Chrome 交互）

```bash
# 1. 启动信令服务器
python interop/signaling/signaling_server.py --port 8765

# 2. 打开 Chrome（headless）
chrome --headless=new \
       --virtual-time-budget=10000 \
       --use-fake-ui-for-media-stream \
       --use-fake-device-for-media-stream \
       "file:///$(pwd)/interop/chrome/test_chrome_opus.html?room=interop&ws=ws://localhost:8765/interop"

# 3. 另一终端：用 demo-p2p 生成 offer 并保存
./build/examples/demo-p2p/demo-p2p > /tmp/offer.sdp

# 4. 通过信令服务器手动交换 SDP（需要 demo-p2p 支持 WebSocket）
```

## CI 集成

`interop.yml` 在以下情况触发：

- 推送到 `main` / `feat/**` / `fix/**` / `chore/**` 分支
- PR 合入 `main` 时
- 修改 `interop/`、`src/engine/`、`src/modules/sdp/`、`src/modules/rtp/`、`src/modules/opus/`、`examples/` 时

CI 运行内容：
1. 构建 NimRTC（`--preset debug -DNIMRTC_BUILD_EXAMPLES=ON`）
2. 运行 `sdp_exchange` 测试
3. 运行 `loopback_ice` 测试
4. 启动信令服务器，运行 `chrome_opus_interop` 测试
5. 上传 SDP fixture 到 artifacts

## P1 互通口径（Tier 0）

当前仅承诺 **音频单向**（NimRTC 发送，Chrome 接收）：
- Chrome → NimRTC answer 的 SDP offer（NimRTC 解析 + 生成 answer）
- ICE 连接建立（STUN 握手）
- Chrome 接收 NimRTC 发出的 Opus RTP 包

**不含**：
- Chrome → NimRTC 的音频（需要 `demo-p2p` 接入麦克风）
- DataChannel（属于 P2 范畴）
- 视频（属于 P1 范畴）

## Docker 隔离环境

如果不想在 host 装 Chromium + Playwright，可以用 docker-compose 跑完整测试：

```bash
# 一次性构建镜像 (5-10 分钟，包括编译 demo-p2p)
docker compose -f interop/docker/docker-compose.yml build

# 运行 Chrome interop 测试 (signaling + NimRTC + Chrome 三容器)
docker compose -f interop/docker/docker-compose.yml up \
    --abort-on-container-exit --exit-code-from nimrtc
```

容器内会自动：
1. 编译 NimRTC `demo-p2p` (Debug 配置)
2. 启动 `signaling_server.py` 健康检查
3. 启动 `nimrtc` 容器跑 `chrome_opus_interop` 测试
4. 结果写入 stderr；非零退出码表示失败

## 预期失败项（P1 完成前）

| 测试 | 当前预期 | 原因 |
|------|---------|------|
| `chrome_opus_interop.audioReceived` | FAIL | libopus 是 stub，不产生真实 Opus 包 |
| `chrome_opus_interop.iceConnected` | PASS (loopback) / SKIP (Chrome WS) | 需要信令服务器 |

libopus vendor 完成后，这些测试应该自动通过。
