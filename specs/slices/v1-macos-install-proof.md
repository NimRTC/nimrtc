# Slice: v1-macos-install-proof

## Summary

**macOS 那条腿也要证明「装得上、找得到、下游能编译」。** Windows（MSVC）与 Linux 的 CI 已经在每次 push 上做「安装到暂存目录 → 用 `find_package(NimRTC)` 编译一个最小消费者 → 跑起来」的**下游证明**，但 `macos-clang` 那条腿只做**树内**编译与测试 —— **没有** install + downstream proof。

结果是 macOS 上的导出路径（`NimRTCConfig.cmake` 的生成、`CMAKE_INSTALL_*` 的路径、`nimrtc::engine` 的 imported target 定义）**没有任何自动化的证据**。而 macOS 这条腿在 v1.4.0 身上背着**三个**自己的说法：D2 的 Metal 渲染器、D3 的默认设备表、D4 的 AVFoundation 摄像头 —— 这些都只在 macOS 上跑过。

N9 做两件事：

1. **给 `macos-clang` 加 install + downstream proof**（与 Windows / Linux 同形）。
2. **修掉这个证明会揭出来的导出缺陷**。

发布包格式按计划 §5 **N9-1** 的已决：**纯归档**（`.tar.xz` / `.zip`）—— 包格式是**分发**的决定，不是 SDK 的决定。CI 证明 `find_package` 两种都行。

架构决策在 `docs/adr/ADR-034-macos-install-downstream-proof.md`（**Proposed**，待签字）。

## 目标

**让 macOS 的导出路径有自动化的证据，而不是靠「Windows 上能跑，macOS 应该也行」。**

具体地：一条 `macos-clang` 的 CI 步骤，做「安装到 `build/_install/macos-arm64` → 找到 `NimRTCConfig.cmake` → 用它编译一个只依赖已安装树的最小消费者 → 跑起来 → 打印版本号」。它**必须**在**树外**（out-of-tree）进行，且**不得**引用源码树或构建树的路径 —— 这正是「宿主项目」的样子。

## 现状核实（写规格时的树状态）

- **Windows 的证明已在**：`ci.yml` 的 `windows-msvc` 腿有 `Install + downstream proof` 步骤（`ci.yml:189`）—— 用 `pwsh`，安装到 `build/_install/windows-x86_64`，`find_package(NimRTC 1.0 REQUIRED)`，链接 `nimrtc::engine`，跑 `downstream_proof.exe`。它的注释里说清了**为什么 Windows 那条腿不可省**：「GCC 11+ 默认 gnu++17，够公开头用；MSVC 默认 C++14，需要包配置声明标准」。
- **Linux 的证明已在**：`ci.yml` 的 Linux 腿（`ci.yml:864`）同形，用 `sh`，`CMAKE_PREFIX_PATH` 指向暂存目录。
- **macOS 那条腿没有**：`ci.yml` 的 `macos-clang` 腿（`ci.yml:1391`）的步骤是 —— checkout、vendor 校验、API freeze 三道门、unused-const 自检与门、装 Python、pre-flight、缓存、provision TLS、Configure、Build、Test、失败时上传构建目录。**没有** install + downstream proof。
- **macOS 的发布包已经存在**：`release.yml` 的 `build-macos-clang` 腿（`release.yml:731`）会 `cmake --install build --prefix build/_install/macos-arm64`，然后 `tar -C "$staging" -cJf ...tar.xz .` —— **发布包本身是对的**。缺的只是**自动化地证明「从那个包里能找到 NimRTC 并编译」**。
- **macOS 的 runner 是 arm64**：`macos-15`（`release.yml` 的注释解释了 `macos-14` 在 2026-11-02 被退役）。

### 与其它片的边界（本片**不做**）

- **不做** N6 的 SDK 样例 —— 那是波次 6，依赖本片与其它所有片先落地。本片的消费者是**最小**的（一个 `main()` 打印版本号）。
- **不做** 包格式的选择 —— 计划 §5 **N9-1** 已决：**纯归档**。本片不引入 `.pkg` / `.deb` / Homebrew formula。
- **不做** 改 Windows / Linux 的证明 —— 它们已经有了。
- **不做** 改 `src/` 的 C++ —— 本片只碰 CI 与导出配置（若证明揭出导出缺陷，改的是 CMake 的安装/导出规则）。

## 范围

- .github/workflows/ci.yml
- .github/workflows/release.yml
- cmake/NimRTCConfig.cmake.in
- cmake/NimRTCInstall.cmake
- tools/check_install_export.py
- tools/check_install_export_self_test.py
- tests/acceptance/v1-macos-install-proof/acceptance.test.cjs
- docs/adr/ADR-034-macos-install-downstream-proof.md
- docs/plan/v1.4-plan.md
- CHANGELOG.md
- specs/slices/v1-macos-install-proof.md

> 只读 `src/` —— 本片**不改**任何 C++ 源码。
> 只读 `CMakeLists.txt` —— 顶层构建脚本**不改**；导出规则在 `cmake/NimRTCInstall.cmake`（见下）。

## 参考

以下是本片**只读**的路径 —— agent 读它们、仿照它们，但**不改**它们：

- 只读 `NimRTCConfig.cmake` —— 安装**之后**才存在的产物（模板是 `cmake/NimRTCConfig.cmake.in`）。
- 只读 `plugins/ice_transport.hpp` —— 引擎头的依赖面举例（证明消费者的 `#include` 会暴露它）。
- 只读 `nimrtc/engine/engine.hpp` —— 消费者要包含的引擎公开头。
- 只读 `scripts/api_freeze_scan.js` —— CI 已有的门，本片跑它、不改它。
- 只读 `scripts/check_api_manifest.py` —— CI 已有的门，本片跑它、不改它。
- 只读 `build/downstream-proof/main.cpp` —— CI 步骤**生成**的消费者源文件（不在仓库里）。
- 只读 `build/downstream-proof/CMakeLists.txt` —— CI 步骤**生成**的消费者构建脚本（不在仓库里）。
- 只读 `main.cpp` —— 同上（CI 步骤生成，消费者源文件）。
- 只读 `check_readme_release.py` —— 既有的自检套路先例（`ci.yml` 里已有 `--self-test`）。
- 只读 `check_release_tag_filter.py` —— 同上（`ci.yml` 里已有 `--self-test`）。

## 设计

### 一、`macos-clang` 加下游证明

**位置**：`ci.yml` 的 `macos-clang` 腿，在 `Test` 步骤**之后**、失败时上传构建目录**之前**。

**形状**（与 Linux 那条腿同形，macOS 的 shell 是 bash）：

```yaml
      # Track D downstream proof (macOS / Apple Clang): install into a
      # staging dir and build a minimal find_package(NimRTC) consumer
      # against it.  Without this leg the macOS export path has **no**
      # automated evidence: Windows (MSVC) and Linux prove theirs on
      # every push, and macOS only ever built in-tree.  A macOS-only
      # export defect (a wrong CMAKE_INSTALL_* path, a config template
      # that assumes GNU, a target that forgets its INTERFACE_* props)
      # would ship green until a host tried to consume the SDK on a
      # Mac — the one platform where v1.4.0's own claims (D2's Metal
      # renderer, D3's default table, D4's AVFoundation camera) live.
      - name: Install + downstream proof
        shell: bash
        run: |
          cmake --install build --prefix build/_install/macos-arm64
          found="$(find build/_install/macos-arm64 -name NimRTCConfig.cmake -print -quit)"
          test -n "$found" && test -f "$found"
          mkdir -p build/downstream-proof
          cat > build/downstream-proof/main.cpp <<'EOF'
          #include <nimrtc/engine/engine.hpp>
          #include <cstdio>
          int main() {
            std::printf("nimrtc %s\n", NIMRTC_API_VERSION_STRING);
            return 0;
          }
          EOF
          cat > build/downstream-proof/CMakeLists.txt <<'EOF'
          cmake_minimum_required(VERSION 3.20)
          project(nimrtc_downstream_proof LANGUAGES CXX)
          find_package(NimRTC 1.0 REQUIRED)
          add_executable(downstream_proof main.cpp)
          target_link_libraries(downstream_proof PRIVATE nimrtc::engine)
          EOF
          cmake -S build/downstream-proof -B build/downstream-proof/build \
            -DCMAKE_PREFIX_PATH="$PWD/build/_install/macos-arm64"
          cmake --build build/downstream-proof/build
          ./build/downstream-proof/build/downstream_proof
```

**为什么消费者 `#include <nimrtc/engine/engine.hpp>` 而不是 Linux 那条腿的 `<nimrtc/core/api_version.hpp>`**：Linux 那条腿只证明「配置能被找到、包能被链接」；**包含引擎头**才是真正的宿主形状 —— 它会暴露「引擎头依赖的第三方头没被装进去」这类缺陷（比如 `plugins/ice_transport.hpp` 或 `plugins::TurnTransport`）。Windows 那条腿用的就是引擎头，本片与它同形 —— **macOS 要证明的正是「装出来的树够一个真实宿主用」**。

**为什么消费者**不**钉 `CMAKE_CXX_STANDARD`**：与 Windows 那条腿的注释同一个道理 —— 「从包里继承标准」正是必须成立的事。包配置该声明什么标准就声明什么。

**为什么不钉 `CMAKE_OSX_ARCHITECTURES`**：runner 与构建机同架构（macos-15 是 arm64），且**通用二进制**由发布那侧负责；本片证明的是「导出路径」，不是「架构切片」。

### 二、导出缺陷的修法

**先跑，后修**：这道证明可能揭出导出缺陷（macOS 特有的、或三平台共有的）。揭出来就修，**修在 `cmake/NimRTCConfig.cmake.in`**（配置模板）或 `cmake/NimRTCInstall.cmake`（安装规则）里 —— **不是**在 CI 里打补丁。

**导出机制的真源**（写规格时核实）：`cmake/NimRTCInstall.cmake` 的 §3 里有

```cmake
configure_package_config_file(
  "${CMAKE_SOURCE_DIR}/cmake/NimRTCConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/NimRTCConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/NimRTC"
  PATH_VARS CMAKE_INSTALL_INCLUDEDIR CMAKE_INSTALL_LIBDIR)
```

**关键事实**：`PATH_VARS` 只传了**两个**变量（`CMAKE_INSTALL_INCLUDEDIR` / `CMAKE_INSTALL_LIBDIR`），模板里用的正是 `@PACKAGE_CMAKE_INSTALL_INCLUDEDIR@` / `@PACKAGE_CMAKE_INSTALL_LIBDIR@` —— 这两个由 `configure_package_config_file` **自动**从 `PATH_VARS` 生成。模板里另有 `@PACKAGE_INIT@` 与 `@PROJECT_VERSION@`。

**已知需要核对的点**（macOS 特别容易踩的）：

| 疑点 | 为什么 macOS 会踩 |
|---|---|
| `CMAKE_INSTALL_LIBDIR` 在 macOS 上是 `lib` 而不是 `lib64` | 模板里若硬编了 `lib64`，`find_package` 会找不到库文件 |
| `.dylib` 与 `.a` 的文件名（`libnimrtc_engine.dylib` vs `libnimrtc_engine.a`） | imported target 的 `IMPORTED_LOCATION` 得跟着平台走 |
| `NimRTCConfig.cmake` 里的 `find_dependency` 链 | 公开头依赖的第三方包（如 libsrtp / openssl）得在包里也被找到 |
| `CMAKE_INSTALL_PREFIX` 是**相对**路径时的行为 | 暂存目录用相对路径 `build/_install/macos-arm64`，模板不能假设绝对路径 |

**这些是「要核对的地方」，不是「已知的缺陷」** —— 本片的第一件事就是**跑一遍证明看它报什么**。

### 三、一个可本地跑的导出检查

CI 的证明只在 macOS runner 上跑 —— 开发机（Windows / Linux）上跑不了。给开发机一个**能跑的等价物**：`tools/check_install_export.py`。

**它做什么**（纯静态检查，不装不编）：

1. 读 `cmake/NimRTCConfig.cmake.in`，确认它**没有**硬编平台相关的路径前缀（`lib64` / `/usr/local` / 绝对路径）。
2. 确认模板里的每个 `@...@` 占位符都在 `cmake/NimRTCInstall.cmake` 的 `configure_package_config_file()` 调用里**有来源**（`@PACKAGE_INIT@` / `@PACKAGE_<PATH_VAR>@` 自动提供，其余必须在 `PATH_VARS` 里）—— 漏一个 → 装出来的 config 里是空字符串 → `find_package` 成功但 target 是坏的。
3. 确认 `PATH_VARS` 里的每个变量名，模板里**确实**用到了（多传一个只是冗余，报成提示不是错误）。
4. 确认模板里**没有** GNU-only 的命令（`GNUInstallDirs` 之外的 GNU 专有用法等）。

**配套自检**：`tools/check_install_export_self_test.py` —— 闸门只报它认识的形态，所以自检要**钉住那个行为**：一个「模板里漏了变量」的样本必须**被报出来**，一个「正常模板」必须**不被报出来**（与 `ci.yml` 里既有的 `check_readme_release.py --self-test` / `check_release_tag_filter.py --self-test` 同一个套路）。

**为什么要有这个工具**：CI 的证明是**事后**的 —— 合并之后才发现导出坏了。而导出模板是**手写**的，改它的时候最容易漏一个占位符。本地能跑的工具让改模板的人在**提交前**就知道。

### 四、发布包：核对 macOS 那条腿的归档

`release.yml` 的 macOS 腿已经归档了 `build/_install/macos-arm64`（`tar -C "$staging" -cJf ...`）。N9 要**核对**三件事（不是重写它）：

1. 归档里**含** `NimRTCConfig.cmake`（没有它，宿主拿到包也用不了）。
2. 归档里**含**引擎的公开头（`nimrtc/engine/engine.hpp`）。
3. 归档的大小仍然在合理范围（现有那道 64 MB 的闸门继续管着）。

**这三件事怎么验**：CI 的下游证明**已经**证明了 1 和 2（它从暂存目录编译，不是从归档）。归档本身用 `tar -tf` 列一下就行 —— 本片在 release 的 macOS 腿里加一道**列表断言**（列出归档内容并断言关键路径在里面）。

**为什么不是「从归档里编译」**：那需要解压 + 重跑一遍证明，多花几分钟而收益为零 —— 归档是暂存目录的**无损压缩**（`tar -C staging -cJf ... .`），内容等价。断言「归档里有关键文件」+「暂存目录能编译」合起来就足够了。

## 验收标准

### 一、CI 的 macOS 腿

1. `ci.yml` 的 `macos-clang` 腿有 `Install + downstream proof` 步骤。
2. 该步骤在 `Test` 之后。
3. 该步骤把安装树装到 `build/_install/macos-arm64`（**暂存目录**，不是系统路径）。
4. 该步骤找到 `NimRTCConfig.cmake` 并断言它存在。
5. 该步骤在**树外**（`build/downstream-proof/`，独立于源码树）配置并编译一个消费者。
6. 消费者用 `find_package(NimRTC 1.0 REQUIRED)`。
7. 消费者链接 `nimrtc::engine`。
8. 消费者**包含** `<nimrtc/engine/engine.hpp>`（真实宿主形状）。
9. 消费者**不**钉 `CMAKE_CXX_STANDARD`（标准从包里继承）。
10. 消费者**编译后被运行**，退出码 0。
11. 消费者的 `CMAKE_PREFIX_PATH` **只**指向暂存目录 —— **不**引用源码树或构建树的路径。

### 二、导出检查

12. `tools/check_install_export.py` 存在，能跑，退出码 0（在**当前树**上）。
13. 它能查出「模板里漏了变量占位符」这种缺陷 —— 自检里的**负样本必须被报出来**。
14. 它**不**误报正常的模板（自检里的**正样本必须干净**）。
15. `tools/check_install_export_self_test.py` 存在且跑绿。
16. 该检查在 `ci.yml` 的**所有三条腿**（Windows / Linux / macOS）上都跑（它是纯静态的，跨平台）。
17. `cmake/NimRTCConfig.cmake.in` 里**没有**硬编的 `lib64` / 绝对路径 / 平台专有前缀。

### 三、发布包

18. `release.yml` 的 macOS 腿断言归档里**含** `NimRTCConfig.cmake`。
19. `release.yml` 的 macOS 腿断言归档里**含**引擎的公开头。
20. 现有的 64 MB 大小闸门**仍然在**（不被本片移除）。
21. 归档格式仍是**纯归档**（`.tar.xz` / `.zip`）—— 不引入包管理器格式。

### 四、边界与回归

22. 本片**不**改 `src/` 的任何 C++ 源码（静态检查：改动清单里没有 `src/` 下的 `.cpp` / `.hpp`）。
23. `scripts/api_freeze_scan.js` 跑绿（CI 已有的三道门，本片不能碰坏）。
24. `scripts/check_api_manifest.py` 跑绿（CI 已有的门）。
25. `python tools/check_install_export.py` 跑绿。
26. `python tools/check_install_export_self_test.py` 跑绿。
27. `.github/workflows/ci.yml` 与 `.github/workflows/release.yml` 的 **YAML 语法**有效（`python -c "import yaml, sys; yaml.safe_load(...)"` 能解析）。

### 五、文档

28. `docs/adr/ADR-034-macos-install-downstream-proof.md` 记录决策与理由。
29. `docs/plan/v1.4-plan.md` 的状态表把 N9 标成 signed（签字后改）。
30. `CHANGELOG.md` 记录 macOS 的下游证明与本片修掉的导出缺陷。

### 六、验收测试（本片自己的验收文件）

门禁②要求：每一条验收标准对应一条「真正跑起来」的用例，不能是 todo/skip。
本片**没有 C++ 改动**，所以验收文件走**静态 + 执行既有工具**这条路
（与 N8 同族的验证手段）：

31. `tests/acceptance/v1-macos-install-proof/acceptance.test.cjs` 存在，且**每条用例都是真断言**（无 `todo` / `skip`）。
32. 该文件把上面 30 条验收标准逐组落成真实断言：
    - 一组断言 `ci.yml` 的 `macos-clang` 腿有 `Install + downstream proof` 步骤，且在 `Test` 之后。
    - 一组断言该步骤装到 `build/_install/macos-arm64`（暂存目录，不是系统路径）。
    - 一组断言该步骤在**树外**（`build/downstream-proof/`）编译消费者，且 `CMAKE_PREFIX_PATH` **只**指暂存目录。
    - 一组断言消费者用 `find_package(NimRTC ... REQUIRED)`、链 `nimrtc::engine`、包含 `<nimrtc/engine/engine.hpp>`、**不**自己设 `CMAKE_CXX_STANDARD`。
    - 一组断言 `release.yml` 的 macOS 腿断言归档里有 `NimRTCConfig.cmake` 与 `nimrtc/engine/engine.hpp`。
    - 一组**真正执行** `tools/check_install_export.py` 与 `tools/check_install_export_self_test.py`，断言退出码 0。
    - 一组断言 `ci.yml` / `release.yml` 的 YAML 可解析。
    - 一组断言改动清单里**没有** `src/` 下的 `.cpp` / `.hpp`（边界）。
33. 验收文件**不**依赖编译产物（纯 JS + 调 Python 工具），所以它**不需要** build 树。


## 本片不做

- **不做** N6 的 SDK 样例 —— 那是波次 6，依赖本片先落地。
- **不做** 包格式的选择 —— §5 **N9-1** 已决：**纯归档**。
- **不做** 改 Windows / Linux 的证明步骤 —— 它们已经有了，本片不动。
- **不做** 改 `src/` 的 C++ —— 本片只碰 CI、导出模板与检查工具。
- **不做** 通用二进制 / 架构切片 —— 发布那侧的事。
- **不做** 从归档里编译的证明 —— 归档是暂存目录的无损压缩，断言关键文件在包里就够（见 §设计四）。

## 接口变更

**无 C++ 接口变更。** 本片只碰 CI 配置、CMake 导出模板与两个 Python 检查工具。

| 文件 | 变化 |
|---|---|
| `.github/workflows/ci.yml` | `macos-clang` 腿加 `Install + downstream proof` 步骤；三条腿加 `check_install_export.py` 步骤 |
| `.github/workflows/release.yml` | macOS 腿加归档内容断言 |
| `cmake/NimRTCConfig.cmake.in` | 只在证明揭出缺陷时改（见 §设计二） |
| `cmake/NimRTCInstall.cmake` | 只在证明揭出缺陷时改（见 §设计二） |
| `tools/check_install_export.py` | **新增** |
| `tools/check_install_export_self_test.py` | **新增** |
| `docs/adr/ADR-034-macos-install-downstream-proof.md` | **新增** |

## 测试策略

### 本地可跑（本片的核心验证）

- `python tools/check_install_export.py` —— 静态导出检查，在当前树上跑绿。
- `python tools/check_install_export_self_test.py` —— 自检：正样本干净、负样本被报出。
- YAML 解析：`python -c "import yaml; yaml.safe_load(open('.github/workflows/ci.yml'))"` 与 `release.yml`。

### CI 上跑（macOS 腿）

- `Install + downstream proof` —— 真实的安装 + 树外编译 + 运行。

### 回归（CI 已有的门，本片不能碰坏）

- `scripts/api_freeze_scan.js`（三道门之一）。
- `scripts/check_api_manifest.py`。
- 三条腿的既有 Build / Test。

## 风险

| 风险 | 缓解 |
|---|---|
| **证明揭出导出缺陷，改不动** | 缺陷分两类：**三平台共有的**（改 `cmake/NimRTCConfig.cmake.in`，一处修好三边）与**macOS 特有的**（多半是 `CMAKE_INSTALL_LIBDIR` / `.dylib` 文件名）。前者必须在本片修完（否则三边都坏）；后者若超出本片范围，**降级为「记录在 ADR 的未决项里 + CI 那一步标记为 `continue-on-error` 并说明」**，并**明确写进 ADR** —— 不许默默跳过。 |
| **macOS runner 上装出来的树缺东西**（第三方依赖没被装） | 这正是本片要发现的。若确实缺，**要么**把该依赖装进去（改安装规则），**要么**在 `NimRTCConfig.cmake.in` 里 `find_dependency` —— 选哪个由 ADR 记。 |
| **`cmake --install` 在 macOS 上默认装到哪里** | 显式给 `--prefix build/_install/macos-arm64`（与 Windows / Linux 同形）—— 绝不装到系统路径。 |
| **本片改了 CI 却把 macOS 腿弄红了**（YAML / 命令写错） | 验收标准 27（YAML 可解析）+ 步骤照抄 Linux 那条腿的成熟形状（只把 `linux-x86_64` 换成 `macos-arm64`）。 |
| **「下游证明」名不副实**（其实还在用源码树） | 验收标准 11：`CMAKE_PREFIX_PATH` **只**指暂存目录；且消费者的源文件在 `build/downstream-proof/`（树外）。 |
| **发布包与暂存目录不一致** | 归档是 `tar -C "$staging" -cJf ... .`（无损）；本片加归档内容断言（验收标准 18/19）兜住。 |

## 归属

**`version/v3`**。本片只碰 CI 与导出配置 —— **不撞 N8 的 C++ 区域**，但波次 5 的其余四片（N3 / N7 / N8）已进 `version/v3`，本片接着做。

**前置**：N3 / N7 / N8 已合入 `version/v3`。本片**不依赖**它们的技术内容 —— 只依赖「导出树里已经有引擎头」这个既成事实。

**后续**：N6（SDK 样例）要在**安装树**上编译 —— 本片证明的正是那条路径可行，所以 N6 可以直接复用 CI 里的证明形状。
