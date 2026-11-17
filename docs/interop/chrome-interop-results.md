# Chrome ↔ NimRTC 互操作结果

**范围**：Case E 的两个平面——DTLS-only 互通（`--case=e`）与
WebTransport 互通（`--case=e_webtransport`）。

**为什么有这份文档**：v1.2 计划里的 **E4** 要求「在 DataChannel 与视频
Part B 合并之后重跑互操作矩阵，并把结果记录下来」。此前 `docs/` 下只有
`dtls_chrome_interop_review.md`（一份*代码审查*，不是*运行结果*），而
Case E 从未被真正执行过——`interop.yml` 只生成了 WebTransport 的证书材料，
却从不把 Case E 当作断言来跑。

**状态**：两个平面均 **PASS**，并已接入 CI 作为断言
（`.github/workflows/interop.yml` 的 `Run Case E interop` 步骤）。

---

## 1. 运行环境

| 项 | 值 |
|---|---|
| 平台 | Windows 11 (10.0.26200), x64 |
| 工具链 | Visual Studio 2022 (MSVC), `debug.msvc` preset |
| 浏览器 | Google Chrome 157.0.8103.79 (系统安装) |
| 自动化 | Playwright (Python) |
| 构建配置 | `-DNIMRTC_VENDORED_WEBRTC_APM=OFF` |
| 日期 | 2026-11-17 |

## 2. 复现方法

```powershell
# 1. 构建（Case E 需要 examples：demo-p2p 与 demo-wtserver）
cmake --preset debug.msvc -DNIMRTC_BUILD_EXAMPLES=ON -DNIMRTC_BUILD_TESTS=ON `
      -DNIMRTC_VENDORED_WEBRTC_APM=OFF
cmake --build build --config Debug -j

# 2. 生成 WebTransport 的 TLS 材料（并把证书装进受信任根）
powershell -NoProfile -ExecutionPolicy Bypass `
  -File ./scripts/generate_wtserver_cert.ps1 -BuildDir "$PWD/build"

# 3. 跑两个平面
python tools/run_e2e_acceptance.py --case=e
python tools/run_e2e_acceptance.py --case=e_webtransport
```

机器可读的结果落在 `build/e2e/case_e_results.json` 与
`build/e2e/case_e_webtransport_results.json`。

## 3. 平面一：DTLS-only 互通（`--case=e`）

Chrome 用 `RTCPeerConnection` 建连，NimRTC 用 `demo-p2p` 建连，信令走本地
WebSocket 代理。

| # | 断言 | 结果 | 证据 |
|---|---|---|---|
| E1 | Chrome WebSocket 已连接 | ✅ PASS | `wsConnected: true` |
| E2 | Chrome ICE 已连接 | ✅ PASS | `iceConnected: true` |
| E3 | Chrome `dtlsState === 'connected'` | ✅ PASS | `dtlsState: "connected"` |
| E4 | Chrome 已安装 SRTP 密钥 | ✅ PASS | `srtpCipher: SRTP_AES128_CM_HMAC_SHA1_80` |
| E5 | NimRTC 侧 DTLS 握手完成 | ✅ PASS | 日志 `engine[dtls→srtp]: DTLS reached Connected` + keylog 里的 `CLIENT_RANDOM` |

**结论：PASS。**

E5 有两条独立证据：wolfSSL 导出的 SSLKEYLOGFORMAT keylog（`CLIENT_RANDOM`
证明主密钥已导出，即握手越过了服务端 Finished），以及 demo-p2p 的日志行
`DTLS reached Connected`（证明 NimRTC 自己认定了握手完成）。两条都通过。

### 稳定性

连续 3 次运行全部 PASS，无 flake。

> **一个已知的非代码干扰源**：如果上一次运行留下了残留的 Chrome 进程，
> harness 的 `browser.close()` 可能挂住，表现为 orchestrator 侧
> `case E timed out after 120 s`。但此时 Chrome 侧与 NimRTC 侧的断言其实
> **都已经通过**（见 `build/e2e/case_e_chrome_console.log` 与
> `case_e_proxy.err`），只是进程没能退出。清理残留 Chrome 进程后立即
> 复跑即通过。CI 上每次都是全新 runner，不存在这个问题。

## 4. 平面二：WebTransport 互通（`--case=e_webtransport`）

Chrome 用 `WebTransport` API（QUIC + HTTP/3），NimRTC 用 `demo-wtserver`
（msquic 后端）应答。

| # | 断言 | 结果 | 证据 |
|---|---|---|---|
| E1 | Chrome WebTransport API 可用 | ✅ PASS | `apiAvailable: true` |
| E2 | `session.ready` 已 resolve | ✅ PASS | `connected: true` |
| E3 | Datagram 已发出 | ✅ PASS | `datagramSent: true` |
| E4 | Datagram 回显已收到 | ✅ PASS | `datagramPayload: "NIMRTC-WT-DGRAM-ECHO-01"` |
| E5 | 双向流已打开并写入 | ✅ PASS | `streamSent: true` |
| E6 | 流回显已收到 | ✅ PASS | `streamRecv: true` |

**结论：PASS。**

## 5. 根因：WebTransport 平面此前**从未通过**

这是本次工作最有价值的发现。Case E 的 WebTransport 平面从 2026-10-24
第一次尝试起就是失败的，而且失败原因不在 NimRTC 的协议实现里，而在
**证书信任**上。

### 症状

Chrome 侧的控制台：

```
Failed to establish a connection to https://localhost:9443/webtransport:
net::ERR_QUIC_PROTOCOL_ERROR.QUIC_TLS_CERTIFICATE_UNKNOWN
(TLS handshake failure (ENCRYPTION_HANDSHAKE) 46: certificate unknown.
 SSLErrorStack: ... error:1000007d:SSL routines:OPENSSL_internal:CERTIFICATE_VERIFY_FAILED)
```

NimRTC 侧（`demo-wtserver`）：

```
[info] TLS certificate loaded from Windows store (SHA1 hash: 8f9fb07b...)
[info] WebTransport listener started on UDP port 9443
[info] QUIC connection accepted by listener
[error] QUIC connection shut down by transport: status=0x-2143223506 error_code=0x302
```

注意：**QUIC 连接被接受了**——传输层没问题；死在 TLS 握手阶段。

### 根因

`scripts/generate_wtserver_cert.ps1` 用
`New-SelfSignedCertificate -CertStoreLocation Cert:\CurrentUser\My`
生成证书。这只把它装进了**个人存储**（personal store）——那正是 msquic
通过 `QUIC_CREDENTIAL_TYPE_CERTIFICATE_HASH_STORE` 加载它的地方，所以
**服务端永远是好的**。

但客户端校验的是**证书链**。自签证书自己就是自己的签发者（`CN=localhost`
既是 subject 也是 issuer），除非这张具体的证书出现在某个**受信任存储**
里，否则它就是一个"未知根"，握手必然以 alert 46 告终。

而 harness 里的 `--ignore-certificate-errors` **救不了 WebTransport**：
Chromium 的 QUIC 证明校验器
（`net/quic/crypto/proof_verifier_chromium.cc`）会把"校验通过但签发者不是
已知根"的结果转成 `ERR_QUIC_CERT_ROOT_NOT_KNOWN`。harness 里已经为此写了
`--webtransport-developer-mode`，但即使叠加这两个开关，**已知根的判定
本身**仍然不成立。

### 修复

证书必须同时装进 `Cert:\CurrentUser\Root`（受信任根存储）。`CurrentUser\Root`
不需要管理员权限，所以在 CI runner 和开发机上都能用。

修复落在 `scripts/generate_wtserver_cert.ps1`：生成证书之后，额外把它
`Add` 到 `Cert:\CurrentUser\Root`。导入失败时脚本**不中断**（证书仍在
个人存储里，demo-wtserver 照样能起来；受影响的只是浏览器的信任判定），
而是打印一条明确的告警，说明 Chrome 会以 alert 46 拒绝握手。

修复后本机复跑，两个平面均 PASS。

## 6. CI 接入

`.github/workflows/interop.yml` 新增 `Run Case E interop (DTLS + WebTransport)`
步骤：

- 在既有的 `Provision WebTransport TLS material` 之后执行；
- 依次跑 `--case=e` 与 `--case=e_webtransport`，**任一失败即让 job 失败**；
- 与其余 e2e 用例分开成独立步骤，因为 Case E 需要浏览器的机器信任那份
  证书，而且 orchestrator 的 `--case=all` 有意不包含 Case E（`all` 指的是
  A/B/C/D 四个平面，Case E 是显式请求的互操作对）。

此前 Case E 是"准备了材料却从不执行"，所以一个坏掉的 Chrome 互操作
**无法让 CI 变红**。现在它是一个断言。

## 7. 尚未覆盖的部分

这份文档记录的是 Case E 的**两个**平面。仍未在 CI 中作为断言运行的是：

- **Case D / D-rev**（音频平面的 Chrome 互通）：仍然在跑，但属于 Case D
  的既有范畴，不在 E4 的范围内。
- **DataChannel（`RTCDataChannel`）互通**：remote-desktop 计划里的 **G1b**
  标记为可选；`usrsctp` 已按 **E7** 移除，因此 NimRTC 侧的 SCTP 后端当前
  解析到 `"stub"` 工厂，浏览器侧 `RTCDataChannel` 互通目前**不可能通过**。
  这一项需要单独决策。
