# Slice: v1-cmake

## 目的
配置和优化 CMake 构建系统，确保 Windows (VS 2022) 和 Linux/macOS (GCC) 双平台可构建。

## 范围
- `CMakeLists.txt` — 根构建文件
- `cmake/**` — CMake 辅助模块
- `CMakePresets.json` — 构建预设

## 验收标准
1. `cmake --build build --config Debug` 成功
2. `ctest --test-dir build --output-on-failure -C Debug` 全部通过
3. 至少 Debug 和 Release 两个预设可用

## 风险
- 三方依赖（mbedTLS, libsrtp 等）需预先编译
