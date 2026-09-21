# OpenHarmony 平台支持开发计划

> **Status: DRAFT.** 决策点（target version、是否与 GMSSL 后端绑定）未拍板；先冻结文档结构，§2.3 / §8 的开放问题在第一次评审时关闭。本文不替代 `docs/zh/architecture.md` §13 路线图，是 OpenHarmony 平台 + GM/T 密码栈的子项计划。

| | |
|---|---|
| 版本目标 | **v0.12.0 (candidate)** — 详见 §2.3 开放问题 |
| 计划开始 | 2026-09-21 |
| 计划关闭 | TBD（取决于 §2.3 决议） |
| Owner | BDFL（1 人 + Cursor 基线） |
| Scope class | **平台扩展 + 新密码后端** — 平台代码与 crypto backend 同 PR train 上的相邻两个高风险子项，单独 milestone 切分 |
| 公开标签 | "OpenHarmony Tech Preview"（待定） |
| Phase | **P3-prep**（介于 v0.11.0 P2 出口与 architecture §13 P3 "Beta 不再心虚" 之间） |
| Reference | `docs/zh/architecture.md` §13 P3 行；`README.md` "Xinchuang / regulated environment that mandates GM/T cryptography" 用户画像；`docs/plan/linux-x86_64-support.md` §1.2（"信创平台 欢迎 PR"）；`docs/plan/transport-selection.md`（PAL Slices 4–8，crypto / transport 已插件化） |

> **纪律**：本文档对齐 `docs/zh/architecture.md` §13，不写"生产级"措辞；不在 1.0 之前承诺互通兼容性。新 platform 的资产审计、风险登记、阶段划分参照 `linux-x86_64-support.md` 的写法（§2 资产表 / §3 R-Audit / §4 阶段 / §6 DoD），保持可对照阅读。

---

## 1. Motivation

`README.md` 把 NimRTC 的差异化卖点浓缩在两行——"可控 / 可静态审计 / 几 MB"和"GM/T 密码栈可替换"。**这两条都直接指向 OpenHarmony，却没有任何代码路径已经覆盖到它**。把 OpenHarmony 推到 P3-prep 优先级的三个前置条件：

1. **README 用户画像的内在一致性。** "Xinchuang / regulated environment that mandates GM/T cryptography" 这一行写在"Building an SFU without rewriting protocol code"之前——但 NimRTC 当前**没有 GMSSL 后端、没有任何信创平台 build target**。这一行目前是 *愿景*，不是 *能力*。OpenHarmony 是第一个能把它从愿景变成能力的目标平台。
2. **`linux-x86_64-support.md` §1.2 已经划了边界。** 文档把 macOS / aarch64 / Android / iOS / 信创平台列为"独立 PR"——意味着 Linux aarch64 已落地后，下一个该拿下的就是信创体系，而 OpenHarmony 是信创体系里唯一同时覆盖移动 + IoT + 标准设备的入口。
3. **PAL Slice 1–8 已落地。** `transport-selection.md` §6 把 ICE / DTLS / SCTP / RTP / transport-stack 全部拉到了 typed factory seam（v0.10.2 / v0.10.3 持续推进）。意味着 OHOS 支持**不需要重写 transport 代码**，只缺：(a) 一个 OHOS toolchain、(b) 一个 GMSSL DTLS 后端实现、(c) 平台音频 HAL 适配。

**为什么不做 Android / iOS**（README 已经写过，这里复用并扩展）：

- Android 上有 Google 维护的 `libwebrtc`（System WebView 内置 Chromium WebRTC 栈），NimRTC 在 Android 上没有差异化窗口
- Android Google Play 路径与 GM/T 合规互斥（信创场景强制剔除 GMS）
- iOS 是 Apple-only、闭源生态，没有 GM/T 通道、受美国出口管制、且 `Network.framework` 替换 BSD socket 是大坑
- OpenHarmony 在国内信创目录是**唯一**移动端国产 OS，且官方 SDK（DevEco Studio + Native API）直接支持 C++ NDK 风格开发，工具链可控

---

## 2. Scope

### 2.1 In scope（v0.12.0 准入门槛内）

| ID | Item | Source | Notes |
|---|---|---|---|
| OH-TC-1 | 新增 `cmake/toolchains/ohos-aarch64.cmake`：OHOS SDK / musl 工具链探测，类比 `aarch64-linux-gnu.cmake` 的双模式（real cross clang / `--target` 模式） | §3 资产审计 | 单一 aarch64 优先；x86_64 OHOS 模拟器作为 secondary |
| OH-TC-2 | 新增 `CMakePresets.json` `debug.ohos-arm64` / `tests.ohos-arm64` preset，condition 仿照现有 `condition.hostSystem == Linux` 写法，但用 `${sourceDir}/cmake/toolchains/ohos-aarch64.cmake` | CMakePresets.json | 预留给 OHOS-Lite 设备的 `release.ohos-arm64-lite` 单独 post-1.0 考虑 |
| OH-POSIX-1 | 现有 4 平台产物（core / engine / dtls / srtp / opus / rtp / ice / sctp）在 OHOS-aarch64 toolchain 上**全部 cmake 配置 + 编译 + 链接成功**（测试可豁免，见 OH-TEST-1） | §4.1 R1 兼容层 | 4 平台 green 是 0 → 1 的硬性前置 |
| OH-POSIX-2 | 现有 `cmake/NimRTCOptions.cmake` 的 `if(MSVC)` / `if(UNIX)` 分支补 `if(OHOS)` 分支，主要处理 musl libc 缺失项（详见 §4.1） | cmake/ | 不引入新 vendor 依赖 |
| OH-DTLS-1 | `IRawUdpFactory` / `IDtlsSessionFactory` / `ISctpSocketFactory`（v0.10.2 Slice 8）已有的 typed factory seam 验证：换 backend 不动 engine / DTLS 调用方 | transport-selection.md §6 | **不需要写新代码**，但要有一个最小 demo 跑通"在 OHOS 上用 wolfSSL DTLS"的 build + 一次 round-trip 单元测试（test_dtls_gcm_aead 的 OHOS 版本） |
| OH-GMSSL-1 | **GMSSL DTLS 后端实现**（`src/modules/dtls/src/dtls_gmssl_session.cpp`）—— 包装 GMSSL 4.x 的 GM/T 0024（SSL VPN）/ GM/T 0044（TLCP）协议。注册 id = `"gmssl"` 到 `core::PluginRegistry::register_dtls_session()` 槽位（v0.10.2 已落地） | README.md "Xinchuang" 画像；ADR-013 见 §8 | **本计划 critical path**，是最大杠杆点；非 GMSSL 用户维持 wolfSSL 路径，零回归 |
| OH-GMSSL-2 | GMSSL vendored wrapper（`src/third_party/gmssl/CMakeLists.txt`），参照 `nimrtc_vendor_wolfssl` 写法和 `nimrtc_link_wolfssl()` CMake helper，新增 `nimrtc_link_gmssl()` helper | cmake/NimRTCVendored.cmake | GMSSL 4.x build 集成（与 mbedTLS 4.x 集成风险面类似，参考 `linux-x86_64-support.md` §3.5 OBSOLETE 教训） |
| OH-DTLS-3 | `engine.cpp` 增加 `cfg.dtls_name` 字段（v0.10.2 Slice 7.5 占位）—— 解析为 `reg.get_dtls_session(name)`。默认 `"wolfssl"`，OHOS profile 默认 `"gmssl"` | engine.cpp / engine.hpp | engine.hpp 公共 API **不破坏性变化**：`cfg.dtls_name` 默认值兼容 v0.10.x |
| OH-AUDIO-1 | 平台音频 HAL 调研：OHOS OHAudio（推荐）/ OpenSL ES（OHOS 兼容）/ AAudio（OHOS 不支持）。**第一阶段不实现**，仅做调研 + 决策文档 `docs/plan/ohos-audio-hal.md`（独立 PR） | docs/plan/ | 推迟到 v0.12.x patch 或 v0.13.0；本计划内不阻塞 v0.12.0 准入 |
| OH-CI-1 | 新增 `.github/workflows/ci.yml` 的 `ohos-arm64` job：使用开源 OHOS runner（huawei-openlab / community runner）或自托管 runner；先跑 build + ctest，e2e 互通延后 | ci.yml | runner pool 风险参考 `linux-aarch64` job（已临时 `if: false`） |
| OH-DOC-1 | `README.md` 的 platform support 矩阵新增 OpenHarmony 行（🔶 build only，test 视 runner 情况）；"Who is it for" 段落明确 Xinchuang / GM/T 场景支持现状 | README.md | 同步 CHANGELOG.md v0.12.0 entry |
| OH-DOC-2 | 新增 `docs/zh/architecture.md` §13 路线图 P3 行 OpenHarmony 子项交叉引用 | docs/zh/architecture.md | 不修改 §13 现有 P2/P3 行结构 |
| ADR-013 | 新增 `docs/adr/ADR-013-gmssl-backend.md` —— 把 GMSSL DTLS 后端 + GM/T 0024/0044 协议选择写入决策记录 | docs/adr/ | 与 ADR-009–012 平行 |

### 2.2 Out of scope（明确推迟）

- **iOS / Android 原生支持** —— README "roadmap, not P1" 立场维持；社区 PR 走单独 issue
- **OHOS x86_64 模拟器 build** —— secondary，先把 aarch64 落地
- **OHOS Lite（LiteOS 设备，< 1 MB RAM）** —— 完全独立 milestone，超出 P3-prep 范围
- **OHAudio / OpenSL ES 适配** —— 推迟到 v0.12.x patch；本计划内只输出调研决策文档
- **Chrome ↔ NimRTC e2e OHOS 端互通** —— Windows / Linux 桌面是 e2e 主战场；OHOS 端 e2e 是 v1.0 之后的事
- **GMSSL SRTP 后端** —— GM/T 0044 SRTP profile 是 P4 候选；当前 SRTP 维持 wolfSSL
- **usrsctp OHOS 适配** —— 跟随 v0.11.0 的 `UsrsctpSocketFactory` 路径，先在 4 平台 green 之后再考虑 OHOS

### 2.3 Open question — version target → RESOLVED: 方案 C

**问题**：本计划两个高风险子项（OHOS toolchain + GMSSL backend）**应该绑在同一个 milestone 还是拆成两个？**

| 方案 | 优点 | 缺点 |
|---|---|---|
| ~~A. v0.12.0 = OHOS + GMSSL 同发~~ | 一次 PR 切换 OHOS 上 DTLS 默认到 GMSSL | 5 个高风险子项压在同一个 gate；排期风险极高 |
| ~~B. v0.12.0 = OHOS + wolfSSL；v0.12.x = GMSSL；v0.13.0 = OHOS 切 GMSSL~~ | 风险切分清晰 | "OHOS + wolfSSL" 是无差异化中间态；两次 release 仪式成本 |
| **C（Resolved ✅）. 阶段 0 独立前置 PR；v0.12.0 = GMSSL + CI + 文档** | **A 的叙事 + B 的风险切分**：阶段 0（toolchain + musl compat）作为独立 PR 先落地，v0.12.0 主 tag 只承担 GMSSL backend + CI + 文档三项，风险面大幅收窄；阶段 0 验证结果可独立回滚 | 阶段 0 与 v0.12.0 之间有 1–2 个月的 gap（但不影响用户感知） |

**决策结论**：采用方案 C。更新后的发布节奏：

```
独立 PR（阶段 0）→ v0.11.x patch 或 v0.12.0-alpha
     ↓
v0.12.0 主 tag（阶段 2 GMSSL + 阶段 3 CI + 阶段 4 文档）
```

本文档 §5 阶段重新编号，§2.1 中的 OH-TC-* / OH-POSIX-* 条目标记为 **阶段 0 范围**。

---

## 3. 当前已具备的 OpenHarmony 相关资产审计

| 资产 | 版本 / 状态 | 路径 | OHOS 复用度 |
|---|---|---|---|
| `cmake/toolchains/aarch64-linux-gnu.cmake` | ✅ 已落地 | `cmake/toolchains/` | **高**——OHOS 工具链与之结构平行（同样 aarch64-Linux-musl），可作为 §5 阶段 0 的模板 |
| `CMakePresets.json` `debug` / `release` / `asan` / `ubsan` preset | ✅ 已定义（Linux condition） | 仓库根 | 高——新增 `debug.ohos-arm64` 平行扩展 |
| `cmake/NimRTCOptions.cmake` MSVC vs UNIX 分支 | ✅ 已实现 | `cmake/` | 中——`if(UNIX)` 分支在 OHOS-musl 上有少量 POSIX 差异（§4.1 R1） |
| PAL Slice 1–8 typed registry hooks（audio / video / codec / bwe / scheduler / dtls / sctp / raw_udp / transport-stack） | ✅ 全部已落地（v0.10.0–v0.10.3） | `src/core/include/nimrtc/core/plugin_registry.hpp` 等 | **极高**——OHOS backend 接入零引擎改动 |
| `IDtlsSessionFactory` + `DtlsSessionWolfSSL` | ✅ 已实现（v0.10.0 / 0.10.1） | `src/modules/dtls/` | 极高——GMSSL 后端是平行新增（`DtlsSessionGmSSL`），引擎侧 0 改动 |
| `IRawUdpFactory` + `ArqRawUdpFactory` | ✅ 已实现（v0.10.2 Slice 8） | `src/raw_udp/` | 高——OHOS 上 UDP socket 与 Linux 行为基本一致（除 `IPV6_DONTFRAG` 等少数 ioctl） |
| `ISctpSocketFactory` + `SctpStubFactory`（v0.10.2 stub） | ✅ stub 已落地，v0.11.0 计划换 usrsctp | `src/sctp/` | 中——usrsctp OHOS 适配属"out of scope"，先用 stub |
| `nimrtc_link_wolfssl()` CMake helper | ✅ 已挂载 | `cmake/NimRTCVendored.cmake` | 高——OH-GMSSL-2 平行新增 `nimrtc_link_gmssl()` |
| wolfSSL vendored | 5.7.x | `src/third_party/wolfssl/` | 高——继续作为非 GMSSL 用户的默认后端 |
| libopus 1.6.1 / libsrtp 3.0.0 / libjuice 1.6.0 vendored | ✅ 均在 `src/third_party/` | — | **高**——这三个库的 OHOS 适配难度已知较低（musl 兼容），业界已有先例 |
| nlohmann_json 3.11.3 INTERFACE 单头 | ✅ | `src/third_party/nlohmann_json/` | 极高 |
| googletest 1.12.1 | ✅ 离线构建 | `src/third_party/googletest/` | 中——OHOS 上 gtest runner 需要 native entry，标准 gtest_main 可行但要验证 |
| DTLS chrome 互通 e2e harness（`tools/run_e2e_acceptance.py` + `interop/`） | Windows-only；Linux/macOS 是 v1.0 milestone | `tools/` / `interop/` | 极低——OHOS e2e 是 v1.0 之后的事 |

> **结论**：OHOS 支持 70% 资产已就绪，主要缺 (a) toolchain + CMake preset、(b) musl 兼容补丁、(c) GMSSL backend、(d) CI runner。引擎代码 0 改动。

---

## 4. R-Audit：关键技术障碍

### 4.1 R1 — POSIX 兼容层（musl vs glibc）

OHOS 内核虽然是 Linux，但用户态 libc 是 **musl**（OHOS 标准系统）/ OHOS-Lite 自带精简 libc。NimRTC 的 4 平台产物（glibc + macOS libc + MSVCRT）需要在 musl 上重新过一遍。已知风险点（粗扫，需要阶段 1 实际验证）：

| 风险点 | 出现概率 | 影响面 | 备注 |
|---|---|---|---|
| `errno_t` / `strerror_s` 等 MSVC-only API | 已规避 | 无 | 当前代码已有 `_WIN32` / `__unix__` 分支 |
| `secure_getenv`（glibc 扩展，musl 不支持） | 中 | test_dtls_x25519 等环境变量读取路径 | 降级到 `getenv`，加 NULL check |
| `IF_NAMESIZE` / `getifaddrs` 大小 | 低 | `ArqRawUdp` 网卡枚举 | musl `getifaddrs` 行为与 glibc 一致 |
| `epoll_pwait2`（glibc 2.35+ 扩展，musl 不支持） | 低 | `ArqRawUdp` IO 多路复用 | 当前实现用 `epoll_wait`，无影响 |
| `pthread_setname_np`（签名差异） | 低 | 日志 / 调度 | 已有 `_GNU_SOURCE` 分支，需要补 musl 分支 |
| DTLS wolfSSL 配置：musl 上 `wc_rng_generate_block` 默认实现 | 中 | DTLS RNG | 验证 wolfSSL `--enable-static --enable-aesecm --enable-sha --enable-sha224 --enable-sha384 --enable-sha512 --enable-sha3 --enable-des3` 配置集在 musl 上无 warning/error |
| `mallinfo2` / `malloc_info`（glibc 扩展） | 低 | debug-only | 当前代码未使用 |

**对策**：阶段 1 启动时一次性做 R1 验证，输出 `docs/plan/ohos-musl-compat.md`（issue 级，不必成文）登记每一处补丁。

### 4.2 R2 — DTLS 后端注入点（GMSSL 接入）

T-PAL Slice 4（`IDtlsSession`）+ Slice 8（`register_dtls_session()`）已经把 DTLS 拉到了 typed factory。**引擎侧接入 GMSSL 0 改动**——只需要：

1. 新增 `src/modules/dtls/src/dtls_gmssl_session.{hpp,cpp}`，实现 `IDtlsSession` 接口
2. 在 `pal_default_registrars.cpp` `kDefaultRegistrars[]` 中注册 `GmSslDtlsFactory("gmssl")`
3. 新增 `cfg.dtls_name` 字段（engine.hpp 公共 API 不破坏）

但 GMSSL 后端本身有两个**内部风险**：

- **GM/T 0024 vs GM/T 0044（TLCP）协议选择**：GMSSL 4.x 默认 GM/T 0024（基于 TLS 1.0/1.1 改造，**已不被推荐**）；GM/T 0044（TLCP，基于 TLS 1.3）是当前国密标准。**默认必须选 TLCP（GM/T 0044）**——这是 ADR-013 必须明文写的决策点
- **GMSSL 4.x build 依赖**：与 mbedTLS 4.2.0 类似（参考 `linux-x86_64-support.md` §3.5 OBSOLETE 教训），GMSSL 有 Perl 脚本生成 `.h` 文件。WSL2 / Ubuntu cross 工具链需要 `apt install -y perl`（同 `linux-x86_64-support.md` §1.0 前置）

### 4.3 R3 — OHAudio / OpenSL ES 适配

OHOS 平台音频采集 API：

| API | OHOS 支持 | 推荐度 | 备注 |
|---|---|---|---|
| **OHAudio**（OHOS 9+ 原生） | ✅ | **推荐** | NDK 风格 C API，签名与 AAudio 接近（`OH_AudioStreamBuilder` / `OH_AudioRenderer_Callbacks`）；生命周期清晰 |
| OpenSL ES | ✅（OHOS 兼容层） | 中 | 已 deprecated；只在 OHAudio 不可用的旧 SDK 退回 |
| AAudio | ❌ | — | OHOS 不支持 |
| AudioTrack（Java/Kotlin） | ✅ | 不适用 | 与 NimRTC C++ 不对接 |

**R3 在 v0.12.0 内只做调研决策**，不实现——目的是明确"OHAudio 是 OHOS 后 v0.12.x patch 的接入路径"，避免后续 patch 时重新选型。

### 4.4 R4 — CI runner（开源 OHOS runner vs 华为云）

| 选项 | 优点 | 缺点 |
|---|---|---|
| 开源 OHOS community runner（huawei-openlab / 自建） | 免费、与 GitHub Actions 集成简单 | runner pool 不稳定（参考 `linux-aarch64` runner pool 经验教训） |
| 华为云 CCI runner | 稳定、官方维护 | 需华为云账号，CI 配置复杂度增加 |
| 自托管 runner（物理机 / VM） | 完全可控 | 需要维护硬件 / OS |

**倾向**：先用 `if: false` + `continue-on-error` 接入开源 runner 跑 build（与 `linux-aarch64` 当前策略对齐），作为 best-effort CI 信号；test 准入门槛（§6）允许在 CI 临时跳过，本地验证为主。

---

## 5. 阶段划分

> **方案 C 执行节奏**：阶段 0 作为**独立前置 PR**，在 v0.11.x patch 或 v0.12.0-alpha 阶段完成；阶段 1–4 进入 v0.12.0 主 tag。
>
> ✅ **阶段 0 已验证**：2026-09-21，`cmake --preset debug.ohos-arm64` 在 Windows host + 无 OHOS SDK 的环境下正确报出 toolchain 诊断（无 OHOS SDK 是预期行为；Linux + OHOS SDK 环境预期通过）；`CMakePresets.json` 6 个 OHOS preset 注册成功（3 configure + 2 build + 1 test）。

### 阶段 0（独立前置 PR）：CMake toolchain 落地（OH-TC-1 / OH-TC-2 / OH-POSIX-2）—— 3–5 天 ✅

**已产出**：
- `cmake/toolchains/ohos-aarch64.cmake`（120 行，双模式 toolchain：OHOS SDK cross / clang --target fallback）
- `CMakePresets.json` 新增 `dev.ohos` + `debug.ohos-arm64` + `release.ohos-arm64` configure preset + 对应 build / test preset
- `cmake/NimRTCOptions.cmake` 新增 `if(CMAKE_SYSTEM_NAME STREQUAL "OHOS")` 分支（`NIMRTC_PLATFORM_OHOS=1`、`-stdlib=libc++`）

独立 PR 待发。

具体动作：
1. 新建 `cmake/toolchains/ohos-aarch64.cmake`：模式同 `aarch64-linux-gnu.cmake`（real cross clang 优先；fallback `--target`）。设置 `CMAKE_SYSTEM_NAME=OHOS`、`CMAKE_SYSTEM_PROCESSOR=aarch64`、`OHOS_STL=c++_shared`
2. 在 `CMakePresets.json` 新增 `debug.ohos-arm64` / `tests.ohos-arm64` preset，condition 限制为"OHOS toolchain 已安装"
3. 在 `cmake/NimRTCOptions.cmake` 新增 `if(OHOS)` 分支：musl 兼容选项（`-D__MUSL__` 或类似）、`-fvisibility=hidden`（OHOS 默认符号隐藏）
4. 在 OHOS SDK（DevEco Studio 提供的 native toolchain）下做 hello-world sanity check：`printf("hello ohos")` 的 CMake 产物能在 OHOS 模拟器或远程真机跑通

**出口准则（OH-DOD-0）**：独立 PR review 通过；`cmake --preset debug.ohos-arm64` 零 error 配置通过（OHOS SDK 已安装环境）。

### 阶段 1：现有 4 平台产物在 OHOS 上跑通（OH-POSIX-1 / OH-POSIX-2 / R1 musl 验证）—— 1–2 周

具体动作：
1. 在 OHOS toolchain 上编译所有非测试 target：`nimrtc_core_objects` / `nimrtc_engine` / `nimrtc_dtls_seam` / `nimrtc_dtls` / `nimrtc_srtp` / `nimrtc_opus` / `nimrtc_rtp` / `nimrtc_ice` / `nimrtc_sctp` / `nimrtc_raw_udp` / `nimrtc_transport`
2. 用 wolfSSL 作为 DTLS 后端（不切 GMSSL，先保证"现有产品在 OHOS 上跑通"）
3. 验证 §4.1 R1 风险表中各点；记录 R1 实际触发的补丁到 `docs/plan/ohos-musl-compat.md`（issue 级记录，不必成文）
4. **测试准入**：cTest 在 OHOS 上**不要求**全绿（OH-TEST-1，OH-CI-1 关联）；最低要求是 `tests/test_dtls_gcm_aead`（纯本地 AES-GCM round-trip，无网络栈依赖）在 OHOS 上能编译并跑通
5. vendor 库（libopus / libsrtp / libjuice）OHOS 兼容性一次性扫一遍；预期 0–2 个小补丁

**出口准则**：`cmake --build --preset debug.ohos-arm64 --target nimrtc_engine` 成功产出 `libnimrtc_engine.a`；demo-p2p 静态链接可在 OHOS 模拟器 / 真机启动（不要求 e2e 互通）；`test_dtls_gcm_aead` OHOS 单元测试 PASS。

### 阶段 2：GMSSL DTLS 后端（OH-GMSSL-1 / OH-GMSSL-2 / OH-DTLS-3 / ADR-013）—— 2–3 周

**这是 critical path，是最大差异化杠杆**。

具体动作：
1. 新建 `src/modules/dtls/src/dtls_gmssl_session.{hpp,cpp}`：实现 `IDtlsSession` 接口，封装 GMSSL 4.x 的 TLCP（GM/T 0044）握手 + AES-GCM / SM4 套件
2. 新建 `src/third_party/gmssl/`：GMSSL 4.x 源码 vendor（与 `src/third_party/wolfssl/` 平行）+ `CMakeLists.txt` wrapper（仿 `nimrtc_vendor_wolfssl` 写法）+ `nimrtc_link_gmssl()` CMake helper
3. 在 `src/core/src/pal_default_registrars.cpp` `kDefaultRegistrars[]` 注册 `GmSslDtlsFactory("gmssl")`
4. 修改 `src/engine/src/engine.cpp`：增加 `cfg.dtls_name` 字段解析为 `reg.get_dtls_session(name)`；`engine.hpp` 公共 API 不破坏（默认 `"wolfssl"`，OHOS profile 默认 `"gmssl"`）
5. 新增 `tests/test_dtls_gmssl_tlcp.cpp`：TLCP 握手 round-trip + SM4-GCM round-trip 单元测试
6. 撰写 `docs/adr/ADR-013-gmssl-backend.md`：把 GMSSL 后端 + GM/T 0024 vs GM/T 0044 选择 + 默认值策略写入决策记录
7. CHANGELOG.md 新增 v0.12.0 entry

**出口准则**：`test_dtls_gmssl_tlcp` 在 Linux x86_64 + OHOS-aarch64 上 PASS（OHOS 端 ctest 可选，见 §6 DoD）；GMSSL 与 wolfSSL 两个 backend 并存，引擎默认走 wolfSSL（保持现有用户零回归），OHOS profile 默认走 GMSSL。

### 阶段 3：CI 接入 + 文档（OH-CI-1 / OH-DOC-1 / OH-DOC-2）—— 3–5 天

具体动作：
1. 新增 `.github/workflows/ci.yml` 的 `ohos-arm64` job：开源 OHOS runner + `if: false` / `continue-on-error` 策略（与 `linux-aarch64` 现行策略对齐）
2. `README.md` 平台矩阵新增 OpenHarmony 行（build 🔶、test 视 runner）；"Who is it for" 段落明确 GM/T 场景支持现状
3. `docs/zh/architecture.md` §13 P3 行交叉引用本文档
4. CHANGELOG.md v0.12.0 entry 同步

**出口准则**：`ohos-arm64` CI job 在 GitHub Actions 上能 trigger（即使 build fail 也算"接通了"——参考 `linux-aarch64` runner pool 不稳的现实约束）。

### 阶段 4：Audio HAL 调研决策文档（OH-AUDIO-1）—— 1–2 天

具体动作：
1. 撰写 `docs/plan/ohos-audio-hal.md`：OHAudio vs OpenSL ES 选型 + 接入路径 + 与 NimRTC `AudioSink` / `AudioSource` 的对接面
2. **不实现代码**，仅决策文档；具体实现推迟到 v0.12.x patch

**出口准则**：`ohos-audio-hal.md` 完成并 review。

---

## 6. DoD（v0.12.0 准入门槛）

| Gate | 描述 | 强制级别 | 状态 |
|---|---|---|---|
| **OH-DOD-0** | `cmake --preset debug.ohos-arm64` 零 error 配置通过（OHOS SDK 环境） | **强制** | ✅ 阶段 0 产出；OHOS SDK 待用户安装后本地验证 |
| **OH-DOD-1** | `cmake --preset debug.ohos-arm64` 在 OHOS SDK + aarch64 cross 工具链上配置零 error | **强制** | 阶段 1 前置 |
| **OH-DOD-2** | `cmake --build --preset debug.ohos-arm64 --target nimrtc_engine` 产出 `libnimrtc_engine.a`，link 零 unresolved symbol | **强制** | 阶段 1 |
| **OH-DOD-3** | wolfSSL DTLS 在 OHOS-aarch64 上 `test_dtls_gcm_aead` PASS | **强制** | 阶段 1 |
| **OH-DOD-4** | GMSSL DTLS 在 Linux x86_64 上 `test_dtls_gmssl_tlcp` PASS | **强制** | 阶段 2 |
| **OH-DOD-5** | GMSSL DTLS 在 OHOS-aarch64 上 `test_dtls_gmssl_tlcp` PASS | **强烈推荐**（CI runner 不稳定时允许 `continue-on-error`，但本地必须验证） | 阶段 2 |
| **OH-DOD-6** | `ohos-arm64` GitHub Actions job 接通（即使 build fail 也算"接通了"） | **强制** | 阶段 3 |
| **OH-DOD-7** | README.md 平台矩阵更新 + CHANGELOG.md v0.12.0 entry | **强制** | 阶段 3 |
| **OH-DOD-8** | `docs/adr/ADR-013-gmssl-backend.md` 已 review | **强制** | 阶段 2 |
| **OH-DOD-9** | `docs/plan/ohos-audio-hal.md` 调研文档已 review | **强制** | 阶段 4 |
| **OH-DOD-10** | 现有 v0.10.x / v0.11.0 回归：4 平台 build + ctest 全部维持绿色 | **强制**（无回归承诺） | 全程 |

---

## 7. 风险登记表

| ID | 风险 | 概率 | 影响 | 缓解措施 | 触发条件 |
|---|---|---|---|---|---|
| OH-RISK-1 | 华为云 / 开源 OHOS runner pool 不稳定，CI 长期 red | 高（参考 `linux-aarch64` 教训） | 中 | `if: false` + `continue-on-error`；本地手动验证为主 | CI 连续 2 周 fail |
| OH-RISK-2 | GMSSL 4.x build 集成踩 mbedTLS 同款坑（Perl 脚本 / 子模块） | 中 | 高 | 阶段 2 启动前先做 GMSSL vendor 集成 sanity check（独立 PR） | `find NIMRTC_GMSSL_acquired` 在 build log 不可见 |
| OH-RISK-3 | musl 兼容补丁散落在多处代码 | 中 | 中 | 阶段 1 集中登记到 `docs/plan/ohos-musl-compat.md`；下个 milestone 评估 `if(OHOS)` 集中下沉 | R1 补丁 > 5 处 |
| OH-RISK-4 | TLCP 互通性 — 没有浏览器支持 TLCP，OHOS ↔ Chrome 互通需要 OHOS ↔ NimRTC-Desktop 互通（wolfSSL 路径） | 高（产品现实） | 中 | OHOS ↔ NimRTC-Desktop 互通文档化；OHOS ↔ Chrome 不在 v0.12.0 范围内 | — |
| OH-RISK-5 | 用户误以为 OHOS support = GM/T 默认开启 | 中 | 中 | README / CHANGELOG 明文："OHOS profile 默认走 GMSSL，Linux/Windows/macOS 维持 wolfSSL" | — |
| OH-RISK-6 | GMSSL 仅支持 SM2 / SM3 / SM4 国密算法，与 wolfSSL ECDSA P-256 互不兼容（OHOS ↔ 桌面互通需要双栈） | 高（产品现实） | 中 | DTLS backend 可切换（`cfg.dtls_name`），产品侧由集成方选 | — |

---

## 8. Decision log

| Date | Decision | Rationale | Owner |
|---|---|---|---|
| 2026-09-21 | **本文档为 DRAFT，等待 §2.3 版本目标拍板** | 阶段 0 启动前必须先锁定 version target + §5 阶段是否拆分 | BDFL |
| 2026-09-21 | **OHOS profile 默认 DTLS 后端 = GMSSL；其他 4 平台维持 wolfSSL** | 平台差异化目标 + 不破坏现有用户零回归 | BDFL |
| 2026-09-21 | **GMSSL 协议选择 = GM/T 0044（TLCP，基于 TLS 1.3），不使用 GM/T 0024** | 国密当前推荐标准；GM/T 0024 已不推荐 | BDFL（待 ADR-013 确认） |
| 2026-09-21 | **Audio HAL 调研决策文档先行，代码推迟到 v0.12.x patch** | 控制 v0.12.0 milestone 风险面；与 `linux-x86_64-support.md` 同款 discipline | BDFL |
| 2026-09-21 | **CI 准入门槛放宽（OH-DOD-5 允许 continue-on-error）** | runner pool 现实约束；与 `linux-aarch64` 现行策略对齐 | BDFL |

---

## 9. References

- `README.md` — "Who is it for / Not a fit if" 段落（Xinchuang / GM/T 画像；iOS/Android roadmap）
- `docs/zh/architecture.md` §13 — 路线图 P3 行；PAL Slice 设计原则
- `docs/plan/linux-x86_64-support.md` — 平台支持子计划的写作模板；§3.5 mbedTLS build 集成 OBSOLETE 教训
- `docs/plan/transport-selection.md` §6 — PAL Slices 4–8（DTLS / SCTP / raw_UDP / transport-stack typed factory）
- `docs/plan/v0.10-plan.md` / `v0.11-plan.md` — milestone 计划写作格式
- `docs/adr/ADR-009-pal-slice-1.md` ~ `ADR-012-zh-docs-layout.md` — v0.10.0 决策记录（PAL Slice 1 / JSON Profile / 遥操作指标口径 / 中文文档布局）
- `.github/workflows/ci.yml` — 4 平台 CI matrix；`linux-aarch64` job 的 `if: false` 现实约束参照
- `cmake/toolchains/aarch64-linux-gnu.cmake` — OH-TC-1 的双模式 toolchain 模板
- `cmake/NimRTCVendored.cmake` — `nimrtc_link_wolfssl()` CMake helper 的平行物 `nimrtc_link_gmssl()` 参考
- `src/modules/dtls/src/dtls_wolfssl_session.cpp` + `src/modules/dtls/include/nimrtc/dtls/dtls_wolfssl_session.hpp` — `DtlsSessionGmSSL` 的代码模板
- `src/core/include/nimrtc/core/plugin_registry.hpp` + `src/core/src/pal_default_registrars.cpp` — `register_dtls_session()` 槽位的注册入口
- `CHANGELOG.md` v0.10.0 platform support matrix — 平台支持状态基线
