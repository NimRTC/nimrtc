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

# 2. 生成 WebTransport 的 TLS 材料（并写出 SPKI 哈希）
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
既是 subject 也是 issuer），除非这张具体的证书被信任，它就是一个"未知根"，
握手必然以 alert 46 告终。

而 harness 里的 `--ignore-certificate-errors` **救不了 WebTransport**：
Chromium 的 QUIC 证明校验器
（`net/quic/crypto/proof_verifier_chromium.cc`）会把"校验通过但签发者不是
已知根"的结果转成 `ERR_QUIC_CERT_ROOT_NOT_KNOWN`。harness 里已经为此写了
`--webtransport-developer-mode`，但即使叠加这两个开关，**已知根的判定
本身**仍然不成立。

### 修复：SPKI 白名单

修复走的是**同一个 case 的 DTLS 平面已经在用**的机制：公布证书的
**SPKI SHA-256**，通过 `--ignore-certificate-errors-spki-list` 交给
Chrome，把信任判定精确地钉在这张证书的公钥上。

- `scripts/generate_wtserver_cert.ps1` 现在把哈希写到
  `build/wtserver_spki.b64`；
- `tests/chrome_interop_harness/run_webtransport_harness.py` 读取它并传给
  Chrome。

SPKI（SubjectPublicKeyInfo）的结构是：

```
SubjectPublicKeyInfo ::= SEQUENCE {
    algorithm         AlgorithmIdentifier,
    subjectPublicKey  BIT STRING }
```

脚本**直接从证书自身的 DER 编码里读出**这个元素，而不是从证书存储的
`PublicKey.EncodedParameters` / `PublicKey.EncodedKeyValue` 两个属性把它
重新拼出来。原因有两个，第二个是修好之前真实存在的缺陷：

1. `$cert.PublicKey` 会通过平台的加密提供程序打开私钥。在**非交互的
   runner 上这个调用不会返回**——它会一直阻塞，等一个没有 UI 可对话的
   提供程序，于是脚本挂到 job 的 30 分钟超时被杀。读 `$cert.RawData`
   完全不碰任何提供程序。

2. 之前的手工拼接方式把 `rsaEncryption` 这个 AlgorithmIdentifier **写死**
   了，再把公钥重新包成 BIT STRING。对 RSA 密钥而言，结果与真正的 SPKI
   **逐字节相同**；但换成别的密钥类型，它会**默默地算错**——一张 EC 证书
   会被拼上一个 RSA 的 AlgorithmIdentifier。直接从证书里取字节，
   AlgorithmIdentifier 就是证书实际用的那一个。

SPKI 的哈希结果**逐字节**与一份独立的纯 Python DER 遍历实现交叉验证过
（两者必须完全相等）；RSA（294 字节）与 EC（91 字节）两种证书都验证过，
EC 那一例确认取到的是 `ecPublicKey` OID，而不是写死的 `rsaEncryption`。

### 被否掉的替代方案

**装进 `Cert:\CurrentUser\Root`。** 这是最直观的做法，而且**最先试的就是
它**——但它在**非交互的 runner 上会挂死**：受保护的根存储会弹出一个确认
对话框，而 GitHub Actions runner 上没有人能回答它，于是脚本永远不返回，
job 一直跑到超时被杀。

> 这一点特别值得记下来：**它在交互式桌面上是能用的**。也就是说这个缺陷
> 在本地测试时完全看不出来，只有上了 CI 才暴露成"任务超时"。第一次 CI
> 运行（`windows / fast`）就是这样失败的：日志里有 `Certificate created`，
> 但没有随后的 `Trusted root installed`，然后 job 挂了 30 分钟。

它还有第二个问题：为了一个**测试**证书去改动整台机器的信任判定，会在证书
的有效期内改变机器上**每一个**其他程序对所有 TLS 连接的信任结论。

**只靠 `--ignore-certificate-errors`。** 如上所述，不够。

### 修复后

本机复跑（且受信任根存储里**没有** `CN=localhost` 证书，纯净起点），
WebTransport 平面的 E1~E6 全部 PASS。

## 6. CI 接入

`.github/workflows/interop.yml` 新增 `Run Case E interop (DTLS + WebTransport)`
步骤：

- 在既有的 `Provision WebTransport TLS material` 之后执行；
- 依次跑 `--case=e` 与 `--case=e_webtransport`，**任一失败即让 job 失败**；
- 与其余 e2e 用例分开成独立步骤，因为 Case E 需要 WebTransport 的 TLS
  材料，而且 orchestrator 的 `--case=all` 有意不包含 Case E（`all` 指的是
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
