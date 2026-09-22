# ADR-013: GMSSL — 国密 DTLS 后端

| | |
|---|---|
| Number | 013 |
| Status | **Accepted** |
| Date | 2026-09-21 (Proposed) → 2026-09-21 (Accepted) |
| Phase | P1 — v0.12.0 阶段 2 |
| Bound to | v0.12.0 |
| Related | `docs/plan/transport-selection.md` §5.2 / §6.1, `docs/adr/ADR-001.md` |

## Context

NimRTC v0.11.0 在阶段 1（PAL Slice 4 / TPAL-4）完成了 DTLS seam 接口（`IDtlsSession` + `IDtlsSessionFactory`）的抽象化，并将现有的 wolfSSL DTLS 后端改造为第一个可插拔的后端（id = `"wolfssl"`）。引擎层通过 `EngineConfig::dtls_name` 选择后端，`core::PluginRegistry::get_dtls_session(id)` 在运行时解析对应的工厂。

随着鸿蒙 OpenHarmony 支持进入实质性集成阶段（stage1 PR），出现了两个对 GMSSL（国密 SSL）后端的需求：

1. **鸿蒙设备上无 wolfSSL 预装**：OpenHarmony 自身不提供 wolfSSL，交叉编译 wolfSSL 并集成进 OpenHarmony 的 musl + oh-dev 环境比预装 OpenSSL 代价更高。GMSSL（https://github.com/guanzhi/GmSSL）作为独立的开源项目，自带 SM2/SM3/SM4 crypto 原语，**不依赖 OpenSSL**，提供 GM/T 0044（TLCP）DTLS 1.2 协议栈。OpenHarmony 设备可通过系统预装或 sysroot vendor 路径直接集成。
2. **国内政企场景的合规要求**：在金融、电力、政务等场景，GM/T 0028 / GM/T 0044 合规要求使用 SM2/SM3/SM4 算法。GMSSL 同时支持 SM2 密钥体系 + GM/T 0044（TLCP）握手 + GM/T 0024（SSL VPN，已不推荐），覆盖这些场景。

**问题**：是否应该在 NimRTC 中添加第二个 DTLS 后端（GMSSL），以支持鸿蒙/OpenHarmony 设备和 SM 合规部署场景？

## Decision

**添加 GMSSL 作为第二个可选 DTLS 后端（id = `"gmssl"`），与 wolfSSL 并存，由 `EngineConfig::dtls_name` 驱动选择。**

### 具体决策

1. **后端 id = `"gmssl"`**，与 wolfSSL 的 `"wolfssl"` 并存于同一个 `PluginRegistry::dtls_session_` 表中。引擎 `dtls_name` 字段默认为空（走 wolfSSL），填 `"gmssl"` 走 GMSSL 路径。

2. **默认 cipher suite 跟随 GMSSL TLCP 套件**
   （`TLS_cipher_ecdhe_sm4_gcm_sm3` 0xe051 / `TLS_cipher_ecdhe_sm4_cbc_sm3`
   0xe011，基于 SM2 证书 + SM3 PRF + SM4-GCM AEAD）。
   GMSSL 后端服务于 SM-only 部署场景——这些场景下无浏览器参与，
   互通性由 wolfSSL 后端（默认 backend）保证。SM-only 与西方套件
   互不兼容（OH-RISK-6），是产品层面的取舍：集成方按部署场景
   选择 `cfg.dtls_name`。

3. **构建系统 opt-in**：默认 `NIMRTC_ENABLE_DTLS_GMSSL=OFF`，
   只编译 wolfSSL 相关代码。启用 GMSSL 后 CMake 定义
   `NIMRTC_HAS_DTLS_GMSSL=1` 公共宏并链接 GMSSL v3.x native 库
   （`<gmssl/tls.h>` 等）。

4. **`DtlsSessionGmSSL` 继承 `IDtlsSession`**：与 `DtlsSessionWolfSSL` 共享完全相同的接口，不触碰引擎代码。seam surface 的每个方法（`open` / `tick` / `feed_inbound` / `take_outbound` / `set_role` / `srtp_keying_material` / `export_srtp_key_material` 等）一一对应。

5. **GMSSL 后端使用 SM2 自签证书**：与 wolfSSL 后端不复用证书
   （SM2 算法 vs ECDSA P-256 不兼容），独立 SM2 证书 + 私钥。
   wolfSSL 与 Chrome / Firefox / Safari 互通路径不变（默认 backend）。

6. **不向仓库 vendoring GMSSL 源码**：与 wolfSSL 保持一致的策略（wolfSSL 也未 vendoring）。`cmake/NimRTCVendored.cmake::nimrtc_link_gmssl()` 通过 `find_path` + `find_library` 在 `/usr/local`、`/usr`、`$ENV{GMSSL_ROOT}` 探测系统 GMSSL 安装，**不依赖 OpenSSL**（GMSSL 自带 SM2/SM3/SM4 crypto 原语）。

## Alternatives Considered

### A — 只用 OpenSSL 1.1.x（不做 GMSSL 专用封装）

OpenSSL 1.1.1 本身有 DTLS 1.2 支持，且许多 Linux / Android 设备预装了它。

**缺点**：
- OpenSSL 本身不支持 DTLS-SRTP 扩展（`SSL_export_keying_material` + `SRTP_AES128_CM_SHA1_80`），需要额外打 patch（`openssl-srtp` fork）才完整。
- 不支持 SM2/SM3/SM4 — 国内合规场景无法覆盖。
- 交叉编译 OpenSSL + SRTP patch 到鸿蒙 musl 环境与 wolfSSL 代价相当。

### B — 只支持 wolfSSL，放弃鸿蒙/OpenSSL 场景

维持现状，鸿蒙开发者自行 fork NimRTC 嫁接 GMSSL。

**缺点**：
- NimRTC 对 OpenHarmony 的承诺（见 `docs/plan/openharmony-support.md`）无法兑现。
- 每个下游 fork 各自维护 DTLS 后端，维护碎片化。
- 不利于在国内政企场景推广 NimRTC。

### C — GMSSL 与 wolfSSL 二选一编译（feature flag 互斥）

构建时通过 `NIMRTC_DTLS_BACKEND=wolfssl|gmssl` 选择其一，生成的二进制只能二选一。

**缺点**：
- 运行时无法切换（需要两个编译产物）。
- 测试需要两个独立编译，增加 CI 成本。
- 无法在一个进程中同时服务西方 peer 和 SM-only peer。

### D — GMSSL 仅用于 SM 密码套件，不做完整 DTLS 封装

只把 GMSSL 当作 SM2/SM3/SM4 的密码学引擎，不实现完整的 DTLS 握手路径。

**缺点**：
- 引入额外的内部抽象层（GMSSL → DTLS 状态机），复杂度高。
- 与 PAL Slice 4 设计原则相悖（seam 边界在 `IDtlsSession`，不在密码学原语层）。

## Consequences

### Positive

- 鸿蒙/OpenHarmony 设备可在不引入 wolfSSL 的前提下运行 NimRTC。
- 国内政企场景可使用 GMSSL + SM2/SM4 满足合规要求；wolfSSL 后端（默认）保持与 Chrome / Firefox 的西方互操作。集成方按部署场景选择 `cfg.dtls_name`。
- 代码增量约 1000 行（`dtls_gmssl_session.cpp` 900 行 + `dtls_gmssl_factory.cpp` 83 行），seam surface 与 wolfSSL 后端一一对应，引擎代码零改动。
- 构建 opt-in 保证了默认二进制不引入额外依赖。
- GMSSL 自带 SM2/SM3/SM4 crypto 原语，**不依赖 OpenSSL**——与系统中其他 OpenSSL 用户互不干扰。

### Negative / risks

- **SM 与西方套件互不兼容（OH-RISK-6）**：SM2 证书与 ECDSA P-256 证书不互通。OHOS ↔ 桌面互通需要部署双栈（OHOS 端 GMSSL，桌面端 wolfSSL）。这是产品现实，已在 `docs/plan/openharmony-support.md` §7 OH-RISK-6 登记。
- **没有 E2E 测试覆盖**（无 CI GMSSL 环境）：ADR Accepted 后的 OHOS SDK 验证阶段（OH-DOD-1 / OH-DOD-5）会交叉验证 GMSSL 后端。
- **GMSSL 源码未 vendoring**：`nimrtc_link_gmssl()` 通过 `find_path` + `find_library` 在 `/usr/local`、`/usr`、`$ENV{GMSSL_ROOT}` 探测系统 GMSSL 安装；集成方需自行安装 GMSSL v3.x。Windows 环境无 GMSSL 预编译产物，验证需在 Linux x86_64 + OHOS-aarch64 上完成。

## Verification

| Check | Where | Required |
|---|---|---|
| `DtlsSessionGmSSL::open()` 调用成功（SM2 + ECDHE-SM4-GCM-SM3） | 本地 Linux + GMSSL v3.x 环境 | yes |
| `test_dtls_gmssl_tlcp` 同进程 Client + Server TLCP 握手 round-trip 通过 | `tests/test_dtls_gmssl_tlcp.cpp` (~345 行) | yes |
| SRTP 密钥材料导出（60 字节：client_write_key[16] + server_write_key[16] + client_write_salt[14] + server_write_salt[14]） | 同上 | yes |
| GMSSL factory 在 `PluginRegistry::list_dtls_sessions()` 中出现（id="gmssl"） | 同上 | yes |
| wolfSSL 后端不被 GMSSL 路径影响（OH-DOD-10 无回归） | 4 平台 ctest 全绿 | yes |

## Implementation notes

### File layout

```
src/dtls/
  include/nimrtc/dtls/
    dtls_gmssl_session.hpp      # 新增：148 行，类声明（IDtlsSession 接口）
    dtls_plugin.hpp             # 修改：新增 test_only::get_gmssl_factory()
  src/
    dtls_gmssl_session.cpp      # 新增：900 行，DtlsSessionGmSSL 实现（GMSSL v3.x native C API）
    dtls_gmssl_factory.cpp      # 新增：83 行，GmSSLDtlsFactory + test_only slot
    dtls_plugin.cpp             # 修改：条件注册 GMSSL factory（#ifdef NIMRTC_HAS_DTLS_GMSSL）
  CMakeLists.txt                # 修改：option NIMRTC_ENABLE_DTLS_GMSSL + 条件 source
cmake/NimRTCVendored.cmake      # 修改：新增 nimrtc_link_gmssl() helper
tests/test_dtls_gmssl_tlcp.cpp  # 新增：345 行，OH-DOD-4 强制门槛测试
tests/CMakeLists.txt            # 修改：条件绑定 test_dtls_gmssl_tlcp
docs/adr/ADR-013-gmssl-dtls-backend.md  # 本文件（Status: Accepted）
docs/plan/openharmony-support.md # 修改：阶段 2 标记 ✅
CHANGELOG.md                    # 修改：v0.12.0 entry 新增
README.md                       # 修改：版本号 → v0.12.0-alpha，新增 OpenHarmony + GMSSL 行
```

### CMake 选项

```cmake
option(NIMRTC_ENABLE_DTLS_GMSSL
       "Build the GMSSL-backed DTLS factory (DtlsSessionGmSSL)" OFF)
# 启用时定义 NIMRTC_HAS_DTLS_GMSSL=1 公共宏 + 调用 nimrtc_link_gmssl(target)
# nimrtc_link_gmssl() 在 /usr/local、/usr、$ENV{GMSSL_ROOT} 探测
# <gmssl/tls.h> 与 libgmssl / gmssl.lib；不依赖 OpenSSL
```

### 集成方接入步骤

1. 在系统（Linux / OHOS-aarch64 sysroot）安装 GMSSL v3.x：从 https://github.com/guanzhi/GmSSL 编译并 `cmake --install` 到 `/usr/local` 或 sysroot。
2. CMake 配置：`cmake -DNIMRTC_ENABLE_DTLS_GMSSL=ON -DGMSSL_ROOT=/path/to/install ..`
3. 运行：`cmake --build .`
4. 代码选择：`EngineConfig cfg; cfg.dtls_name = "gmssl";`
5. OHOS native build：`NIMRTC_ENABLE_DTLS_GMSSL=ON` 传入 OHOS 的 cmake cross-compile。

## Out of scope

- SM2/SM3/SM4 cipher suite 与 RFC 5764 §5 Western 套件互通（OH-RISK-6；产品层面取舍，集成方按场景选择 backend）。
- GMSSL 源码 vendoring（保持与 wolfSSL 一致的不 vendoring 策略）。
- Windows MSVC 下 GMSSL 交叉编译（OHOS 目标以 Linux/ARM 为主；Windows 验证 OH-DOD-4 走 Linux x86_64）。
- GMSSL DTLS 在 OHOS-aarch64 上的 ctest 自动覆盖（CI runner 现实约束；OH-DOD-5 允许 `continue-on-error`，但本地必须验证）。
