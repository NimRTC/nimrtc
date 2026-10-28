# AGENTS.md — nimrtc-zelto

## 项目是什么

NimRTC 是一个 **原生 C++20 WebRTC 替代实现**，面向 AI Agent 和嵌入式场景。
不是 libwebrtc 的 fork——从头实现的模块化引擎，支持可插拔后端（DTLS、RTP、3A 编解码器）。

## 硬约束（改任何代码前先读，改完自查）

**L1** `src/core/` 是唯一的 INTERFACE 库，只暴露稳定类型，保持 header-only
**L2** `src/plugins/` 只包含抽象接口和工厂接口，不包含任何实现
**L3** 每个模块自包含于 `src/modules/<name>/`，有独立的 CMakeLists.txt + include/ + src/ + tests/
**L4** `src/engine/` 只做插件编排，不直接包含任何具体模块头文件
**L5** `src/third_party/` 是 vendor 代码的唯一存放位置，`NIMRTC_VENDORED` 必须保持 ON
**L6** 任何模块不得导出全局可变状态——单例（Logger、PluginRegistry）只能存在于 `nimrtc::core::`
**L7** 保护域不可被策略规则覆盖：`policy/`、`.zelto/`、`AGENTS.md`

## 技术栈

- C++20，CMake ≥ 3.20
- 构建：Visual Studio 2022 (Windows) / GCC (Linux/macOS)
- 测试：Google Test（vendored under `src/third_party/googletest/`）
- 格式化：`.clang-format`（LLVM style, 100-col limit）
- 三方依赖全部 vendored（mbedTLS, libsrtp, libopus, libjuice, nlohmann_json）

## 目录

```
nimrtc-zelto/
├── src/
│   ├── core/          header-only 基础类型
│   ├── plugins/       header-only 插件接口
│   ├── modules/       具体实现（rtp/sdp/jb/dtls/srtp/ice/opus/…）
│   ├── engine/        引擎编排
│   ├── log/           日志实现
│   └── third_party/   vendored 依赖
├── tests/             跨模块/集成测试
├── cmake/             CMake 辅助
├── specs/slices/      zelto 切片规格
├── policy/            策略规则
└── .zelto/            zelto 运行时状态
```

## 提交规范

- Conventional Commits（feat/fix/docs/perf/chore/refactor/test）
- DCO 签名（`Signed-off-by:`）
- 提交信息用英文

## 禁区

- 不许改 `src/core/` 为非 header-only
- 不许在 `src/plugins/` 中添加实现代码
- 不许绕过 vendored 依赖使用系统安装的 crypto/DTLS/SRTP 库
- 不许修改 `tests/acceptance/**`（除非对应切片规格一起改）
- 不许引入全局可变状态

## 遇到不确定时

停下来问，不要自己选。规格里没写的行为 → 问。
