# ADR-038: 安装树补上视频依赖 —— 「装好的 SDK 能链出一个带视频的宿主」

| | |
|---|---|
| Number | 038 |
| Status | **Proposed** |
| Date | 2026-11-25 |
| Phase | v1.4（打包层） |
| Bound to | v1.4.0 |
| Supersedes | — |
| Related | `docs/adr/ADR-015-api-stability-policy.md`（本片不碰公开 API 基线）；`docs/adr/ADR-034-macos-install-downstream-proof.md`（N9：装得上、能被消费 —— 本片把那条证明**扩展到视频**）；`docs/adr/ADR-036-sdk-sample-line-budget.md`（N6：缺口的发现者）；`specs/slices/v1-install-tree-video-deps.md` |

---

## 1. 背景

N6（SDK 样例）在搭双进程验收腿时撞出缺口（2026-11-25 `ZELTO_QUESTION`）：

- **构建树**里，`find_package(NimRTC REQUIRED CONFIG)` + `nimrtc::engine` 能链出视频。
- **安装树**里不行：暂存的 `cmake/NimRTCConfig.cmake.in` **不链** `nimrtc_nvcodec` /
  `nimrtc_vendor_openh264_*`，也**不链** `d3d11` / `dxgi`。
- 视频宿主（h264 + headless sink）链 `nimrtc::engine` 报 **31 个 LNK2019**。

音频路通、基础路通，**只有视频路断**。本 ADR 记录打包层的四个决定：
**装什么 / 链什么 / 顺序为什么重要 / NVIDIA import lib 的处置**。

## 2. 决策

### 2.1 决策一：装什么 —— `nvcodec` 进安装列表，公开头一起装

`cmake/NimRTCInstall.cmake` 的 `_nimrtc_install_shorts` 在 `h264` 之后加 `nvcodec`。

核实点（写规格时的树状态 + 实现时的确认）：

- `nimrtc_nvcodec` 在**所有平台**都构建（`src/modules/nvcodec/CMakeLists.txt` 无平台
  guard）：VideoToolbox 路径在非 Apple 上编译成「不可用」的桩，NVIDIA 路径由
  `nimrtc_link_nvenc` / `nimrtc_link_nvdec` 变成 no-op。所以它**总是**是一个
  `STATIC_LIBRARY`，安装规则**不需要**平台分支。
- `nimrtc_h264` 用 `target_link_libraries(... PUBLIC nimrtc::nvcodec)` 链它 ——
  安装树是**平铺静态链**，所以 `nimrtc_nvcodec` 的 archive 必须出现在同一串里，
  且在 `h264` **之后**（见 2.3）。
- nvcodec 的公开头（`src/modules/nvcodec/include`）**一起装**：一个注册
  NVENC / NVDEC 插件的宿主需要 include 它们，bundle 不自带就是「宿主机器恰好
  有别的库」—— 违反 §五「安装树必须自洽」。

### 2.2 决策二：链什么 —— openh264 的三个 archive + 链接顺序清单

`cmake/NimRTCConfig.cmake.in` 链 openh264 的三个 archive
（`nimrtc_vendor_openh264_encoder` / `_decoder` / `_common`），顺序来自
**链接顺序清单** `openh264_link_order.txt`（装进 `<libdir>/`）。

做法与 WebRTC APM 完全相同（`NIMRTC_APM_LINK_ARCHIVES` + `apm_link_order.txt`，
`NimRTCInstall.cmake` §2c 的既有形状）：

1. `src/third_party/openh264/CMakeLists.txt` 把 archive 集合 + 顺序发布成
   **GLOBAL 属性** `NIMRTC_OPENH264_LINK_ARCHIVES`（encoder / decoder / common）。
   —— 拥有顺序的是**建 archive 的模块**，不是安装逻辑。
2. `NimRTCInstall.cmake` §2d 逐 target `install(TARGETS ... ARCHIVE ...)` 到
   `<libdir>/`，并写 + 装 `openh264_link_order.txt`。
3. `NimRTCConfig.cmake.in` 读 manifest，按行把 archive 追加到平铺链里；
   找不到就警告并跳过（module-OFF 构建安静跳过，同 APM）。

为什么用**清单**而不是在 `find_library()` 的返回顺序上赌：
`find_library()` 返回**路径**，不返回**顺序**；而顺序是静态链接的契约
（见 2.3）。清单跟着 archive 一起装，config 读顺序 —— 与 APM 同形状，
本片不发明新机制。

### 2.3 决策三：顺序为什么重要 —— GNU 静态链从左往右解析符号

一个 GNU 静态链接器**从左往右**扫描 archive：archive 只被扫描一次，且只为
**当时仍未定义**的符号拉 object。三个 openh264 archive 互相依赖：

| archive | 提供 | 需要 |
|---|---|---|
| `common` | `WelsLog()`、`WelsCommon::g_kuiMbCountScan4Idx` | （无） |
| `encoder` | — | `WelsLog()`、`g_kuiMbCountScan4Idx`（`welsEncoderExt.cpp` 等） |
| `decoder` | — | `g_kuiMbCountScan4Idx`（`deblocking.cpp`） |

所以顺序必须是 **encoder / decoder / common**：两个消费者在前（先被扫描，
把引用它的 object 拉进来），`common` 在后（解析它们留下的未定义符号）。

反例（把 `common` 排第一 —— 最顺眼的字母序）：链接器扫 `common` 时还没有
任何符号需要 `WelsLog()` → `welsCodecTrace.cpp.o` 不被拉入 → 链到结尾报
`undefined reference to WelsLog(...)` / `g_kuiMbCountScan4Idx`。

MSVC 忽略 archive 顺序，所以这个顺序在 Windows 上是 no-op —— 修法对所有
工具链无害。上游 Meson 用 `link_whole` 绕开整个问题，本片没有那个奢侈
（静态 bundle 只能是平铺链）。

同样的理由让 `nimrtc_nvcodec` 必须排在 `nimrtc_h264` **之后**（2.1），
也让 engine 必须排在最前（它引用所有模块）。

### 2.4 决策四：NVIDIA import lib 的处置 —— 不需要，只有 d3d11/dxgi

实现时核实了设计 §四的问题：NVIDIA SDK 的 import lib（`nvidia-encode` /
`nvidia-video`）**要不要**进 bundle？

**结论：不要。** `src/modules/nvcodec/src/nvenc_encoder.cpp` 与
`nvdec_decoder.cpp` 走 **`LoadLibrary` + `GetProcAddress`**（dynlink loader），
对 NVIDIA import lib **没有链接期引用** —— bundle 不需要装它们，链上也
不需要它们。

但 `nvenc_encoder.cpp` 有 `#pragma comment(lib, "d3d11.lib")`，且
`nimrtc_video_sink` 在 Windows 上 PRIVATE 链 `d3d11` / `dxgi`。安装树里的
archive 丢了这些属性，所以 `NimRTCConfig.cmake.in` 的 WIN32 系统库列表
补 `d3d11` / `dxgi`（在既有 `ws2_32 bcrypt iphlpapi winmm crypt32` 之后）。

> 若未来的某次改动让 NVENC 走**静态** NVIDIA import lib，本决定必须重审。

### 2.5 决策五：安装树必须自洽 —— 干净 prefix 上验

bundle **不能**依赖宿主机器上恰好装了别的东西。验收在**干净的 prefix**
（`build/_install/windows-x86_64`）上做，下游视频宿主只用 bundle 里的
archive 链通（0 个 LNK2019）。这个自洽由 `tools/check_install_tree_video_deps.py`
在源侧 + 树侧逐项验证。

## 3. 后果

- 安装树里 `nimrtc::engine` 能链出带视频的宿主（h264 + headless sink，
  0 个 LNK2019）。
- N6 执行腿的缺口 B（暂存 config 不链 `nvcodec` + `openh264`）被解除。
- 音频 / 基础路径的既有链（vendor 名、APM 清单、系统库）**不动** ——
  只在 WIN32 列表尾部追加两个系统库。
- `src/engine/` 与 `src/modules/ice/` **不碰**（本片是打包层的事）。

## 4. 验证

- `tools/check_install_tree_video_deps.py` 对暂存树跑绿 + 自检负样本被报出。
- `tests/acceptance/v1-install-tree-video-deps/acceptance.test.cjs` 真的
  暂存一个干净 prefix、真的链一个下游视频宿主、断言 0 个 LNK2019。
