# Linux x86_64 平台支持开发计划

| | |
|---|---|
| 版本 | v1.0（草案） |
| 日期 | 2026-09-08 |
| 状态 | **SUPERSEDED** — 阶段 1 (chore/cleanup-onboarding) + 阶段 2 + 阶段 4 (CI 接入) 已在 [PR #B3] 落地：新增 `linux-gcc` / `linux-aarch64` / `macos-clang` CI jobs（见 `.github/workflows/ci.yml`）、新增 `debug.aarch64` / `tests.aarch64` preset 与 `cmake/toolchains/aarch64-linux-gnu.cmake`、CHANGELOG 平台矩阵从 "Windows-only" 切换到 ✅/🔶 状态。**本文档保留为审计归档**：§3.1 / §3.2 / §3.3 的 R1 审计记录仍然有用，§阶段 1.0 前置（mbedTLS build 集成打通）已随 wolfSSL 迁移整体作废——见文档内 OBSOLETE 标记。1.0 实际准入门槛见 `CHANGELOG.md` "Platform support matrix"。 |
| 目标 | Linux x86_64 (GCC + Clang) 编译 + 单元测试 + loopback-p2p smoke 全链路通过 |

> **纪律**：本文档对齐 `docs/zh/NimRTC-V2-技术文档.md` §13 路线图（1.0.0 准入门槛），不写"生产级"措辞。本计划不替代 §13 roadmap，是它的 Linux x86_64 子项。

---

## 1. 目标与非目标

### 1.1 目标（In Scope）

- Linux x86_64 上 GCC 10+ 与 Clang 12+ 双编译器均能完成：
  - `cmake --preset debug` 配置通过
  - `cmake --build --preset debug` 编译通过
  - `ctest --preset tests` 单元测试通过（当前 290+ 测试）
  - `loopback-p2p` smoke test 退出码 0
- ASan / UBSan / Coverage preset 在 Linux 上工作正常
- GitHub Actions 上 `linux-gcc` CI job 稳定运行

### 1.2 非目标（Out of Scope）

- **e2e Chrome 互通验证**——仍以 Windows x86_64 + 桌面 Chrome 为主战场（与 §13.1 口径一致）
- macOS / aarch64 / Android / iOS / 信创平台（独立 PR）
- 不引入新的 vendored 依赖；mbedTLS 4.2.0 源码仍在 `src/third_party/mbedtls/`，但 DTLS 生产路径自 `chore/cleanup-onboarding` 切换至 wolfSSL，mbedTLS 仅作为历史源码 vendor（**build 未集成也不再集成**）
- 不重构现有 plugin 抽象层（§ADR-001），仅在加密学子模块下沉一层轻抽象
- **peer ECDSA 签名验证** —— 计划 §1.1 不包含 `crypto::verify_p256` 的实现；当前 NimRTC 跳过 peer 签名验证，靠 SDP fingerprint + TLS Finished verify_data 保证完整性，此口径不变

---

## 2. 当前已具备的 Linux 资产审计

| 资产 | 版本 / 状态 | 路径 | 备注 |
|---|---|---|---|
| `CMakePresets.json` 的 `debug` / `release` / `asan` / `ubsan` / `coverage` | ✅ 已定义 | 仓库根 | 单 `condition: Linux` 限制 |
| `cmake/NimRTCOptions.cmake` 非 MSVC 分支 | ✅ 已实现 | `cmake/` | `-Wall -Wextra -Wpedantic` + sanitizer |
| `NIMRTC_ASAN` / `NIMRTC_UBSAN` 选项 | ✅ 已实现 | `cmake/` | Linux 默认 ON/OFF 切换 |
| libjuice | 1.6.0 | `src/third_party/libjuice/` | `add_subdirectory` 上游 + INTERFACE wrapper，`juice` STATIC |
| libsrtp | 3.0.0-dev | `src/third_party/libsrtp/` | `add_subdirectory` 上游 + INTERFACE wrapper，`CRYPTO_LIBRARY=internal` |
| mbedtls | 4.2.0（**历史源码 vendor，仅作 archive；DTLS 生产路径已切至 wolfSSL**） | `src/third_party/mbedtls/src/` | 不再调上游 `add_subdirectory`；阶段 1 中所有"打通 mbedTLS build 集成"目标作废 |
| libopus | 1.6.1 | `src/third_party/libopus/` | `add_subdirectory` 上游，`NO_ASSERTS=1` |
| googletest | 1.12.1 | `src/third_party/googletest/` | 离线构建 |
| nlohmann_json | 3.11.3 | `src/third_party/nlohmann_json/` | INTERFACE 单头 |
| `nimrtc_link_wolfssl(target)` CMake helper | ✅ 已挂载 | `cmake/NimRTCVendored.cmake` | DTLS 模块 CMakeLists 调用（wolfssl 当前为生产路径） |

## 3. 当前阻断 Linux 构建的关键障碍

> **重要更新（commit `chore/cleanup-onboarding`）**：本节 §3.1 / §3.2 / §3.3 撰写时假设 `src/modules/dtls/src/dtls.cpp` + `dtls_prf.cpp` 仍以 BCrypt 直写方式实现 DTLS。**wolfSSL 迁移 WIP 已删除这两个文件**（现在 DTLS 仅走 `DtlsSessionWolfSSL` 封装 wolfSSL），故 §3.1 中所述的"89 处 BCrypt 调用点"现在为 **0**——本节后续表格保留为 R1 审计的归档记录，不再反映当前代码状态。
> 阶段 1 实际工作已简化为：**§1.0 前置的 mbedTLS build 集成目标整体作废**，转而验证 `src/third_party/wolfssl/` 的 vendored 产物在 Linux 上能正确 link（已在 WIP `cmake/NimRTCVendored.cmake` 中接入 wolfSSL 分支，见 §2 已挂载项）。

#### 3.1 ~~`dtls.cpp` 内 BCrypt 调用清单（R1 审计结果）~~ — OBSOLETE (post wolfSSL migration)

**原 R1 审计结论**：`src/modules/dtls/src/dtls.cpp` 中 84 处 + `dtls_prf.cpp` 中 5 处，合计 89 处 BCrypt 调用点（文件已被 `chore/cleanup-onboarding` 删除）。

**当前实际状态**：上述两文件已不存在；DTLS 唯一实现是 `src/modules/dtls/src/dtls_wolfssl_session.cpp`（封装 wolfSSL DTLS 1.2 + SRTP + ECDSA + AES-GCM），头文件 `src/modules/dtls/include/nimrtc/dtls/dtls_wolfssl_session.hpp` 定义 `DtlsSessionWolfSSL` API。**BCrypt 调用点总数 = 0**。

下表保留为 R1 audit 的历史归档（不在 1.0 范围内重新审计）：

- **无 `#else` 分支**（函数在 Linux 下整体消失）：`bcrypt_random`、`bcrypt_sha256`、`bcrypt_sha256_concat`、`bcrypt_ecdsa_sign_der`、`bcrypt_import_aes_gcm_key`、`aes_gcm_seal`、`aes_gcm_open`、`Impl::teardown_crypto`、`Impl::enqueue_handshake` 内 epoch≥1 BCrypt 调用、`Impl::process_record` 内 epoch≥1 BCrypt 调用、`Impl::make_server_hello` 内 random 调用、`Impl::handle_client_hello` 内 random 调用、`DtlsSession::open` 内 client_random 调用
- **有 `#else` 分支但返回占位值**（编译过但跑不动）：`Impl::init_crypto`（0xAA pubkey）、`Impl::hs_update`（空）、`Impl::hs_clone_digest`（32B 0）、`Impl::build_self_signed_cert`（return false）、`Impl::derive_pre_master_secret`（32B 0）、`Impl::compute_handshake_hash_snapshot`（32B 0）

**API 调用统计**（按调用次数降序）：

| BCrypt API | 次数 | 替代 mbedTLS API |
|---|---|---|
| `BCryptCloseAlgorithmProvider` | 13 | RAII 析构自动消失 |
| `BCryptDestroyKey` | 11 | RAII 析构自动消失 |
| `BCryptOpenAlgorithmProvider` | 8 | `mbedtls_md_setup` / `mbedtls_gcm_init` / `mbedtls_pk_setup` |
| `BCryptHashData` | 6 | `mbedtls_md_update` |
| `BCryptExportKey` | 6 | `mbedtls_ecp_point_write_binary` / `mbedtls_ecdh_compute_shared` |
| `BCryptCreateHash` | 5 | `mbedtls_md_setup` |
| `BCryptGetProperty` | 5 | mbedTLS 内部尺寸固定，无需查询 |
| `BCryptDeriveKey` | 4 | `mbedtls_ecdh_compute_shared` |
| `BCryptFinishHash` | 4 | `mbedtls_md_finish` |
| `BCryptDestroyHash` | 4 | RAII 析构自动消失 |
| `BCryptSetProperty` | 3 | mbedTLS 直接配 tag 长度 |
| `BCryptGenerateKeyPair` | 3 | `mbedtls_ecp_gen_key` / `mbedtls_pk_setup` |
| `BCryptFinalizeKeyPair` | 3 | 同上，单步 |
| `BCryptSignHash` | 2 | `mbedtls_pk_sign`（已直接输出 DER） |
| `BCryptImportKeyPair` | 2 | `mbedtls_ecp_point_read_binary` / `mbedtls_ecdh_read_peer_public` |
| `BCryptGenerateSymmetricKey` | 2 | `mbedtls_gcm_setkey` |
| `BCryptSecretAgreement` | 2 | `mbedtls_ecdh_compute_shared`（无 secret handle 概念） |
| `BCryptEncrypt` / `BCryptDecrypt` | 1 / 1 | `mbedtls_gcm_crypt_and_tag` / `mbedtls_gcm_auth_decrypt` |
| `BCryptGenRandom` | 1 | `mbedtls_ctr_drbg_random` |
| `BCryptDestroySecret` | 1 | RAII 自动消失 |
| **`BCryptVerifySignature`** | **0** | **不在范围**——NimRTC 不做 peer 签名验证，靠 SDP fingerprint + Finished verify_data 保证完整性 |

#### 3.2 `dtls.cpp` 内**非 BCrypt** 的 Win32 API（同样阻塞 Linux）

| API | 位置 | Linux 等价 | 备注 |
|---|---|---|---|
| `gmtime_s` | `dtls.cpp:356`（`der_encode_utctime`） | `gmtime_r` | **已有 `#ifdef _WIN32` 兼容分支**，无需改 |
| `CreateFileA` / `WriteFile` | `dtls.cpp:663`（`dtls_hex_dump` 写文件） | `std::ofstream` | **需在阶段 1 一并迁移**；hex dump 是 debug-only |
| `_dupenv_s` | `dtls.cpp:~2520`（NSS keylog） | `getenv` | **需在阶段 1 一并迁移** |

> **结论**：阶段 1 的实现 subagent 在替换 BCrypt 调用之外，必须顺带把这 3 处非 BCrypt 的 Win32 API 也迁移掉；否则 Linux 链接或运行时会失败。

#### 3.3 `dtls_prf.cpp`（独立小文件）

- 130 行，所有 BCrypt 调用集中在 `dtls_hmac_sha256`（41–82 行）一个函数内
- 整文件**几乎可以整体替换**为 `mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), ...)` 调用
- 阶段 1 拆为**独立 micro-task**（半天内可完成）

#### 3.4 项目根 `tests/` 下三个独立程序（决策待用户拍板）

| 文件 | 性质 | Linux 下的处理（待决策） |
|---|---|---|
| `tests/test_dtls_gcm_aead.cpp` | BCrypt-direct AES-GCM round-trip | 待用户拍板，见 D5 |
| `tests/test_dtls_inproc.cpp` | BCrypt-direct in-process 互通 | 同上 |
| `tests/test_dtls_x25519.cpp` | BCrypt-direct X25519 互通 | 同上 |

它们**不**链接进 `nimrtc_dtls.a`，是 Windows-only 独立 sanity-check。

#### 3.5 ~~mbedTLS 4.2.0 build 集成未打通（隐藏的关键障碍）~~ — OBSOLETE

**原始 R2 阻塞**：mbedTLS 4.2.0 源码已 git clone 到 `src/third_party/mbedtls/src/`，但 NimRTC wrapper `src/third_party/mbedtls/CMakeLists.txt` 仅声明 INTERFACE 库、未真正编译上层 `add_subdirectory`。

**当前决定（wolfSSL 迁移后）**：mbedTLS 不再作为生产 DTLS 路径；R2 揭示的"打开 mbedTLS build 集成"目标整体作废，Perl / `framework/` git submodule / `find_package(mbedtls CONFIG)` 等前置全部从 1.0 退出准则删除。`src/third_party/mbedtls/` 保留为历史源码 vendor（不再被 link、不再被 include），删除该目录需要独立 PR（archive 决策），不在本计划范围内。

wolfSSL 作为生产 DTLS 后端的 Linux build 验证落在 §阶段 2（同 `cmake --preset debug` / `--build` / `ctest` 流程），export 阶段无新增 Perl / framework 依赖。

```cmake
# 当前 wrapper 行为（src/third_party/mbedtls/CMakeLists.txt）
add_library(nimrtc_vendor_mbedtls INTERFACE)
target_include_directories(nimrtc_vendor_mbedtls INTERFACE ${_src_dir}/include)
target_compile_definitions(nimrtc_vendor_mbedtls INTERFACE NIMRTC_USE_MBEDTLS=1)
# —— 关键：未调用上游 add_subdirectory(src/CMakeLists.txt) ——
# wrapper 注释明示：For now we do NOT actually compile mbedtls.
# The upstream build needs Perl (for scripts/generate_*.pl)
# and a populated `framework` submodule, which the vendored archive is missing.
```

后果：即使某个模块 `target_link_libraries(... nimrtc_MBEDTLS_acquired)`，INTERFACE 库不产二进制，link 阶段看似过，但 DTLS 模块一旦调 `mbedtls_*` 实际链接就会爆 `undefined reference`。**阶段 1.1 之前的 1.0 前置必须先把这个 wrapper 改造为真正调用上游 `add_subdirectory`**（或者改用上游推荐的 `find_package(mbedtls CONFIG REQUIRED)` 路径）。

**额外前置**：
- WSL2 工具链：`perl` 必须装（mbedTLS 4.x `scripts/generate_*.pl` 在 Linux 上需要 Perl 5.x）；现有 `apt install` 一行要追加 `perl`
- mbedTLS `framework/` git submodule：当前目录已有内容（时间戳晚于主 clone），但需在阶段 1.0 启动时再校验一次 `ls third_party/mbedtls/src/framework/scripts/` 是否齐全

---

## 4. 阶段划分

### 阶段 1：DTLS 密码学抽象 + mbedTLS 后端（P1 DoD，预计 2–3 周）

**这是整个计划的 critical path。**

#### 1.0 前置：mbedTLS build 集成打通（1 天，必须先于 1.1 开工）

**这是 R2 揭示的隐藏关键路径**——1.1 写 mbedTLS 后端调用前，build 必须先能产出真实二进制。

具体动作：
1. 修改 `src/third_party/mbedtls/CMakeLists.txt`：
   - 移除"`do NOT actually compile mbedtls`"注释 + 提前 `return()`
   - 改为调用 `add_subdirectory(${_src_dir} ${CMAKE_BINARY_DIR}/_deps/nimrtc_mbedtls_build)`
   - 暴露上游 target：`nimrtc_vendor_mbedtls` 改为包装 `mbedtls::mbedtls` / `mbedtls::mbedcrypto` 等真实 target（用 INTERFACE IMPORTED 或 ALIAS）
   - 类比 `nimrtc_vendor_libopus` 的 wrapper 写法（MSVC runtime 切 `/MDd` / Linux 切 `-Wno-*` 等 vendor-friendly 编译选项）
2. 修改 `src/third_party/CMakeLists.txt` 中 `_nimrtc_vendor_add_if_populated(mbedtls)` 检测，让 NimRTC wrapper 与上游 CMakeLists 共存
3. WSL2 一次性 `sudo apt install -y perl` 装好（追加进阶段 2.0 工具链）
4. 验证：在 WSL2 内 `cmake --preset debug -DNIMRTC_VENDORED=ON` + `cmake --build --preset debug --target nimrtc_vendor_mbedtls` 应**无需 link `nimrtc_dtls`** 也能产出 mbedtls 静态库（独立 sanity-check）

**出口准则**：`find NIMRTC_MBEDTLS_acquired` 在 build 配置日志里可见；`mbedtls_version_string()` 在一个临时 hello 程序里能跑通（验证 `mbedtls_*` 真实链接进来，不只是 header path 暴露）。

#### 1.1 抽象加密原语层（2–3 天）
- 在 `src/core/include/nimrtc/core/crypto/` 下新建薄抽象接口（**仅声明，不实现**）：
  - `sha256.hpp` — `one_shot_sha256(span)` / `incremental_sha256{}` / `finalize()`
  - `aead.hpp` — `seal_aes_gcm(key, nonce, aad, pt) → (ct, tag)` / `open_aes_gcm(...)`
  - `ecdsa.hpp` — `sign_p256(sk, digest) → der_sig`（**不含 verify_p256**——见 §1.2 非目标）
  - `ecdh.hpp` — `compute_shared_p256(priv, peer_pub) → secret`
  - `rng.hpp` — `secure_random(span)`
- 命名空间 `nimrtc::crypto`；后端通过 `PluginRegistry::register_crypto_backend(name)` 注册。
- 每个抽象定义 traits 接口 + 自由函数 fallback（先 traits，后续可下沉到 profile）。

#### 1.2 实现 mbedTLS 后端（1 周）
- 新增 `src/core/src/crypto/mbedtls_*.cpp`，直接调用已 vendor 的 mbedTLS 4.2.0 API：
  - `mbedtls_sha256_*` / `mbedtls_gcm_*` / `mbedtls_pk_sign` / `mbedtls_ecdh_compute_shared` / `mbedtls_ctr_drbg_*`
- ECDSA 输出走 ASN.1 DER 编码（与现有 BCrypt 路径一致的 wire 格式，Chrome 互通口径不变）。
- AES-GCM tag 兼容逻辑**整段删掉**——mbedTLS `mbedtls_gcm` 接受任意 tag 长度，BCrypt 12B→wire 16B 的手工 pad 0 兼容逻辑不再需要。
- `bcrypt_ecdsa_sign_der` helper 整段删掉——`mbedtls_pk_sign` 已直接输出 DER，无须 raw→DER 适配。
- `bcrypt_sha256_concat` helper 整段删掉——R1 审计确认无 caller。
- `BcryptAlg` / `BcryptAesKey` / `BcryptHash` RAII 包装整段删掉——mbedTLS 的 `mbedtls_md_context_t` / `mbedtls_gcm_context` 用标准 RAII 自己包。

#### 1.3 改造 `dtls.cpp` 使用抽象层（3–4 天）
- 全部 89 处 BCrypt 调用（见 §3.1 表）→ 替换为 `crypto::sha256(...)` / `crypto::aead::seal(...)` 等抽象调用。
- 删除整个 `#if NIMRTC_HAS_BCRYPT` 大段代码，改为单一路径 + 后端选择。
- `Impl` 类成员（`dtls.cpp:828-862`）的 BCrypt 句柄字段整段替换为抽象类型（`crypto::HashCtx`、`crypto::EcdhKey`、`crypto::EcdsaKey` 等）。
- 自签名 X.509 证书构建（`build_self_signed_cert`）：保留 `der_encode_*`（已是平台无关 DER 编码，约 80% 函数体不变），把 3 个具体 BCrypt 调用点（`bcrypt_random(16)` 序列号、`bcrypt_sha256(tbs)` 摘要、`bcrypt_ecdsa_sign_der` 签名）换成抽象层。
- `bcrypt_export_p256_pub` / `bcrypt_import_p256_peer_pub` 之类的 BCRYPT_ECCPUBLIC_BLOB 适配（`dtls.cpp:973-991` / `dtls.cpp:2427-2433`）→ 改用 mbedTLS `mbedtls_ecp_point_write_binary` / `mbedtls_ecp_point_read_binary`（直接读写 65 字节 uncompressed point 0x04‖X‖Y）。
- 顺带迁移 3 处非 BCrypt 的 Win32 API（见 §3.2）：`CreateFileA`/`WriteFile` → `std::ofstream`，`_dupenv_s` → `getenv`；`gmtime_s` 已有兼容分支不动。
- `dtls_prf.cpp`（130 行）作为独立 micro-task 单独处理：整文件 BCrypt 集中在 `dtls_hmac_sha256`（41–82 行），整段换为 `mbedtls_md_hmac`。

#### 1.4 验证（3 天，强制）
- 在 Windows 上跑现成的 `test_dtls.cpp`（10 个测试）+ `test_cert_dump.cpp` —— 抽象层在 Windows 下应自动选 BCrypt 后端，行为必须与改前**字节级一致**。
- 跑 `loopback-p2p` + `tools/run_e2e_acceptance.py` Case D（Chrome 互通）—— 保证 §10 0.9.0-rc1 已记录的 DTLS↔Chrome 修复不退化。

**出口准则**：Windows 上 `ctest --preset tests.msvc` 全部 290+ 测试 PASS，Chrome 互通保持 0.9.0-rc1 当前状态（DTLS 完整报文发送，ServerHello reject 是已记录 known issue）。

---

### 阶段 2：Linux 编译 + 单测 + loopback（1–2 天）

#### 2.0 验证环境

**首选**：本机 WSL2 Ubuntu 22.04（开发机已就位），与 GitHub Actions `ubuntu-22.04` runner 镜像一致，可并行验证。本地 WSL2 与 CI 任何一处红，都视为阶段 2 未通过。

**配置要求（一次性）**：

```bash
# 在 WSL2 内执行
sudo apt update
sudo apt install -y g++ cmake ninja-build clang python3 git ca-certificates
# 可选：sudo snap install cmake --classic   # 如需更新的 CMake
```

**预装状态校验**（开发机当前）：GCC 11.4 / g++ 11.4 ✅；Clang / CMake / Ninja 待装，详见上述 `apt install` 一行。

**目录与 I/O 纪律**：

| 位置 | 放什么 | 为什么不放另一边 |
|---|---|---|
| Windows `\\wsl$\Ubuntu-22.04\home\<user>\NimRTC`（或 IDE 内 `/home/<user>/NimRTC`） | 源码编辑 | WSL2 ↔ Windows 跨边界 I/O 慢，**禁止**在此目录放 build/ |
| WSL2 `~/NimRTC/build/` | 所有构建产物 | 必须在 WSL2 文件系统内，否则 ninja incremental link 阶段会卡顿 |
| Windows 侧编辑器 | View / Edit 源码 | 不在 Windows 侧跑 `cmake` / `ninja` |

**双编译器切换**：

```bash
# GCC（默认）
cmake --preset debug -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++

# Clang（必跑，作为 GCC 通过后的第二验证）
cmake --preset debug-clang -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
```

`debug-clang` preset 与 `debug` preset 同源，仅切换编译器；若尚未在 `CMakePresets.json` 里定义，阶段 2 准备阶段先加一行 `inherits: debug` 的子 preset。

#### 2.1 验证命令

```bash
cmake --preset debug -DNIMRTC_VENDORED=ON -DNIMRTC_BUILD_EXAMPLES=ON
cmake --build --preset debug -j$(nproc)
ctest --preset tests --output-on-failure
./build/examples/loopback-p2p/debug/loopback-p2p   # 或对应 multi-config 子目录
```

#### 预期问题
1. **GCC 比 MSVC 严格**：`-Wsign-conversion` / `-Wconversion` / `-Wshadow` 触发更多警告，因 `-Werror` 失败。
2. **vendor 库与 `-Werror` 边界**：vendor 的 C 库走 `SYSTEM` include，不受 NimRTC 的 `-Werror` 约束；这点需确认 `nimrtc_apply_options()` 没把 vendor 拉进来。
3. **链接器路径**：libopus / libjuice 的 Linux 库路径无问题（vendored 源码编进 `nimrtc_dtls` 同进程）。
4. **`loopback-p2p` 网络栈**：WSL2 在同 distro 内 `127.0.0.1` UDP 互通与真机一致，无需考虑 Windows ↔ WSL2 localhost 镜像问题。

**出口准则**：GCC 与 Clang 两套 preset 各跑一次；当前 290+ 测试全部 PASS；loopback-p2p 退出码 0；编译无新增警告。

---

### 阶段 3：Sanitizer 验证（可选，1–2 天）

确认 sanitizer preset 在 Linux 上可用：

```bash
cmake --preset asan   && cmake --build --preset asan   && ctest --preset tests
cmake --preset ubsan  && cmake --build --preset ubsan  && ctest --preset tests
cmake --preset coverage && cmake --build --preset coverage && ctest --preset tests
```

- `NIMRTC_ASAN` / `NIMRTC_UBSAN` 在 GCC/Clang 上是 `-fsanitize=address` / `-fsanitize=undefined`，已实现。
- 把 sanitizer 加入 CI 矩阵（夜间跑或 PR 触发）。

---

### 阶段 4：CI 集成（1 天）

#### 4.1 重新启用 Linux job
- 编辑 `.github/workflows/ci.yml`：在 `windows` job 之后添加 `linux-gcc` job。
- runner：`ubuntu-22.04`（与本地 WSL2 镜像一致）。
- steps：
  1. `actions/checkout@v4` with `submodules: recursive`
  2. `apt-get install -y g++ cmake ninja-build python3`（与本地 2.0 节同一行命令）
  3. `cmake --preset debug -DNIMRTC_BUILD_EXAMPLES=ON`
  4. `cmake --build --preset debug -j 2`
  5. `ctest --preset tests --output-on-failure -j 2`
  6. Loopback smoke：跑 `./build/examples/loopback-p2p/debug/loopback-p2p`
  7. （可选）增加 `linux-clang` job，与上同步骤，仅切 `CC=clang CXX=clang++`，作为第二编译器验证

#### 4.2 必要护栏
- `concurrency.cancel-in-progress: true`（已在 workflow 顶层）
- **首 2 周**：linux job 设 `continue-on-error: true`，仅给反馈不阻断 PR 合入 —— 等 5 个连续绿再切换为 blocking。

**出口准则**：`ci.yml` 含 `linux-gcc` job，从一次绿起算连续 5 个 PR 都绿。

---

### 阶段 5：文档与发布（1 天）

#### 5.1 README.md
- 把 0.9.0-rc1 的 "Platform support notice" 段落从 "Windows-only" 改写为：
  > "Windows 10 / MSVC + Linux x86_64 (GCC 10+ / Clang 12+) 验证通过；macOS / aarch64 / 移动端不在本期支持范围。"
- 更新 Quick Start：在 Linux/macOS 部分去掉 "may not actually configure" 的预警。

#### 5.2 CHANGELOG.md
- 在 `[Unreleased]` 加一条：
  > "Linux x86_64 (GCC, Clang) build verification added; address DTLS BCrypt→mbedTLS migration (#XXX)."

#### 5.3 CONTRIBUTING.md
- 把 Quick Start 里 `cmake --preset debug && cmake --build --preset debug && ctest --preset tests`（Linux/macOS）的注释去掉，让它正式生效。

#### 5.4 `interop/README.md`
- 更新基准浏览器互通矩阵：明确互通仍以 Windows + x86_64 Chrome 为主，Linux CI 仅做 build + 单测 + loopback。

---

## 5. 关键风险与缓解

| 风险 | 概率 | 影响 | 缓解 |
|---|---|---|---|
| DTLS 抽象层在 Windows 上回归 | 中 | 高 | 阶段 1.4 强制跑全测 + Chrome 互通保持 |
| mbedTLS 后端 ECDSA 输出与 BCrypt ASN.1 DER 不一致 | 低 | 高 | 单元测试覆盖两后端同输入的字节级 diff |
| GCC `-Wconversion` 在已 vendor 的 C 代码里大面积冒警告 | 高 | 中 | vendor 库走 `SYSTEM` include；新代码逐模块修 |
| Loopback 在 Linux 容器里 UDP 端口被占用 | 低 | 低 | 选 40000+ 端口 + 重试机制 |
| aarch64 CI 误开成 x86_64 Linux，混淆读者 | 低 | 低 | job name 显式标 `linux-gcc-x86_64`；arch runner 后置 |
| 时间预算失控（阶段 1 抽象层拖 > 1 周） | 中 | 中 | 退路：跳过抽象层直接 `#ifdef _WIN32 / __linux__` 双实现 |
| mbedTLS 默认 config 在 Linux 构建系统不识别 | 低 | 中 | CMake 层走已 vendor 路径，不依赖系统 mbedTLS |
| 本地 WSL2 与 CI Runner 配置漂移 | 中 | 中 | 阶段 2.0 列同一行 `apt-get install` 与同一 Ubuntu 22.04 版本；任何漂移立即同步 |
| Windows ↔ WSL2 边界 I/O 性能反噬增量构建 | 高 | 低 | 强制约束：build/ 必须位于 WSL2 文件系统内（不进 `/mnt/c`） |
| **mbedTLS 4.x build 集成未打通**（vendor wrapper 只导 INTERFACE，未实际编译 mbedtls） | **高（已验证存在）** | **高** | **阶段 1.0 前置专项处理**；如不做，DTLS 一旦调 `mbedtls_*` 即 `undefined reference`；在 1.0 出口准则里加 mbedtls_version_string() 临时 hello 验证 |
| mbedTLS 4.x `framework/` submodule 未完整填充（脚本生成依赖） | 中 | 中 | 阶段 1.0 启动时校验 `framework/scripts/` 目录齐全；缺则 `git submodule update --init --recursive` |
| mbedTLS 4.x `scripts/generate_*.pl` 依赖 Perl | 中 | 中 | WSL2 一次性 `apt install -y perl`；mbedTLS 上游 build 不允许无 Perl |
| `tests/test_dtls_{gcm_aead,inproc,x25519}.cpp` 三个 BCrypt-direct 独立程序 | 中 | 低 | D5 决策项（见 §7 待定决策登记） |
| dtls.cpp 内非 BCrypt 的 Win32 API（`CreateFileA`/`_dupenv_s`）漏改 | 中 | 中 | 阶段 1.3 任务 spec 显式列出这 3 处；R1 报告 §3.2 已锁定位置 |

---

## 6. 时间估算

| 阶段 | 工作量 | 依赖 |
|---|---|---|
| 1. DTLS 抽象 + mbedTLS 后端 + Windows 回归 | 2–3 周 | 无 |
| 2. Linux 编译 + 单测 + loopback | 1–2 天 | 阶段 1 |
| 3. Sanitizer 验证 | 1–2 天 | 阶段 2（可选） |
| 4. CI job 接入 | 1 天 | 阶段 2 |
| 5. 文档更新 | 1 天 | 阶段 4 |
| **合计** | **约 3–4 周** | — |

---

## 7. 待定决策登记

| 编号 | 决策点 | 默认值（若未拍板） | 影响范围 |
|---|---|---|---|
| **D5** | 项目根 `tests/test_dtls_{gcm_aead,inproc,x25519}.cpp` 三个 BCrypt-direct 独立程序在 Linux 下如何处理 | 待用户拍板 | 阶段 1.3 验收 |
|   | 备选 A：保留，文件顶部加 `#if defined(_WIN32)` 整段排除，Linux 下不编不进测试 | 保留 sanity-check 在 Windows 上仍跑；Linux build 自动跳过 | 三个文件加 guard 即可 |
|   | 备选 B：删掉三文件，由阶段 1.4 输出的 `test_dtls.cpp`（已用 nimrtc_dtls 抽象层）覆盖相同 case | 依赖抽象层单测到位；少 3 个独立 sanity check | 删除 3 文件，约 200 行 |
|   | 备选 C：改写为 mbedTLS-direct 等价版本 | 多约 2 天工作量；新 mbedTLS 测试资产沉淀 | 重写 3 文件 |

**倾向**：**A（最低成本，不丢 sanity-check）**——除非用户更看重"测试资产统一"，否则没必要为 Linux 改写一次 BCrypt-direct 测试。

---

## 8. commit 拆分建议

1. `feat(crypto): add platform-neutral crypto primitives abstraction (sha256/aead/ecdsa/ecdh/rng)`
2. `feat(crypto+mbedtls): implement mbedTLS backend`
3. `feat(crypto+bcrypt): wire BCrypt backend to the new abstraction`
4. `refactor(dtls): migrate dtls.cpp from BCrypt to crypto abstraction`
5. `test(dtls): parametrize dtls tests over backend implementation`
6. `ci: enable Linux GCC build + test job`
7. `docs: mark Linux x86_64 as verified platform`

---

## 9. 与更大路线图的对齐

- **§P0 "Linux/macOS support is a 1.0.0 acceptance gate"** 的 Linux 部分，本计划完成后即可勾掉。
- **macOS** 是独立后续 PR：本计划验证完 GCC/Clang 编译器 + linker 假设后，Apple Clang 只需补 `__APPLE__` 平台分支（详见后续 `docs/plan/macos-x86_64-support.md`）。
- **aarch64 交叉编译**仍按 §13.1 走 GitHub Actions ARM runner + QEMU smoke test，本计划不涉及。

---

## 10. 前置决策（已拍板）

| 决策点 | 结果 | 日期 |
|---|---|---|
| **D1 抽象层粒度** | ✅ **方式 1：mbedTLS 单后端，直接切掉 BCrypt** | 2026-09-08 |
| **D2 Linux CI 是否首日即阻塞 main** | ⏳ 待定（见下方默认值） | — |

**D1 选方式 1 的理由**（记录归档）：
- 架构自洽：§D1 原话是"DTLS 走 mbedTLS/BoringSSL 后端"，BCrypt 是例外不是主线
- 交付节奏：Linux 支持是 1.0.0 发版门槛，mbedTLS 单后端工期 2–3 周，双后端 3–4 周
- 无数据不决策：BCrypt vs mbedTLS 在 DTLS 握手（一次 ECDHE + 一次 ECDSA + 几次 AES-GCM）上的性能差异对通话建立时延无感知，P3 性能优化阶段再加 BCrypt 硬件加速不迟
- 后续补 BCrypt：P3 在 plugin 体系（ADR-001）里注册 `BcryptBackendFactory{}` 即可，无需改动已稳定的单路径

**D2 默认值**：首 2 周 `continue-on-error: true`，连续 5 个绿后切换为 blocking。后续按实际 CI 反馈决定是否提前升为 blocking。

---

## 11. 前置侦察记录（archive）

| 侦察 | 范围 | 关键发现 | 计划影响 |
|---|---|---|---|
| **R1** | `src/modules/dtls/src/dtls.cpp` BCrypt 全量审计 | 89 处 BCrypt 调用；`BCryptVerifySignature` 0 处调用；`bcrypt_sha256_concat` 死代码；`bcrypt_ecdsa_sign_der` 整段可删（mbedTLS 直接输出 DER）；AES-GCM tag 12B↔16B 兼容逻辑随 mbedTLS 整段简化；非 BCrypt 的 Win32 API 散布（`CreateFileA`/`_dupenv_s`）也需迁移；`dtls_prf.cpp` 130 行可整体替换 | §3.1 / §3.2 / §3.3 / 阶段 1.3 / 风险表 |
| **R2** | vendored mbedTLS 资产盘点 | mbedTLS 4.2.0 源码已 vendor，但 wrapper CMakeLists.txt 只声明 INTERFACE 库、未实际编译；需在阶段 1.0 前置专项打通 wrapper；WSL2 工具链追加 `perl`；`nimrtc_link_mbedtls()` helper 已写好未挂载 | §3.5 / 阶段 1.0 / 风险表 |

R1 / R2 subagent 输出已归档，可随时复核。
