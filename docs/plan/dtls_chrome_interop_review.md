# `dtls_wolfssl_session.cpp` — Chrome 兼容性审查

**审查范围**: `src/modules/dtls/src/dtls_wolfssl_session.cpp`
**目的**: 在跑 Chrome 端到端互通测试前，识别所有可能导致 Chrome 拒接或
DTLS 握手失败的问题。
**严重度**: 🔴 Blocker (握手直接失败) · 🟠 High (Chrome 版本差异) · 🟡 Medium (边缘情况)

---

## 🔴 Blocker 1 — Cipher list 不符合 RFC 5764 §5 DTLS-SRTP 强制要求

**位置**: `setup_context()` 第 ~245 行

```cpp
int rc = wolfSSL_CTX_set_cipher_list(
    ctx, "ECDHE-ECDSA-AES128-GCM-SHA256:"
         "ECDHE-ECDSA-AES128-SHA256");   // ⚠ 这个套件被 RFC 5764 §5 排除
```

**问题**: RFC 5764 §5 明确要求 DTLS-SRTP **只能** 使用下列套件:

- `TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256` (0xC02B) — MTI (mandatory-to-implement)
- `TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384` (0xC02C) — 可选

`ECDHE-ECDSA-AES128-SHA256` 是 CBC + HMAC-SHA256 套件 (0xC023)，不在
RFC 5764 列表里。Chrome 默认会从这个列表里挑，如果 NimRTC 不提供，
协商直接挂掉。

**修复**: cipher list 改成:

```cpp
wolfSSL_CTX_set_cipher_list(
    ctx,
    "ECDHE-ECDSA-AES128-GCM-SHA256:"
    "ECDHE-ECDSA-AES256-GCM-SHA384");
```

并删除 `ECDHE-ECDSA-AES128-SHA256`。

---

## 🔴 Blocker 2 — `verify_peer_spki()` 默认拒绝握手

**位置**: `verify_peer_spki()` 第 ~485 行

```cpp
if (!expected_peer_fp_set_) {
    // Returning false here routes through on_handshake_complete()
    // which sets state = Failed.
    nimrtc::core::log::Logger::instance().error(
        "wolfSSL: no peer SPKI fingerprint was configured ... ");
    return false;
}
```

**问题**: 当前实现是 **fail-closed**。如果 caller 还没来得及调用
`set_peer_fingerprint()` 就 pump 了 handshake，DTLS 直接进 Failed。
**问题更严重的地方**: `engine.cpp` 收到 SDP offer 后**先**调用
`dtls.set_role(Client)` + 开始 DTLS pump，**然后**才从 SDP 解析出
remote fingerprint 调 `dtls.set_peer_fingerprint()`。如果有任何代码路径
顺序错了，握手永远失败。

**修复方向** (二选一):

1. **fail-open + 警告**: 没有 pin 时仍允许握手完成，但 `log.warn(...)`。
   这样 demo/早期开发不会被 MITM 风险 block，但生产代码必须设置 pin。
2. **明确 ordering**: 在 `engine.cpp` 里严格保证
   `set_peer_fingerprint()` 在 `process_remote_sdp()` 的最早期调用，
   并加单元测试验证 ordering。

我推荐 (1) — 因为 WebRTC 规范本身在 SDP `a=fingerprint` 缺失时就是
fail-open (Chrome 默认行为)。

---

## 🔴 Blocker 3 — 没有 `wolfSSL_dtls_got_timeout()` retransmit pump

**位置**: 整个类，缺失

**问题**: DTLS (RFC 6347 §4.2.4) 握手消息需要 retransmit timer。
wolfSSL 在非阻塞模式下需要外部调用 `wolfSSL_dtls_got_timeout(ssl)`
触发 retransmit。当前代码只在 `feed_inbound()` 被调用时 pump
handshake，**没有任何 timer tick**。结果:

- 如果 NimRTC 是 client，先发 ClientHello → 等 server HelloVerifyRequest
  → server 延迟 → client 不会 retransmit → 永远卡住。
- 如果 NimRTC 是 server，第一次 ClientHello 收到后 `wolfSSL_accept()`
  会触发 HelloVerifyRequest；client 必须 retransmit 带 cookie 的
  ClientHello，但 NimRTC 端如果 timer 不跑，server-side 也不会
  retransmit HelloVerifyRequest。

**修复**: 在 `DtlsSessionWolfSSL` 里加 `tick()` 方法，在 `engine.cpp`
的 tick 循环里每 ~50ms 调用一次，内部用
`wolfSSL_dtls_got_timeout()` 推进 wolfSSL 内部 timer。

---

## 🟠 High 1 — EMS (Extended Master Secret) 没有显式确认

**位置**: `setup_context()` 注释第 ~262 行

```cpp
// wolfSSL enables EMS by default for DTLS 1.2 methods ...
// This comment is a tripwire: do NOT add a Disable call here
```

**问题**: Chrome 自 M76 起 **强制** EMS。wolfSSL 的 EMS 启用条件
依赖于编译选项 (`HAVE_EXTENDED_MASTER_SECRET`)，不是默认打开。
我们的 `src/third_party/wolfssl/CMakeLists.txt` 可能没启它。

**修复**:

1. 在 `src/third_party/wolfssl/CMakeLists.txt` 里加 `set(WOLFSSL_EXTENDED_MASTER_SECRET "yes")`
2. 在 `dtls_wolfssl_session.cpp::setup_context()` 里加 runtime 检查:
   ```cpp
   if (!wolfSSL_CTX_DisableExtendedMasterSecret(ctx)) {  // returns 0 if already enabled
       log::error("EMS not enabled at compile time — Chrome will reject");
       return false;
   }
   ```
   等等，`DisableExtendedMasterSecret` 语义是"禁用"，要反着用 `wolfSSL_CTX_get_*`。
   实际验证方法是在 handshake 完成后查 `wolfSSL_get_version()` 看
   cipher suite 是否包含 EMS extension。简单做法: build-time 加宏
   `WOLFSSL_EXTENDED_MASTER_SECRET` 并断言 wolfSSL 已支持。

---

## 🟠 High 2 — cipher list 不包含 AES256-GCM-SHA384

**位置**: 同 Blocker 1

**问题**: 部分 Chrome 版本 (尤其在 Linux/Android 上) 默认优先选
`TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384`。如果 NimRTC 只声明 128-bit
版本，会 downgrade 到 128-bit。功能上能跑，但某些企业 Chrome 配置
会强制 AES-256。

**修复**: 把 cipher list 加为:

```cpp
"ECDHE-ECDSA-AES128-GCM-SHA256:"
"ECDHE-ECDSA-AES256-GCM-SHA384"
```

(已经在 Blocker 1 修复方案里包含了)

---

## 🟠 High 3 — `SRTP_AES128_GCM_80` profile 名称错误

**位置**: `srtp_profile_name()` 第 ~177 行

```cpp
case SrtpProfile::Aes128Gcm:        return "SRTP_AES128_GCM_80";
```

**问题**: wolfSSL 期望的 use_srtp profile 字符串是 `SRTP_AES128_GCM`
(没有 `_80` 后缀)。`SRTP_AES128_GCM_80` wolfSSL 不识别，会 silently
no-op，导致 `wolfSSL_export_dtls_srtp_keying_material` 返回 0 长度。
SRTP keys 拿不到。

**修复**:

```cpp
case SrtpProfile::Aes128Gcm:        return "SRTP_AES128_GCM";  // no _80
```

---

## 🟡 Medium 1 — IPv6 不支持

**位置**: `set_peer()` 第 ~358 行

```cpp
peer_addr.sin_family = AF_INET;
peer_addr.sin_port   = htons(port);
inet_pton(AF_INET, host.c_str(), &peer_addr.sin_addr);
if (ssl) {
    wolfSSL_dtls_set_peer(ssl, &peer_addr, sizeof(peer_addr));
}
```

**问题**: 硬编码 IPv4。WebRTC 默认 ICE 同时 gather IPv4 + IPv6，
peer 可能选 IPv6 pair。当前代码 IPv6 path 会失败。

**修复**: 用 `getaddrinfo()` + `wolfSSL_dtls_set_peer()` 接受
`sockaddr_storage` (大小写)。简单做法: 暂时只支持 IPv4，
但在 `Config` 里加 warning: "IPv6 not yet supported — Chrome will
fall back to IPv4 candidate".

---

## 🟡 Medium 2 — 没有 DTLS connection migration 支持

**位置**: 整个类

**问题**: WebRTC ICE 在 NAT rebinding 时会切换 transport。
DTLS 不支持 in-band migration (不像 QUIC)；必须 client 主动
re-handshake。当前实现没有 re-handshake path。

**影响**: NAT rebinding 概率低，常见 WebRTC 部署 5-10 分钟一次。
生产环境需要 fix，但不是 Blocker (demo 测试不会触发)。

**修复**: 增加 `rehandshake()` 方法，在 `engine.cpp` 检测
ICE `connection_state` 从 `connected` → `disconnected` → `connected`
第二次时触发。

---

## 🟡 Medium 3 — `dtls.cpp` / `dtls_wolfssl.cpp` 旧文件已删但 CMakeLists 残留?

**位置**: `src/modules/dtls/CMakeLists.txt`

让我看下当前 CMakeLists.txt 确认已清干净。

---

## 🟡 Medium 4 — `feed_inbound` 重入问题

**位置**: `feed_inbound()` 第 ~750 行

```cpp
std::size_t DtlsSessionWolfSSL::feed_inbound(...) {
    ...
    impl_->pump_handshake();  // 可能 flush send_buf_
    return bytes.size();
}

std::vector<DtlsRecord> DtlsSessionWolfSSL::take_outbound() {
    auto recs = std::move(impl_->outbound_);
    impl_->outbound_.clear();
    if (impl_->ssl && state != Connected && state != Failed && state != Closed) {
        impl_->pump_handshake();  // ⚠ 可能产生更多 record
        ...
    }
    return recs;
}
```

**问题**: `take_outbound()` 在 handshake 未完成时再次 pump，
可能产生无限循环 (虽然实际不会因为 wolfSSL accept/connect 返回
WANT_READ/WANT_WRITE 时 flush_send_buf 只 flush 一次)。

**修复**: 在 `pump_handshake()` 里设个 `pumping_` flag 防止重入。
或者让 `take_outbound()` 调用 `wolfSSL_dtls_got_timeout()` 推进
而不是再 accept/connect。

---

## 🟡 Medium 5 — `wolfSSL_set_using_nonblock(ssl, 1)` 与当前 io_recv 行为不一致

**位置**: `create_ssl_object()` 第 ~342 行

```cpp
wolfSSL_set_using_nonblock(ssl, 1);
```

**问题**: 我们 `io_recv` 在 `recv_buf_` 空时返回 `WOLFSSL_CBIO_ERR_WANT_READ`。
但 wolfSSL 在 nonblock 模式下可能假设调用者会在 timer 到时重试
(用 `wolfSSL_dtls_got_timeout`)。当前 `io_recv` 没把这种语义暴露出来。

**修复**: 同 Blocker 3，加 `tick()` 方法。

---

## 🟢 Low 1 — `SOCKADDR_IN peer_addr{}` 在 Windows 上行为

Windows 上 `SOCKADDR_IN` 字段顺序与 Linux 略有不同，但 `sin_family`、
`sin_port`、`sin_addr` 都兼容。OK。

---

## 🟢 Low 2 — 死代码

**位置**: `export_srtp_keys()` 第 ~580 行

```cpp
if (rc != 0 && rc != WOLFSSL_SUCCESS) {  // WOLFSSL_SUCCESS == 1
    // so this is equivalent to (rc != 0) — dead branch
}
```

**修复**: `if (rc != 0)` 即可。

---

## 🟢 Low 3 — 没有统计 `retransmits`

wolfSSL 暴露 `wolfSSL_dtls_get_current_timeout()` 可用于观测。
当前 `Stats` 字段 `retransmits` 永远是 0。

---

# 总结: 修复优先级

| 优先级 | 项目 | 影响 |
|--------|------|------|
| 🔴 P0 | **Cipher list 去掉 CBC 套件** | Chrome 一定拒接 |
| 🔴 P0 | **加 `tick()` + `wolfSSL_dtls_got_timeout`** | 握手卡死 |
| 🔴 P0 | **SRTP profile 字符串 `SRTP_AES128_GCM`** | Aes128Gcm 模式下 keys 为空 |
| 🟠 P1 | **`verify_peer_spki` fail-open 默认行为** | Demo 路径走不通 |
| 🟠 P1 | **AES256-GCM-SHA384 加入 cipher list** | 部分 Chrome 版本降级失败 |
| 🟠 P1 | **wolfSSL 编译选项加 EMS** | Chrome 76+ 拒接 |
| 🟡 P2 | IPv6, rehandshake, 重入 guard | 边缘 case |

---

# 推荐实施顺序

1. **修复 Blocker 1, 2, 3 + High 3** (P0) — 这四个修完，单元测试
   `test_dtls_inproc` 仍然过。
2. **修复 High 1 (EMS)** — 加 wolfSSL 编译宏 + 单元测试。
3. **修复 Medium 4, 5 (重入)** — 修 `pump_handshake()` 重入。
4. **跑 bridge 测试** (`tests/wolfssl_dtls/bridge`) 验证 SPKI 互通。
5. **跑 Chrome 端到端** (任务 3)。

让我现在开始 P0 修复。
