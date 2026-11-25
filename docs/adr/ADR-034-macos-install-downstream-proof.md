# ADR-034: macOS 的安装与下游证明 —— 导出路径要有自动化证据

| | |
|---|---|
| Number | 034 |
| Status | **Proposed** |
| Date | 2026-11-24 |
| Phase | v1.4 (N9) |
| Bound to | v1.4.0 |
| Supersedes | — |
| Related | `docs/plan/v1.4-plan.md` §N9；`specs/slices/v1-macos-install-proof.md`；`docs/adr/ADR-015-api-stability-policy.md` |

---

## 1. 背景

Windows（MSVC）与 Linux 的 CI 每次 push 都做「安装到暂存目录 → 用 `find_package(NimRTC)`
编译一个最小消费者 → 跑起来」的**下游证明**（`ci.yml` 的 `Install + downstream proof` 步骤）。
但 `macos-clang` 那条腿只做**树内**编译与测试 —— **没有** install + downstream proof。

结果：macOS 上的导出路径（`NimRTCConfig.cmake` 的生成、`CMAKE_INSTALL_*` 的路径、
`nimrtc::engine` 的 imported target 定义）**没有任何自动化的证据**。而 macOS 在 v1.4.0 身上背着
**三个**自己的说法：D2 的 Metal 渲染器、D3 的默认设备表、D4 的 AVFoundation 摄像头 ——
这些都只在 macOS 上跑过。「Windows 上能跑，macOS 应该也行」不是证据。

写规格时的树状态（`specs/slices/v1-macos-install-proof.md` §现状核实）：

- 导出机制的真源在 `cmake/NimRTCInstall.cmake` §3：`configure_package_config_file()` 把
  `cmake/NimRTCConfig.cmake.in` 处理成安装后的 `NimRTCConfig.cmake`，`PATH_VARS` 传
  `CMAKE_INSTALL_INCLUDEDIR` / `CMAKE_INSTALL_LIBDIR` 两个变量。
- 模板里的占位符是 `@PACKAGE_INIT@`、`@PROJECT_VERSION@`、`@PACKAGE_CMAKE_INSTALL_INCLUDEDIR@`、
  `@PACKAGE_CMAKE_INSTALL_LIBDIR@` —— 前两个由 `configure_package_config_file` / `project()` 自动
  供给，后两个由 `PATH_VARS` 自动生成为相对路径。
- macOS 的发布包已经存在（`release.yml` 的 `build-macos-clang` 腿 `tar -C staging -cJf ...`），
  缺的只是「从包里能找到 NimRTC 并编译」的自动化证明。

## 2. 决策

### 2.1 决策一：给 `macos-clang` 加 install + downstream proof（与 Windows / Linux 同形）

`ci.yml` 的 `macos-clang` 腿，在 `Test` 步骤**之后**、失败时上传构建目录**之前**，加一步：

1. `cmake --install build --prefix build/_install/macos-arm64` —— **显式暂存目录**，绝不装到
   系统路径。
2. 在暂存目录里 `find` 到 `NimRTCConfig.cmake` 并断言它存在。
3. 在**树外**（`build/downstream-proof/`）生成一个最小消费者：`#include <nimrtc/engine/engine.hpp>`、
   `find_package(NimRTC 1.0 REQUIRED)`、链接 `nimrtc::engine`、跑起来打印版本号、退出码 0。
4. `CMAKE_PREFIX_PATH` **只**指向暂存目录 —— 不引用源码树或构建树的路径，这正是「宿主项目」
   的样子。

**为什么消费者包含引擎头而不是 Linux 那条腿的 `<nimrtc/core/api_version.hpp>`**：Linux 那条腿只
证明「配置能被找到、包能被链接」；**包含引擎头**才是真正的宿主形状 —— 它会暴露「引擎头依赖的
第三方头没被装进去」这类缺陷。Windows 那条腿用的就是引擎头，macOS 与它同形 —— macOS 要证明的
正是「装出来的树够一个真实宿主用」。

**为什么不钉 `CMAKE_CXX_STANDARD`**：与 Windows 那条腿的注释同一个道理 —— 「从包里继承标准」
正是必须成立的事。包配置该声明什么标准就声明什么（模板里 `nimrtc::core` 与 `nimrtc::engine` 的
`INTERFACE_COMPILE_FEATURES "cxx_std_20"` 干的就是这件事）。

**为什么不钉 `CMAKE_OSX_ARCHITECTURES`**：runner 与构建机同架构（`macos-15` 是 arm64），
通用二进制由发布那侧负责；本片证明的是「导出路径」，不是「架构切片」。

### 2.2 决策二：导出缺陷修在导出规则里，不在 CI 里打补丁

这道证明**可能**揭出导出缺陷（macOS 特有的、或三平台共有的）。揭出来就修，**修在
`cmake/NimRTCConfig.cmake.in`**（配置模板）或 **`cmake/NimRTCInstall.cmake`**（安装规则）里。

缺陷分两类：

| 类型 | 修法 |
|---|---|
| **三平台共有** | 改 `cmake/NimRTCConfig.cmake.in`，一处修好三边（Windows / Linux / macOS 的证明共用同一个模板） |
| **macOS 特有** | 多半是 `CMAKE_INSTALL_LIBDIR`（macOS 上是 `lib` 而不是 `lib64`）、`.dylib` 与 `.a` 的文件名、`find_dependency` 链 |

若 macOS 特有的缺陷超出本片范围，**降级为「记录在 ADR 的未决项里 + CI 那一步标记为
`continue-on-error` 并说明」**，并**明确写进 ADR** —— 不许默默跳过。

写本 ADR 时的树状态：静态导出检查（决策三）在当前树上跑绿 —— 模板四个占位符全部有来源、
无硬编路径、无 GNU-only 构造；Windows / Linux 的证明一直在绿。没有已知缺陷需要修。macOS
runner 会是第一个真正跑这道证明的地方；若它揭出缺陷，按上表处理。

### 2.3 决策三：一个可本地跑的导出检查（`tools/check_install_export.py` + 自检）

CI 的证明只在 macOS runner 上跑 —— 开发机（Windows / Linux）上跑不了。给开发机一个能跑的
等价物：`tools/check_install_export.py`，纯静态检查，不装不编，做四件事：

1. 读 `cmake/NimRTCConfig.cmake.in`，确认它**没有**硬编平台相关的路径前缀（`lib64` /
   `/usr/local` / 绝对路径）。
2. 确认模板里的每个 `@...@` 占位符都在 `cmake/NimRTCInstall.cmake` 的
   `configure_package_config_file()` 调用里**有来源**（`@PACKAGE_INIT@` /
   `@PACKAGE_<PATH_VAR>@` 自动提供，其余必须是配置时**保证在作用域**的 CMake 变量，如
   `@PROJECT_VERSION@`）。漏一个 → 装出来的 config 里是空字符串 → `find_package` 成功但
   target 是坏的。
3. 确认 `PATH_VARS` 里的每个变量名模板里**确实**用到了（多传一个只是冗余，报成提示不是错误）。
4. 确认模板里**没有** GNU-only 的命令。

配套自检 `tools/check_install_export_self_test.py`：闸门只报它认识的形态，所以自检要**钉住那个
行为** —— 一个「模板里漏了变量」的样本必须**被报出来**，一个「正常模板」必须**不被报出来**
（与 `ci.yml` 里既有的 `check_readme_release.py --self-test` /
`check_release_tag_filter.py --self-test` 同一个套路）。

**为什么要有这个工具**：CI 的证明是**事后**的 —— 合并之后才发现导出坏了。而导出模板是
**手写**的，改它的时候最容易漏一个占位符。本地能跑的工具让改模板的人在**提交前**就知道。
它在 `ci.yml` 的**所有三条腿**（Windows / Linux / macOS）上都跑（纯静态，跨平台）。

### 2.4 决策四：发布包格式不变（纯归档），核对归档内容

发布包格式按计划 §5 **N9-1** 的已决：**纯归档**（`.tar.xz` / `.zip`）—— 包格式是**分发**的
决定，不是 SDK 的决定。本片**不**引入 `.pkg` / `.deb` / Homebrew formula。

N9 只**核对** macOS 那条腿的归档（`release.yml` 的 `build-macos-clang`），不重写它：

1. 归档里**含** `NimRTCConfig.cmake`（没有它，宿主拿到包也用不了）。
2. 归档里**含**引擎的公开头（`nimrtc/engine/engine.hpp`）。
3. 归档的大小仍然在合理范围 —— 现有那道 64 MB 的闸门**继续管着**（本片不碰它）。

**为什么不是「从归档里编译」**：那需要解压 + 重跑一遍证明，多花几分钟而收益为零 —— 归档是
暂存目录的**无损压缩**（`tar -C "$staging" -cJf ... .`），内容等价。断言「归档里有关键文件」
+「暂存目录能编译」（决策一）合起来就足够了。

## 3. 后果

### 3.1 公开面

**无 C++ 接口变更。** 本片只碰 CI 配置、CMake 导出模板的检查工具与文档：

| 文件 | 变化 |
|---|---|
| `.github/workflows/ci.yml` | `macos-clang` 腿加 `Install + downstream proof` 步骤；三条腿加 `check_install_export.py` 步骤 |
| `.github/workflows/release.yml` | macOS 腿加归档内容断言（`NimRTCConfig.cmake` + 引擎公开头） |
| `cmake/NimRTCConfig.cmake.in` | 只在证明揭出缺陷时改（本片未发现缺陷，未改） |
| `cmake/NimRTCInstall.cmake` | 同上（未改） |
| `tools/check_install_export.py` | **新增**（静态导出检查） |
| `tools/check_install_export_self_test.py` | **新增**（自检：正样本干净、负样本被报出） |
| `docs/adr/ADR-034-macos-install-downstream-proof.md` | **新增**（本文件） |

### 3.2 依赖方向

- 本片不依赖 N3 / N7 / N8 的技术内容 —— 只依赖「导出树里已经有引擎头」这个既成事实。
- 后续 N6（SDK 样例）要在**安装树**上编译 —— 本片证明的正是那条路径可行，N6 可以直接复用
  CI 里的证明形状。

### 3.3 与既有证明的关系

Windows / Linux 的证明步骤**不动**。macOS 与 Windows 同形（用引擎头），与 Linux 同 shell
（bash）。`scripts/api_freeze_scan.js` / `scripts/check_api_manifest.py` 等既有门**不碰**。

## 4. 备选方案与不选的理由

| 备选 | 不选的理由 |
|---|---|
| macOS 继续只做树内构建（现状） | 导出路径无自动化证据；macOS 背着的三个 v1.4.0 说法（D2 / D3 / D4）只在 macOS 上跑过 |
| 证明只断言 `find_package` 能跑，不含引擎头 | Linux 那条腿只证明「配置能链接」；包含引擎头才是真实宿主形状，会暴露「第三方头没装进去」 |
| 用 `cmake --install` 默认前缀（系统路径） | 绝不装到系统路径；与 Windows / Linux 同形，显式 `--prefix build/_install/macos-arm64` |
| 引入 `.pkg` / `.deb` / Homebrew formula | 包格式是分发决定不是 SDK 决定（§5 N9-1 已决）；CI 证明 `find_package` 两种都行 |
| 从归档里编译做证明 | 归档是暂存目录的无损压缩，解压重编多花几分钟而收益为零 |
| 导出缺陷在 CI 步骤里打补丁 | 治标不治本；一处模板缺陷三边都踩，修在模板一处好三边 |

## 5. 落地时对规格的几处实现取舍

1. **`check_install_export.py` 对 `@PROJECT_VERSION@` 的处理**：规格说「其余必须在 `PATH_VARS`
   里」，但模板实际用 `@PROJECT_VERSION@`（`project()` 保证在作用域、`configure_file` 会替换）。
   实现把它归入「配置时保证在作用域的 CMake 变量」一类，而不是报错 —— 否则当前树就红。
2. **`PATH_VARS` 冗余是提示不是错误**：多传一个 PATH_VARS 不会让安装坏掉，只是冗余；自检钉住
   「冗余 → NOTE、不是 FAIL」。
3. **release 归档断言用 `tar -tf | grep` 子串匹配**：不硬编 `lib` / `lib64` —— 断言 `grep -q
   "NimRTCConfig.cmake"` 与 `grep -q "nimrtc/engine/engine.hpp"`，libdir 差异不敏感。

## 6. 未决项

- 本片实现时**未发现**需要修的导出缺陷（静态检查跑绿、Windows / Linux 证明一直绿）。若
  `macos-clang` 的 `Install + downstream proof` 在真实 runner 上揭出 macOS 特有的缺陷，按
  §2.2 处理：能改就改在导出规则里；超出范围就记录在这里 + 该 CI 步骤 `continue-on-error`
  并说明 —— 不许默默跳过。
