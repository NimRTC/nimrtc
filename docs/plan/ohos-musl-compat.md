# OpenHarmony musl 兼容性审计（R-Audit R1 静态分析）

> 文档类型：issue 级记录（阶段 1 出口准则 §5 Plan 要求）
>
> **结论**：通过静态分析，NimRTC 源码在 musl libc 上**零需要强制改动**。
> 所有 R1 风险点均已通过现有平台抽象规避，或位于第三方 vendor 代码（不动）。
> 实际 OHOS-aarch64 交叉编译验证（OH-DOD-1）需在有 OHOS SDK 的 Linux 机器上完成。

关联：`docs/plan/openharmony-support.md` §4.1 R-Audit R1

---

## R1 风险表（plan §4.1 逐项核对）

| 风险点 | 计划预期 | 静态分析结果 | 状态 |
|---|---|---|---|
| `secure_getenv`（glibc 扩展） | 中概率，需降级到 `getenv` | **0 匹配** — 全部 `getenv` 调用均为标准 POSIX `std::getenv()`，加 NULL check | ✅ 无需改动 |
| `IF_NAMESIZE` / `getifaddrs` 大小差异 | 低概率 | **在 vendor 代码中**：libjuice `src/udp.c:539`、usrsctp `user_recv_thread.c:105`、`sctp_bsd_addr.c:424`。musl `getifaddrs` 与 glibc 接口一致，不会触发此问题 | ✅ 无需改动 |
| `epoll_pwait2`（glibc 2.35+） | 低概率 | **0 匹配** — 代码只用 `epoll_wait`，无影响 | ✅ 无需改动 |
| `pthread_setname_np` 签名差异 | 低概率 | **在 vendor 代码中**：`usrsctplib/netinet/sctp_userspace.c:87`。这是 usrsctp 内部调用，已通过 `_GNU_SOURCE` 分支处理 | ⚠️ vendor，不动 |
| DTLS wolfSSL musl 上 RNG | 中概率 | wolfSSL `wc_rng_generate_block` 默认实现无需 musl 特定适配；需 OHOS 实际运行验证（OH-DOD-3） | ⏳ 待 OH-DOD-3 验证 |
| `mallinfo2` / `malloc_info` | 低概率 | **0 匹配** | ✅ 无需改动 |

---

## 平台抽象层分析

### `src/core/include/nimrtc/core/time.hpp`

- 纯 C++ `std::chrono`（`SteadyClock`、`SystemClock`）
- 无 POSIX 时间 API 调用
- **musl 兼容**：✅

### `src/core/include/nimrtc/core/log.hpp`

- 纯 C++ 标准库（`std::ostringstream`、`std::shared_ptr`）
- 无平台特定 API
- **musl 兼容**：✅

### `src/modules/dtls/src/dtls_wolfssl_session.cpp`

- `std::getenv("NIMRTC_DTLS_TRACE")`、`getenv("NIMRTC_DTLS_KEYLOG")`、`getenv("NIMRTC_DTLS_DUMP")`
- 标准 POSIX `getenv`，加 NULL 检查
- `#if defined(__GNUC__) || defined(__clang__)` — OHOS clang 会命中
- `#ifndef _WIN32` — OHOS 走 Unix 分支
- **musl 兼容**：✅

### `src/raw_udp/src/udp_socket.cpp`

- 全量 `#ifdef _WIN32` 分支
- Unix 分支走标准 POSIX socket API
- **musl 兼容**：✅

### `src/modules/sched/`、`src/modules/bwe/` 等

- 标准 C++，无平台特定代码
- **musl 兼容**：✅

---

## 第三方 vendor 库说明

以下库位于 `src/third_party/`，已确认 musl 兼容：

| 库 | OHOS musl 兼容性 | 备注 |
|---|---|---|
| libopus 1.6.1 | ✅ 已知 musl 兼容 | 业界已在 musl 设备上使用 |
| libsrtp 3.0.0 | ✅ 已知 musl 兼容 | 纯 C，无 syscall 依赖 |
| libjuice 1.6.0 | ⚠️ 需观察 | `getifaddrs()` 在 musl 上与 glibc 行为一致；`NO_IFADDRS` 宏在 OHOS 上应不存在 |
| wolfSSL 5.7.x | ✅ 已知 musl 兼容 | wolfSSL 在 musl 上已有生产部署 |
| usrsctp | ✅ 已知 musl 兼容 | 用户空间 SCTP，无内核依赖 |

---

## 待 OH-DOD-1 / OH-DOD-3 验证项

1. **OH-DOD-1**：实际交叉编译是否成功（`--preset debug.ohos-arm64 --target nimrtc_engine`）
2. **OH-DOD-3**：`test_dtls_gcm_aead` OHOS 单元测试 PASS
3. **vendor 实际链接**：libjuice `getifaddrs` 在 OHOS 真机上是否有任何运行时问题

以上三项均需 OHOS SDK 环境，本文档为静态分析，不做运行时结论。

---

## 改动记录

| 日期 | 改动 | 原因 | 验证方式 |
|---|---|---|---|
| 2026-09-21 | 无强制改动 | 静态分析通过 | 代码扫描 |

---

*本文档为阶段 1 出口准则 DOH-DOD-1 前置记录，不替代 OHOS SDK 实际交叉编译验证。*
