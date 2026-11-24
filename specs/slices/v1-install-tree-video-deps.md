# Slice: v1-install-tree-video-deps

## Summary

**安装出来的 SDK 链不上视频 —— 视频宿主在安装树上编不过。**

N6（SDK 样例）在搭双进程验收腿时撞出这个缺口（2026-11-25 `ZELTO_QUESTION`）：

- 构建树里，`find_package(NimRTC REQUIRED CONFIG)` + `nimrtc::engine` 能链出视频。
- **安装树里不行**：暂存的 `cmake/NimRTCConfig.cmake.in` **不链** `nimrtc_nvcodec` /
  `nimrtc_vendor_openh264_*`，也**不链** `d3d11` / `dxgi`。
- 视频宿主（h264 + headless sink）链 `nimrtc::engine` 报 **31 个 LNK2019**。

也就是说：**v1.4 的整个卖点是「宿主能拿到一个装好的 SDK」**（N9 的 macOS
安装证明 / 计划 §0「几行代码就能通话」），但**视频这条路在安装树上根本没通**。
音频路通、基础路通，**只有视频路断** —— 而视频恰恰是 RTC 的一半。

本片修的是**打包层**，不是引擎。

## 目标

**让安装树里的 `nimrtc::engine` 能链出一个带视频的宿主。**

具体地：

1. 安装树里**能链到** `nimrtc_nvcodec`（NVIDIA / VideoToolbox 编解码适配器）。
2. 安装树里**能链到** `nimrtc_vendor_openh264_*`（软件 H.264 编解码器），
   且**顺序正确**（静态链接的左右顺序问题）。
3. Windows 上链到 `d3d11` / `dxgi`（D3D11 视频 sink / 硬件路径）。
4. 装出来的 bundle **自洽** —— 不依赖宿主机器上恰好有别的库。

### 现状核实（写规格时的树状态）

- `cmake/NimRTCInstall.cmake:96-135` 的 `_nimrtc_install_shorts` 列表里：
  **有** `h264` / `video_frame` / `video_pipeline` / `video_sink` / `video_source`，
  **没有** `nvcodec`。
- `cmake/NimRTCInstall.cmake:140-150`：列表里的目标若是 `STATIC_LIBRARY`
  则 `install(TARGETS ...)`。
- `cmake/NimRTCConfig.cmake.in:141-150` 的 `_nimrtc_vendor_names` 列表：
  `opus` / `srtp3` / `juice` / `wolfssl` / `msquic` / `msquic_platform` ——
  **没有** openh264 的几个 archive。
- `cmake/NimRTCConfig.cmake.in:203-207`：Windows 上补的系统库是
  `ws2_32` / `bcrypt` / `iphlpapi` / `winmm` / `crypt32` ——
  **没有** `d3d11` / `dxgi`。
- `src/third_party/openh264/CMakeLists.txt` 产出三个静态库：
  `nimrtc_vendor_openh264_common` / `_encoder` / `_decoder`，外加一个
  `nimrtc_vendor_openh264` INTERFACE 目标把三者串起来
  （`CMakeLists.txt:330-341`）。
- `src/modules/h264/CMakeLists.txt:65` 用 `nimrtc_link_openh264(nimrtc_h264)`
  —— 构建树里这个 helper 负责把 vendor 目标接上。
- `src/modules/nvcodec/CMakeLists.txt:43` 的 `nimrtc_nvcodec` 是 `STATIC_LIBRARY`，
  **不在**安装列表里 → **根本没被安装**。
- `src/engine/CMakeLists.txt:411` 链 `nimrtc::h264`；nvcodec 是**插件**式接入
  （`nimrtc::plugins` 的 `IVideoCodec`），由宿主注册 —— 所以宿主要链它，
  安装树必须**提供**它。

### 与其它片的边界（本片**不做**）

- **不做** 改 `src/engine/` 的公开面 —— 本片是**打包层**的事。
- **不做** 改引擎 / 模块的**代码** —— 只改「装什么」与「链什么」。
- **不做** 改 `src/modules/nvcodec/CMakeLists.txt` 本身（除非发现必须改才能装，
  且 ADR-038 写明）—— 主要改 `cmake/NimRTCInstall.cmake` 与
  `cmake/NimRTCConfig.cmake.in`。
- **不做** 改 N6 样例 —— 样例已经在那里了，它只是受害者。
- **不做** 改 ICE 层 —— 另一片（`v1-turn-tls-trust`）碰那里。

## 范围

- cmake/NimRTCInstall.cmake
- cmake/NimRTCConfig.cmake.in
- cmake/NimRTCVendored.cmake
- src/modules/nvcodec/CMakeLists.txt
- src/third_party/openh264/CMakeLists.txt
- tests/acceptance/v1-install-tree-video-deps/acceptance.test.cjs
- tools/check_install_tree_video_deps.py
- docs/adr/ADR-038-install-tree-video-dependencies.md
- docs/api/engine_api.md
- CHANGELOG.md
- specs/slices/v1-install-tree-video-deps.md

> **只读** `src/engine/` —— 本片**不**改它。
> **只读** `src/modules/ice/` —— 本片**不**改它。
> **只读** `cmake/apm_link_order.txt` —— 参照它的**链接顺序清单**形状
> （APM 的既有做法），本片给 openh264 用同一形状。
> **只读** `cmake/FindNVENC.cmake` / `cmake/FindLibVA.cmake` —— 核实 NVIDIA
> import lib 的来源（设计 §四），本片**不**改查找逻辑。
> **只读** `cmake/FindAMF.cmake` / `cmake/FindLibVPL.cmake` / `cmake/FindMsQuic.cmake` ——
> 其它 vendor 的查找，本片**不**动。

## 设计

### 一、装什么：`nvcodec` 进安装列表

`cmake/NimRTCInstall.cmake` 的 `_nimrtc_install_shorts` 加 `nvcodec`。

**核实点**：`nimrtc_nvcodec` 在**所有平台**都构建（`nvcodec/CMakeLists.txt:43`
无平台 guard —— VideoToolbox 路径在非 Apple 上编译成「不可用」的桩，
NVIDIA 路径由 `nimrtc_link_nvenc` / `nimrtc_link_nvdec` 变成 no-op）。
所以它**总是**是一个 `STATIC_LIBRARY`，安装规则**不需要**平台分支。

但要注意：`nimrtc_nvcodec` 的 `target_link_libraries` 里是
`nimrtc::core` / `nimrtc::plugins` / `nimrtc::video_frame` —— 这些**也是**静态库，
在安装树里必须**同时**在链上。`video_frame` **已经在**安装列表里；
`core` / `plugins` 是 INTERFACE 目标（安装树里由 `NimRTCConfig.cmake.in`
单独处理，见该文件 `:224` 附近的注释）—— 需核实安装树是否**提供**了它们的
archive；`NimRTCConfig.cmake.in:220-240` 显示 `nimrtc::engine` 用的是
`UNKNOWN IMPORTED` + 一串平铺 archive 路径，说明**静态库是平铺链**的，
`core` / `plugins` 的 archive 必须在**同一串**里。

### 二、链什么：openh264 的三个 archive + 顺序

`cmake/NimRTCConfig.cmake.in` 的 `_nimrtc_vendor_names` 加 openh264 的三个：
`openh264_common` / `openh264_encoder` / `openh264_decoder`（名字取决于
`nimrtc_vendor_openh264_*` 目标名 —— 装出来时 archive 的**文件名**是什么，
需要在实现时**核实**，不能猜）。

**顺序是硬要求**：静态链接器从左往右解析符号。`openh264_encoder` 引用
`openh264_common` 的符号，`openh264_decoder` 也引用它 —— 所以
`common` 必须在 `encoder` / `decoder` **之后**（或之前，取决于谁引用谁，
但**必须**让引用方的符号在**被引用方之后**被解析）。

`NimRTCConfig.cmake.in:143-146` 的注释已经点明这个坑：
"observed: 82x LNK2001 juice_*/srtp_*/wolfSSL_*/opus_*/MsQuicOpenVersion
when only the nimrtc_* archives were chained"。

**本片的做法**：像 APM 那样用**链接顺序清单**（`cmake/apm_link_order.txt`，
`NimRTCConfig.cmake.in:171-196` 的既有形状），而不是在 `find_library()` 的
返回顺序上赌。安装时把清单一起装到 `<libdir>/`，config 从清单读顺序。

**核实点**：`_nimrtc_vendor_names` 那段用 `find_library()` 逐个找，找不到就
**跳过**（module-OFF 构建）。openh264 若在某个平台没构建，就会被静默跳过 ——
那正是它今天的问题（`openh264` 在 vendor 列表里**根本不存在**，所以永远
找不到）。

### 三、`d3d11` / `dxgi`

`cmake/NimRTCConfig.cmake.in:203-207` 的 Windows 系统库列表加
`d3d11` / `dxgi`。

**核实点**：这两个库是**系统库**，不是 NimRTC 自己的 archive。但
`nimrtc_video_sink`（D2 的硬件 sink）是否需要它们，取决于那个模块在
Windows 上是不是真用了 D3D11 —— 若用了，缺它们就是链接期 LNK2019
（`d3d11.lib` / `dxgi.lib`）。

### 四、`nvcodec` 的额外依赖（Windows）

`nimrtc_nvcodec` 在 Windows 上通过 `nimrtc_link_nvenc` / `nimrtc_link_nvdec`
接 **NVIDIA 驱动 SDK**（`cmake/FindNVENC.cmake` / `cmake/FindLibVA.cmake` 等）。
若 SDK 存在，链接上它**带入**的 import lib 也得在链上。

**核实点**：NVIDIA SDK 的 import lib（`nvidia-encode` / `nvidia-video`）
在安装树里**存在吗**？若存在且被 `nvcodec` 引用，链 `nvcodec` 时**必须**把
它们也链上，否则又一批 LNK2019。**这是个真实的可能性**，不是理论。

### 五、安装树必须**自洽**

装出来的 bundle **不能**依赖宿主机器上恰好装了别的东西。
本片验收时**必须**在一个**干净的 prefix** 上验（`build/_install`），并
在**只有 bundle 里的东西**的前提下链接成功。

## 验收标准

### 一、装了什么

1. 安装后的 `<libdir>/` 里**有** `nimrtc_nvcodec` 的 archive。
2. 安装后的 `<libdir>/` 里**有** openh264 的**全部三个** archive
   （common / encoder / decoder），或明确记录「本平台未构建 openh264」。
3. 安装后的 `<libdir>/` 里**有**链接顺序清单（openh264 的顺序按 §二 的形状）。

### 二、链了什么

4. 暂存的 `cmake/NimRTCConfig.cmake.in` **链** `nimrtc_nvcodec`。
5. 暂存的 `cmake/NimRTCConfig.cmake.in` **链** openh264 的 archive。
6. openh264 的 archive 在链上**顺序正确**（§二 的要求）。
7. Windows 上暂存的 `cmake/NimRTCConfig.cmake.in` **链** `d3d11` 与 `dxgi`。
8. 若 `nvcodec` 引用了 NVIDIA import lib，暂存的 config **也链**它（§四）。

### 三、装得对

9. 安装树里的 `nimrtc::engine` **能**被一个**带视频**的宿主链接
   （h264 + headless sink）—— **0 个 LNK2019**。
10. 上述链接**只**用 bundle 里的东西（干净 prefix，无外部依赖）。
11. 音频 / 基础路径**不回归**（原有的安装 + 下游证明仍然通过）。
12. `nimrtc::engine` 的公开头**不**因为本片而变（头文件集合不变）。

### 四、可重复

13. `tools/check_install_tree_video_deps.py` 存在，对暂存安装树**跑绿**。
14. 该工具**自测**（对故意缺依赖的树**必须**报红）—— 不能只有一个「永远绿」的检查。
15. 同一工具在 CI 上**再跑一次**绿（不是只在本地绿）。

### 五、边界与回归

16. `src/engine/` **不**被修改。
17. `src/engine/include/` 的公开面**不**变（公开头集合不变）。
18. `src/modules/ice/` **不**被修改。
19. 现有的安装 + 下游证明 CI 腿**仍然绿**。

### 六、文档与契约

20. `docs/adr/ADR-038-install-tree-video-dependencies.md` 存在，说明
    「装什么 / 链什么 / 顺序为什么重要 / NVIDIA import lib 的处置」。
21. `docs/api/engine_api.md` 记录安装树里现在提供了哪些视频依赖。
22. `CHANGELOG.md` 有对应条目。
23. `specs/slices/v1-install-tree-video-deps.md` 记录实施中的发现。

## 归属

**`version/v3`**。

**前置**：N9（`v1-macos-install-proof`）已合入 —— 它证明了「装得上、能被消费」，
本片把那条证明**扩展到视频**。N6（`v1-sdk-sample`）已合入 —— 它是缺口的发现者。

**并行**：与 `v1-turn-tls-trust` **写集不重叠**，可并行。
本片碰 `cmake/`，那一片碰 `src/engine/` + `src/plugins/`。

**后续**：本片合入后，N6（`v1-sdk-sample`）的执行腿（判据 24-27）**不再**因缺口 B
被阻塞 —— 验收测试里的 `blockedReason()` 对缺口 B 的检查
（暂存 config 里同时含 `nvcodec` 与 `openh264`）应当**不再**命中。

两片**都**合入后，N6 的执行腿应当**真正跑起来**，而不是 `SKIPPED`。
那之后 N6 自己的门槛还差最后一样：**归档的双机跑**（需要真实机器与真实 NAT）。
